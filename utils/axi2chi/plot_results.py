#!/usr/bin/env python3
"""
AXI2CHI Bridge Experiment Result Plotter
  1. 延迟对比柱状图 (Baseline vs CNBA)
  2. 吞吐量对比图
  3. TxnID 使用率随时间变化图
  4. 请求放大率对比
  5. 消融实验结果图
"""

import argparse
import os
import sys

try:
    import matplotlib

    matplotlib.use("Agg")  # 无头模式
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker

    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    print("Warning: matplotlib not installed. Using text-based output.")


# ==================================================================
# 绘图函数
# ==================================================================


def plot_latency_comparison(
    baseline_stats, cnba_stats, output_path="latency_compare.pdf"
):
    """延迟对比柱状图

    Ref: CF'20 Paper Figure 5 style
    """
    if not HAS_MATPLOTLIB:
        _text_bar_chart(
            "Latency Comparison", baseline_stats, cnba_stats, "avgLatency"
        )
        return

    fig, ax = plt.subplots(figsize=(8, 5))

    metrics = ["avgLatency"]
    labels = ["Average Latency"]
    base_vals = [baseline_stats.get(m, 0) for m in metrics]
    cnba_vals = [cnba_stats.get(m, 0) for m in metrics]

    x = range(len(labels))
    width = 0.35

    bars1 = ax.bar(
        [i - width / 2 for i in x],
        base_vals,
        width,
        label="Baseline (CF'20)",
        color="#4472C4",
        edgecolor="black",
    )
    bars2 = ax.bar(
        [i + width / 2 for i in x],
        cnba_vals,
        width,
        label="CNBA (Ours)",
        color="#ED7D31",
        edgecolor="black",
    )

    ax.set_ylabel("Latency (ticks)", fontsize=12)
    ax.set_title("Bridge Crossing Latency Comparison", fontsize=14)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11)
    ax.legend(fontsize=11)
    ax.grid(axis="y", alpha=0.3)

    # 在柱子上标注数值
    for bar in bars1:
        h = bar.get_height()
        ax.annotate(
            f"{h:.0f}",
            xy=(bar.get_x() + bar.get_width() / 2, h),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
        )
    for bar in bars2:
        h = bar.get_height()
        ax.annotate(
            f"{h:.0f}",
            xy=(bar.get_x() + bar.get_width() / 2, h),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    plt.tight_layout()
    plt.savefig(output_path, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_path}")
    plt.close()


def plot_throughput_comparison(
    baseline_stats, cnba_stats, output_path="throughput_compare.pdf"
):
    """吞吐量对比图"""
    if not HAS_MATPLOTLIB:
        _text_bar_chart(
            "Throughput Comparison", baseline_stats, cnba_stats, "throughput"
        )
        return

    fig, ax = plt.subplots(figsize=(8, 5))

    labels = ["Throughput"]
    base_vals = [baseline_stats.get("throughput", 0)]
    cnba_vals = [cnba_stats.get("throughput", 0)]

    x = range(len(labels))
    width = 0.35

    ax.bar(
        [i - width / 2 for i in x],
        base_vals,
        width,
        label="Baseline (CF'20)",
        color="#4472C4",
        edgecolor="black",
    )
    ax.bar(
        [i + width / 2 for i in x],
        cnba_vals,
        width,
        label="CNBA (Ours)",
        color="#ED7D31",
        edgecolor="black",
    )

    ax.set_ylabel("Throughput (Bytes/s)", fontsize=12)
    ax.set_title("Bridge Throughput Comparison", fontsize=14)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11)
    ax.legend(fontsize=11)
    ax.grid(axis="y", alpha=0.3)
    ax.yaxis.set_major_formatter(ticker.EngFormatter())

    plt.tight_layout()
    plt.savefig(output_path, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_path}")
    plt.close()


def plot_request_amplification(
    results_dict, output_path="req_amplification.pdf"
):
    """请求放大率对比 (多模式/多流量)

    Args:
        results_dict: {label: {axi_reqs, chi_reqs}, ...}
    """
    if not HAS_MATPLOTLIB:
        print("\n=== Request Amplification ===")
        for label, vals in results_dict.items():
            ratio = vals.get("chi_reqs", 0) / max(vals.get("axi_reqs", 1), 1)
            print(f"  {label}: {ratio:.4f}x")
        return

    fig, ax = plt.subplots(figsize=(10, 5))

    labels = list(results_dict.keys())
    ratios = []
    for v in results_dict.values():
        axi = max(v.get("axi_reqs", 1), 1)
        chi = v.get("chi_reqs", 0)
        ratios.append(chi / axi)

    colors = plt.cm.Set2(range(len(labels)))
    bars = ax.bar(labels, ratios, color=colors, edgecolor="black")

    ax.set_ylabel("CHI/AXI Request Ratio", fontsize=12)
    ax.set_title("Transaction Request Amplification", fontsize=14)
    ax.axhline(y=1.0, color="red", linestyle="--", alpha=0.5, label="1:1")
    ax.legend(fontsize=10)
    ax.grid(axis="y", alpha=0.3)

    for bar, ratio in zip(bars, ratios):
        ax.annotate(
            f"{ratio:.2f}x",
            xy=(bar.get_x() + bar.get_width() / 2, bar.get_height()),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    plt.xticks(rotation=30, ha="right")
    plt.tight_layout()
    plt.savefig(output_path, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_path}")
    plt.close()


def plot_ablation_study(results_dict, output_path="ablation_study.pdf"):
    """消融实验结果图

    Args:
        results_dict: {
            "CNBA Full": {...stats...},
            "CNBA No-Merge": {...stats...},
            "Baseline": {...stats...},
        }
    """
    if not HAS_MATPLOTLIB:
        print("\n=== Ablation Study ===")
        for label, vals in results_dict.items():
            lat = vals.get("avgLatency", 0)
            tp = vals.get("throughput", 0)
            print(f"  {label}: latency={lat:.2f}, throughput={tp:.2f}")
        return

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

    labels = list(results_dict.keys())
    latencies = [v.get("avgLatency", 0) for v in results_dict.values()]
    throughputs = [v.get("throughput", 0) for v in results_dict.values()]
    colors = ["#4472C4", "#ED7D31", "#70AD47"]

    # Left: Latency
    bars1 = ax1.bar(
        labels, latencies, color=colors[: len(labels)], edgecolor="black"
    )
    ax1.set_ylabel("Average Latency (ticks)", fontsize=12)
    ax1.set_title("Ablation: Latency Impact", fontsize=13)
    ax1.grid(axis="y", alpha=0.3)

    for bar in bars1:
        h = bar.get_height()
        ax1.annotate(
            f"{h:.0f}",
            xy=(bar.get_x() + bar.get_width() / 2, h),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    # Right: Throughput
    bars2 = ax2.bar(
        labels, throughputs, color=colors[: len(labels)], edgecolor="black"
    )
    ax2.set_ylabel("Throughput (Bytes/s)", fontsize=12)
    ax2.set_title("Ablation: Throughput Impact", fontsize=13)
    ax2.grid(axis="y", alpha=0.3)
    ax2.yaxis.set_major_formatter(ticker.EngFormatter())

    for bar in bars2:
        h = bar.get_height()
        ax2.annotate(
            f"{h:.1e}",
            xy=(bar.get_x() + bar.get_width() / 2, h),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    plt.tight_layout()
    plt.savefig(output_path, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_path}")
    plt.close()


def plot_latency_sweep(sweep_results, output_path="latency_sweep.pdf"):
    """延迟扫描折线图

    Args:
        sweep_results: {latency_value: {"baseline": stats, "cnba": stats}}
    """
    if not HAS_MATPLOTLIB:
        print("\n=== Latency Sweep ===")
        for lat, modes in sweep_results.items():
            bl = modes.get("baseline", {}).get("avgLatency", 0)
            cn = modes.get("cnba", {}).get("avgLatency", 0)
            print(f"  lat={lat}: baseline={bl:.2f}, cnba={cn:.2f}")
        return

    fig, ax = plt.subplots(figsize=(8, 5))

    latencies = sorted(sweep_results.keys())
    bl_vals = [
        sweep_results[l].get("baseline", {}).get("avgLatency", 0)
        for l in latencies
    ]
    cn_vals = [
        sweep_results[l].get("cnba", {}).get("avgLatency", 0)
        for l in latencies
    ]

    ax.plot(
        latencies,
        bl_vals,
        "o-",
        label="Baseline (CF'20)",
        color="#4472C4",
        linewidth=2,
        markersize=8,
    )
    ax.plot(
        latencies,
        cn_vals,
        "s-",
        label="CNBA (Ours)",
        color="#ED7D31",
        linewidth=2,
        markersize=8,
    )

    ax.set_xlabel("Bridge Latency (cycles)", fontsize=12)
    ax.set_ylabel("Average Transaction Latency (ticks)", fontsize=12)
    ax.set_title("Latency Sensitivity Analysis", fontsize=14)
    ax.legend(fontsize=11)
    ax.grid(alpha=0.3)

    plt.tight_layout()
    plt.savefig(output_path, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_path}")
    plt.close()


# ==================================================================
# 文本模式回退
# ==================================================================


def _text_bar_chart(title, stats1, stats2, metric):
    """当 matplotlib 不可用时的文本版对比"""
    v1 = stats1.get(metric, 0)
    v2 = stats2.get(metric, 0)
    max_val = max(v1, v2, 1)

    print(f"\n=== {title} ({metric}) ===")
    bar_width = 40
    bar1 = int(v1 / max_val * bar_width)
    bar2 = int(v2 / max_val * bar_width)
    print(f"  Baseline: {'█' * bar1} {v1:.2f}")
    print(f"  CNBA:     {'█' * bar2} {v2:.2f}")
    if v1 > 0:
        improvement = (v1 - v2) / v1 * 100
        print(f"  Improvement: {improvement:+.2f}%")


# ==================================================================
# 主函数
# ==================================================================

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Plot AXI2CHI bridge experiment results"
    )
    parser.add_argument(
        "--baseline", type=str, help="Path to baseline stats.txt"
    )
    parser.add_argument("--cnba", type=str, help="Path to CNBA stats.txt")
    parser.add_argument(
        "--ablation", type=str, help="Path to ablation (no-merge) stats.txt"
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default="plots/",
        help="Output directory for plots",
    )
    parser.add_argument(
        "--format",
        type=str,
        default="pdf",
        choices=["pdf", "png", "svg"],
        help="Output image format",
    )

    args = parser.parse_args()
    os.makedirs(args.output_dir, exist_ok=True)

    # 如果提供了 stats 文件，解析并绘图
    sys.path.insert(0, os.path.dirname(__file__))
    from parse_stats import StatsParser

    if args.baseline and args.cnba:
        bp = StatsParser()
        bp.parse_file(args.baseline)
        base_stats = bp.get_bridge_stats()

        cp = StatsParser()
        cp.parse_file(args.cnba)
        cnba_stats = cp.get_bridge_stats()

        ext = args.format
        plot_latency_comparison(
            base_stats,
            cnba_stats,
            os.path.join(args.output_dir, f"latency_compare.{ext}"),
        )
        plot_throughput_comparison(
            base_stats,
            cnba_stats,
            os.path.join(args.output_dir, f"throughput_compare.{ext}"),
        )

        # 请求放大率
        amp_data = {
            "Baseline": {
                "axi_reqs": base_stats.get("totalAxiRequests", 0),
                "chi_reqs": base_stats.get("totalChiRequests", 0),
            },
            "CNBA": {
                "axi_reqs": cnba_stats.get("totalAxiRequests", 0),
                "chi_reqs": cnba_stats.get("totalChiRequests", 0),
            },
        }
        plot_request_amplification(
            amp_data, os.path.join(args.output_dir, f"req_amplification.{ext}")
        )

    # 消融实验
    if args.baseline and args.cnba and args.ablation:
        ap = StatsParser()
        ap.parse_file(args.ablation)
        ab_stats = ap.get_bridge_stats()

        ablation_data = {
            "Baseline (CF'20)": base_stats,
            "CNBA No-Merge": ab_stats,
            "CNBA Full": cnba_stats,
        }
        plot_ablation_study(
            ablation_data,
            os.path.join(args.output_dir, f"ablation_study.{ext}"),
        )

    print(f"\nAll plots saved to: {args.output_dir}")
