# AXI2CHI Bridge - Python SimObject 绑定
#
# 定义 AXI2CHIBridge SimObject 的参数和端口绑定，
# 供 gem5 配置脚本 (axi2chi_exp.py) 实例化使用。
#
# 本文件定义了桥接器的所有可配置参数:
#   - 模式控制: baseline / cnba / naive 三种转换策略
#   - 硬件参数: Cache Line 大小, AXI beat 大小, TxnID 数量
#   - 队列参数: 请求/响应队列深度
#   - QoS 参数: WRR 权重

from m5.objects.ClockedObject import ClockedObject
from m5.params import *


class AXI2CHIBridge(ClockedObject):
    """AXI-to-CHI 协议桥接器 SimObject

    可配置为三种工作模式:
    - Baseline (CF'20 Paper): use_baseline_logic=True
      → Size<5 按 beat 拆分, Size==5 合并成 64B CHI, 无合并优化
    - CNBA (本设计):          use_baseline_logic=False, enable_merge_split=True
      → 任意 Size 统一拆成 64B CHI (Size==6), 可选 QoS 调度
    - Naive (朴素桥):         use_naive_logic=True
      → 逐 beat 拆分 (一条 AXI→Len+1 条 CHI), 无 CL 对齐, 无合并

    端口连接方式:
      上游(AXI侧).port → bridge.axi_slave_port
      bridge.chi_master_port → 下游(CHI侧).port
    """

    type = "AXI2CHIBridge"
    cxx_header = "dev/axi2chi/AXI2CHIBridge.hh"
    cxx_class = "gem5::AXI2CHIBridge"

    # ---- 端口定义 ----
    chi_master_port = RequestPort(
        "CHI-side master port: sends CHI requests to downstream memory"
    )
    axi_slave_port = ResponsePort(
        "AXI-side slave port: receives AXI burst requests from upstream"
    )

    # ---- 桥接模式控制 ----
    enable_merge_split = Param.Bool(
        True, "Enable transaction merge/split optimization (CNBA mode)"
    )
    use_baseline_logic = Param.Bool(
        False, "Use baseline CF'20 logic (fixed split, no merge, INCR only)"
    )
    use_naive_logic = Param.Bool(
        False, "Use naive bridge logic (per-beat split, Len+1 CHI requests)"
    )
    qos_enabled = Param.Bool(
        False, "Enable QoS-aware scheduling with priority queues and WRR"
    )

    # ---- 硬件参数 ----
    cache_line_size = Param.Unsigned(
        64, "Cache line size in bytes (must be power of 2)"
    )
    axi_beat_size = Param.Unsigned(
        32, "AXI beat size in bytes (2^AxSIZE, used by naive/baseline)"
    )
    random_axi_beat_size = Param.Bool(
        False,
        "Randomize AXI beat size per request (AxSIZE in [min..5], 1/2/4/8/16/32B)",
    )
    min_axi_size = Param.Unsigned(
        3,
        "Minimum AxSIZE value for random beat size (0=1B .. 5=32B, default 3=8B)",
    )
    traffic_pattern = Param.String(
        "random",
        "Traffic pattern: 'random' (uniform AxSIZE) or 'conv' "
        "(5%% size=3, 10%% size=4, 85%% size=5)",
    )
    max_axi_requests = Param.Unsigned(
        0,
        "Stop after receiving this many AXI requests and draining all in-flight "
        "transactions/queues (0 = unlimited)",
    )
    max_txn_id = Param.Unsigned(
        32,
        "Maximum number of outstanding TxnIDs (all modes share this limit)",
    )
    bridge_latency = Param.Cycles(1, "Bridge crossing latency in cycles")
    convert_time = Param.Cycles(
        5,
        "Transaction conversion latency in cycles "
        "(delay from AXI request to start CHI request sending)",
    )

    # ---- 队列大小 ----
    req_queue_size = Param.Unsigned(
        1024, "Maximum number of CHI requests that can be buffered"
    )
    resp_queue_size = Param.Unsigned(
        1024, "Maximum number of AXI responses that can be buffered"
    )

    # ---- 地址范围 ----
    ranges = VectorParam.AddrRange(
        [AllMemory], "Address ranges accepted by this bridge"
    )

    # ---- QoS WRR 权重 ----
    wrr_high_weight = Param.Unsigned(4, "WRR weight for high-priority queue")
    wrr_low_weight = Param.Unsigned(1, "WRR weight for low-priority queue")
