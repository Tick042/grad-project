#!/usr/bin/env python3
"""
AXI2CHI Bridge 三组对比绘图脚本

对比 Naive Bridge / Baseline (CF'20) / CNBA 三种模式的性能指标:
  1. 请求放大率 (CHI/AXI Ratio)
  2. 平均延迟 (Average Latency)
  3. 吞吐量 (Throughput)
  4. ROB Stall Cycles
  5. 综合对比表

用法:
  python3 utils/axi2chi/plot_three_way.py \
      --naive   m5out/naive_varied/stats.txt \
      --baseline m5out/baseline_varied/stats.txt \
      --cnba    m5out/cnba_varied/stats.txt \
      --output-dir m5out/plots
"""

import argparse
import os
import sys
from collections import OrderedDict

sys.path.insert(0, os.path.dirname(__file__))
from parse_stats import StatsParser

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker

    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    print("Warning: matplotlib not available, using text output.")


def parse_all(args):
    """解析三组 stats.txt"""
    results = OrderedDict()
    for label, path in [
        ("Naive Bridge", args.naive),
        ("Baseline (CF'20)", args.baseline),
        ("CNBA (Ours)", args.cnba),
    ]:
        if path and os.path.exists(path):
            p = StatsParser()
            p.parse_file(path)
            results[label] = p.get_bridge_stats()
        else:
            print(f"Warning: {label} stats file not found: {path}")
    return results


def plot_bar(ax, labels, values, ylabel, title, colors):
    bars = ax.bar(labels, values, color=colors, edgecolor="black", width=0.6)
    ax.set_ylabel(ylabel, fontsize=12)
    ax.set_title(title, fontsize=13, fontweight="bold")
    ax.grid(axis="y", alpha=0.3, linestyle="--")
    for bar, val in zip(bars, values):
        h = bar.get_height()
        fmt = f"{val:.0f}" if val > 100 else f"{val:.2f}"
        ax.annotate(
            fmt,
            xy=(bar.get_x() + bar.get_width() / 2, h),
            xytext=(0, 4),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
            fontweight="bold",
        )
    return bars


def plot_all(results, output_dir, fmt="png"):
    """生成所有对比图"""
    os.makedirs(output_dir, exist_ok=True)
    labels = list(results.keys())
    colors = ["#A5D6A7", "#4472C4", "#ED7D31"]  # green, blue, orange

    if not HAS_MATPLOTLIB:
        text_output(results)
        return

    # ===== Figure 1: 请求放大率 =====
    fig, ax = plt.subplots(figsize=(8, 5))
    ratios = []
    for v in results.values():
        axi = max(v.get("totalAxiRequests", 1), 1)
        chi = v.get("totalChiRequests", 0)
        ratios.append(chi / axi)
    plot_bar(
        ax,
        labels,
        ratios,
        "CHI / AXI Request Ratio",
        "Request Amplification Ratio",
        colors,
    )
    ax.axhline(y=1.0, color="red", linestyle="--", alpha=0.5, linewidth=1)
    ax.text(
        len(labels) - 0.5,
        1.01,
        "1:1 (ideal)",
        color="red",
        fontsize=9,
        alpha=0.7,
    )
    plt.tight_layout()
    plt.savefig(
        os.path.join(output_dir, f"req_amplification.{fmt}"),
        dpi=300,
        bbox_inches="tight",
    )
    plt.close()
    print(f"  Saved: req_amplification.{fmt}")

    # ===== Figure 2: 平均延迟 =====
    fig, ax = plt.subplots(figsize=(8, 5))
    latencies = [v.get("avgLatency", 0) for v in results.values()]
    plot_bar(
        ax,
        labels,
        latencies,
        "Average Latency (ticks)",
        "Bridge Crossing Latency",
        colors,
    )
    plt.tight_layout()
    plt.savefig(
        os.path.join(output_dir, f"avg_latency.{fmt}"),
        dpi=300,
        bbox_inches="tight",
    )
    plt.close()
    print(f"  Saved: avg_latency.{fmt}")

    # ===== Figure 3: 吞吐量 =====
    fig, ax = plt.subplots(figsize=(8, 5))
    throughputs = [v.get("throughput", 0) for v in results.values()]
    # 转换为 GB/s
    tp_gbs = [t / 1e9 for t in throughputs]
    plot_bar(
        ax, labels, tp_gbs, "Throughput (GB/s)", "Bridge Throughput", colors
    )
    plt.tight_layout()
    plt.savefig(
        os.path.join(output_dir, f"throughput.{fmt}"),
        dpi=300,
        bbox_inches="tight",
    )
    plt.close()
    print(f"  Saved: throughput.{fmt}")

    # ===== Figure 4: ROB Stall Cycles =====
    fig, ax = plt.subplots(figsize=(8, 5))
    stalls = [v.get("robStallCycles", 0) for v in results.values()]
    plot_bar(
        ax,
        labels,
        stalls,
        "Stall Cycles",
        "ROB Stall Cycles (TxnID Exhaustion)",
        colors,
    )
    plt.tight_layout()
    plt.savefig(
        os.path.join(output_dir, f"stall_cycles.{fmt}"),
        dpi=300,
        bbox_inches="tight",
    )
    plt.close()
    print(f"  Saved: stall_cycles.{fmt}")

    # ===== Figure 5: 综合对比 (2x2 子图) =====
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle(
        "AXI2CHI Bridge Performance Comparison\n"
        "(Naive vs Baseline vs CNBA)",
        fontsize=15,
        fontweight="bold",
    )

    # 5a: 请求放大率
    plot_bar(
        axes[0, 0],
        labels,
        ratios,
        "CHI/AXI Ratio",
        "Request Amplification",
        colors,
    )
    axes[0, 0].axhline(y=1.0, color="red", linestyle="--", alpha=0.5)

    # 5b: 平均延迟
    plot_bar(
        axes[0, 1],
        labels,
        latencies,
        "Latency (ticks)",
        "Average Latency",
        colors,
    )

    # 5c: 吞吐量
    plot_bar(axes[1, 0], labels, tp_gbs, "GB/s", "Throughput", colors)

    # 5d: Stall Cycles
    plot_bar(axes[1, 1], labels, stalls, "Cycles", "ROB Stall Cycles", colors)

    plt.tight_layout(rect=[0, 0, 1, 0.94])
    plt.savefig(
        os.path.join(output_dir, f"combined_comparison.{fmt}"),
        dpi=300,
        bbox_inches="tight",
    )
    plt.close()
    print(f"  Saved: combined_comparison.{fmt}")

    # ===== Figure 6: 分裂次数与合并次数对比 =====
    fig, ax = plt.subplots(figsize=(10, 5))
    splits = [v.get("splitCount", 0) for v in results.values()]
    merges = [v.get("mergeCount", 0) for v in results.values()]
    x = range(len(labels))
    w = 0.35
    bars1 = ax.bar(
        [i - w / 2 for i in x],
        splits,
        w,
        label="Split Count",
        color="#FF7043",
        edgecolor="black",
    )
    bars2 = ax.bar(
        [i + w / 2 for i in x],
        merges,
        w,
        label="Merge Count",
        color="#66BB6A",
        edgecolor="black",
    )
    ax.set_ylabel("Count", fontsize=12)
    ax.set_title(
        "Transaction Split vs Merge Count", fontsize=13, fontweight="bold"
    )
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, fontsize=11)
    ax.legend(fontsize=11)
    ax.grid(axis="y", alpha=0.3, linestyle="--")
    for bar in list(bars1) + list(bars2):
        h = bar.get_height()
        if h > 0:
            ax.annotate(
                f"{h:.0f}",
                xy=(bar.get_x() + bar.get_width() / 2, h),
                xytext=(0, 3),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=8,
            )
    plt.tight_layout()
    plt.savefig(
        os.path.join(output_dir, f"split_merge.{fmt}"),
        dpi=300,
        bbox_inches="tight",
    )
    plt.close()
    print(f"  Saved: split_merge.{fmt}")


def text_output(results):
    """纯文本输出对比结果"""
    print("\n" + "=" * 80)
    print("  AXI2CHI Bridge Three-Way Comparison")
    print("=" * 80)
    print(f"  {'Metric':<25}", end="")
    for label in results:
        print(f" {label:>18}", end="")
    print()
    print(f"  {'-'*25}", end="")
    for _ in results:
        print(f" {'-'*18}", end="")
    print()

    metrics = [
        "totalAxiRequests",
        "totalChiRequests",
        "mergeCount",
        "splitCount",
        "avgLatency",
        "throughput",
        "robStallCycles",
    ]
    for m in metrics:
        print(f"  {m:<25}", end="")
        for v in results.values():
            val = v.get(m, 0)
            if val > 1e6:
                print(f" {val:>18.2f}", end="")
            else:
                print(f" {val:>18.4f}", end="")
        print()

    # Derived: CHI/AXI ratio
    print(f"  {'CHI/AXI Ratio':<25}", end="")
    for v in results.values():
        axi = max(v.get("totalAxiRequests", 1), 1)
        chi = v.get("totalChiRequests", 0)
        print(f" {chi/axi:>18.4f}", end="")
    print()
    print("=" * 80)


def print_table(results):
    """打印表格形式的对比结果"""
    text_output(results)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Plot three-way comparison: Naive vs Baseline vs CNBA"
    )
    parser.add_argument(
        "--naive",
        type=str,
        required=True,
        help="Path to naive bridge stats.txt",
    )
    parser.add_argument(
        "--baseline",
        type=str,
        required=True,
        help="Path to baseline stats.txt",
    )
    parser.add_argument(
        "--cnba", type=str, required=True, help="Path to CNBA stats.txt"
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default="m5out/plots",
        help="Output directory for plots",
    )
    parser.add_argument(
        "--format",
        type=str,
        default="png",
        choices=["pdf", "png", "svg"],
        help="Output image format",
    )

    args = parser.parse_args()
    results = parse_all(args)

    if not results:
        print("Error: No stats files could be parsed.")
        sys.exit(1)

    print("\n[Results Summary]")
    print_table(results)

    print(f"\n[Generating Plots -> {args.output_dir}]")
    plot_all(results, args.output_dir, args.format)
    print(f"\nDone! All plots saved to: {args.output_dir}/")
