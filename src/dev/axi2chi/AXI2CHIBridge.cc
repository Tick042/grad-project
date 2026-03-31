/*
 * AXI2CHI Bridge - 核心实现文件
 *
 * ============================================================
 * 功能概述:
 *   实现了可配置的 AXI→CHI 协议桥接，支持三种工作模式：
 *   1. 朴素桥模式 (Naive):   逐 beat 拆分，一条 AXI 请求→Len+1 条 CHI 请求
 *                             不考虑 Cache Line 对齐，不合并，最简单的转换策略
 *   2. 基准模式 (CF'20 Paper): 固定按 Cache Line (64B) 边界拆分，无合并
 *                             复现论文 "Design of an Open-Source Bridge ..."
 *   3. CNBA 模式 (本设计):   动态聚合/拆分 + QoS 调度
 *                             不跨 CL 的请求合并为 1 个 CHI 请求，跨 CL 智能拆分
 *
 * ============================================================
 * 事务处理流程:
 *   1. AXI 请求从 axiSlavePort 进入 (recvTimingReq)
 *   2. handleAXIRequest() 根据模式选择拆分/合并策略
 *   3. 生成的 CHI 请求进入 chiReqQueue (或 QoS 优先级队列)
 *   4. trySendCHIRequest() 通过 chiMasterPort 发送到下游
 *   5. 下游响应从 chiMasterPort 回来 (recvTimingResp)
 *   6. handleCHIResponse() 更新事务状态，所有子响应收齐后生成 AXI 响应
 *   7. trySendAXIResponse() 通过 axiSlavePort 发送回上游
 *
 * ============================================================
 * TxnID 管理 (三种模式共享):
 *   - 使用 bitset 追踪哪些 TxnID 正在使用
 *   - TxnID 耗尽时桥接器 stall，不再接受新 AXI 请求
 *   - CHI 响应返回后释放 TxnID，解除 stall
 *   - 这是 CF'20 论文指出的性能关键瓶颈
 *
 * Ref: CF'20 Paper Section 3 - Transaction Translation
 * Ref: CNBA Proposal Section 2 - 动态事务聚合/拆分算法
 */

#include "dev/axi2chi/AXI2CHIBridge.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/AXI2CHIBridge.hh"
#include "sim/cur_tick.hh"
#include "sim/sim_exit.hh"
#include "sim/stats.hh"

namespace gem5
{

// ====================================================================
// 构造函数
// ====================================================================

AXI2CHIBridge::AXI2CHIBridge(const Params &p)
    : ClockedObject(p),
      // 端口初始化
      axiSlavePort(p.name + ".axi_slave_port", *this),
      chiMasterPort(p.name + ".chi_master_port", *this),
      // 配置参数
      enableMergeSplit(p.enable_merge_split),
      useBaselineLogic(p.use_baseline_logic),
      useNaiveLogic(p.use_naive_logic),
      qosEnabled(p.qos_enabled),
      cacheLineSize(p.cache_line_size),
      axiBeatSize(p.axi_beat_size),
      randomAxiBeatSize(p.random_axi_beat_size),
      minAxiSize(p.min_axi_size),
      trafficPattern(p.traffic_pattern),
      maxAxiRequests(p.max_axi_requests),
      maxTxnId(p.max_txn_id),
      bridgeLatency(p.bridge_latency),
      convertTime(p.convert_time),
      reqQueueSize(p.req_queue_size),
      respQueueSize(p.resp_queue_size),
      addrRanges(p.ranges.begin(), p.ranges.end()),
      // 事务管理初始化
      nextAxiTxnId(0),
      activeTxnCount(0),
      blocked(false),
      chiWaitingForRetry(false),
      axiWaitingForRetry(false),
      requestLimitReached(false),
      drainExitIssued(false),
      pendingCountedAxiResponses(0),
      // QoS 调度器
      wrrHighWeight(p.wrr_high_weight),
      wrrLowWeight(p.wrr_low_weight),
      wrrHighCounter(0),
      wrrLowCounter(0),
      // 事件回调
      sendCHIReqEvent([this]{ trySendCHIRequest(); },
                      p.name + ".sendCHIReqEvent"),
      sendAXIRespEvent([this]{ trySendAXIResponse(); },
                       p.name + ".sendAXIRespEvent"),
      // 统计
      stats(this)
{
    // 检查参数合法性
    panic_if(maxTxnId > MAX_TXN_IDS,
             "max_txn_id (%d) exceeds MAX_TXN_IDS (%d)",
             maxTxnId, MAX_TXN_IDS);
    panic_if(cacheLineSize == 0 || (cacheLineSize & (cacheLineSize - 1)),
             "cache_line_size must be a power of 2, got %d", cacheLineSize);

    DPRINTF(AXI2CHIBridge,
            "AXI2CHIBridge created: mode=%s, merge_split=%d, "
            "qos=%d, cache_line=%dB, axi_beat=%dB, max_txn=%d, "
            "convert_time=%d cycles\n",
            useNaiveLogic ? "Naive" :
                (useBaselineLogic ? "Baseline(CF'20)" : "CNBA"),
            enableMergeSplit, qosEnabled, cacheLineSize,
            axiBeatSize, maxTxnId, convertTime);
}

// ====================================================================
// 端口连接
// ====================================================================

Port &
AXI2CHIBridge::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "axi_slave_port")
        return axiSlavePort;
    else if (if_name == "chi_master_port")
        return chiMasterPort;
    else
        return ClockedObject::getPort(if_name, idx);
}

void
AXI2CHIBridge::init()
{
    // 确保两端都已连接
    if (!axiSlavePort.isConnected() || !chiMasterPort.isConnected())
        fatal("AXI2CHIBridge: Both ports must be connected.\n");

    axiSlavePort.sendRangeChange();
}

// ====================================================================
// AXISlavePort 实现
// ====================================================================

AXI2CHIBridge::AXISlavePort::AXISlavePort(
    const std::string &_name, AXI2CHIBridge &_bridge)
    : ResponsePort(_name), bridge(_bridge), needRetry(false)
{}

bool
AXI2CHIBridge::AXISlavePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(AXI2CHIBridge,
            "AXISlavePort: Received timing request addr=%#x, size=%d, %s\n",
            pkt->getAddr(), pkt->getSize(),
            pkt->isRead() ? "READ" : "WRITE");

    if (!bridge.handleAXIRequest(pkt)) {
        // 桥接器忙，需要稍后重试
        needRetry = true;
        return false;
    }
    return true;
}

void
AXI2CHIBridge::AXISlavePort::recvRespRetry()
{
    // 下游可以接收响应了，尝试重新发送
    bridge.axiWaitingForRetry = false;
    bridge.trySendAXIResponse();
}

Tick
AXI2CHIBridge::AXISlavePort::recvAtomic(PacketPtr pkt)
{
    // Atomic 模式: 直接转发到 CHI 侧，加上固定延迟
    // Ref: gem5 Bridge 的 Atomic 实现模式
    DPRINTF(AXI2CHIBridge, "AXISlavePort: Atomic access addr=%#x\n",
            pkt->getAddr());
    return bridge.chiMasterPort.sendAtomic(pkt) +
           bridge.clockPeriod() * bridge.bridgeLatency;
}

void
AXI2CHIBridge::AXISlavePort::recvFunctional(PacketPtr pkt)
{
    // Functional 模式: 直接转发
    bridge.chiMasterPort.sendFunctional(pkt);
}

AddrRangeList
AXI2CHIBridge::AXISlavePort::getAddrRanges() const
{
    return bridge.addrRanges;
}

// ====================================================================
// CHIMasterPort 实现
// ====================================================================

AXI2CHIBridge::CHIMasterPort::CHIMasterPort(
    const std::string &_name, AXI2CHIBridge &_bridge)
    : RequestPort(_name), bridge(_bridge)
{}

bool
AXI2CHIBridge::CHIMasterPort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(AXI2CHIBridge,
            "CHIMasterPort: Received CHI response addr=%#x\n",
            pkt->getAddr());
    return bridge.handleCHIResponse(pkt);
}

void
AXI2CHIBridge::CHIMasterPort::recvReqRetry()
{
    // CHI 侧准备好接收了，重新尝试发送
    DPRINTF(AXI2CHIBridge, "CHIMasterPort: Received retry\n");
    bridge.chiWaitingForRetry = false;
    bridge.trySendCHIRequest();
}

// ====================================================================
// 核心事务处理
// ====================================================================

/**
 * handleAXIRequest: 接收并处理一个 AXI 请求。
 *
 * 这是桥接器的核心入口函数，每次 AXI 侧收到一个突发请求时调用。
 *
 * ============================================================
 * 处理流程:
 *   Step 1: 检查是否被阻塞 (TxnID 耗尽 / 请求队列满)
 *           → 若阻塞则返回 false, AXI 侧将在后续重试
 *   Step 2: 解析 AXI Burst 参数 (地址、大小、读写类型)
 *           → 从 gem5 Packet 推导 burst_len 和 burst_size
 *   Step 3: 根据模式选择事务转换策略
 *           → naive:    naiveSplitTransaction()    逐 beat 拆分
 *           → baseline: baselineSplitTransaction() 按 CL 边界拆分
 *           → cnba:     cnbaSplitMergeTransaction() 智能合并/拆分
 *   Step 4: 调度 CHI 请求发送
 *           → 将 sendCHIReqEvent 设定在 bridgeLatency 周期后触发
 *
 * ============================================================
 * 三种模式的拆分策略对比 (以 256B AXI 请求为例):
 *
 *   朴素桥 (beat=32B):
 *     256B ÷ 32B = 8 个 CHI 请求 (不管 CL 边界)
 *     [0x00,32B] [0x20,32B] [0x40,32B] ... [0xE0,32B]
 *
 *   Baseline (CL=64B):
 *     256B ÷ 64B = 4 个 CHI 请求 (按 CL 边界对齐)
 *     [0x00,64B] [0x40,64B] [0x80,64B] [0xC0,64B]
 *
 *   CNBA (CL=64B, 智能合并):
 *     若地址 CL 对齐: 同 Baseline → 4 个 CHI 请求
 *     若请求 <= CL: 直接合并为 1 个 CHI 请求
 */
bool
AXI2CHIBridge::handleAXIRequest(PacketPtr pkt)
{
    if (maxAxiRequests > 0 &&
        stats.totalAxiRequests.value() >= maxAxiRequests) {
        requestLimitReached = true;
        // 达到请求上限后拒绝新请求, 等待已计数事务排空
        // 返回 false 使 TrafficGen 停止发送
        blocked = true;
        return false;
    }

    // Step 1: 检查是否被阻塞
    if (isBlocked()) {
        DPRINTF(AXI2CHIBridge,
                "Bridge blocked: activeTxn=%d, chiQueue=%d\n",
                activeTxnCount, chiReqQueue.size());
        // Ref: CF'20 Paper - TxnID 耗尽时阻塞新请求
        stats.robStallCycles++;
        blocked = true;  // 标记阻塞，等 CHI 响应解除后发 retry
        return false;
    }

    // Step 2: 解析 AXI 请求参数构建 AXITransaction
    AXITransaction txn;
    txn.origPkt = pkt;
    txn.startAddr = pkt->getAddr();
    txn.totalSize = pkt->getSize();
    txn.isWrite = pkt->isWrite();
    txn.arrivalTick = curTick();

    // 从 packet size 推导 AXI burst 参数
    // burstSize = 每个 AXI beat 的字节数 = 2^AxSIZE
    // 随机模式下 AxSIZE 在 [0, 5] 内均匀随机 (1/2/4/8/16/32B)
    unsigned effectiveBeat;
    if (randomAxiBeatSize) {
        unsigned axiSize;
        if (trafficPattern == "conv") {
            // 卷积模式: 5% size=3(8B), 10% size=4(16B), 85% size=5(32B)
            unsigned roll = random_mt.random<unsigned>(0, 99);
            if (roll < 5)
                axiSize = 3;
            else if (roll < 15)
                axiSize = 4;
            else
                axiSize = 5;
        } else {
            axiSize = random_mt.random<unsigned>(minAxiSize, 5);
        }
        effectiveBeat = 1U << axiSize;
    } else {
        effectiveBeat = axiBeatSize;
    }
    txn.burstSize = std::min(txn.totalSize, effectiveBeat);
    txn.burstLen = (txn.totalSize + txn.burstSize - 1) / txn.burstSize;
    txn.endAddr = txn.startAddr + txn.totalSize - 1;
    txn.burstType = BURST_INCR;  // 默认 INCR

    // QoS 优先级分类: totalSize <= 64B → 高优先级 (延迟敏感)
    // 仅在 qosEnabled 时生效，否则 qosPriority 保持默认 0
    if (qosEnabled) {
        txn.qosPriority = (txn.totalSize <= 64) ? 1 : 0;
    }

    // 如果不是 INCR 且在基准模式，报告不支持
    // Ref: CF'20 Paper - 仅支持 INCR 突发
    if (useBaselineLogic && txn.burstType != BURST_INCR) {
        warn("Baseline mode: Only INCR burst supported, "
             "ignoring burst type %d\n", txn.burstType);
    }

    // 分配内部事务 ID
    unsigned axiId = nextAxiTxnId++;
    axiTransactions[axiId] = txn;
    AXITransaction &storedTxn = axiTransactions[axiId];

    const bool countThisTxn =
        (maxAxiRequests == 0) ||
        (stats.totalAxiRequests.value() < maxAxiRequests);
    storedTxn.countedForLimit = countThisTxn;

    // 统计
    if (countThisTxn) {
        stats.totalAxiRequests++;
        stats.totalBytes += storedTxn.totalSize;
        pendingCountedAxiResponses++;
        if (storedTxn.isWrite)
            stats.totalWrites++;
        else
            stats.totalReads++;
    }

    // 达到最大 AXI 请求数后进入 drain 阶段: 等待统计窗口事务排空后退出
    if (maxAxiRequests > 0 && !requestLimitReached &&
        stats.totalAxiRequests.value() >= maxAxiRequests) {
        requestLimitReached = true;
    }

    DPRINTF(AXI2CHIBridge,
            "New AXI txn[%d]: addr=%#x, size=%d, burst_len=%d, "
            "burst_size=%d, %s\n",
            axiId, txn.startAddr, txn.totalSize,
            txn.burstLen, txn.burstSize,
            txn.isWrite ? "WR" : "RD");

    // Step 3: 根据模式选择事务转换策略
    if (useNaiveLogic) {
        // ---- 朴素桥模式: 逐 beat 拆分 ----
        naiveSplitTransaction(axiId, storedTxn);
    } else if (useBaselineLogic) {
        // ---- 基准模式 (CF'20 Paper Logic) ----
        baselineSplitTransaction(axiId, storedTxn);
    } else {
        // ---- CNBA 模式 (本设计 Logic) ----
        if (enableMergeSplit) {
            cnbaSplitMergeTransaction(axiId, storedTxn);
        } else {
            // 消融实验: CNBA 架构但关闭合并, 仅做拆分
            baselineSplitTransaction(axiId, storedTxn);
        }
    }

    // Step 4: 调度 CHI 请求发送
    if (!chiWaitingForRetry && !sendCHIReqEvent.scheduled()) {
        // 使用每条请求的 scheduledTick 控制转换延迟
        Tick next_ready = nextReadyCHITick();
        if (next_ready != MaxTick) {
            schedule(sendCHIReqEvent, std::max(next_ready, curTick()));
        }
    }

    checkDrainAndExit();
    return true;
}

// ====================================================================
// 基准模式: 固定拆分
// Ref: CF'20 Paper Section 3 - 固定按 Cache Line 拆分
// ====================================================================

/**
 * baselineSplitTransaction:
 *
 * Baseline 模式的核心逻辑:
 * - AxSIZE < 5 (beat < 32B): 逐 beat 拆分，每个 beat → 1 个 CHI 请求
 * - AxSIZE == 5 (beat == 32B): 合并为 Size==6 (64B) 的 CHI 请求
 *   (Size==6 在 CHI 中表示 2^6 = 64B = 2 个顺序 beat)
 *
 * 这是比 Naive 更智能的策略，但仍不如 CNBA。
 */
void
AXI2CHIBridge::baselineSplitTransaction(unsigned axiId, AXITransaction &txn)
{
    unsigned beatSize = txn.burstSize;
    if (beatSize == 0) beatSize = 1;

    // 判断是否为宽拍 (AxSIZE == 5，即 beatSize == 32B)
    bool wideBeat = (beatSize == 32);

    if (!txn.splitInitialized) {
        txn.splitInitialized = true;
        txn.splitStrategy = AXITransaction::SplitStrategy::Baseline;
        txn.chiReqIssued = 0;
        txn.chiRespCount = 0;

        txn.splitNextAddr = txn.startAddr;
        txn.splitRemaining = txn.totalSize;

        if (wideBeat) {
            txn.splitChunkSize = 64U;
            txn.splitUseByteEnable = true;
        } else {
            txn.splitChunkSize = beatSize;
            txn.splitUseByteEnable = false;
        }

        txn.chiReqCount =
            (txn.totalSize + txn.splitChunkSize - 1) / txn.splitChunkSize;
        if (txn.chiReqCount > 1) {
            stats.splitCount++;
        }
    }

    issueMoreCHIRequests(axiId, txn);
}

// ====================================================================
// 朴素桥模式: 逐 beat 拆分
// 一条 AXI 请求拆为 Len+1 条 CHI 请求，每个 beat 独立发送
// ====================================================================

/**
 * naiveSplitTransaction: 朴素桥的核心事务转换逻辑
 *
 * ============================================================
 * 设计思路:
 *   最简单的 AXI→CHI 协议转换 —— 完全不考虑 Cache Line 对齐，
 *   将每个 AXI beat 直接映射为一个独立的 CHI 请求。
 *
 * ============================================================
 * 算法:
 *   beat_size = axi_beat_size (配置参数，默认 32B = AXI 256-bit 总线宽度)
 *   num_beats = ceil(totalSize / beat_size) = AXI 协议中的 AxLEN + 1
 *   每个 beat → 1 个 CHI 请求 (大小为 beat_size，最后一个可能更小)
 *
 * ============================================================
 * 与 Baseline 和 CNBA 的关键区别:
 *
 *   朴素桥:  拆分粒度 = beat_size (32B)
 *     256B 请求 → 8 个 CHI 请求 (256/32=8)
 *     64B 请求  → 2 个 CHI 请求 (64/32=2)
 *     4B 请求   → 1 个 CHI 请求 (4<32, 只需1个)
 *
 *   Baseline: 拆分粒度 = cache_line_size (64B), 按 CL 边界对齐
 *     256B 请求 → 4 个 CHI 请求 (256/64=4)
 *     64B 请求  → 1 个 CHI 请求 (CL 对齐时)
 *     4B 请求   → 1 个 CHI 请求
 *
 *   CNBA:     智能合并+拆分
 *     256B 请求 → 4 个 CHI 请求 (等同 Baseline)
 *     64B 请求  → 1 个 CHI 请求 (合并)
 *     4B 请求   → 1 个 CHI 请求 (合并，不跨 CL)
 *
 * ============================================================
 * 性能特点:
 *   - CHI 请求数最多 → TxnID 消耗最快 → stall 概率最高
 *   - 不利用 Cache Line 局部性 → 下游内存效率低
 *   - 作为性能下界 (worst case) 对比使用
 */
void
AXI2CHIBridge::naiveSplitTransaction(unsigned axiId, AXITransaction &txn)
{
    // 每个 beat 的大小: 使用 txn.burstSize（已在 handleAXIRequest 中计算好）
    unsigned beatSize = txn.burstSize;
    if (beatSize == 0) beatSize = 1;

    if (!txn.splitInitialized) {
        txn.splitInitialized = true;
        txn.splitStrategy = AXITransaction::SplitStrategy::Naive;
        txn.chiReqIssued = 0;
        txn.chiRespCount = 0;

        txn.splitNextAddr = txn.startAddr;
        txn.splitRemaining = txn.totalSize;
        txn.splitChunkSize = beatSize;
        txn.splitUseByteEnable = false;

        txn.chiReqCount =
            (txn.totalSize + txn.splitChunkSize - 1) / txn.splitChunkSize;
        if (txn.chiReqCount > 1) {
            stats.splitCount++;
        }
    }

    issueMoreCHIRequests(axiId, txn);
}

// ====================================================================
// CNBA 模式: 动态聚合/拆分
// Ref: CNBA Proposal - 动态事务聚合/拆分算法
// ====================================================================

/**
 * cnbaSplitMergeTransaction:
 *
 * CNBA 核心算法 —— 最高效率的拆分:
 * 无论 AxSIZE 为多大，都将所有 AXI 请求拆分为 Size==6 (64B) 的 CHI 请求。
 * 这是最优的缓存行对齐策略，能最小化 CHI 请求数量。
 *
 * 算法:
 *   - 将 totalSize 的数据分割成多个 64B 块
 *   - 每个块生成 1 个 CHI Size==6 请求
 *   - 若最后不足 64B, 仍发送 64B 请求并设置 byte-enable
 *
 * 性能优势:
 *   - CHI 请求数最少 → TxnID 消耗最低
 *   - 完全按 Cache Line 对齐 → 下游内存效率最高
 *   - ROB stall 最低
 */
void
AXI2CHIBridge::cnbaSplitMergeTransaction(unsigned axiId,
                                         AXITransaction &txn)
{
    if (!txn.splitInitialized) {
        txn.splitInitialized = true;
        txn.splitStrategy = AXITransaction::SplitStrategy::CNBA;
        txn.chiReqIssued = 0;
        txn.chiRespCount = 0;

        txn.splitNextAddr = txn.startAddr;
        txn.splitRemaining = txn.totalSize;
        txn.splitChunkSize = 64U;
        txn.splitUseByteEnable = true;

        txn.chiReqCount =
            (txn.totalSize + txn.splitChunkSize - 1) / txn.splitChunkSize;

        // mergeCount: 相比朴素逐beat拆分节省的 CHI 请求数
        unsigned naiveCount = txn.burstLen;  // Len+1 个 CHI 请求 (朴素桥)
        if (txn.chiReqCount < naiveCount) {
            stats.mergeCount += (naiveCount - txn.chiReqCount);
        }
        if (txn.chiReqCount > 1) {
            stats.splitCount++;
        }
    }

    issueMoreCHIRequests(axiId, txn);
}

size_t
AXI2CHIBridge::pendingChiReqs() const
{
    return chiReqQueue.size() + highPrioQueue.size() + lowPrioQueue.size();
}

void
AXI2CHIBridge::issueMoreCHIRequests(unsigned axiId, AXITransaction &txn)
{
    bool progress = false;

    while (txn.splitRemaining > 0) {
        int txnId = allocateTxnId();
        if (txnId < 0) {
            DPRINTF(AXI2CHIBridge,
                    "TxnID exhausted! Stalling pending split for AXI txn[%u].\n",
                    axiId);
            stats.robStallCycles++;
            blocked = true;
            break;
        }

        const Addr addr = txn.splitNextAddr;
        const unsigned chunk = std::max(1U, txn.splitChunkSize);
        const unsigned remaining = txn.splitRemaining;
        const unsigned effectiveSize =
            txn.splitUseByteEnable ? chunk : std::min(chunk, remaining);

        MemCmd cmd = txn.isWrite ? MemCmd::WriteReq : MemCmd::ReadReq;
        RequestPtr req = std::make_shared<Request>(
            addr, effectiveSize, Request::Flags(0),
            txn.origPkt->req->requestorId());

        if (txn.splitUseByteEnable && remaining < chunk) {
            std::vector<bool> mask(chunk, false);
            for (unsigned i = 0; i < remaining; ++i) {
                mask[i] = true;
            }
            req->setByteEnable(mask);
        }

        PacketPtr chiPkt = new Packet(req, cmd);
        if (txn.isWrite) {
            if (txn.splitUseByteEnable) {
                auto *buf = new uint8_t[chunk];
                std::memset(buf, 0, chunk);
                unsigned offset = addr - txn.startAddr;
                unsigned copySize = std::min(remaining, chunk);
                if (txn.origPkt->hasData() &&
                    offset + copySize <= txn.origPkt->getSize()) {
                    std::memcpy(buf,
                                txn.origPkt->getConstPtr<uint8_t>() + offset,
                                copySize);
                }
                chiPkt->dataDynamic(buf);
            } else {
                chiPkt->allocate();
                if (txn.origPkt->hasData()) {
                    unsigned offset = addr - txn.startAddr;
                    if (offset + effectiveSize <= txn.origPkt->getSize()) {
                        chiPkt->setData(
                            txn.origPkt->getConstPtr<uint8_t>() + offset);
                    }
                }
            }
        } else {
            chiPkt->allocate();
        }

        chiPkt->pushSenderState(new CHISenderState(txnId, axiId));

        // 每个 CHI 请求按顺序发送：第 N 个 CHI 请求在 convertTime + N 周期后发出
        CHIRequest chiReq;
        chiReq.pkt = chiPkt;
        chiReq.txnId = txnId;
        chiReq.parentAxiId = axiId;
        chiReq.scheduledTick = clockEdge(
            convertTime + Cycles(txn.chiReqIssued));

        if (txn.splitStrategy == AXITransaction::SplitStrategy::CNBA &&
            qosEnabled) {
            enqueueCHIRequest(chiReq, txn.qosPriority);
        } else {
            chiReqQueue.push_back(chiReq);
        }

        txn.chiReqIssued++;
        stats.totalChiRequests++;
        progress = true;

        const unsigned advance = std::min(chunk, remaining);
        txn.splitNextAddr += advance;
        txn.splitRemaining -= advance;
    }

    if (progress) {
        DPRINTF(AXI2CHIBridge,
                "AXI txn[%u] issued %u/%u CHI reqs, remaining=%u\n",
                axiId, txn.chiReqIssued, txn.chiReqCount, txn.splitRemaining);

        // 拆分恢复可能发生在 CHI 响应回调中：此时不一定已经调度 sendCHIReqEvent。
        if (!chiWaitingForRetry && !sendCHIReqEvent.scheduled()) {
            Tick next_ready = nextReadyCHITick();
            if (next_ready != MaxTick) {
                schedule(sendCHIReqEvent, std::max(next_ready, curTick()));
            }
        }
    }
}

void
AXI2CHIBridge::resumePendingSplits()
{
    if (isBlocked()) {
        return;
    }

    for (auto &kv : axiTransactions) {
        auto &txn = kv.second;
        if (!txn.splitInitialized || txn.completed || txn.splitRemaining == 0) {
            continue;
        }
        issueMoreCHIRequests(kv.first, txn);
        if (isBlocked()) {
            break;
        }
    }
}

void
AXI2CHIBridge::maybeUnblockAxi()
{
    if (blocked && !isBlocked()) {
        blocked = false;
        if (axiSlavePort.needRetry) {
            axiSlavePort.needRetry = false;
            axiSlavePort.sendRetryReq();
            DPRINTF(AXI2CHIBridge,
                    "Unblocked: sending retry to AXI upstream\n");
        }
    }
}

// ====================================================================
// 地址拆分计算
// Ref: CNBA Proposal 公式
// ====================================================================

unsigned
AXI2CHIBridge::calcSplitCount(Addr startAddr, Addr endAddr,
                               unsigned lineSize)
{
    // 计算跨越的 Cache Line 数量
    // num = (end_addr >> log2(lineSize)) - (start_addr >> log2(lineSize))
    // Ref: CNBA Proposal + AxiWrSlave.sv / AxiRdSlave.sv 中的实现
    unsigned pageBits = (unsigned)std::log2(lineSize);
    unsigned startLine = startAddr >> pageBits;
    unsigned endLine = endAddr >> pageBits;
    return endLine - startLine;  // 0 表示不跨行, >=1 表示跨了N个边界
}

// ====================================================================
// CHI 响应处理
// ====================================================================

/**
 * handleCHIResponse:
 *
 * 收到下游 CHI 侧的响应后:
 * 1. 释放 TxnID
 * 2. 更新 AXI 事务的响应计数
 * 3. 若所有 CHI 子请求都完成, 生成 AXI 响应
 * 4. 若之前因 TxnID 耗尽而阻塞, 重新接受请求
 */
bool
AXI2CHIBridge::handleCHIResponse(PacketPtr pkt)
{
    DPRINTF(AXI2CHIBridge,
            "CHI response: addr=%#x, %s\n",
            pkt->getAddr(), pkt->isRead() ? "RD" : "WR");

    unsigned parentAxiId = std::numeric_limits<unsigned>::max();

    // 从 SenderState 获取 TxnID 和 parentAxiId
    auto *ss = dynamic_cast<CHISenderState*>(pkt->popSenderState());
    if (ss) {
        freeTxnId(ss->txnId);
        parentAxiId = ss->parentAxiId;
        DPRINTF(AXI2CHIBridge,
                "  Freed TxnID=%d, active=%d, parentAxiId=%d\n",
                ss->txnId, activeTxnCount, parentAxiId);
        delete ss;
    }

    auto processResponse = [&](unsigned axiId, AXITransaction &txn) {
        Addr respAddr = pkt->getAddr();
        txn.chiRespCount++;

        // 如果是读请求，将数据复制回原始 AXI 包
        if (pkt->isRead() && txn.origPkt) {
            if (respAddr >= txn.startAddr) {
                unsigned offset = respAddr - txn.startAddr;
                if (offset < txn.totalSize) {
                    unsigned copySize = std::min((unsigned)pkt->getSize(),
                        txn.totalSize - offset);
                    uint8_t *dstPtr = txn.origPkt->getPtr<uint8_t>();
                    if (dstPtr && pkt->hasData()) {
                        std::memcpy(dstPtr + offset,
                                    pkt->getConstPtr<uint8_t>(),
                                    copySize);
                    }
                }
            }
        }

        DPRINTF(AXI2CHIBridge,
                "  AXI txn[%d]: %d/%d CHI responses received\n",
                axiId, txn.chiRespCount, txn.chiReqCount);

        // 检查是否所有 CHI 响应都已收到
        if (txn.chiRespCount >= txn.chiReqCount) {
            txn.completed = true;

            Tick latency = curTick() - txn.arrivalTick;
            if (txn.countedForLimit) {
                stats.latencyHist.sample(latency);
                stats.totalLatency += latency;

                // 分优先级延迟统计 (仅 QoS 开启时有意义)
                if (txn.qosPriority > 0) {
                    stats.highPrioCount++;
                    stats.highPrioLatency += latency;
                } else {
                    stats.lowPrioCount++;
                    stats.lowPrioLatency += latency;
                }
            }

            PacketPtr respPkt = txn.origPkt;
            respPkt->makeResponse();
            axiRespQueue.push_back(respPkt);
            axiRespIsCounted.push_back(txn.countedForLimit);

            if (!axiWaitingForRetry && !sendAXIRespEvent.scheduled()) {
                schedule(sendAXIRespEvent, clockEdge(Cycles(1)));
            }

            DPRINTF(AXI2CHIBridge,
                    "  AXI txn[%d] completed, latency=%llu ticks\n",
                    axiId, latency);
        }
    };

    bool matched = false;
    if (parentAxiId != std::numeric_limits<unsigned>::max()) {
        auto it = axiTransactions.find(parentAxiId);
        if (it != axiTransactions.end() && !it->second.completed) {
            processResponse(parentAxiId, it->second);
            matched = true;
        }
    }

    // 兼容旧包路径: 如果 sender state 丢失, 回退到地址匹配
    if (!matched) {
        Addr respAddr = pkt->getAddr();
        for (auto &[axiId, txn] : axiTransactions) {
            if (txn.completed) {
                continue;
            }
            if (respAddr >= txn.startAddr &&
                respAddr < txn.startAddr + txn.totalSize) {
                processResponse(axiId, txn);
                matched = true;
                break;
            }
        }
    }

    // 释放收到的 CHI 响应包
    delete pkt;

    // TxnID 释放后，优先恢复之前因资源不足而中断的拆分
    resumePendingSplits();
    // 如果之前阻塞了, 检查是否可以解除阻塞
    maybeUnblockAxi();

    checkDrainAndExit();
    return true;
}

// ====================================================================
// TxnID 管理
// Ref: CF'20 Paper - TxnID 限制是性能关键瓶颈
// ====================================================================

int
AXI2CHIBridge::allocateTxnId()
{
    if (activeTxnCount >= maxTxnId) {
        return -1;  // TxnID 耗尽
    }

    // 查找第一个空闲的 TxnID
    for (unsigned i = 0; i < maxTxnId; i++) {
        if (!txnIdBitmap.test(i)) {
            txnIdBitmap.set(i);
            activeTxnCount++;
            return i;
        }
    }

    return -1;
}

void
AXI2CHIBridge::freeTxnId(unsigned id)
{
    panic_if(id >= MAX_TXN_IDS, "Invalid TxnID %d", id);
    panic_if(!txnIdBitmap.test(id), "Freeing unused TxnID %d", id);
    txnIdBitmap.reset(id);
    activeTxnCount--;
}

bool
AXI2CHIBridge::isBlocked() const
{
    // 被阻塞的唯一条件: TxnID 耗尽
    // reqQueueSize 不应成为瓶颈，outstanding 由 TxnID 控制
    return (activeTxnCount >= maxTxnId);
}

void
AXI2CHIBridge::checkDrainAndExit()
{
    if (!requestLimitReached || drainExitIssued) {
        return;
    }

    const size_t pendingReqs = pendingChiReqs();
    const bool axiRespEmpty = axiRespQueue.empty();

    if (pendingCountedAxiResponses == 0 &&
        activeTxnCount == 0 && pendingReqs == 0 && axiRespEmpty) {
        drainExitIssued = true;
        exitSimLoop("AXI request limit reached and drained");
    }
}

// ====================================================================
// QoS 调度器实现
// Ref: CNBA Proposal Section 2.4 - QoS 调度 (优先级 + WRR)
// ====================================================================

void
AXI2CHIBridge::enqueueCHIRequest(CHIRequest &req, unsigned priority)
{
    if (priority > 0) {
        highPrioQueue.push(req);
    } else {
        lowPrioQueue.push(req);
    }

    DPRINTF(AXI2CHIBridge,
            "QoS enqueue: prio=%d, highQ=%d, lowQ=%d\n",
            priority, highPrioQueue.size(), lowPrioQueue.size());
}

/**
 * scheduleNextCHIRequest:
 *
 * WRR (Weighted Round-Robin) 仲裁:
 * 1. 高优先级队列非空时优先服务
 * 2. 按权重交替服务两个队列
 * Ref: CNBA Proposal - 加权轮询仲裁逻辑
 */
Tick
AXI2CHIBridge::nextReadyCHITick() const
{
    Tick next = MaxTick;
    if (!chiReqQueue.empty()) {
        next = std::min(next, chiReqQueue.front().scheduledTick);
    }
    if (!highPrioQueue.empty()) {
        next = std::min(next, highPrioQueue.front().scheduledTick);
    }
    if (!lowPrioQueue.empty()) {
        next = std::min(next, lowPrioQueue.front().scheduledTick);
    }
    return next;
}

// ====================================================================
// QoS 调度器实现
// Ref: CNBA Proposal Section 2.4 - QoS 调度 (优先级 + WRR)
// ====================================================================

bool
AXI2CHIBridge::scheduleNextCHIRequest()
{
    enum class QueueSel { Normal, High, Low, None };
    QueueSel selected = QueueSel::None;
    CHIRequest req;
    const Tick now = curTick();

    auto isReady = [now](const CHIRequest &r) {
        return r.scheduledTick <= now;
    };

    if (!qosEnabled) {
        if (!chiReqQueue.empty() && isReady(chiReqQueue.front())) {
            req = chiReqQueue.front();
            selected = QueueSel::Normal;
        }
    } else {
        bool highReady = !highPrioQueue.empty() && isReady(highPrioQueue.front());
        bool lowReady = !lowPrioQueue.empty() && isReady(lowPrioQueue.front());

        if (wrrHighCounter >= wrrHighWeight &&
            wrrLowCounter >= wrrLowWeight) {
            wrrHighCounter = 0;
            wrrLowCounter = 0;
        }

        if (highReady && wrrHighCounter < wrrHighWeight) {
            req = highPrioQueue.front();
            selected = QueueSel::High;
        } else if (lowReady && wrrLowCounter < wrrLowWeight) {
            req = lowPrioQueue.front();
            selected = QueueSel::Low;
        } else if (highReady) {
            req = highPrioQueue.front();
            selected = QueueSel::High;
        } else if (lowReady) {
            req = lowPrioQueue.front();
            selected = QueueSel::Low;
        }
    }

    if (selected == QueueSel::None) {
        return false;
    }

    if (chiMasterPort.sendTimingReq(req.pkt)) {
        if (selected == QueueSel::Normal) {
            chiReqQueue.pop_front();
        } else if (selected == QueueSel::High) {
            highPrioQueue.pop();
            wrrHighCounter++;
        } else if (selected == QueueSel::Low) {
            lowPrioQueue.pop();
            wrrLowCounter++;
        }

        DPRINTF(AXI2CHIBridge,
                "Sent CHI request: addr=%#x, size=%d, txnId=%d\n",
                req.pkt->getAddr(), req.pkt->getSize(), req.txnId);
        return true;
    }

    return false;
}

// ====================================================================
// 发送事件处理
// ====================================================================

void
AXI2CHIBridge::trySendCHIRequest()
{
    // gem5 timing 协议: sendTimingReq 失败后必须等待 recvReqRetry
    // 如果正在等待重试，不要再发送
    if (chiWaitingForRetry) {
        DPRINTF(AXI2CHIBridge, "trySendCHI: waiting for retry, skip\n");
        return;
    }

    bool hasMore = !chiReqQueue.empty() || !highPrioQueue.empty() ||
                   !lowPrioQueue.empty();
    if (hasMore) {
        if (scheduleNextCHIRequest()) {
            resumePendingSplits();
            maybeUnblockAxi();
            // 发送成功，如果还有更多请求，下一周期继续
            hasMore = !chiReqQueue.empty() || !highPrioQueue.empty() ||
                      !lowPrioQueue.empty();
            if (hasMore && !sendCHIReqEvent.scheduled()) {
                schedule(sendCHIReqEvent, clockEdge(Cycles(1)));
            }
        } else {
            Tick next_ready = nextReadyCHITick();
            if (next_ready != MaxTick && next_ready > curTick()) {
                if (!sendCHIReqEvent.scheduled()) {
                    schedule(sendCHIReqEvent, next_ready);
                }
            } else {
                // 发送失败: 标记等待重试，不调度
                chiWaitingForRetry = true;
                DPRINTF(AXI2CHIBridge,
                        "trySendCHI: send failed, waiting for retry\n");
            }
        }
    }
}

void
AXI2CHIBridge::trySendAXIResponse()
{
    // gem5 timing 协议: sendTimingResp 失败后等待 recvRespRetry
    if (axiWaitingForRetry) {
        DPRINTF(AXI2CHIBridge, "trySendAXIResp: waiting for retry, skip\n");
        return;
    }

    if (!axiRespQueue.empty()) {
        PacketPtr pkt = axiRespQueue.front();

        if (axiSlavePort.sendTimingResp(pkt)) {
            axiRespQueue.pop_front();
            bool countedResp = false;
            if (!axiRespIsCounted.empty()) {
                countedResp = axiRespIsCounted.front();
                axiRespIsCounted.pop_front();
            }
            if (countedResp && pendingCountedAxiResponses > 0) {
                pendingCountedAxiResponses--;
            }
            DPRINTF(AXI2CHIBridge,
                    "Sent AXI response: addr=%#x\n", pkt->getAddr());

            // 清理已完成的事务
            for (auto it = axiTransactions.begin();
                 it != axiTransactions.end(); ) {
                if (it->second.completed &&
                    it->second.origPkt == pkt) {
                    it = axiTransactions.erase(it);
                } else {
                    ++it;
                }
            }

            // 如果还有待发送的响应, 下一周期继续
            if (!axiRespQueue.empty() && !sendAXIRespEvent.scheduled()) {
                schedule(sendAXIRespEvent, clockEdge(Cycles(1)));
            }
            maybeUnblockAxi();
            checkDrainAndExit();
        } else {
            // 发送失败: 标记等待重试
            axiWaitingForRetry = true;
            DPRINTF(AXI2CHIBridge, "trySendAXIResp: send failed, waiting for retry\n");
        }
    }
}

// ====================================================================
// WRAP 突发地址计算
// Ref: CNBA - 全突发模式支持 (INCR/WRAP/FIXED)
// ====================================================================

Addr
AXI2CHIBridge::calcWrapAddress(Addr startAddr, unsigned beatIdx,
                                unsigned burstSize, unsigned burstLen)
{
    // AXI WRAP 突发: 地址在对齐边界内回绕
    // wrap_boundary = (start_addr / (burst_len * burst_size))
    //                * (burst_len * burst_size)
    // 每个 beat 的地址 = wrap_boundary + ((start_offset + beat * size)
    //                     % (burst_len * burst_size))
    unsigned wrapSize = burstLen * burstSize;
    Addr wrapBoundary = (startAddr / wrapSize) * wrapSize;
    unsigned startOffset = startAddr - wrapBoundary;
    Addr beatAddr = wrapBoundary +
                    ((startOffset + beatIdx * burstSize) % wrapSize);
    return beatAddr;
}

// ====================================================================
// Statistics 注册
// ====================================================================

AXI2CHIBridge::BridgeStats::BridgeStats(AXI2CHIBridge *parent)
    : statistics::Group(parent),
      // ---- Scalar stats (初始化顺序必须与声明顺序一致) ----
      ADD_STAT(totalAxiRequests, statistics::units::Count::get(),
               "Total AXI requests received by the bridge"),
      ADD_STAT(totalChiRequests, statistics::units::Count::get(),
               "Total CHI requests sent by the bridge"),
      ADD_STAT(mergeCount, statistics::units::Count::get(),
               "Number of transaction merges performed (CNBA mode)"),
      ADD_STAT(splitCount, statistics::units::Count::get(),
               "Number of transaction splits performed"),
      ADD_STAT(totalLatency, statistics::units::Tick::get(),
               "Total accumulated bridge latency"),
      ADD_STAT(totalBytes, statistics::units::Byte::get(),
               "Total bytes transferred through the bridge"),
      ADD_STAT(robStallCycles, statistics::units::Cycle::get(),
               "Cycles stalled due to ROB full / TxnID exhaustion"),
      ADD_STAT(totalReads, statistics::units::Count::get(),
               "Total read requests"),
      ADD_STAT(totalWrites, statistics::units::Count::get(),
               "Total write requests"),
      // ---- Per-priority stats (QoS) ----
      ADD_STAT(highPrioCount, statistics::units::Count::get(),
               "Number of high-priority AXI requests (totalSize <= 64B)"),
      ADD_STAT(highPrioLatency, statistics::units::Tick::get(),
               "Total latency of high-priority requests"),
      ADD_STAT(lowPrioCount, statistics::units::Count::get(),
               "Number of low-priority AXI requests (totalSize > 64B)"),
      ADD_STAT(lowPrioLatency, statistics::units::Tick::get(),
               "Total latency of low-priority requests"),
      // ---- Histogram ----
      ADD_STAT(latencyHist, statistics::units::Tick::get(),
               "Distribution of bridge crossing latencies"),
      // ---- Formula stats (引用上面已初始化的 Scalar) ----
      ADD_STAT(avgLatency, statistics::units::Tick::get(),
               "Average bridge crossing latency",
               totalLatency / totalAxiRequests),
      ADD_STAT(throughput, statistics::units::Rate<
                   statistics::units::Byte, statistics::units::Second>::get(),
               "Bridge throughput",
               totalBytes / simSeconds),
      ADD_STAT(mergeRate, statistics::units::Ratio::get(),
               "Transaction merge rate (1 - chi_reqs/axi_reqs)",
               1.0 - (totalChiRequests / totalAxiRequests)),
      ADD_STAT(avgHighPrioLatency, statistics::units::Tick::get(),
               "Average latency of high-priority requests",
               highPrioLatency / highPrioCount),
      ADD_STAT(avgLowPrioLatency, statistics::units::Tick::get(),
               "Average latency of low-priority requests",
               lowPrioLatency / lowPrioCount)
{
    latencyHist
        .init(20);
}

} // namespace gem5
