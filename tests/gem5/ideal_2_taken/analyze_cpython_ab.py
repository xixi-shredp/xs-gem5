#!/usr/bin/env python3
"""Analyze completed CPython baseline/ideal-2-taken SimPoint runs."""

import argparse
import csv
import json
import math
from pathlib import Path
import re


STAT_RE = re.compile(
    r"^(\S+)\s+(-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?|nan|inf)\s"
)
PREFIX = "system.cpu.branchPred."
METRICS = (
    "ipc", "branch_mpki", "cond_miss_rate", "fetch_ipc", "icache_mpki",
    "control_squash_mpki", "recovery_slots_pki", "fetch_bubbles_pki",
    "decode_bubbles_pki", "fsq_mean", "fsq_full_per_mcycle",
    "ftq_full_per_mcycle", "redirect_pending_cycle_pct",
    "two_query_cycle_pct", "two_enqueue_cycle_pct", "two_taken_cycle_pct",
    "second_slot_queries_per_kcycle", "second_slot_bubble_stops_per_kcycle",
    "second_slot_ftq_stops_per_kcycle",
)


def stats(path):
    result = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = STAT_RE.match(line)
        if match:
            try:
                result[match.group(1)] = float(match.group(2))
            except ValueError:
                pass
    return result


def div(num, den):
    return num / den if den else math.nan


def value(data, name):
    return data.get(name, 0.0)


def dist_pct(data, name, bucket):
    return 100.0 * div(
        value(data, f"{PREFIX}{name}::{bucket}"),
        value(data, f"{PREFIX}{name}::samples"),
    )


def extract(data):
    insts = value(data, "simInsts")
    cycles = value(data, "system.cpu.numCycles")
    branch_misses = sum(value(data, PREFIX + name) for name in (
        "condMiss", "uncondMiss", "returnMiss", "otherMiss"
    ))
    result = {
        "sim_insts": insts,
        "cycles": cycles,
        "ipc": div(insts, cycles),
        "branch_mpki": 1000.0 * div(branch_misses, insts),
        "cond_miss_rate": 100.0 * div(
            value(data, PREFIX + "condMiss"), value(data, PREFIX + "condNum")
        ),
        "fetch_ipc": div(value(data, "system.cpu.fetch.insts"), cycles),
        "icache_mpki": 1000.0 * div(
            value(data, "system.cpu.icache.demandMisses::cpu.inst"), insts
        ),
        "control_squash_mpki": 1000.0 * div(
            value(data, "system.cpu.commit.squashDueToBranch"), insts
        ),
        "recovery_slots_pki": 1000.0 * div(
            value(data, "system.cpu.commit.recovery_bubble"), insts
        ),
        "fetch_bubbles_pki": 1000.0 * div(
            value(data, "system.cpu.fetch.fetchBubbles"), insts
        ),
        "decode_bubbles_pki": 1000.0 * div(
            value(data, "system.cpu.decode.decodeBubbles"), insts
        ),
        "fsq_mean": value(data, PREFIX + "fsqEntryDist::mean"),
        "fsq_full_per_mcycle": 1e6 * div(
            value(data, PREFIX + "fsqFullCannotEnq"), cycles
        ),
        "ftq_full_per_mcycle": 1e6 * div(
            value(data, PREFIX + "ftqFullCannotEnq"), cycles
        ),
        "redirect_pending_cycle_pct": 100.0 * div(
            value(data, "system.cpu.fetch.redirectPendingOnlyFetchCycles"), cycles
        ),
        "two_query_cycle_pct": dist_pct(
            data, "idealTwoTakenQueriesPerCycle", "2"
        ),
        "two_enqueue_cycle_pct": dist_pct(
            data, "idealTwoTakenEnqueuesPerCycle", "2"
        ),
        "two_taken_cycle_pct": dist_pct(
            data, "idealTwoTakenTakenPerCycle", "2"
        ),
        "second_slot_queries_per_kcycle": 1000.0 * div(
            value(data, PREFIX + "idealTwoTakenSecondSlotQueries"), cycles
        ),
        "second_slot_bubble_stops_per_kcycle": 1000.0 * div(
            value(data, PREFIX + "idealTwoTakenSecondSlotBubbleStops"), cycles
        ),
        "second_slot_ftq_stops_per_kcycle": 1000.0 * div(
            value(data, PREFIX + "idealTwoTakenSecondSlotFTQStops"), cycles
        ),
    }
    return result


def read_weights(workload_root, bench, scale, cpt_ids):
    directory = workload_root / "cpts" / "xsgem5" / f"{bench}_{scale}"
    points = [line.split() for line in (directory / "simpoints0").read_text().splitlines()]
    weights = [line.split() for line in (directory / "weights0").read_text().splitlines()]
    by_cluster = {cluster: float(weight) for weight, cluster in weights}
    result = {point: by_cluster[cluster] for point, cluster in points}
    missing = set(cpt_ids) - set(result)
    extra = set(result) - set(cpt_ids)
    if missing or extra:
        raise RuntimeError(f"{bench}: weight mismatch missing={missing} extra={extra}")
    total = sum(result.values())
    return {key: val / total for key, val in result.items()}


def weighted(rows, config, metric):
    vals = [(row["weight"], row[f"{config}_{metric}"]) for row in rows]
    vals = [(weight, val) for weight, val in vals if math.isfinite(val)]
    total = sum(weight for weight, _ in vals)
    return sum(weight * val for weight, val in vals) / total if total else math.nan


def load_rows(args):
    spec = json.loads(args.workload_list.read_text(encoding="utf-8"))
    workload_root = Path(spec["workload_root"])
    rows = []
    for group in spec["cpts"]:
        bench, scale = group["bench"], group["scale"]
        cpt_ids = [str(item) for item in group["cpt_list"]]
        weights = read_weights(workload_root, bench, scale, cpt_ids)
        for cpt_id in cpt_ids:
            row = {"benchmark": bench, "scale": scale, "cpt_id": cpt_id,
                   "weight": weights[cpt_id]}
            for config in ("baseline", "ideal2"):
                output = args.output_root / f"{bench}_{scale}" / cpt_id / config
                status_path = output / "status.json"
                if not status_path.is_file():
                    raise RuntimeError(f"missing status: {status_path}")
                status = json.loads(status_path.read_text(encoding="utf-8"))
                if status.get("result") != "pass":
                    raise RuntimeError(f"non-passing run: {status_path}")
                metrics = extract(stats(output / "stats.txt"))
                for name, val in metrics.items():
                    row[f"{config}_{name}"] = val
            for metric in METRICS:
                base, ideal = row[f"baseline_{metric}"], row[f"ideal2_{metric}"]
                row[f"delta_{metric}_pct"] = 100.0 * div(ideal - base, base)
            row["speedup"] = div(row["baseline_cycles"], row["ideal2_cycles"])
            rows.append(row)
    return rows


def benchmark_rows(rows):
    result = []
    for bench in dict.fromkeys(row["benchmark"] for row in rows):
        selected = [row for row in rows if row["benchmark"] == bench]
        item = {"benchmark": bench, "slices": len(selected)}
        base_cycles = weighted(selected, "baseline", "cycles")
        ideal_cycles = weighted(selected, "ideal2", "cycles")
        insts = weighted(selected, "baseline", "sim_insts")
        item["baseline_ipc"] = div(insts, base_cycles)
        item["ideal2_ipc"] = div(insts, ideal_cycles)
        item["speedup"] = div(base_cycles, ideal_cycles)
        item["ipc_change_pct"] = 100.0 * (item["speedup"] - 1.0)
        for metric in METRICS:
            if metric == "ipc":
                continue
            base = weighted(selected, "baseline", metric)
            ideal = weighted(selected, "ideal2", metric)
            item[f"baseline_{metric}"] = base
            item[f"ideal2_{metric}"] = ideal
            item[f"delta_{metric}_pct"] = 100.0 * div(ideal - base, base)
        result.append(item)
    return result


def write_csv(path, rows):
    fields = list(rows[0]) if rows else []
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def fmt(value, digits=4):
    return "n/a" if not math.isfinite(value) else f"{value:.{digits}f}"


def arithmetic_mean(benches, field):
    values = [row[field] for row in benches if math.isfinite(row[field])]
    return sum(values) / len(values) if values else math.nan


def report(benches, output):
    geomean = math.exp(sum(math.log(row["speedup"]) for row in benches) / len(benches))
    lines = [
        "# CPython ideal 2-taken performance",
        "",
        f"All {sum(row['slices'] for row in benches)} SimPoint slices passed in both configurations.",
        f"Equal-benchmark geometric-mean speedup: **{geomean:.4f}x "
        f"({100 * (geomean - 1):+.2f}%)**.",
        "",
        "| benchmark | slices | baseline IPC | ideal2 IPC | speedup | branch MPKI Δ | I-cache MPKI Δ |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in benches:
        lines.append(
            f"| {row['benchmark']} | {row['slices']} | {fmt(row['baseline_ipc'])} | "
            f"{fmt(row['ideal2_ipc'])} | {fmt(row['speedup'])}x | "
            f"{fmt(row['delta_branch_mpki_pct'], 2)}% | "
            f"{fmt(row['delta_icache_mpki_pct'], 2)}% |"
        )

    lines += [
        "",
        "## Equal-benchmark frontend averages",
        "",
        "| metric | baseline | ideal2 | change |",
        "|---|---:|---:|---:|",
    ]
    aggregate_metrics = (
        ("Branch MPKI", "branch_mpki"),
        ("Conditional miss rate (%)", "cond_miss_rate"),
        ("Fetch IPC", "fetch_ipc"),
        ("I-cache MPKI", "icache_mpki"),
        ("Control squash MPKI", "control_squash_mpki"),
        ("Recovery slots / ki", "recovery_slots_pki"),
        ("Fetch bubbles / ki", "fetch_bubbles_pki"),
        ("Decode bubbles / ki", "decode_bubbles_pki"),
        ("Mean FSQ occupancy", "fsq_mean"),
        ("Redirect-pending cycles (%)", "redirect_pending_cycle_pct"),
    )
    for label, metric in aggregate_metrics:
        baseline = arithmetic_mean(benches, f"baseline_{metric}")
        ideal = arithmetic_mean(benches, f"ideal2_{metric}")
        change = 100.0 * div(ideal - baseline, baseline)
        lines.append(
            f"| {label} | {fmt(baseline)} | {fmt(ideal)} | "
            f"{fmt(change, 2)}% |"
        )

    lines += [
        "",
        "## Ideal-mode utilization",
        "",
        "| metric | equal-benchmark average |",
        "|---|---:|",
    ]
    utilization_metrics = (
        ("Cycles with two predictor queries (%)", "two_query_cycle_pct"),
        ("Cycles with two enqueued blocks (%)", "two_enqueue_cycle_pct"),
        ("Cycles with two taken targets (%)", "two_taken_cycle_pct"),
        ("Second-slot queries / kcycle", "second_slot_queries_per_kcycle"),
        ("Second-slot bubble stops / kcycle", "second_slot_bubble_stops_per_kcycle"),
        ("Second-slot FTQ stops / kcycle", "second_slot_ftq_stops_per_kcycle"),
    )
    for label, metric in utilization_metrics:
        value = arithmetic_mean(benches, f"ideal2_{metric}")
        lines.append(f"| {label} | {fmt(value)} |")
    lines += [
        "",
        "IPC is computed from SimPoint-weighted cycles for the fixed instruction window; "
        "the suite headline is an equal-benchmark geometric mean. Other suite-level "
        "figures above are arithmetic means of the eight benchmark-level weighted "
        "metrics. Detailed values are in `benchmark_summary.csv` and "
        "`slice_metrics.csv`.",
        "",
    ]
    output.write_text("\n".join(lines), encoding="utf-8")
    return geomean


def json_finite(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {key: json_finite(item) for key, item in value.items()}
    if isinstance(value, list):
        return [json_finite(item) for item in value]
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workload-list", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    rows = load_rows(args)
    benches = benchmark_rows(rows)
    write_csv(args.output_root / "slice_metrics.csv", rows)
    write_csv(args.output_root / "benchmark_summary.csv", benches)
    geomean = report(benches, args.output_root / "report.md")
    summary = {"schema_version": 1, "slice_count": len(rows),
               "benchmark_count": len(benches), "geomean_speedup": geomean,
               "benchmarks": benches}
    (args.output_root / "summary.json").write_text(
        json.dumps(json_finite(summary), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"analyzed {len(rows)} slices; geomean speedup {geomean:.6f}x")


if __name__ == "__main__":
    main()
