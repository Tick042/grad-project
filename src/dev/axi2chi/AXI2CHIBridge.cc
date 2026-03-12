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

#include "base/logging.hh"
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
      maxAxiRequests(p.max_axi_requests),
      maxTxnId(p.max_txn_id),
      bridgeLatency(p.bridge_latency),
      reqQueueSize(p.req_queue_size),
      respQueueSize(p.resp_queue_size),
      addrRanges(p.ranges.begin(), p.ranges.end()),
      // 事务管理初始化
      nextAxiTxnId(0),
      activeTxnCount(0),
      blocked(false),
      chiWaitingForRetry(false),
      axiWaitingForRetry(false),
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
            "qos=%d, cache_line=%dB, axi_beat=%dB, max_txn=%d\n",
            useNaiveLogic ? "Naive" :
                (useBaselineLogic ? "Baseline(CF'20)" : "CNBA"),
            enableMergeSplit, qosEnabled, cacheLineSize,
            axiBeatSize, maxTxnId);
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
    // 随机模式下加权选择: 10% 8B, 15% 16B, 75% 32B
    unsigned effectiveBeat;
    if (randomAxiBeatSize) {
        // 20 个槽位: 2×8B(10%) + 3×16B(15%) + 15×32B(75%)
        static const unsigned BEAT_TABLE[] = {
            8, 8,                                       // 10%
            16, 16, 16,                                 // 15%
            32, 32, 32, 32, 32, 32, 32, 32,            // 75%
            32, 32, 32, 32, 32, 32, 32
        };
        effectiveBeat = BEAT_TABLE[nextAxiTxnId % 20];
    } else {
        effectiveBeat = axiBeatSize;
    }
    txn.burstSize = std::min(txn.totalSize, effectiveBeat);
    txn.burstLen = (txn.totalSize + txn.burstSize - 1) / txn.burstSize;
    txn.endAddr = txn.startAddr + txn.totalSize - 1;
    txn.burstType = BURST_INCR;  // 默认 INCR

    // 如果不是 INCR 且在基准模式，报告不支持
    // Ref: CF'20 Paper - 仅支持 INCR 突发
    if (useBaselineLogic && txn.burstType != BURST_INCR) {
        warn("Baseline mode: Only INCR burst supported, "
             "ignoring burst type %d\n", txn.burstType);
    }

    // 分配内部事务 ID
    unsigned axiId = nextAxiTxnId++;
    axiTransactions[axiId] = txn;

    // 统计
    stats.totalAxiRequests++;
    if (txn.isWrite)
        stats.totalWrites++;
    else
        stats.totalReads++;

    stats.totalBytes += txn.totalSize;

    // 达到最大 AXI 请求数后停止仿真
    if (maxAxiRequests > 0 &&
        stats.totalAxiRequests.value() >= maxAxiRequests) {
        exitSimLoop("AXI request limit reached");
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
        naiveSplitTransaction(axiTransactions[axiId]);
    } else if (useBaselineLogic) {
        // ---- 基准模式 (CF'20 Paper Logic) ----
        baselineSplitTransaction(axiTransactions[axiId]);
    } else {
        // ---- CNBA 模式 (本设计 Logic) ----
        if (enableMergeSplit) {
            cnbaSplitMergeTransaction(axiTransactions[axiId]);
        } else {
            // 消融实验: CNBA 架构但关闭合并, 仅做拆分
            baselineSplitTransaction(axiTransactions[axiId]);
        }
    }

    // Step 4: 调度 CHI 请求发送
    if (!chiWaitingForRetry && !sendCHIReqEvent.scheduled() &&
        !chiReqQueue.empty()) {
        schedule(sendCHIReqEvent, clockEdge(bridgeLatency));
    }

    return true;
}

// ====================================================================
// 基准模式: 固定拆分
// Ref: CF'20 Paper Section 3 - 固定按 Cache Line 拆分
// ====================================================================

/**
 * baselineSplitTransaction:
 *
 * CF'20 论文的核心逻辑 —— 收到一个 AXI Burst 后，
 * 强制按 Cache Line (64B) 拆分为多个 CHI 请求。
 * 不进行任何合并优化。
 *
 * 每个 CHI 请求消耗一个 TxnID，当 TxnID 达到 max_txn_id
 * (论文中为 128) 时，新请求被阻塞。
 *
 * Ref: CF'20 Paper Section 3.1 - "Each AXI beat that crosses
 *      a cache line boundary generates a separate CHI transaction"
 *
 * ============================================================
 * 窄拍处理 (AxSIZE < 5, per-beat < 32B):
 *   CF'20 桥仅在 AXI 每拍 >= 32B 时才做 CL 对齐拆分。
 *   当 AxSIZE < 5 (每拍 < 32B) 时，硬件无法有效地将窄拍
 *   合并为 CL 大小的 CHI 请求，因此每个 AXI beat 独立映射
 *   为一个 CHI 请求 (共 Len+1 个)。
 *
 *   示例 (axiBeatSize=16B, 总大小=128B):
 *     宽拍路径 (>=32B): 128B ÷ 64B = 2 个 CL 对齐的 CHI
 *     窄拍路径 (<32B):  128B ÷ 16B = 8 个逐拍 CHI 请求
 */
void
AXI2CHIBridge::baselineSplitTransaction(AXITransaction &txn)
{
    Addr addr = txn.startAddr;
    unsigned remaining = txn.totalSize;
    unsigned chiCount = 0;

    // 判断是否为窄拍: 每拍字节数 < 32B (即 AxSIZE < 5)
    // txn.burstSize 已在 handleAXIRequest 中计算好（含随机化）
    unsigned effectiveBeatSize = txn.burstSize;
    if (effectiveBeatSize == 0) effectiveBeatSize = 1;
    bool narrowBeat = (effectiveBeatSize < 32);

    while (remaining > 0) {
        unsigned thisSize;

        if (narrowBeat) {
            // 窄拍路径 (AxSIZE < 5): 每拍 → 1 个 CHI 请求
            // 不按 CL 边界对齐，直接按 beat 大小切割
            thisSize = std::min(effectiveBeatSize, remaining);
        } else {
            // 宽拍路径 (AxSIZE >= 5): 按 Cache Line 边界对齐拆分
            Addr lineStart = addr & ~(Addr)(cacheLineSize - 1);
            Addr lineEnd = lineStart + cacheLineSize;
            thisSize = std::min((unsigned)(lineEnd - addr), remaining);
        }

        // 分配 TxnID
        int txnId = allocateTxnId();
        if (txnId < 0) {
            // TxnID 耗尽，阻塞
            // Ref: CF'20 Paper - "performance degrades when TxnIDs
            //       are exhausted under high memory latency"
            DPRINTF(AXI2CHIBridge,
                    "TxnID exhausted! Stalling (baseline mode).\n");
            stats.robStallCycles++;
            blocked = true;
            break;
        }

        // 创建 CHI 请求包
        // 使用 gem5 的 MemCmd 区分读写和一致性类型
        MemCmd cmd = txn.isWrite ? MemCmd::WriteReq : MemCmd::ReadReq;
        RequestPtr req = std::make_shared<Request>(
            addr, thisSize, Request::Flags(0),
            txn.origPkt->req->requestorId());
        PacketPtr chiPkt = new Packet(req, cmd);
        chiPkt->allocate();

        // 将 TxnID 附加到包上，响应时用于释放
        chiPkt->pushSenderState(
            new CHISenderState(txnId, 0));  // parentAxiId TBD

        // 如果是写请求，复制数据
        if (txn.isWrite && txn.origPkt->hasData()) {
            unsigned offset = addr - txn.startAddr;
            if (offset + thisSize <= txn.origPkt->getSize()) {
                chiPkt->setData(txn.origPkt->getConstPtr<uint8_t>() + offset);
            }
        }

        // 构建 CHI 请求记录
        CHIRequest chiReq;
        chiReq.pkt = chiPkt;
        chiReq.txnId = txnId;
        chiReq.scheduledTick = curTick();

        // 加入队列 (基准模式不区分优先级)
        chiReqQueue.push_back(chiReq);
        chiCount++;

        DPRINTF(AXI2CHIBridge,
                "  Baseline split: CHI req addr=%#x, size=%d, txnId=%d, %s\n",
                addr, thisSize, txnId,
                narrowBeat ? "narrow-beat" : "CL-aligned");

        addr += thisSize;
        remaining -= thisSize;
    }

    txn.chiReqCount = chiCount;
    stats.totalChiRequests += chiCount;

    // 如果拆分了 (chiCount > 1)，记录拆分统计
    if (chiCount > 1) {
        stats.splitCount++;
    }

    DPRINTF(AXI2CHIBridge,
            "Baseline: AXI burst(%dB, beatSize=%dB, %s) -> %d CHI requests\n",
            txn.totalSize, effectiveBeatSize,
            narrowBeat ? "narrow" : "wide", chiCount);
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
AXI2CHIBridge::naiveSplitTransaction(AXITransaction &txn)
{
    Addr addr = txn.startAddr;
    unsigned remaining = txn.totalSize;
    unsigned chiCount = 0;

    // 每个 beat 的大小: 使用 txn.burstSize（已在 handleAXIRequest 中计算好）
    unsigned beatSize = txn.burstSize;
    if (beatSize == 0) beatSize = 1;

    while (remaining > 0) {
        unsigned thisSize = std::min(beatSize, remaining);

        // 分配 TxnID
        int txnId = allocateTxnId();
        if (txnId < 0) {
            DPRINTF(AXI2CHIBridge,
                    "TxnID exhausted! Stalling (naive mode).\n");
            stats.robStallCycles++;
            blocked = true;
            break;
        }

        // 创建 CHI 请求包
        MemCmd cmd = txn.isWrite ? MemCmd::WriteReq : MemCmd::ReadReq;
        RequestPtr req = std::make_shared<Request>(
            addr, thisSize, Request::Flags(0),
            txn.origPkt->req->requestorId());
        PacketPtr chiPkt = new Packet(req, cmd);
        chiPkt->allocate();

        chiPkt->pushSenderState(
            new CHISenderState(txnId, 0));

        // 写请求复制数据
        if (txn.isWrite && txn.origPkt->hasData()) {
            unsigned offset = addr - txn.startAddr;
            if (offset + thisSize <= txn.origPkt->getSize()) {
                chiPkt->setData(txn.origPkt->getConstPtr<uint8_t>() + offset);
            }
        }

        CHIRequest chiReq;
        chiReq.pkt = chiPkt;
        chiReq.txnId = txnId;
        chiReq.scheduledTick = curTick();

        chiReqQueue.push_back(chiReq);
        chiCount++;

        DPRINTF(AXI2CHIBridge,
                "  Naive split: CHI req addr=%#x, size=%d, txnId=%d\n",
                addr, thisSize, txnId);

        addr += thisSize;
        remaining -= thisSize;
    }

    txn.chiReqCount = chiCount;
    stats.totalChiRequests += chiCount;

    if (chiCount > 1) {
        stats.splitCount++;
    }

    DPRINTF(AXI2CHIBridge,
            "Naive: AXI burst(%dB, beatSize=%dB) -> %d CHI requests\n",
            txn.totalSize, beatSize, chiCount);
}

// ====================================================================
// CNBA 模式: 动态聚合/拆分
// Ref: CNBA Proposal - 动态事务聚合/拆分算法
// ====================================================================

/**
 * cnbaSplitMergeTransaction:
 *
 * CNBA 核心算法 —— 智能判断是否需要拆分或合并:
 *
 * 1. 地址对齐检测: 检查起始地址是否对齐到 Cache Line 边界
 * 2. 跨行智能切分: 若 AXI Burst 跨越 Cache Line 边界，自动拆分
 * 3. 事务合并: 若请求 Size < Cache Line，可合并为一个 CHI 请求
 *
 * 拆分数量计算公式:
 *   num = (end_addr >> log2(cacheLineSize))
 *       - (start_addr >> log2(cacheLineSize))
 * Ref: CNBA Proposal + Chisel/Verilog 源码
 */
void
AXI2CHIBridge::cnbaSplitMergeTransaction(AXITransaction &txn)
{
    unsigned chiCount = 0;
    unsigned pageBits = (unsigned)std::log2(cacheLineSize);

    // Step 1: 计算需要拆分的数量
    // Ref: CNBA 公式 num = (end_addr >> page_bits) - (start_addr >> page_bits)
    unsigned splitNum = calcSplitCount(txn.startAddr, txn.endAddr,
                                       cacheLineSize);

    DPRINTF(AXI2CHIBridge,
            "CNBA: addr=%#x~%#x, totalSize=%d, splitNum=%d\n",
            txn.startAddr, txn.endAddr, txn.totalSize, splitNum);

    if (splitNum == 0) {
        // ---- 事务合并: 请求不跨 Cache Line，可整体发送 ----
        // Ref: CNBA - 事务合并逻辑
        int txnId = allocateTxnId();
        if (txnId < 0) {
            stats.robStallCycles++;
            blocked = true;
            return;
        }

        MemCmd cmd = txn.isWrite ? MemCmd::WriteReq : MemCmd::ReadReq;
        RequestPtr req = std::make_shared<Request>(
            txn.startAddr, txn.totalSize, Request::Flags(0),
            txn.origPkt->req->requestorId());
        PacketPtr chiPkt = new Packet(req, cmd);
        chiPkt->allocate();

        // 将 TxnID 附加到包上
        chiPkt->pushSenderState(
            new CHISenderState(txnId, 0));

        if (txn.isWrite && txn.origPkt->hasData()) {
            chiPkt->setData(txn.origPkt->getConstPtr<uint8_t>());
        }

        CHIRequest chiReq;
        chiReq.pkt = chiPkt;
        chiReq.txnId = txnId;

        // QoS: 按优先级入队
        if (qosEnabled) {
            enqueueCHIRequest(chiReq, txn.qosPriority);
        } else {
            chiReqQueue.push_back(chiReq);
        }

        chiCount = 1;
        stats.mergeCount++;

        DPRINTF(AXI2CHIBridge,
                "  CNBA merged: single CHI req addr=%#x, size=%d, "
                "txnId=%d\n",
                txn.startAddr, txn.totalSize, txnId);

    } else {
        // ---- 智能拆分: 按 Cache Line 边界拆分 ----
        // Ref: CNBA - 跨行智能切分
        Addr addr = txn.startAddr;
        unsigned remaining = txn.totalSize;

        while (remaining > 0) {
            Addr lineStart = addr & ~(Addr)(cacheLineSize - 1);
            Addr lineEnd = lineStart + cacheLineSize;
            unsigned thisSize = std::min((unsigned)(lineEnd - addr),
                                        remaining);

            int txnId = allocateTxnId();
            if (txnId < 0) {
                stats.robStallCycles++;
                blocked = true;
                break;
            }

            MemCmd cmd = txn.isWrite ? MemCmd::WriteReq : MemCmd::ReadReq;
            RequestPtr req = std::make_shared<Request>(
                addr, thisSize, Request::Flags(0),
                txn.origPkt->req->requestorId());
            PacketPtr chiPkt = new Packet(req, cmd);
            chiPkt->allocate();

            // 将 TxnID 附加到包上
            chiPkt->pushSenderState(
                new CHISenderState(txnId, 0));

            if (txn.isWrite && txn.origPkt->hasData()) {
                unsigned offset = addr - txn.startAddr;
                if (offset + thisSize <= txn.origPkt->getSize()) {
                    chiPkt->setData(
                        txn.origPkt->getConstPtr<uint8_t>() + offset);
                }
            }

            CHIRequest chiReq;
            chiReq.pkt = chiPkt;
            chiReq.txnId = txnId;

            if (qosEnabled) {
                enqueueCHIRequest(chiReq, txn.qosPriority);
            } else {
                chiReqQueue.push_back(chiReq);
            }

            chiCount++;

            DPRINTF(AXI2CHIBridge,
                    "  CNBA split [%d]: CHI req addr=%#x, size=%d, "
                    "txnId=%d\n",
                    chiCount, addr, thisSize, txnId);

            addr += thisSize;
            remaining -= thisSize;
        }

        if (chiCount > 1) {
            stats.splitCount++;
        }
    }

    txn.chiReqCount = chiCount;
    stats.totalChiRequests += chiCount;

    DPRINTF(AXI2CHIBridge,
            "CNBA: AXI burst(%dB) -> %d CHI requests "
            "(merged=%s, split=%s)\n",
            txn.totalSize, chiCount,
            (splitNum == 0) ? "yes" : "no",
            (chiCount > 1) ? "yes" : "no");
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

    // 从 SenderState 获取 TxnID 并释放
    auto *ss = dynamic_cast<CHISenderState*>(pkt->popSenderState());
    if (ss) {
        freeTxnId(ss->txnId);
        DPRINTF(AXI2CHIBridge,
                "  Freed TxnID=%d, active=%d\n",
                ss->txnId, activeTxnCount);
        delete ss;
    }

    // 查找对应的 AXI 事务
    // 简化实现: 遍历找到地址匹配的事务
    for (auto &[axiId, txn] : axiTransactions) {
        if (txn.completed)
            continue;

        // 检查地址范围是否匹配
        Addr respAddr = pkt->getAddr();
        if (respAddr >= txn.startAddr &&
            respAddr < txn.startAddr + txn.totalSize) {

            txn.chiRespCount++;

            // 如果是读请求，将数据复制回原始 AXI 包
            // 注意: TrafficGen 创建的 ReadReq 已通过 dataDynamic() 分配了数据缓冲，
            // 不能再调用 allocate()。直接用 getPtr() 写入即可。
            if (pkt->isRead() && txn.origPkt) {
                unsigned offset = respAddr - txn.startAddr;
                unsigned copySize = std::min((unsigned)pkt->getSize(),
                    txn.totalSize - offset);
                uint8_t *dstPtr = txn.origPkt->getPtr<uint8_t>();
                if (dstPtr && pkt->hasData()) {
                    std::memcpy(dstPtr + offset,
                                pkt->getConstPtr<uint8_t>(),
                                copySize);
                }
            }

            DPRINTF(AXI2CHIBridge,
                    "  AXI txn[%d]: %d/%d CHI responses received\n",
                    axiId, txn.chiRespCount, txn.chiReqCount);

            // 检查是否所有 CHI 响应都已收到
            if (txn.chiRespCount >= txn.chiReqCount) {
                txn.completed = true;

                // 计算延迟统计
                Tick latency = curTick() - txn.arrivalTick;
                stats.latencyHist.sample(latency);
                stats.totalLatency += latency;

                // 生成 AXI 响应
                // Ref: CF'20 Paper - ROB 确保 AXI 响应按序
                PacketPtr respPkt = txn.origPkt;
                respPkt->makeResponse();
                axiRespQueue.push_back(respPkt);

                // 调度发送 AXI 响应
                if (!axiWaitingForRetry && !sendAXIRespEvent.scheduled()) {
                    schedule(sendAXIRespEvent, clockEdge(Cycles(1)));
                }

                DPRINTF(AXI2CHIBridge,
                        "  AXI txn[%d] completed, latency=%llu ticks\n",
                        axiId, latency);
            }

            break;  // 找到匹配的事务
        }
    }

    // 释放收到的 CHI 响应包
    delete pkt;

    // 如果之前阻塞了, 检查是否可以解除阻塞
    if (blocked && !isBlocked()) {
        blocked = false;
        // 通知 AXI 侧可以重新发送请求
        if (axiSlavePort.needRetry) {
            axiSlavePort.needRetry = false;
            axiSlavePort.sendRetryReq();
            DPRINTF(AXI2CHIBridge,
                    "Unblocked: sending retry to AXI upstream\n");
        }
    }

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
    // 被阻塞的条件: TxnID 耗尽 或 请求队列满
    return (activeTxnCount >= maxTxnId) ||
           (chiReqQueue.size() >= reqQueueSize);
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
bool
AXI2CHIBridge::scheduleNextCHIRequest()
{
    CHIRequest req;
    bool found = false;

    if (!qosEnabled) {
        // QoS 未开启，直接从普通队列取
        if (!chiReqQueue.empty()) {
            req = chiReqQueue.front();
            chiReqQueue.pop_front();
            found = true;
        }
    } else {
        // WRR 仲裁
        // 高优先级优先: 若高优先级队列非空且还有配额
        if (!highPrioQueue.empty() && wrrHighCounter < wrrHighWeight) {
            req = highPrioQueue.front();
            highPrioQueue.pop();
            wrrHighCounter++;
            found = true;
        }
        // 低优先级
        else if (!lowPrioQueue.empty() && wrrLowCounter < wrrLowWeight) {
            req = lowPrioQueue.front();
            lowPrioQueue.pop();
            wrrLowCounter++;
            found = true;
        }
        // 重置计数器
        else if (wrrHighCounter >= wrrHighWeight &&
                 wrrLowCounter >= wrrLowWeight) {
            wrrHighCounter = 0;
            wrrLowCounter = 0;
            // 递归重试
            return scheduleNextCHIRequest();
        }
        // 只有一个队列有数据
        else if (!highPrioQueue.empty()) {
            req = highPrioQueue.front();
            highPrioQueue.pop();
            found = true;
        }
        else if (!lowPrioQueue.empty()) {
            req = lowPrioQueue.front();
            lowPrioQueue.pop();
            found = true;
        }
    }

    if (found) {
        // 发送 CHI 请求到下游
        if (chiMasterPort.sendTimingReq(req.pkt)) {
            DPRINTF(AXI2CHIBridge,
                    "Sent CHI request: addr=%#x, size=%d, txnId=%d\n",
                    req.pkt->getAddr(), req.pkt->getSize(), req.txnId);
            return true;
        } else {
            // 下游忙，放回队列头部
            chiReqQueue.push_front(req);
            return false;
        }
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
            // 发送成功，如果还有更多请求，下一周期继续
            hasMore = !chiReqQueue.empty() || !highPrioQueue.empty() ||
                      !lowPrioQueue.empty();
            if (hasMore && !sendCHIReqEvent.scheduled()) {
                schedule(sendCHIReqEvent, clockEdge(Cycles(1)));
            }
        } else {
            // 发送失败: 标记等待重试，不调度
            chiWaitingForRetry = true;
            DPRINTF(AXI2CHIBridge, "trySendCHI: send failed, waiting for retry\n");
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
               1.0 - (totalChiRequests / totalAxiRequests))
{
    latencyHist
        .init(20);
}

} // namespace gem5
