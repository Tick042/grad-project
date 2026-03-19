#!/usr/bin/env python3
"""
AXI2CHI Bridge 实验配置主脚本

=================================================================
功能概述:
  本脚本：
  1. 创建一套完整的 gem5 仿真系统
  2. 使用 TrafficGen 生成 AXI 突发激励（模拟 DMA/CPU 端的非一致性流量）
  3. 将流量注入 AXI2CHI 桥接器，观察桥接器的性能表现
  4. 收集延迟、吞吐量、TxnID 耗尽等统计指标

系统拓扑:
  TrafficGen → [AXI Bus] → AXI2CHIBridge → [CHI Bus] → SimpleMemory
    (激励源)     (非一致性总线)   (协议桥)     (一致性总线)    (内存模型)

=================================================================
支持的实验模式:
  1. baseline  — CF'20 论文基准: 固定按 64B Cache Line 拆分, max_txn_id=128
  2. cnba      — 本设计 (CNBA): 动态聚合/拆分 + QoS 调度, max_txn_id=256
  3. ablation  — 消融实验: CNBA 架构但关闭合并 (仅拆分)
  4. naive     — 朴素桥: 逐 beat 拆分 (一条 AXI→Len+1 条 CHI), 无 CL 对齐
  5. sweep     — 延迟扫描: 在不同桥延迟下对比 baseline vs CNBA

=================================================================
用法:
  gem5.opt axi2chi_exp.py --mode baseline
  gem5.opt axi2chi_exp.py --mode cnba
  gem5.opt axi2chi_exp.py --mode naive --axi-beat-size 32
  gem5.opt axi2chi_exp.py --mode ablation
  gem5.opt axi2chi_exp.py --mode sweep --sweep-latency 1,2,4,8

Ref: CF'20 Paper - "Design of an Open-Source Bridge Between
     Non-Coherent Burst-Based and Coherent Cache-Line-Based
     Memory Systems"
Ref: CNBA Proposal (开题报告) - 动态事务聚合/拆分 + QoS 调度
"""

import argparse
import os
import sys

import m5
from m5.objects import *
from m5.util import addToPath

# ==================================================================
# 命令行参数
# ==================================================================

parser = argparse.ArgumentParser(
    description="AXI2CHI Bridge Experiment Configuration",
    formatter_class=argparse.ArgumentDefaultsHelpFormatter,
)

parser.add_argument(
    "--mode",
    type=str,
    default="cnba",
    choices=["baseline", "cnba", "ablation", "naive", "sweep"],
    help="Experiment mode: baseline(CF'20) / cnba / ablation / naive / sweep",
)

parser.add_argument(
    "--cache-line-size",
    type=int,
    default=64,
    help="Cache line size in bytes",
)

parser.add_argument(
    "--max-txn-id",
    type=int,
    default=32,
    help="Maximum outstanding TxnIDs (shared by all bridge modes)",
)

parser.add_argument(
    "--bridge-latency",
    type=int,
    default=1,
    help="Bridge crossing latency in cycles",
)

parser.add_argument(
    "--mem-latency",
    type=str,
    default="80ns",
    help="Memory access latency (80ns = 80 cycles at 1GHz)",
)

parser.add_argument(
    "--mem-size",
    type=str,
    default="512MB",
    help="Total memory size",
)

parser.add_argument(
    "--num-requests",
    type=int,
    default=10000,
    help="Number of requests to generate",
)

parser.add_argument(
    "--traffic-pattern",
    type=str,
    default="random",
    choices=["random", "conv"],
    help="Traffic pattern: random (uniform AxSIZE) or conv (5%% size=3, 10%% size=4, 85%% size=5)",
)

parser.add_argument(
    "--burst-size",
    type=int,
    default=512,
    help="AXI burst size in bytes (typical: 64, 128, 256, 512)",
)

parser.add_argument(
    "--axi-beat-size",
    type=int,
    default=32,
    help="AXI beat size in bytes (2^AxSIZE), ignored when --random-beat-size",
)

parser.add_argument(
    "--random-beat-size",
    action="store_true",
    default=False,
    help="Randomize AXI beat size per request (AxSIZE in [min..5])",
)
parser.add_argument(
    "--min-axi-size",
    type=int,
    default=3,
    help="Minimum AxSIZE value for random beat size (0=1B .. 5=32B, default 3=8B)",
)
parser.add_argument(
    "--convert-time",
    type=int,
    default=5,
    help="Bridge transaction conversion delay in cycles",
)

parser.add_argument(
    "--read-percent",
    type=int,
    default=65,
    help="Percentage of read requests (0-100)",
)

parser.add_argument(
    "--qos-enabled",
    action="store_true",
    default=False,
    help="Enable QoS scheduling (CNBA mode)",
)

parser.add_argument(
    "--sweep-latency",
    type=str,
    default="1,2,4,8,16",
    help="Comma-separated latency values for sweep mode",
)

parser.add_argument(
    "--output-dir",
    type=str,
    default="m5out/axi2chi",
    help="Output directory for stats",
)

args = parser.parse_args()


# ==================================================================
# 流量生成函数
# ==================================================================


def create_traffic_generator(
    pattern, num_requests, burst_size, read_percent, mem_size_bytes
):
    """创建不同访问模式的流量生成器 (备用接口，当前未使用)

    实际流量生成使用 create_tgen_config() 生成配置文件后交给 TrafficGen。
    gem5 TrafficGen 通过状态转移图驱动多种访问模式的混合生成。

    Ref: tutor.md - 使用 Polybench 等矩阵运算的内存访问模式
    """
    tgen = PyTrafficGen()
    return tgen


def create_linear_generator(num_requests, burst_size, read_percent, mem_range):
    """线性顺序读写流量 - 模拟 DMA 流式传输 (备用接口)

    LinearlTrafficGen 按顺序地址发送固定大小的请求。
    适用于模拟 DMA 连续搬运或流式处理场景。
    """
    tgen = LinearTrafficGen()
    tgen.duration = "1ms"
    tgen.min_addr = mem_range.start
    tgen.max_addr = mem_range.end
    tgen.block_size = burst_size
    tgen.read_percent = read_percent
    tgen.min_period = "1ns"
    tgen.max_period = "1ns"
    tgen.data_limit = num_requests * burst_size
    return tgen


# ==================================================================
# 构建系统
# ==================================================================


def build_system(mode, args):
    """构建包含 AXI2CHI Bridge 的 gem5 仿真系统

    ============================================================
    系统拓扑图:

      TrafficGen ──→ [AXI NoncoherentXBar] ──→ AXI2CHIBridge ──→ [CHI NoncoherentXBar] ──→ SimpleMemory
      (流量发生器)       (AXI侧总线,32B宽)        (协议桥接器)       (CHI侧总线,32B宽)         (DDR模型)

    各组件功能:
      - TrafficGen: 产生 AXI 突发激励 (多种大小: 4B~256B, 读写混合)
                    通过 create_tgen_config() 生成的配置文件驱动
                    多个 STATE 代表不同突发大小,随机转移模拟真实场景
      - AXI Bus:    NoncoherentXBar, 256-bit 数据宽度 (32 bytes)
                    模拟 AXI 非一致性互连,不需要 snoop
      - Bridge:     AXI2CHIBridge, 根据 mode 参数选择不同转换策略
      - CHI Bus:    下游一致性总线, 连接内存控制器
      - Memory:     SimpleMemory, 30ns 延迟, 12.8GB/s 带宽

    Args:
        mode: 实验模式 (baseline/cnba/ablation/naive)
        args: 命令行参数
    """
    system = System()
    system.clk_domain = SrcClockDomain(
        clock="1GHz",
        voltage_domain=VoltageDomain(),
    )

    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange(args.mem_size)]

    # 设置系统 cache_line_size 为较大值，允许 TrafficGen 发送大突发包
    # 桥接器内部使用自己的 cache_line_size (64B) 进行拆分
    system.cache_line_size = 512

    # ---- 创建内存 ----
    system.mem_ctrl = SimpleMemory(
        latency=args.mem_latency,
        bandwidth="12.8GB/s",
        range=system.mem_ranges[0],
    )

    # ---- 创建总线 ----
    # 使用 NoncoherentXBar: AXI 是非一致性协议，不需要 snoop
    system.axi_bus = NoncoherentXBar(
        width=32,  # 256-bit AXI 数据宽度 (32 bytes)
        frontend_latency=1,
        forward_latency=0,
        response_latency=1,
    )

    # CHI 侧总线 (下游)
    system.chi_bus = NoncoherentXBar(
        width=32,
        frontend_latency=0,
        forward_latency=0,
        response_latency=0,
    )

    # ---- 创建 AXI2CHI 桥接器 ----
    bridge_params = {
        "cache_line_size": args.cache_line_size,
        "bridge_latency": args.bridge_latency,
        "axi_beat_size": args.axi_beat_size,
        "random_axi_beat_size": (
            args.random_beat_size or args.traffic_pattern in ("random", "conv")
        ),
        "traffic_pattern": args.traffic_pattern,
        "max_axi_requests": args.num_requests,
        "convert_time": args.convert_time,
        "min_axi_size": args.min_axi_size,
        "ranges": system.mem_ranges,
    }

    if mode == "baseline":
        # CF'20 Paper 基准配置
        bridge_params["use_baseline_logic"] = True
        bridge_params["use_naive_logic"] = False
        bridge_params["enable_merge_split"] = False
        bridge_params["max_txn_id"] = args.max_txn_id
        bridge_params["qos_enabled"] = False
        print(
            f"[Config] Baseline mode (CF'20): "
            f"max_txn_id={bridge_params['max_txn_id']}, "
            f"axi_beat_size={args.axi_beat_size}B, "
            f"no merge, INCR only"
        )

    elif mode == "cnba":
        # CNBA 全功能配置
        bridge_params["use_baseline_logic"] = False
        bridge_params["use_naive_logic"] = False
        bridge_params["enable_merge_split"] = True
        bridge_params["max_txn_id"] = args.max_txn_id
        bridge_params["qos_enabled"] = args.qos_enabled
        print(
            f"[Config] CNBA mode: "
            f"max_txn_id={args.max_txn_id}, "
            f"merge_split=ON, qos={args.qos_enabled}"
        )

    elif mode == "ablation":
        # 消融实验: CNBA 架构但关闭合并
        bridge_params["use_baseline_logic"] = False
        bridge_params["use_naive_logic"] = False
        bridge_params["enable_merge_split"] = False
        bridge_params["max_txn_id"] = args.max_txn_id
        bridge_params["qos_enabled"] = False
        print(
            f"[Config] Ablation mode: "
            f"max_txn_id={args.max_txn_id}, "
            f"merge_split=OFF (ablation)"
        )

    elif mode == "naive":
        # 朴素桥模式: 逐 beat 拆分，无合并，无 cache line 对齐
        bridge_params["use_baseline_logic"] = False
        bridge_params["use_naive_logic"] = True
        bridge_params["enable_merge_split"] = False
        bridge_params["max_txn_id"] = args.max_txn_id
        bridge_params["qos_enabled"] = False
        print(
            f"[Config] Naive bridge mode: "
            f"max_txn_id={bridge_params['max_txn_id']}, "
            f"axi_beat_size={args.axi_beat_size}B, "
            f"per-beat split, no merge"
        )

    system.bridge = AXI2CHIBridge(**bridge_params)

    # ---- 连接端口 ----
    # TrafficGen -> AXI Bus -> Bridge -> CHI Bus -> Memory
    system.bridge.axi_slave_port = system.axi_bus.mem_side_ports
    system.bridge.chi_master_port = system.chi_bus.cpu_side_ports
    system.mem_ctrl.port = system.chi_bus.mem_side_ports

    # ---- 创建流量生成器 ----
    system.tgen = TrafficGen(
        config_file=create_tgen_config(args),
    )
    system.tgen.port = system.axi_bus.cpu_side_ports

    # 系统端口
    system.system_port = system.axi_bus.cpu_side_ports

    return system


def create_tgen_config(args):
    """生成 TrafficGen 配置文件 — AXI 激励的核心生成逻辑

    ============================================================
    AXI 激励原理:
      gem5 TrafficGen 使用「状态转移图」来生成混合流量:
      - 每个 STATE 代表一种访问模式 (LINEAR/RANDOM) + 一种突发大小
      - STATE 之间随机转移,模拟真实场景中不同大小请求的交替出现
      - 各 STATE 等概率转移 (1/N),保证各种突发大小均匀覆盖

    ============================================================
    配置文件格式 (TrafficGen .cfg):
      STATE <id> <duration_ticks> <type> <read_percent> <start_addr>
            <end_addr> <access_size_bytes> <min_period> <max_period>
            <data_limit>
      INIT <initial_state>
      TRANSITION <from_state> <to_state> <probability>

    ============================================================
    AXI 突发参数模拟:
      不同 STATE 使用不同 block_size 模拟各种 AXI 突发组合:
      - burst_sizes = [4, 8, 16, 32, 64, 128, 256] (bytes)
      - 4B~32B:  小突发,落在单个 Cache Line 内 → CNBA 可合并
      - 64B:     恰好一个 Cache Line → 所有模式 1:1 转换
      - 128B~256B: 大突发,跨越 Cache Line 边界 → 需要拆分

      对应 AXI 协议参数:
        AxSIZE = 0~5 (1B~32B per beat)
        AxLEN  = 0~255 (1~256 beats)
        total  = 2^AxSIZE × (AxLEN + 1)

      示例: block_size=256 → 可表示 AxSIZE=5(32B/beat), AxLEN=7(8 beats)
    """
    import tempfile

    mem_size = int(args.mem_size.replace("MB", "")) * 1024 * 1024
    read_pct = args.read_percent
    period = 1  # 1 tick → 尽可能快地发送请求（背靠背）
    # 每个 STATE 持续时间: 短暂停留后切换，保证 burst size 混合
    duration = period * 50  # 每个 STATE 发 ~50 个请求后转移

    # ===========================================================
    # 代表性 AXI 突发总大小 (bytes):
    #   32B, 64B, 128B, 256B, 512B
    #   与随机 AxSIZE (0-5) 组合，覆盖所有 AXI burst 参数组合
    #
    # AXI Size 范围: AxSIZE = 0~5 (1B~32B per beat)
    #   - Size 0: 1B per beat
    #   - Size 1: 2B per beat
    #   - Size 2: 4B per beat
    #   - Size 3: 8B per beat
    #   - Size 4: 16B per beat
    #   - Size 5: 32B per beat
    #
    # 注意: block_size 必须 <= system.cache_line_size (设为 512B)
    # ===========================================================
    burst_sizes = [32, 64, 128, 256, 512]

    config_lines = []

    if args.traffic_pattern in ("random", "conv"):
        # random 和 conv 的 TrafficGen 激励完全相同（随机地址+随机burst大小）
        # 两者区别仅在于桥内部 AxSIZE 分布不同
        for i, bsize in enumerate(burst_sizes):
            config_lines.append(
                f"STATE {i} {duration} RANDOM {read_pct} 0 {mem_size} "
                f"{bsize} {period} {period} 0"
            )

    n = len(config_lines)
    config_content = ""
    for line in config_lines:
        config_content += line + "\n"

    config_content += "INIT 0\n"

    # 均匀随机转移: 每个 state 等概率转移到所有 state (包括自身)
    for i in range(n):
        for j in range(n):
            prob = 1.0 / n
            config_content += f"TRANSITION {i} {j} {prob:.6f}\n"

    config_file = os.path.join(args.output_dir, "tgen_config.cfg")
    os.makedirs(os.path.dirname(config_file), exist_ok=True)
    with open(config_file, "w") as f:
        f.write(config_content)

    print(f"[Config] TrafficGen config written to: {config_file}")
    return config_file


# ==================================================================
# Sweep 模式
# ==================================================================


def run_sweep(args):
    """延迟扫描实验: 在不同延迟配置下对比 baseline vs CNBA"""
    latencies = [int(x) for x in args.sweep_latency.split(",")]
    results = {}

    for lat in latencies:
        for mode in ["baseline", "cnba"]:
            print(f"\n{'='*60}")
            print(f"Sweep: mode={mode}, latency={lat} cycles")
            print(f"{'='*60}")

            args.bridge_latency = lat
            system = build_system(mode, args)

            root = Root(full_system=False, system=system)
            m5.instantiate()

            print(f"Starting simulation: {mode} @ latency={lat}")
            exit_event = m5.simulate(args.num_requests * 10000)
            print(f"Simulation done: {exit_event.getCause()}")

            # 收集统计结果
            m5.stats.dump()
            m5.stats.reset()

            tag = f"{mode}_lat{lat}"
            results[tag] = {
                "mode": mode,
                "latency": lat,
                "cause": exit_event.getCause(),
            }

    print("\n\nSweep Results Summary:")
    for tag, res in results.items():
        print(f"  {tag}: {res}")


# ==================================================================
# 主函数
# ==================================================================


def main():
    if args.mode == "sweep":
        run_sweep(args)
        return

    system = build_system(args.mode, args)

    root = Root(full_system=False, system=system)
    m5.instantiate()

    print(f"\n{'='*60}")
    print(f"Starting AXI2CHI Bridge Simulation")
    print(f"  Mode:           {args.mode}")
    print(f"  Traffic:        {args.traffic_pattern}")
    print(f"  Burst Size:     {args.burst_size}B")
    print(f"  Cache Line:     {args.cache_line_size}B")
    print(f"  Max TxnID:      {args.max_txn_id}")
    print(f"  Bridge Latency: {args.bridge_latency} cycles")
    print(f"  Convert Time:   {args.convert_time} cycles")
    print(f"  Mem Latency:    {args.mem_latency}")
    print(f"  Requests:       {args.num_requests}")
    print(f"  Read Percent:   {args.read_percent}%")
    print(f"{'='*60}\n")

    # 运行仿真
    # 桥接器在收到 max_axi_requests 个 AXI 请求后会调用 exitSimLoop 停止
    # sim_ticks 作为安全上限，防止永远不停
    sim_ticks = args.num_requests * 100000000  # 充足的仿真时间上限
    exit_event = m5.simulate(sim_ticks)
    print(
        f"\nSimulation complete: {exit_event.getCause()} @ tick {m5.curTick()}"
    )
    print(f"Simulated time: {m5.curTick() / 1e12:.6f} seconds")

    # 输出统计
    m5.stats.dump()
    print(f"\nStats dumped to: {args.output_dir}")


if __name__ == "__m5_main__":
    main()
