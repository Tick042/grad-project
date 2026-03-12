#!/usr/bin/env python3
"""
Traffic Patterns Generator for AXI2CHI Bridge Experiments

生成不同的 Polybench 式内存访问模式，用于论文对比实验:
  - GEMM (矩阵乘法): 顺序行读 + 跨步列读 + 顺序写
  - 2D Conv (2D卷积): 滑窗读 + 核权重读 + 顺序写
  - Linear (线性顺序): DMA 类顺序传输
  - Random (纯随机): 高竞争随机访问

Ref: tutor.md - Polybench 矩阵运算内存访问模式
"""

import argparse
import os


class TrafficPatternGenerator:
    """流量模式生成器，生成 gem5 TrafficGen 配置文件"""

    def __init__(self, mem_size_mb=512, cache_line_size=64):
        self.mem_size = mem_size_mb * 1024 * 1024
        self.cache_line_size = cache_line_size

    def generate_gemm(self, burst_size=256, duration_ns=1000000):
        """GEMM 矩阵乘法访问模式

        C[i][j] += A[i][k] * B[k][j]
        - A: 行顺序读 (连续访问, 高局部性)
        - B: 列跨步读 (跨步访问, 低局部性)
        - C: 行顺序写 (连续写)

        Ref: Polybench gemm kernel
        """
        a_start = 0
        a_end = self.mem_size // 3
        b_start = a_end
        b_end = 2 * self.mem_size // 3
        c_start = b_end
        c_end = self.mem_size

        dur = duration_ns * 1000  # ps -> gem5 ticks
        period = 1000  # 1ns min period

        states = [
            # 读矩阵 A (顺序)
            f"STATE 0 {dur} LINEAR {a_start} {a_end} "
            f"{burst_size} {period} {period} 100 0",
            # 读矩阵 B (随机模拟跨步)
            f"STATE 1 {dur} RANDOM {b_start} {b_end} "
            f"{burst_size} {period} {period} 100 0",
            # 写矩阵 C (顺序)
            f"STATE 2 {dur} LINEAR {c_start} {c_end} "
            f"{burst_size} {period} {period} 0 0",
        ]

        transitions = [
            "TRANSITION 0 1 1",
            "TRANSITION 1 2 1",
            "TRANSITION 2 0 1",
        ]

        return self._format_config(states, transitions)

    def generate_conv2d(self, burst_size=256, duration_ns=1000000):
        """2D卷积访问模式

        对输入特征图做滑窗卷积:
        - Input: 随机滑窗读 (重复读局部区域)
        - Kernel: 小范围重复读 (高局部性)
        - Output: 顺序写

        Ref: Polybench 2mm/3mm kernels
        """
        input_start = 0
        input_end = self.mem_size // 2
        kernel_start = input_end
        kernel_end = kernel_start + 64 * 1024  # 64KB kernel
        output_start = kernel_end
        output_end = self.mem_size

        dur = duration_ns * 1000
        period = 1000

        states = [
            # 读输入特征图 (随机滑窗)
            f"STATE 0 {dur} RANDOM {input_start} {input_end} "
            f"{burst_size} {period} {period} 100 0",
            # 读卷积核 (小范围线性)
            f"STATE 1 {dur // 4} LINEAR {kernel_start} {kernel_end} "
            f"{min(burst_size, 64)} {period} {period} 100 0",
            # 写输出 (顺序)
            f"STATE 2 {dur} LINEAR {output_start} {output_end} "
            f"{burst_size} {period} {period} 0 0",
        ]

        transitions = [
            "TRANSITION 0 1 1",
            "TRANSITION 1 2 1",
            "TRANSITION 2 0 1",
        ]

        return self._format_config(states, transitions)

    def generate_linear(
        self, burst_size=256, read_pct=65, duration_ns=1000000
    ):
        """线性顺序访问 - 模拟 DMA 传输"""
        dur = duration_ns * 1000
        period = 1000

        states = [
            f"STATE 0 {dur} LINEAR 0 {self.mem_size} "
            f"{burst_size} {period} {period} {read_pct} 0",
        ]

        transitions = [
            "TRANSITION 0 0 1",
        ]

        return self._format_config(states, transitions)

    def generate_random(
        self, burst_size=256, read_pct=65, duration_ns=1000000
    ):
        """纯随机访问 - 高竞争压力测试"""
        dur = duration_ns * 1000
        period = 1000

        states = [
            f"STATE 0 {dur} RANDOM 0 {self.mem_size} "
            f"{burst_size} {period} {period} {read_pct} 0",
        ]

        transitions = [
            "TRANSITION 0 0 1",
        ]

        return self._format_config(states, transitions)

    def _format_config(self, states, transitions):
        """格式化为 gem5 TrafficGen 配置文件内容"""
        lines = ["INIT 0"]
        lines.extend(states)
        lines.extend(transitions)
        return "\n".join(lines) + "\n"


def generate_all_configs(
    output_dir, mem_size_mb=512, cache_line_size=64, burst_size=256
):
    """生成全部预定义流量模式配置文件

    用于自动化批量实验:
    output_dir/
      tgen_gemm.cfg
      tgen_conv2d.cfg
      tgen_linear.cfg
      tgen_random.cfg
    """
    os.makedirs(output_dir, exist_ok=True)
    gen = TrafficPatternGenerator(mem_size_mb, cache_line_size)

    patterns = {
        "gemm": gen.generate_gemm(burst_size),
        "conv2d": gen.generate_conv2d(burst_size),
        "linear": gen.generate_linear(burst_size),
        "random": gen.generate_random(burst_size),
    }

    for name, config in patterns.items():
        filepath = os.path.join(output_dir, f"tgen_{name}.cfg")
        with open(filepath, "w") as f:
            f.write(config)
        print(f"Generated: {filepath}")

    return patterns


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate TrafficGen configs for AXI2CHI experiments"
    )
    parser.add_argument("--output-dir", type=str, default="tgen_configs/")
    parser.add_argument(
        "--mem-size", type=int, default=512, help="Memory size in MB"
    )
    parser.add_argument("--cache-line", type=int, default=64)
    parser.add_argument("--burst-size", type=int, default=256)

    args = parser.parse_args()
    generate_all_configs(
        args.output_dir, args.mem_size, args.cache_line, args.burst_size
    )
