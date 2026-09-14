#!/usr/bin/env python3
"""Audit completed CPython ideal-2-taken A/B runs."""

import argparse
import collections
import importlib.util
import json
import re
from pathlib import Path


EXPECTED_EXIT = "because a thread reached the max instruction count"


def load_stats_checker(path):
    spec = importlib.util.spec_from_file_location("ideal2_check_stats", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument(
        "--stats-checker",
        type=Path,
        default=Path(__file__).with_name("check_stats.py"),
    )
    args = parser.parse_args()
    checker = load_stats_checker(args.stats_checker)

    paths = sorted(args.output_root.glob("*/*/*/status.json"))
    errors = []
    pairs = collections.defaultdict(dict)
    sim_insts = []
    hashes = collections.defaultdict(set)
    ideal_second_slot = []
    baseline_second_slot = []

    for path in paths:
        status = json.loads(path.read_text(encoding="utf-8"))
        identity = status.get("identity", {})
        config = identity.get("config_name")
        key = (
            identity.get("bench"),
            identity.get("scale"),
            identity.get("cpt_id"),
        )
        pairs[key][config] = (path, status)
        sim_insts.append(float(status.get("sim_insts", 0)))
        for field in (
            "gem5_sha256",
            "config_sha256",
            "workload_list_sha256",
        ):
            hashes[field].add(identity.get(field))

        ideal = config == "ideal2"
        if (
            status.get("result") != "pass"
            or status.get("returncode") != 0
            or status.get("errors")
        ):
            errors.append(f"bad status: {path}")
        if (
            identity.get("ideal_two_taken") is not ideal
            or identity.get("maxinsts") != 50_000_000
            or identity.get("warmup_insts_no_switch") != 0
            or identity.get("difftest_enabled") is not False
        ):
            errors.append(f"bad identity: {path}")

        config_ini = (path.parent / "config.ini").read_text(
            encoding="utf-8", errors="replace"
        )
        expected_flag = "idealTwoTaken=" + ("true" if ideal else "false")
        if expected_flag not in config_ini:
            errors.append(f"bad config flag: {path}")

        stdout = (path.parent / "stdout.log").read_text(
            encoding="utf-8", errors="replace"
        )
        stderr = (path.parent / "stderr.log").read_text(
            encoding="utf-8", errors="replace"
        )
        if EXPECTED_EXIT not in stdout:
            errors.append(f"bad exit cause: {path}")
        if re.search(r"kernel panic|\bOops\b", stdout + stderr, re.IGNORECASE):
            errors.append(f"panic/oops: {path}")

        values = checker.read_stats(path.parent / "stats.txt")
        second_slot = checker.matching_stats(
            values, "idealTwoTakenSecondSlotQueries"
        )
        second_slot_count = max(second_slot.values(), default=0)
        if ideal:
            stat_errors = checker.validate(
                values,
                require_second_slot=True,
                require_two_enqueues=True,
            )
            if stat_errors:
                errors.append(f"invalid ideal stats {path}: {stat_errors}")
            ideal_second_slot.append(second_slot_count)
        else:
            baseline_second_slot.append(second_slot_count)

    for key, modes in pairs.items():
        if set(modes) != {"baseline", "ideal2"}:
            errors.append(f"incomplete pair {key}: {set(modes)}")
            continue
        baseline = modes["baseline"][1]["identity"]
        ideal = modes["ideal2"][1]["identity"]
        for field in (
            "checkpoint",
            "checkpoint_size",
            "checkpoint_mtime_ns",
            "gem5_sha256",
            "config_sha256",
            "workload_list_sha256",
            "maxinsts",
            "warmup_insts_no_switch",
            "difftest_enabled",
        ):
            if baseline.get(field) != ideal.get(field):
                errors.append(f"pair mismatch {key}: {field}")

    result = {
        "status_count": len(paths),
        "pair_count": len(pairs),
        "errors": errors,
        "sim_insts_min": min(sim_insts, default=None),
        "sim_insts_max": max(sim_insts, default=None),
        "hashes": {key: sorted(value) for key, value in hashes.items()},
        "ideal_runs_checked": len(ideal_second_slot),
        "ideal_second_slot_min": min(ideal_second_slot, default=None),
        "baseline_second_slot_max": max(baseline_second_slot, default=None),
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
