#!/usr/bin/env python3
"""
gem5 AXI2CHI Bridge Statistics Parser

解析 gem5 输出的 stats.txt，提取 AXI2CHI Bridge 相关统计信息。

用法:
  python parse_stats.py m5out/stats.txt
  python parse_stats.py --compare m5out/baseline/stats.txt m5out/cnba/stats.txt
"""

import argparse
import os
import re
import sys
from collections import OrderedDict


class StatsParser:
    """gem5 stats.txt 解析器"""

    # AXI2CHI Bridge 相关的统计指标
    BRIDGE_STATS = [
        "totalAxiRequests",
        "totalChiRequests",
        "mergeCount",
        "splitCount",
        "avgLatency",
        "totalLatency",
        "totalBytes",
        "throughput",
        "robStallCycles",
        "totalReads",
        "totalWrites",
        "mergeRate",
    ]

    def __init__(self):
        self.stats = OrderedDict()

    def parse_file(self, filepath):
        """解析单个 stats.txt 文件"""
        if not os.path.exists(filepath):
            print(f"Error: {filepath} not found")
            return None

        stats = OrderedDict()
        bridge_prefix = "system.bridge."

        with open(filepath) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("---") or line.startswith("#"):
                    continue

                # 解析 key value 对
                parts = line.split()
                if len(parts) >= 2:
                    key = parts[0]
                    value = parts[1]

                    # 提取 bridge 相关统计
                    if bridge_prefix in key:
                        stat_name = key.replace(bridge_prefix, "")
                        try:
                            stats[stat_name] = float(value)
                        except ValueError:
                            stats[stat_name] = value

                    # 也提取全局统计
                    if key in (
                        "sim_seconds",
                        "sim_ticks",
                        "sim_insts",
                        "host_seconds",
                        "host_tick_rate",
                    ):
                        try:
                            stats[key] = float(value)
                        except ValueError:
                            stats[key] = value

        self.stats = stats
        return stats

    def get_bridge_stats(self):
        """提取桥接器核心指标"""
        result = OrderedDict()
        for key in self.BRIDGE_STATS:
            if key in self.stats:
                result[key] = self.stats[key]
        return result

    def print_summary(self, label=""):
        """打印统计摘要"""
        bridge = self.get_bridge_stats()

        print(f"\n{'='*60}")
        if label:
            print(f"  {label}")
            print(f"{'='*60}")

        if not bridge:
            print("  No AXI2CHI bridge statistics found.")
            return

        print(f"  {'Metric':<25} {'Value':>15}")
        print(f"  {'-'*25} {'-'*15}")

        for key, val in bridge.items():
            if isinstance(val, float):
                if val > 1e6:
                    print(f"  {key:<25} {val:>15.2f}")
                elif val < 0.01 and val != 0:
                    print(f"  {key:<25} {val:>15.6f}")
                else:
                    print(f"  {key:<25} {val:>15.4f}")
            else:
                print(f"  {key:<25} {str(val):>15}")

        # 派生指标
        axi_req = bridge.get("totalAxiRequests", 0)
        chi_req = bridge.get("totalChiRequests", 0)
        if axi_req > 0 and chi_req > 0:
            ratio = chi_req / axi_req
            print(f"\n  --- Derived Metrics ---")
            print(f"  {'CHI/AXI Ratio':<25} {ratio:>15.4f}")
            print(f"  {'Req Amplification':<25} {ratio:>15.4f}x")

        merges = bridge.get("mergeCount", 0)
        splits = bridge.get("splitCount", 0)
        if axi_req > 0:
            print(f"  {'Merge Rate':<25} " f"{merges/axi_req*100:>14.2f}%")
            print(f"  {'Split Rate':<25} " f"{splits/axi_req*100:>14.2f}%")

        stalls = bridge.get("robStallCycles", 0)
        if stalls > 0:
            print(f"  {'Stall Cycles':<25} {stalls:>15.0f}")

        print(f"{'='*60}\n")


def compare_stats(files, labels=None):
    """对比多个实验的统计结果

    典型用法: 对比 baseline vs CNBA
    """
    parsers = []
    for i, f in enumerate(files):
        p = StatsParser()
        p.parse_file(f)
        label = labels[i] if labels else os.path.basename(os.path.dirname(f))
        p.print_summary(label)
        parsers.append(p)

    # 对比表
    if len(parsers) >= 2:
        print(f"\n{'='*70}")
        print(
            f"  Comparison: {labels[0] if labels else 'exp1'} "
            f"vs {labels[1] if labels else 'exp2'}"
        )
        print(f"{'='*70}")

        stats0 = parsers[0].get_bridge_stats()
        stats1 = parsers[1].get_bridge_stats()

        print(
            f"  {'Metric':<25} {'Baseline':>12} {'CNBA':>12} "
            f"{'Improvement':>12}"
        )
        print(f"  {'-'*25} {'-'*12} {'-'*12} {'-'*12}")

        for key in StatsParser.BRIDGE_STATS:
            v0 = stats0.get(key, 0)
            v1 = stats1.get(key, 0)
            if isinstance(v0, (int, float)) and isinstance(v1, (int, float)):
                if v0 != 0:
                    improvement = (v0 - v1) / v0 * 100
                    print(
                        f"  {key:<25} {v0:>12.2f} {v1:>12.2f} "
                        f"{improvement:>11.2f}%"
                    )
                else:
                    print(
                        f"  {key:<25} {v0:>12.2f} {v1:>12.2f} " f"{'N/A':>12}"
                    )

        print(f"{'='*70}\n")


def export_csv(filepath, stats_dict, label=""):
    """导出统计到 CSV 文件"""
    import csv

    with open(filepath, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["label", "metric", "value"])
        for key, val in stats_dict.items():
            writer.writerow([label, key, val])

    print(f"Exported CSV: {filepath}")


# ==================================================================
# 主函数
# ==================================================================

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Parse gem5 stats for AXI2CHI bridge experiments"
    )
    parser.add_argument(
        "stats_files", nargs="+", help="Path(s) to stats.txt file(s)"
    )
    parser.add_argument(
        "--compare",
        action="store_true",
        help="Compare mode: provide exactly 2 stats files",
    )
    parser.add_argument(
        "--labels",
        type=str,
        default=None,
        help="Comma-separated labels for comparison "
        "(e.g., 'Baseline,CNBA')",
    )
    parser.add_argument(
        "--csv", type=str, default=None, help="Export results to CSV file"
    )

    args = parser.parse_args()

    labels = args.labels.split(",") if args.labels else None

    if args.compare and len(args.stats_files) >= 2:
        compare_stats(args.stats_files, labels)
    else:
        for i, f in enumerate(args.stats_files):
            p = StatsParser()
            p.parse_file(f)
            label = labels[i] if labels and i < len(labels) else f
            p.print_summary(label)

            if args.csv:
                base = os.path.splitext(args.csv)[0]
                export_csv(f"{base}_{i}.csv", p.get_bridge_stats(), label)
