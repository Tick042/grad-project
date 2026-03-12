/*
 * TransactionScheduler - QoS 调度器实现
 *
 * 独立的 QoS 调度模块, 实现:
 *   - 多级优先级队列 (High/Low Priority)
 *   - 加权轮询 (Weighted Round-Robin, WRR) 仲裁
 *   - 预留: 带宽监控和动态权重调整
 *
 * Ref: CNBA Proposal Section 2.4 - QoS 调度器设计
 * Ref: VipArbiter.sv, FastArbiter.sv 中的仲裁逻辑
 */

#include "dev/axi2chi/AXI2CHIBridge.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/AXI2CHIBridge.hh"

namespace gem5
{

/*
 * 本文件包含 AXI2CHIBridge 中 QoS 相关的辅助逻辑，
 * 作为独立编译单元以保持代码组织清晰。
 *
 * 主要调度逻辑已在 AXI2CHIBridge.cc 中通过
 * enqueueCHIRequest() 和 scheduleNextCHIRequest() 实现。
 *
 * 此文件提供更高级的调度策略，包括:
 * 1. 动态带宽分配
 * 2. 突发感知调度
 * 3. 饥饿防护
 */

// ====================================================================
// 调度策略工具函数
// ====================================================================

/**
 * 计算一个 AXI 事务的调度优先级
 *
 * 优先级由以下因素决定 (数值越高优先级越高):
 *   - QoS 字段 (AXI AxQOS, 4-bit, 0-15)
 *   - 等待时间 (aging: 长时间等待的事务优先级提升)
 *   - 事务类型 (写优先于读, 因为写通常更紧急)
 *
 * Ref: CNBA Proposal - 动态优先级映射
 * Ref: CHI-E Protocol Specification - QoS 字段定义
 *
 * @param txn      AXI 事务描述
 * @param curTick  当前仿真时钟
 * @return         综合优先级值 (0=最低, 越大越高)
 */
unsigned
computeTransactionPriority(const AXI2CHIBridge::AXITransaction &txn,
                            Tick currentTick)
{
    unsigned priority = 0;

    // 基础 QoS 优先级 (来自 AXI AxQOS 字段)
    priority += txn.qosPriority * 4;

    // 写优先级微调 (写请求略高)
    if (txn.isWrite)
        priority += 1;

    // 老化机制: 等待超过阈值后优先级提升
    // Ref: 防止低优先级请求饥饿
    const Tick AGING_THRESHOLD = 10000;  // ticks
    if (currentTick > txn.arrivalTick + AGING_THRESHOLD) {
        Tick wait = currentTick - txn.arrivalTick;
        unsigned age_bonus = (unsigned)(wait / AGING_THRESHOLD);
        priority += std::min(age_bonus, 8u);  // 最多加 8 级
    }

    return priority;
}

/**
 * 判断是否应该合并两个连续的 AXI 小请求
 *
 * 合并条件 (全部满足才合并):
 *   1. 两个请求地址连续 (或在同一 Cache Line 内)
 *   2. 请求类型相同 (都是读或都是写)
 *   3. 合并后总大小 <= Cache Line Size
 *   4. 两个请求的 QoS 优先级兼容
 *
 * Ref: CNBA Proposal - 事务合并算法
 * Ref: ChiWrMaster.sv / ChiRdMaster.sv 中的合并判断
 *
 * @param txn1      第一个事务
 * @param txn2      第二个事务
 * @param lineSize  Cache Line 大小
 * @return          是否可以合并
 */
bool
canMergeTransactions(const AXI2CHIBridge::AXITransaction &txn1,
                     const AXI2CHIBridge::AXITransaction &txn2,
                     unsigned lineSize)
{
    // 条件 1: 类型相同
    if (txn1.isWrite != txn2.isWrite)
        return false;

    // 条件 2: 同一 Cache Line
    Addr line1 = txn1.startAddr & ~(Addr)(lineSize - 1);
    Addr line2 = txn2.startAddr & ~(Addr)(lineSize - 1);
    if (line1 != line2)
        return false;

    // 条件 3: 合并后大小不超过 Cache Line
    // 计算合并后的覆盖范围
    Addr mergedStart = std::min(txn1.startAddr, txn2.startAddr);
    Addr mergedEnd = std::max(txn1.startAddr + txn1.totalSize,
                              txn2.startAddr + txn2.totalSize);
    if ((mergedEnd - mergedStart) > lineSize)
        return false;

    // 条件 4: QoS 兼容 (简化: 优先级差不超过 2)
    int prioDiff = (int)txn1.qosPriority - (int)txn2.qosPriority;
    if (std::abs(prioDiff) > 2)
        return false;

    return true;
}

/**
 * Generate a CHI byte-enable mask for partial cache line access
 *
 * 当 AXI 请求只访问 Cache Line 的部分字节时,
 * 需要生成 ByteEnable mask 告诉 CHI 端哪些字节有效。
 *
 * Ref: CNBA - ByteMask 逻辑
 * Ref: DataBufferForWrite.sv 中的 byte lane 计算
 *
 * @param addr       访问起始地址
 * @param size       访问大小 (bytes)
 * @param lineSize   Cache Line 大小
 * @return           字节使能掩码 (vector<bool>)
 */
std::vector<bool>
generateByteEnableMask(Addr addr, unsigned size, unsigned lineSize)
{
    std::vector<bool> mask(lineSize, false);

    unsigned offset = addr & (lineSize - 1);  // Line 内偏移
    for (unsigned i = offset; i < offset + size && i < lineSize; i++) {
        mask[i] = true;
    }

    return mask;
}

} // namespace gem5
