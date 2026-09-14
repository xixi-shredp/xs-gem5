"""Audit simulation evidence without running or repairing an experiment."""

import importlib.util
import json
import math
from pathlib import Path
import re

from .model import address, empty_counters


def legacy():
    spec = importlib.util.spec_from_file_location(
        "h2p_detection_runner", Path(__file__).resolve().parents[1] / "run-cpython-h2p.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def stats_values(path):
    """All numeric columns, including percentages and NaN/Inf, once per dump."""
    values = {}
    for line in Path(path).read_text().splitlines():
        content = line.split("#", 1)[0].split()
        if not content or content[0].startswith("-"):
            continue
        name = content[0]
        if name.startswith("host") or ".h2pHint." in name:
            continue
        numeric = []
        for token in content[1:]:
            try:
                value = float(token.rstrip("%"))
            except ValueError:
                break
            numeric.append("nan" if math.isnan(value) else value)
        if numeric:
            if name in values:
                raise ValueError(f"multiple performance dumps for {name}")
            values[name] = numeric
    return values


def compare_observer(baseline, observed):
    left, right = stats_values(Path(baseline) / "stats.txt"), stats_values(Path(observed) / "stats.txt")
    differences = [{"stat": name, "off": left.get(name), "collect": right.get(name)}
                   for name in sorted(left.keys() | right.keys())
                   if left.get(name) != right.get(name)]
    h2p_equal = ((Path(baseline) / "h2p-indirect.txt").read_bytes() ==
                 (Path(observed) / "h2p-indirect.txt").read_bytes())
    return {"pass": not differences and h2p_equal,
            "numeric_statistics": len(left), "differences": differences,
            "h2p_aggregate_equal": h2p_equal}


def validate_hint_stats(data, total_misses, mode, candidates):
    if (data.get("format") != "h2p-hint-stats" or data.get("version") != 2 or
            data.get("mode") != mode):
        raise ValueError("invalid Hint statistics identity")
    rows = [data["global"], *data["pcs"]]
    for row in rows:
        for name in empty_counters():
            if type(row.get(name)) is not int or row[name] < 0:
                raise ValueError(f"invalid Hint counter {name}")
        if (row["baseline_misses"] - row["final_misses"] !=
                row["corrected"] - row["harmed"]):
            raise ValueError("Hint corrected/harmed conservation failed")
        if not (row["overrides"] <= row["conf_enabled"] <= row["tag_hits"] <=
                row["eligible"] <= row["executions"]):
            raise ValueError("Hint coverage counters are inconsistent")
        if max(row["baseline_misses"], row["final_misses"]) > row["executions"]:
            raise ValueError("Hint misses exceed executions")
    pcs = [address(row["pc"]) for row in data["pcs"]]
    if len(pcs) != len(set(pcs)) or not set(candidates) <= set(pcs):
        raise ValueError("missing or duplicate Hint per-PC counters")
    if data["global"]["final_misses"] != total_misses:
        raise ValueError("global Hint final misses differ from H2P aggregate")
    queue = data["queue"]
    for name in ("enqueued", "applied", "dropped", "pending", "carried_in",
                 "zero_transitions", "recoveries", "max_occupancy"):
        if type(queue.get(name, 0)) is not int or queue.get(name, 0) < 0:
            raise ValueError("invalid confidence queue counter")
    if queue.get("carried_in", 0) + queue["enqueued"] != queue["applied"] + queue["pending"]:
        raise ValueError("confidence queue conservation failed")
    return data


def audit_run(output, returncode, warmup, measure, interval, mode, candidates):
    output = Path(output)
    checker = legacy()
    errors, sim_insts, records = checker.validate(output, returncode, warmup, measure, interval)
    stats = None
    total = 0
    try:
        parsed = checker.parse_h2p_output(output / "h2p-indirect.txt")
        total = sum(row[0] for row in parsed.values())
        if mode != "off":
            stats = validate_hint_stats(json.loads((output / "hint-stats.json").read_text()),
                                        total, mode, candidates)
            if not (output / "conf0.txt").is_file():
                raise ValueError("missing conf0.txt")
            transitions = sum(bool(line.strip()) for line in
                              (output / "conf0.txt").read_text().splitlines())
            if transitions != stats["queue"]["zero_transitions"]:
                raise ValueError("conf0 lines differ from actual confidence transitions")
        if mode == "collect" and not (output / "hint-profile.jsonl.gz").is_file():
            raise ValueError("missing collect profile")
        config = (output / "config.ini").read_text()
        if f"hintMode={mode}\n" not in config:
            raise ValueError("Hint configuration mode mismatch")
        command = (output / "cmd.txt").read_text()
        if "enable_difftest=true" in config or "--mem-type=DDR4_2400_8x8" not in command:
            raise ValueError("difftest or memory configuration differs")
        if "--raw-cpt" in command:
            raise ValueError("zstd checkpoint was run as raw memory")
    except (OSError, KeyError, ValueError) as error:
        errors.append(str(error))
    phases = {}
    try:
        phases = checker.read_phases((output / "stdout.log").read_text() +
                                     (output / "stderr.log").read_text())
    except (OSError, ValueError) as error:
        errors.append(str(error))
    numeric = stats_values(output / "stats.txt") if (output / "stats.txt").exists() else {}
    return {"result": "pass" if not errors else "fail", "errors": errors,
            "returncode": returncode, "sim_insts": sim_insts,
            "h2p_pc_count": records, "final_misses": total,
            "window_count": len(range(0, measure, interval)) if interval else 0,
            "phases": phases, "sim_ticks": numeric.get("simTicks", [None])[0],
            "ipc": numeric.get("system.cpu.ipc", [None])[0],
            "hint": stats}
