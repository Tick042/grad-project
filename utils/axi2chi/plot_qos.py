#!/usr/bin/env python3
"""
AXI2CHI Bridge QoS 消融实验绘图

对比 CNBA (QoS Off) vs CNBA (QoS On):
  - 高/低优先级平均延迟
  - 整体吞吐量
  - 汇总表

用法:
  python3 utils/axi2chi/plot_qos.py \
      --qos-off m5out/qos_random_off/stats.txt \
      --qos-on  m5out/qos_random_on/stats.txt \
      --output-dir m5out/plots_qos
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import matplotlib
from parse_stats import StatsParser

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import rcParams

rcParams.update(
    {
        "font.family": "DejaVu Sans",
        "font.size": 11,
        "axes.titlesize": 13,
        "axes.titleweight": "bold",
        "axes.labelsize": 12,
        "xtick.labelsize": 10,
        "ytick.labelsize": 10,
        "legend.fontsize": 10,
        "figure.dpi": 150,
        "savefig.dpi": 300,
        "savefig.bbox": "tight",
        "axes.grid": True,
        "grid.alpha": 0.3,
        "grid.linestyle": "--",
        "axes.spines.top": False,
        "axes.spines.right": False,
    }
)


def load(path):
    p = StatsParser()
    raw = p.parse_file(path)
    bridge = p.get_bridge_stats()
    # Also grab QoS-specific stats
    for k in (
        "highPrioCount",
        "highPrioLatency",
        "lowPrioCount",
        "lowPrioLatency",
        "avgHighPrioLatency",
        "avgLowPrioLatency",
    ):
        if k in p.stats:
            bridge[k] = p.stats[k]
    if "sim_ticks" in p.stats:
        bridge["sim_ticks"] = p.stats["sim_ticks"]
    return bridge


def plot_latency_comparison(off, on, output_dir):
    """Grouped bar: high-prio vs low-prio latency, QoS off vs on."""
    fig, ax = plt.subplots(figsize=(9, 5))

    off_avg = off.get("avgLatency", 0) / 1000
    on_high = on.get("avgHighPrioLatency", 0) / 1000
    on_low = on.get("avgLowPrioLatency", 0) / 1000
    on_avg = on.get("avgLatency", 0) / 1000

    labels = [
        "Overall\n(QoS Off)",
        "High-Prio\n(QoS On)",
        "Low-Prio\n(QoS On)",
        "Overall\n(QoS On)",
    ]
    values = [off_avg, on_high, on_low, on_avg]
    colors = ["#64B5F6", "#81C784", "#E57373", "#FFB74D"]

    bars = ax.bar(
        labels,
        values,
        width=0.55,
        color=colors,
        edgecolor="#444",
        linewidth=0.8,
    )
    ax.set_ylabel("Average Latency (cycles)")
    ax.set_title("QoS Scheduling: Latency Impact")
    ax.set_ylim(bottom=0, top=max(values) * 1.2)

    for bar in bars:
        h = bar.get_height()
        ax.annotate(
            f"{h:,.0f}",
            xy=(bar.get_x() + bar.get_width() / 2, h),
            xytext=(0, 4),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
            fontweight="bold",
        )

    # Reduction annotation
    if off_avg > 0 and on_high > 0:
        reduction = (off_avg - on_high) / off_avg * 100
        ax.annotate(
            f"High-prio latency\n−{reduction:.1f}%",
            xy=(1, on_high),
            xytext=(2.5, on_high * 0.5),
            fontsize=10,
            color="#2E7D32",
            fontweight="bold",
            arrowprops=dict(arrowstyle="->", color="#2E7D32"),
        )

    plt.tight_layout()
    path = os.path.join(output_dir, "qos_latency_comparison.png")
    plt.savefig(path)
    plt.close()
    print(f"  Saved: {path}")


def plot_throughput_comparison(off, on, output_dir):
    """Side-by-side throughput."""
    fig, ax = plt.subplots(figsize=(6, 4.5))

    tp_off = off.get("throughput", 0) / 1e9
    tp_on = on.get("throughput", 0) / 1e9

    bars = ax.bar(
        ["CNBA\n(QoS Off)", "CNBA\n(QoS On)"],
        [tp_off, tp_on],
        width=0.45,
        color=["#64B5F6", "#81C784"],
        edgecolor="#444",
        linewidth=0.8,
    )
    ax.set_ylabel("Throughput (GB/s)")
    ax.set_title("QoS Scheduling: Throughput Impact")
    ax.set_ylim(bottom=0, top=max(tp_off, tp_on) * 1.15)

    for bar in bars:
        h = bar.get_height()
        ax.annotate(
            f"{h:.2f}",
            xy=(bar.get_x() + bar.get_width() / 2, h),
            xytext=(0, 4),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=10,
            fontweight="bold",
        )

    plt.tight_layout()
    path = os.path.join(output_dir, "qos_throughput_comparison.png")
    plt.savefig(path)
    plt.close()
    print(f"  Saved: {path}")


def plot_summary_table(off, on, output_dir):
    """Summary table image."""
    fig, ax = plt.subplots(figsize=(10, 3))
    ax.axis("off")

    off_avg = off.get("avgLatency", 0) / 1000
    on_high = on.get("avgHighPrioLatency", 0) / 1000
    on_low = on.get("avgLowPrioLatency", 0) / 1000
    on_avg = on.get("avgLatency", 0) / 1000
    tp_off = off.get("throughput", 0) / 1e9
    tp_on = on.get("throughput", 0) / 1e9
    hi_count = int(on.get("highPrioCount", 0))
    lo_count = int(on.get("lowPrioCount", 0))

    col_labels = ["CNBA (QoS Off)", "CNBA (QoS On)"]
    row_labels = [
        "High-Prio Count",
        "Low-Prio Count",
        "High-Prio Avg Latency (cyc)",
        "Low-Prio Avg Latency (cyc)",
        "Overall Avg Latency (cyc)",
        "Throughput (GB/s)",
        "simTicks",
    ]

    def fmt_lat(v):
        return f"{v:,.1f}" if v == v else "N/A"

    cell_data = [
        ["0 (all equal)", f"{hi_count:,}"],
        [f"{int(off.get('totalAxiRequests', 0)):,}", f"{lo_count:,}"],
        [fmt_lat(off_avg), fmt_lat(on_high)],
        [fmt_lat(off_avg), fmt_lat(on_low)],
        [fmt_lat(off_avg), fmt_lat(on_avg)],
        [f"{tp_off:.2f}", f"{tp_on:.2f}"],
        [f"{off.get('sim_ticks', 0):,.0f}", f"{on.get('sim_ticks', 0):,.0f}"],
    ]

    table = ax.table(
        cellText=cell_data,
        rowLabels=row_labels,
        colLabels=col_labels,
        loc="center",
        cellLoc="center",
    )
    table.auto_set_font_size(False)
    table.set_fontsize(10)
    table.scale(1.0, 1.5)

    for j in range(2):
        table[0, j].set_facecolor("#64B5F6" if j == 0 else "#81C784")
        table[0, j].set_text_props(fontweight="bold", color="white")

    ax.set_title(
        "QoS Scheduling Ablation Summary",
        fontsize=14,
        fontweight="bold",
        pad=15,
    )
    plt.tight_layout()
    path = os.path.join(output_dir, "qos_summary_table.png")
    plt.savefig(path)
    plt.close()
    print(f"  Saved: {path}")


def main():
    parser = argparse.ArgumentParser(description="QoS ablation plots")
    parser.add_argument(
        "--qos-off", required=True, help="CNBA without QoS stats.txt"
    )
    parser.add_argument(
        "--qos-on", required=True, help="CNBA with QoS stats.txt"
    )
    parser.add_argument("--output-dir", default="m5out/plots_qos")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    print("Parsing stats...")
    off = load(args.qos_off)
    on = load(args.qos_on)

    print("Generating QoS ablation plots...")
    plot_latency_comparison(off, on, args.output_dir)
    plot_throughput_comparison(off, on, args.output_dir)
    plot_summary_table(off, on, args.output_dir)

    print(f"\nDone! 3 plots saved to {args.output_dir}/")


if __name__ == "__main__":
    main()
