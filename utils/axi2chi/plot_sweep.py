#!/usr/bin/env python3
"""
AXI2CHI Bridge 参数扫描折线图

生成:
  1. 性能 vs 内存延迟 折线图 (4 指标 × 3 桥)
  2. 性能 vs TxnID 数量 折线图 (4 指标 × 3 桥)

用法:
  python3 utils/axi2chi/plot_sweep.py \
      --base-dir m5out \
      --output-dir m5out/plots
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

MODES = ["naive", "baseline", "cnba"]
STYLE = {
    "naive": {"color": "#E57373", "marker": "o", "label": "Naive Bridge"},
    "baseline": {
        "color": "#64B5F6",
        "marker": "s",
        "label": "Baseline (CF'20)",
    },
    "cnba": {"color": "#81C784", "marker": "D", "label": "CNBA (Ours)"},
}

METRICS = [
    (
        "throughput",
        "Throughput (GB/s)",
        lambda v: v.get("throughput", 0) / 1e9,
    ),
    (
        "chi_axi_ratio",
        "CHI/AXI Request Ratio",
        lambda v: v.get("totalChiRequests", 0)
        / max(v.get("totalAxiRequests", 1), 1),
    ),
    (
        "robStallCycles",
        "TxnID Stall Cycles",
        lambda v: v.get("robStallCycles", 0),
    ),
    (
        "simTicks",
        "Simulation Time (M ticks)",
        lambda v: v.get("sim_ticks", 0) / 1e6 if v.get("sim_ticks") else 0,
    ),
]


def load_stats(stats_path):
    """Parse a stats.txt and return merged bridge + global stats."""
    p = StatsParser()
    raw = p.parse_file(stats_path)
    if raw is None:
        return None
    bridge = p.get_bridge_stats()
    # Also carry sim_ticks from global stats
    if "sim_ticks" in p.stats:
        bridge["sim_ticks"] = p.stats["sim_ticks"]
    return bridge


def collect_sweep(base_dir, prefix, param_values, param_fmt):
    """Collect stats for a parameter sweep.

    Returns: {mode: [(param_val, stats_dict), ...]}
    """
    data = {m: [] for m in MODES}
    for val in param_values:
        for mode in MODES:
            dirname = param_fmt(mode, val)
            stats_path = os.path.join(base_dir, dirname, "stats.txt")
            stats = load_stats(stats_path)
            if stats:
                data[mode].append((val, stats))
            else:
                print(f"  Warning: missing {stats_path}")
    return data


def plot_sweep_panel(data, x_values, xlabel, title_prefix, output_path):
    """Draw a 2x2 panel of line plots for one sweep dimension."""
    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    fig.suptitle(
        f"{title_prefix}\nNaive vs Baseline (CF'20) vs CNBA",
        fontsize=14,
        fontweight="bold",
        y=0.98,
    )

    panels = [
        (axes[0, 0], "(a)"),
        (axes[0, 1], "(b)"),
        (axes[1, 0], "(c)"),
        (axes[1, 1], "(d)"),
    ]

    for (ax, panel_label), (metric_key, ylabel, extractor) in zip(
        panels, METRICS
    ):
        for mode in MODES:
            s = STYLE[mode]
            points = data[mode]
            xs = [p[0] for p in points]
            ys = [extractor(p[1]) for p in points]
            ax.plot(
                xs,
                ys,
                marker=s["marker"],
                color=s["color"],
                label=s["label"],
                linewidth=2,
                markersize=7,
            )
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        ax.set_title(f"{panel_label} {ylabel}")
        ax.legend(loc="best", framealpha=0.9)

    plt.tight_layout(rect=[0, 0, 1, 0.93])
    plt.savefig(output_path)
    plt.close()
    print(f"  Saved: {output_path}")


def plot_sweep_individual(data, x_values, xlabel, tag, output_dir):
    """Draw individual line plots per metric for one sweep dimension."""
    for metric_key, ylabel, extractor in METRICS:
        fig, ax = plt.subplots(figsize=(8, 5))
        for mode in MODES:
            s = STYLE[mode]
            points = data[mode]
            xs = [p[0] for p in points]
            ys = [extractor(p[1]) for p in points]
            ax.plot(
                xs,
                ys,
                marker=s["marker"],
                color=s["color"],
                label=s["label"],
                linewidth=2,
                markersize=7,
            )
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        ax.set_title(f"{ylabel} vs {xlabel}")
        ax.legend(loc="best", framealpha=0.9)
        plt.tight_layout()
        fname = f"{tag}_{metric_key}.png"
        plt.savefig(os.path.join(output_dir, fname))
        plt.close()
        print(f"  Saved: {fname}")


def main():
    parser = argparse.ArgumentParser(description="AXI2CHI sweep line plots")
    parser.add_argument(
        "--base-dir",
        default="m5out",
        help="Base directory containing experiment output folders",
    )
    parser.add_argument(
        "--output-dir",
        default="m5out/plots",
        help="Output directory for plots",
    )
    parser.add_argument(
        "--prefix",
        default="",
        help="Directory name prefix for sweep experiments (e.g. 'conv_' for conv_exp5_*/conv_exp6_*)",
    )
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    pfx = args.prefix

    # ========== Memory Latency Sweep ==========
    lat_ns_values = [10, 20, 40, 80, 160]
    lat_labels = ["10ns", "20ns", "40ns", "80ns", "160ns"]

    print("Collecting memory latency sweep data...")
    lat_data = collect_sweep(
        args.base_dir,
        f"{pfx}exp5",
        param_values=lat_labels,
        param_fmt=lambda mode, val: f"{pfx}exp5_{mode}_{val}",
    )

    # Convert x-axis to numeric ns for proper scaling
    lat_num = {m: [] for m in MODES}
    for mode in MODES:
        for lab, ns in zip(lat_labels, lat_ns_values):
            for val_label, stats in lat_data[mode]:
                if val_label == lab:
                    lat_num[mode].append((ns, stats))
                    break

    print("Generating memory latency sweep plots...")
    plot_sweep_panel(
        lat_num,
        lat_ns_values,
        "Memory Latency (ns)",
        "Performance vs Memory Latency",
        os.path.join(args.output_dir, "07_memlat_sweep_panel.png"),
    )
    plot_sweep_individual(
        lat_num,
        lat_ns_values,
        "Memory Latency (ns)",
        "memlat",
        args.output_dir,
    )

    # ========== TxnID Sweep ==========
    tid_values = [8, 16, 32, 64, 128]

    print("Collecting TxnID sweep data...")
    tid_data = collect_sweep(
        args.base_dir,
        f"{pfx}exp6",
        param_values=tid_values,
        param_fmt=lambda mode, val: f"{pfx}exp6_{mode}_tid{val}",
    )

    print("Generating TxnID sweep plots...")
    plot_sweep_panel(
        tid_data,
        tid_values,
        "Max TxnID",
        "Performance vs TxnID Count",
        os.path.join(args.output_dir, "08_txnid_sweep_panel.png"),
    )
    plot_sweep_individual(
        tid_data, tid_values, "Max TxnID", "txnid", args.output_dir
    )

    print(f"\nDone! Sweep plots saved to {args.output_dir}/")


if __name__ == "__main__":
    main()
