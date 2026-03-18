/*
 * AXI2CHI Bridge - Header File
 *
 * 本设计实现了一个可配置的 AXI 到 CHI 协议桥接模型，
 * 用于毕业设计的性能评估。支持两种工作模式：
 *   1. 基准模式 (CF'20 Paper): 固定按 Cache Line 拆分，无合并
 *   2. CNBA 模式 (本设计):  动态聚合/拆分 + QoS 调度
 *   3. 朴素桥模式 (Naive): 逐 beat 拆分，不考虑 CL 边界，不合并
 *
 */

#ifndef __DEV_AXI2CHI_AXI2CHI_BRIDGE_HH__
#define __DEV_AXI2CHI_AXI2CHI_BRIDGE_HH__

#include <bitset>
#include <deque>
#include <queue>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/port.hh"
#include "params/AXI2CHIBridge.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

/**
 * AXI2CHIBridge: 连接 Non-Coherent AXI 总线和 Coherent CHI 互连。
 *
 * 一端接收来自 DMA/CPU 的 AXI 突发请求(模拟 Non-Coherent 流量)，
 * 另一端向一致性内存系统发送 CHI 事务(模拟 Coherent 流量)。
 *
 * 支持通过 Python 配置参数切换基准模式与 CNBA 模式。
 */
class AXI2CHIBridge : public ClockedObject
{
  public:
    PARAMS(AXI2CHIBridge);
    AXI2CHIBridge(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void init() override;

  public:
    // ================================================================
    // SenderState: 用于在 CHI 请求/响应中追踪 TxnID
    // ================================================================
    struct CHISenderState : public Packet::SenderState
    {
        unsigned txnId;
        unsigned parentAxiId;
        CHISenderState(unsigned _txnId, unsigned _axiId)
            : txnId(_txnId), parentAxiId(_axiId) {}
    };

    // ================================================================
    // AXI 突发类型定义 (public for TransactionScheduler access)
    // ================================================================
    enum AXIBurstType {
        BURST_FIXED = 0,  // 固定突发: 每个beat地址不变
        BURST_INCR  = 1,  // 递增突发: 地址递增
        BURST_WRAP  = 2   // 回绕突发: 地址到达上界后回绕
    };

    // ================================================================
    // 内部事务追踪结构
    // ================================================================

    /**
     * AXITransaction: 跟踪一个来自 AXI 侧的原始请求。
     * 一个 AXI burst 可能会被拆分为多个 CHI 事务。
     */
    struct AXITransaction
    {
        PacketPtr origPkt;         // 原始 AXI 请求包
        Addr startAddr;            // 起始地址
        Addr endAddr;              // 结束地址
        unsigned burstLen;         // AXI Burst Length (AxLEN + 1)
        unsigned burstSize;        // 每个beat的字节数 (2^AxSIZE)
        AXIBurstType burstType;    // AXI 突发类型
        unsigned totalSize;        // 总传输字节数
        bool isWrite;              // 读/写标记

        // 目标 CHI 请求数（不是“已发出”的数量）。当 TxnID/队列资源不足时，
        // 拆分可能被分多次完成，chiReqCount 用于判断事务何时可以完成。
        unsigned chiReqCount;
        unsigned chiReqIssued;     // 已发出的 CHI 请求数（用于拆分恢复/调试）
        unsigned chiRespCount;     // 已收到的 CHI 响应数
        bool completed;            // 所有 CHI 响应已收到

        // 拆分恢复状态：当 TxnID 耗尽或请求队列满时，拆分会暂停，等待资源释放后继续。
        enum class SplitStrategy : uint8_t { Naive, Baseline, CNBA };
        SplitStrategy splitStrategy;
        bool splitInitialized;
        Addr splitNextAddr;        // 下一段要生成 CHI 请求的地址
        unsigned splitRemaining;   // 还未转换成 CHI 的字节数
        unsigned splitChunkSize;   // 每个 CHI 请求覆盖的“步长”（Naive/Baseline=beat或64，CNBA=64）
        bool splitUseByteEnable;   // true: 固定 splitChunkSize 请求 + byte-enable 掩码(剩余不足时)

        // QoS 优先级 (CNBA 模式)
        unsigned qosPriority;      // 0=低, 1=高

        Tick arrivalTick;          // 到达时间 (用于延迟统计)
        bool countedForLimit;      // 是否属于 max_axi_requests 统计窗口

        AXITransaction()
            : origPkt(nullptr), startAddr(0), endAddr(0),
              burstLen(1), burstSize(1), burstType(BURST_INCR),
              totalSize(0), isWrite(false),
              chiReqCount(0), chiReqIssued(0), chiRespCount(0), completed(false),
              splitStrategy(SplitStrategy::Baseline),
              splitInitialized(false),
              splitNextAddr(0), splitRemaining(0),
              splitChunkSize(0), splitUseByteEnable(false),
              qosPriority(0), arrivalTick(0), countedForLimit(false)
        {}
    };

    /**
     * CHIRequest: 一个发往下游的 CHI 事务。
     * 可能对应一个完整的 AXI burst，也可能是拆分后的一部分。
     */
    struct CHIRequest
    {
        PacketPtr pkt;             // gem5 请求包
        unsigned txnId;            // CHI TxnID
        unsigned parentAxiId;      // 所属 AXI 事务 ID
        Tick scheduledTick;        // 计划发送时间

        CHIRequest()
            : pkt(nullptr), txnId(0), parentAxiId(0), scheduledTick(0)
        {}
    };

  protected:
    // ================================================================
    // 端口定义
    // ================================================================

    /**
     * AXI 从端口: 接收来自 DMA/CPU 的突发请求。
     * 在 gem5 中建模为 ResponsePort (从所连接的Requestor角度看是Responder)。
     */
    class AXISlavePort : public ResponsePort
    {
      private:
        AXI2CHIBridge &bridge;

      public:
        bool needRetry;  // 是否需要向上游发送 retry

        AXISlavePort(const std::string &_name, AXI2CHIBridge &_bridge);

      protected:
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;
    };

    /**
     * CHI 主端口: 向一致性内存系统发送请求。
     * 在 gem5 中建模为 RequestPort。
     */
    class CHIMasterPort : public RequestPort
    {
      private:
        AXI2CHIBridge &bridge;

      public:
        CHIMasterPort(const std::string &_name, AXI2CHIBridge &_bridge);

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    // ================================================================
    // 端口实例
    // ================================================================
    AXISlavePort  axiSlavePort;
    CHIMasterPort chiMasterPort;

    // ================================================================
    // 配置参数 (从 Python Params 获取)
    // ================================================================
    const bool enableMergeSplit;     // 是否开启动态聚合/拆分 (CNBA)
    const bool useBaselineLogic;     // 是否使用 CF'20 基准模式
    const bool useNaiveLogic;       // 是否使用朴素桥模式 (逐beat拆分)
    const bool qosEnabled;          // 是否开启 QoS 调度
    const unsigned cacheLineSize;   // Cache Line 大小 (默认 64B)
    const unsigned axiBeatSize;     // AXI 每beat字节数
    const bool randomAxiBeatSize;   // 是否随机化 beat 大小
    const unsigned minAxiSize;       // 随机 AxSIZE 最小值 (0..5)
    const unsigned maxAxiRequests;  // 接收多少个 AXI 请求后结束仿真 (0=不限)
    const unsigned maxTxnId;        // 最大 CHI TxnID 数量 (默认 128)
    const Cycles bridgeLatency;     // 桥接处理延迟
    const Cycles convertTime;       // 事务转换延迟 (从接收到开始发送的周期数, 默认 5)
    const unsigned reqQueueSize;    // 请求队列深度
    const unsigned respQueueSize;   // 响应队列深度
    const AddrRangeList addrRanges; // 地址范围

    // ================================================================
    // 事务管理
    // ================================================================

    /**
     * ROB (Re-Order Buffer): 按 TxnID 索引的事务表。
     * Ref: CF'20 Paper Section 3.1 - ROB 机制
     */
    std::unordered_map<unsigned, AXITransaction> axiTransactions;
    unsigned nextAxiTxnId;

    /**
     * TxnID 管理: 使用 bitset 追踪哪些 TxnID 正在使用。
     * 基准模式下 TxnID 耗尽时必须 stall (论文关键瓶颈);
     * CNBA 模式下合并减少事务数，降低耗尽概率。
     */
    static inline constexpr unsigned MAX_TXN_IDS = 256;
    std::bitset<MAX_TXN_IDS> txnIdBitmap;
    unsigned activeTxnCount;

    // 待发送的 CHI 请求队列
    std::deque<CHIRequest> chiReqQueue;

    // 待发送的 AXI 响应队列
    std::deque<PacketPtr> axiRespQueue;
    std::deque<bool> axiRespIsCounted;
    unsigned pendingCountedAxiResponses;

    // 是否因为某种原因正在暂停接收
    bool blocked;

    // gem5 timing 协议: 跟踪是否等待 XBar 重试回调
    // 当 sendTimingReq/Resp 失败后设为 true，收到 retry 后清除
    bool chiWaitingForRetry;
    bool axiWaitingForRetry;
    bool requestLimitReached;
    bool drainExitIssued;

    // ================================================================
    // QoS 调度器 (CNBA 模式)
    // Ref: CNBA Proposal Section 2.4 - QoS 调度
    // ================================================================

    /**
     * 多级优先级队列 + 加权轮询 (WRR)
     * highPrioQueue: 高优先级请求队列
     * lowPrioQueue:  低优先级请求队列
     * wrrWeight:     WRR 权重 (高:低)
     * wrrCounter:    当前 WRR 计数器
     */
    std::queue<CHIRequest> highPrioQueue;
    std::queue<CHIRequest> lowPrioQueue;
    unsigned wrrHighWeight;
    unsigned wrrLowWeight;
    unsigned wrrHighCounter;
    unsigned wrrLowCounter;

    // ================================================================
    // 核心处理函数
    // ================================================================

    /**
     * 处理来自 AXI 侧的请求。
     * 根据模式选择基准逻辑或 CNBA 逻辑。
     */
    bool handleAXIRequest(PacketPtr pkt);

    /**
     * 处理来自 CHI 侧的响应。
     * 更新事务状态，检查是否可以向 AXI 发送响应。
     */
    bool handleCHIResponse(PacketPtr pkt);

    /**
     * 基准模式: 固定拆分 AXI burst 为 CHI 事务。
     * Ref: CF'20 Paper Section 3 - Transaction Translation
     */
    void baselineSplitTransaction(unsigned axiId, AXITransaction &txn);

    /**
     * CNBA 模式: 动态聚合/拆分。
     * Ref: CNBA Proposal - 动态事务聚合/拆分算法
     */
    void cnbaSplitMergeTransaction(unsigned axiId, AXITransaction &txn);

    /**
     * 朴素桥模式: 逐 beat 拆分，一条 AXI 请求拆成 Len+1 条 CHI 请求。
     * 不考虑 Cache Line 边界对齐，不合并，每个 beat 独立发送。
     */
    void naiveSplitTransaction(unsigned axiId, AXITransaction &txn);

    /**
     * CNBA 地址对齐算法: 计算跨 Cache Line 的拆分数量。
     * 公式: num = (end_addr >> page_bits) - (start_addr >> page_bits)
     * Ref: CNBA Proposal + Chisel 源码
     */
    unsigned calcSplitCount(Addr startAddr, Addr endAddr,
                            unsigned cacheLineSize);

    /**
     * 将生成的 CHI 请求加入发送队列。
     * QoS 模式下按优先级分流到不同队列。
     */
    void enqueueCHIRequest(CHIRequest &req, unsigned priority);

    /**
     * 从 QoS 队列中选取下一个要发送的请求 (WRR 仲裁)。
     */
    bool scheduleNextCHIRequest();

    /**
     * 返回当前待发送 CHI 请求中最早可发送的时间戳。
     * 若无请求则返回 MaxTick。
     */
    Tick nextReadyCHITick() const;

    /**
     * 尝试发送 CHI 请求队列头部的请求。
     */
    void trySendCHIRequest();

    /**
     * 尝试发送 AXI 响应。
     */
    void trySendAXIResponse();

    /**
     * 分配一个空闲的 TxnID; 若无可用则返回 -1。
     */
    int allocateTxnId();

    /**
     * 释放一个 TxnID。
     */
    void freeTxnId(unsigned id);

    /**
     * 检查是否因 TxnID 耗尽或队列满而阻塞。
     */
    bool isBlocked() const;

    /**
     * 检查是否满足“达到请求上限后已完全排空”的退出条件。
     * 条件满足时触发 exitSimLoop。
     */
    void checkDrainAndExit();

    // ---- 拆分恢复与阻塞管理 ----
    size_t pendingChiReqs() const;
    void issueMoreCHIRequests(unsigned axiId, AXITransaction &txn);
    void resumePendingSplits();
    void maybeUnblockAxi();

    /**
     * 计算 AXI WRAP 突发的实际地址序列。
     * Ref: CNBA - 全突发模式支持
     */
    Addr calcWrapAddress(Addr startAddr, unsigned beatIdx,
                         unsigned burstSize, unsigned burstLen);

    // 事件回调
    EventFunctionWrapper sendCHIReqEvent;
    EventFunctionWrapper sendAXIRespEvent;

    // ================================================================
    // 统计 (Statistics)
    // ================================================================
    struct BridgeStats : public statistics::Group
    {
        BridgeStats(AXI2CHIBridge *parent);

        // ---- Scalar stats (必须在 Formula 之前声明) ----
        statistics::Scalar totalAxiRequests;
        statistics::Scalar totalChiRequests;
        statistics::Scalar mergeCount;
        statistics::Scalar splitCount;
        statistics::Scalar totalLatency;
        statistics::Scalar totalBytes;
        statistics::Scalar robStallCycles;
        statistics::Scalar totalReads;
        statistics::Scalar totalWrites;

        // ---- Histogram ----
        statistics::Histogram latencyHist;

        // ---- Formula stats (引用上面的 Scalar) ----
        statistics::Formula avgLatency;
        statistics::Formula throughput;
        statistics::Formula mergeRate;
    };

    BridgeStats stats;
};

} // namespace gem5

#endif // __DEV_AXI2CHI_AXI2CHI_BRIDGE_HH__
