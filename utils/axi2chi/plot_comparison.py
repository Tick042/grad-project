#!/usr/bin/env python3
"""

用法:
  python3 utils/axi2chi/plot_comparison.py \
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
import matplotlib
from parse_stats import StatsParser

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib import rcParams

# ===================================================================
# 全局美化样式
# ===================================================================
rcParams.update(
    {
        "font.family": "DejaVu Sans",
        "font.size": 11,
        "axes.titlesize": 14,
        "axes.titleweight": "bold",
        "axes.labelsize": 12,
        "xtick.labelsize": 11,
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

# 配色方案: 学术论文风格
COLORS = {
    "naive": "#E57373",  # 柔和红
    "baseline": "#64B5F6",  # 柔和蓝
    "cnba": "#81C784",  # 柔和绿
}
HATCHES = {
    "naive": "///",
    "baseline": "...",
    "cnba": "",
}
LABELS = {
    "naive": "Naive Bridge",
    "baseline": "Baseline (CF'20)",
    "cnba": "CNBA (Ours)",
}


def parse_all(args):
    """解析三组 stats.txt"""
    results = OrderedDict()
    for key, path in [
        ("naive", args.naive),
        ("baseline", args.baseline),
        ("cnba", args.cnba),
    ]:
        if path and os.path.exists(path):
            p = StatsParser()
            p.parse_file(path)
            results[key] = p.get_bridge_stats()
        else:
            print(f"Warning: {LABELS[key]} stats file not found: {path}")
    return results


def add_bar_labels(ax, bars, fmt=".2f", offset=4, fontsize=9):
    """在柱状图顶部添加数值标签"""
    for bar in bars:
        h = bar.get_height()
        if h == 0:
            continue
        text = f"{h:{fmt}}" if isinstance(fmt, str) else fmt(h)
        ax.annotate(
            text,
            xy=(bar.get_x() + bar.get_width() / 2, h),
            xytext=(0, offset),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=fontsize,
            fontweight="bold",
            color="#333333",
        )


def styled_bar(ax, x_labels, values, colors, hatches, ylabel, title):
    """统一风格的柱状图"""
    bars = ax.bar(
        x_labels,
        values,
        width=0.55,
        color=colors,
        edgecolor="#444444",
        linewidth=0.8,
        hatch=[
            hatches.get(k, "")
            for k in (["naive", "baseline", "cnba"][: len(values)])
        ],
    )
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_ylim(bottom=0, top=max(values) * 1.18 if max(values) > 0 else 1)
    return bars


def plot_request_amplification(results, output_dir):
    """图1: 请求放大率"""
    fig, ax = plt.subplots(figsize=(7, 4.5))

    keys = list(results.keys())
    labels = [LABELS[k] for k in keys]
    ratios = []
    for k in keys:
        v = results[k]
        axi = max(v.get("totalAxiRequests", 1), 1)
        chi = v.get("totalChiRequests", 0)
        ratios.append(chi / axi)

    colors = [COLORS[k] for k in keys]
    bars = styled_bar(
        ax,
        labels,
        ratios,
        colors,
        HATCHES,
        "CHI / AXI Request Ratio",
        "Request Amplification Ratio",
    )
    add_bar_labels(ax, bars)

    # 理想线
    ax.axhline(
        y=1.0, color="#D32F2F", linestyle="--", linewidth=1.2, alpha=0.6
    )
    ax.annotate(
        "1:1 (ideal)",
        xy=(len(keys) - 0.6, 1.02),
        fontsize=9,
        color="#D32F2F",
        alpha=0.8,
        style="italic",
    )

    plt.tight_layout()
    path = os.path.join(output_dir, "01_req_amplification.png")
    plt.savefig(path)
    plt.close()
    print(f"  Saved: {path}")


def plot_throughput(results, output_dir):
    """图2: 吞吐量"""
    fig, ax = plt.subplots(figsize=(7, 4.5))

    keys = list(results.keys())
    labels = [LABELS[k] for k in keys]
    tp_gbs = [results[k].get("throughput", 0) / 1e9 for k in keys]
    colors = [COLORS[k] for k in keys]

    bars = styled_bar(
        ax,
        labels,
        tp_gbs,
        colors,
        HATCHES,
        "Throughput (GB/s)",
        "Bridge Throughput",
    )
    add_bar_labels(ax, bars)

    plt.tight_layout()
    path = os.path.join(output_dir, "02_throughput.png")
    plt.savefig(path)
    plt.close()
    print(f"  Saved: {path}")


def plot_latency_stall(results, output_dir):
    """图3: 平均延迟 + Stall 组合图"""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4.5))

    keys = list(results.keys())
    labels = [LABELS[k] for k in keys]
    colors = [COLORS[k] for k in keys]

    # 左图: 平均延迟
    latencies = [results[k].get("avgLatency", 0) for k in keys]
    bars1 = styled_bar(
        ax1,
        labels,
        latencies,
        colors,
        HATCHES,
        "Average Latency (ticks)",
        "Bridge Crossing Latency",
    )
    add_bar_labels(ax1, bars1, fmt=".0f")

    # 右图: Stall cycles
    stalls = [results[k].get("robStallCycles", 0) for k in keys]
    bars2 = styled_bar(
        ax2,
        labels,
        stalls,
        colors,
        HATCHES,
        "Stall Cycles",
        "TxnID Exhaustion Stalls",
    )
    add_bar_labels(ax2, bars2, fmt=".0f")

    plt.tight_layout(w_pad=3)
    path = os.path.join(output_dir, "03_latency_stall.png")
    plt.savefig(path)
    plt.close()
    print(f"  Saved: {path}")


def plot_split_merge(results, output_dir):
    """图4: 拆分/合并次数"""
    fig, ax = plt.subplots(figsize=(8, 4.5))

    keys = list(results.keys())
    labels = [LABELS[k] for k in keys]
    x = range(len(keys))
    w = 0.32

    splits = [results[k].get("splitCount", 0) for k in keys]
    merges = [results[k].get("mergeCount", 0) for k in keys]

    bars1 = ax.bar(
        [i - w / 2 for i in x],
        splits,
        w,
        label="Split Count",
        color="#EF5350",
        edgecolor="#444",
        linewidth=0.8,
        hatch="///",
    )
    bars2 = ax.bar(
        [i + w / 2 for i in x],
        merges,
        w,
        label="Merge Count",
        color="#66BB6A",
        edgecolor="#444",
        linewidth=0.8,
    )

    ax.set_xticks(list(x))
    ax.set_xticklabels(labels)
    ax.set_ylabel("Count")
    ax.set_title("Transaction Split vs Merge Operations")
    ax.legend(loc="upper left", framealpha=0.9)

    for bar in list(bars1) + list(bars2):
        h = bar.get_height()
        if h > 0:
            ax.annotate(
                f"{h:,.0f}",
                xy=(bar.get_x() + bar.get_width() / 2, h),
                xytext=(0, 3),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=8,
                fontweight="bold",
                color="#333",
            )

    plt.tight_layout()
    path = os.path.join(output_dir, "04_split_merge.png")
    plt.savefig(path)
    plt.close()
    print(f"  Saved: {path}")


def plot_combined_panel(results, output_dir):
    """图5: 综合 2x2 面板图"""
    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    fig.suptitle(
        "AXI2CHI Bridge Performance Comparison\n"
        "Naive vs Baseline (CF'20) vs CNBA",
        fontsize=15,
        fontweight="bold",
        y=0.98,
    )

    keys = list(results.keys())
    labels = [LABELS[k] for k in keys]
    colors = [COLORS[k] for k in keys]

    # (a) 请求放大率
    ratios = []
    for k in keys:
        v = results[k]
        axi = max(v.get("totalAxiRequests", 1), 1)
        chi = v.get("totalChiRequests", 0)
        ratios.append(chi / axi)
    bars = styled_bar(
        axes[0, 0],
        labels,
        ratios,
        colors,
        HATCHES,
        "CHI/AXI Ratio",
        "(a) Request Amplification",
    )
    add_bar_labels(axes[0, 0], bars)
    axes[0, 0].axhline(
        y=1.0, color="#D32F2F", linestyle="--", linewidth=1, alpha=0.5
    )

    # (b) 吞吐量
    tp = [results[k].get("throughput", 0) / 1e9 for k in keys]
    bars = styled_bar(
        axes[0, 1], labels, tp, colors, HATCHES, "GB/s", "(b) Throughput"
    )
    add_bar_labels(axes[0, 1], bars)

    # (c) 平均延迟
    lat = [results[k].get("avgLatency", 0) for k in keys]
    bars = styled_bar(
        axes[1, 0],
        labels,
        lat,
        colors,
        HATCHES,
        "Ticks",
        "(c) Average Latency",
    )
    add_bar_labels(axes[1, 0], bars, fmt=".0f")

    # (d) Stall Cycles
    stalls = [results[k].get("robStallCycles", 0) for k in keys]
    bars = styled_bar(
        axes[1, 1],
        labels,
        stalls,
        colors,
        HATCHES,
        "Cycles",
        "(d) TxnID Exhaustion Stalls",
    )
    add_bar_labels(axes[1, 1], bars, fmt=".0f")

    plt.tight_layout(rect=[0, 0, 1, 0.93])
    path = os.path.join(output_dir, "05_combined_panel.png")
    plt.savefig(path)
    plt.close()
    print(f"  Saved: {path}")


def plot_summary_table(results, output_dir):
    """图6: 数据汇总表"""
    fig, ax = plt.subplots(figsize=(10, 3.5))
    ax.axis("off")

    keys = list(results.keys())
    col_labels = [LABELS[k] for k in keys]
    row_labels = [
        "AXI Requests",
        "CHI Requests",
        "CHI/AXI Ratio",
        "Throughput (GB/s)",
        "Avg Latency (ticks)",
        "Stall Cycles",
        "Merge Count",
        "Split Count",
    ]

    cell_data = []
    for row_label in row_labels:
        row = []
        for k in keys:
            v = results[k]
            if row_label == "AXI Requests":
                row.append(f"{v.get('totalAxiRequests', 0):,.0f}")
            elif row_label == "CHI Requests":
                row.append(f"{v.get('totalChiRequests', 0):,.0f}")
            elif row_label == "CHI/AXI Ratio":
                axi = max(v.get("totalAxiRequests", 1), 1)
                chi = v.get("totalChiRequests", 0)
                row.append(f"{chi / axi:.3f}")
            elif row_label == "Throughput (GB/s)":
                row.append(f"{v.get('throughput', 0) / 1e9:.2f}")
            elif row_label == "Avg Latency (ticks)":
                row.append(f"{v.get('avgLatency', 0):,.0f}")
            elif row_label == "Stall Cycles":
                row.append(f"{v.get('robStallCycles', 0):,.0f}")
            elif row_label == "Merge Count":
                row.append(f"{v.get('mergeCount', 0):,.0f}")
            elif row_label == "Split Count":
                row.append(f"{v.get('splitCount', 0):,.0f}")
        cell_data.append(row)

    table = ax.table(
        cellText=cell_data,
        rowLabels=row_labels,
        colLabels=col_labels,
        loc="center",
        cellLoc="center",
    )
    table.auto_set_font_size(False)
    table.set_fontsize(10)
    table.scale(1.0, 1.6)

    # 表头颜色
    for j, k in enumerate(keys):
        table[0, j].set_facecolor(COLORS[k])
        table[0, j].set_text_props(fontweight="bold", color="white")
    # 行标签颜色
    for i in range(len(row_labels)):
        table[i + 1, -1].set_facecolor("#F5F5F5")

    ax.set_title(
        "Performance Summary Table", fontsize=14, fontweight="bold", pad=15
    )
    plt.tight_layout()
    path = os.path.join(output_dir, "06_summary_table.png")
    plt.savefig(path)
    plt.close()
    print(f"  Saved: {path}")


def main():
    parser = argparse.ArgumentParser(
        description="AXI2CHI Bridge beautified comparison plots"
    )
    parser.add_argument(
        "--naive", type=str, required=True, help="Path to naive mode stats.txt"
    )
    parser.add_argument(
        "--baseline",
        type=str,
        required=True,
        help="Path to baseline mode stats.txt",
    )
    parser.add_argument(
        "--cnba", type=str, required=True, help="Path to CNBA mode stats.txt"
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default="m5out/plots",
        help="Output directory for plots",
    )
    args = parser.parse_args()

    print("Parsing stats files...")
    results = parse_all(args)
    if len(results) < 2:
        print("Error: Need at least 2 stats files for comparison.")
        sys.exit(1)

    print(f"Generating plots to {args.output_dir}/ ...")
    os.makedirs(args.output_dir, exist_ok=True)

    plot_request_amplification(results, args.output_dir)
    plot_throughput(results, args.output_dir)
    plot_latency_stall(results, args.output_dir)
    plot_split_merge(results, args.output_dir)
    plot_combined_panel(results, args.output_dir)
    plot_summary_table(results, args.output_dir)

    print(f"\nDone! {6} plots saved to {args.output_dir}/")


if __name__ == "__main__":
    main()
