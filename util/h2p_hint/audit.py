"""Audit simulation evidence without running or repairing an experiment."""

import importlib.util
import gzip
import json
import math
from pathlib import Path
import re
import shlex

from .model import address, empty_counters, sha256, validate_metadata


def legacy():
    spec = importlib.util.spec_from_file_location(
        "h2p_detection_runner",
        Path(__file__).resolve().parents[1] / "run-cpython-h2p.py",
    )
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


def compare_observer(baseline, observed, check_windows=False):
    left, right = stats_values(Path(baseline) / "stats.txt"), stats_values(
        Path(observed) / "stats.txt"
    )
    differences = [
        {"stat": name, "off": left.get(name), "collect": right.get(name)}
        for name in sorted(left.keys() | right.keys())
        if left.get(name) != right.get(name)
    ]
    h2p_equal = (Path(baseline) / "h2p-indirect.txt").read_bytes() == (
        Path(observed) / "h2p-indirect.txt"
    ).read_bytes()
    different_windows = []
    if check_windows:
        for offset in range(0, 50_000_000, 1_000_000):
            name = f"{offset}.txt"
            if (Path(baseline) / "h2p-indirec" / name).read_bytes() != (
                Path(observed) / "h2p-indirec" / name
            ).read_bytes():
                different_windows.append(name)
    return {
        "pass": not differences and h2p_equal and not different_windows,
        "numeric_statistics": len(left),
        "differences": differences,
        "h2p_aggregate_equal": h2p_equal,
        "checked_windows": 50 if check_windows else 0,
        "different_windows": different_windows,
    }


def validate_hint_stats(data, total_misses, mode, candidates, ideal=False, finite_v3=False):
    if (
        data.get("format") != "h2p-hint-stats"
        or data.get("version") != (7 if finite_v3 else 3 if ideal else 2)
        or data.get("mode") != mode
    ):
        raise ValueError("invalid Hint statistics identity")
    if finite_v3:
        from .finite_v3 import contract
        if data.get("context_encoding") != contract.CONTEXT_ENCODING or data.get("hash_version") != 7:
            raise ValueError("Hint fork statistics encoding mismatch")
    rows = [data["global"], *data["pcs"]]
    for row in rows:
        for name in empty_counters():
            if type(row.get(name)) is not int or row[name] < 0:
                raise ValueError(f"invalid Hint counter {name}")
        if (
            row["baseline_misses"] - row["final_misses"]
            != row["corrected"] - row["harmed"]
        ):
            raise ValueError("Hint corrected/harmed conservation failed")
        if not (
            row["overrides"]
            <= row["conf_enabled"]
            <= row["tag_hits"]
            <= row["eligible"]
            <= row["executions"]
        ):
            raise ValueError("Hint coverage counters are inconsistent")
        if (
            max(row["baseline_misses"], row["final_misses"])
            > row["executions"]
        ):
            raise ValueError("Hint misses exceed executions")
    pcs = [address(row["pc"]) for row in data["pcs"]]
    if len(pcs) != len(set(pcs)) or not set(candidates) <= set(pcs):
        raise ValueError("missing or duplicate Hint per-PC counters")
    if data["global"]["final_misses"] != total_misses:
        raise ValueError("global Hint final misses differ from H2P aggregate")
    queue = data["queue"]
    for name in (
        "enqueued",
        "applied",
        "dropped",
        "pending",
        "carried_in",
        "zero_transitions",
        "recoveries",
        "max_occupancy",
    ):
        if type(queue.get(name, 0)) is not int or queue.get(name, 0) < 0:
            raise ValueError("invalid confidence queue counter")
    if finite_v3:
        for name in ("history_window_stalls", "meta_queue_stalls", "lhr_stalls"):
            if type(queue.get(name)) is not int or queue[name] < 0:
                raise ValueError(f"invalid measured Hint enqueue stall count {name}")
        if (queue.get("carried_in", 0) + queue["enqueued"] !=
                queue["applied"] + queue["pending"] + queue.get("canceled", 0)):
            raise ValueError("finite-v3 confidence queue conservation failed")
    elif (queue.get("carried_in", 0) + queue["enqueued"] !=
          queue["applied"] + queue["pending"]):
        raise ValueError("confidence queue conservation failed")
    if not finite_v3 and queue["enqueued"] + queue["dropped"] != data["global"]["tag_hits"]:
        raise ValueError(
            "not every committed tag hit scored or dropped exactly one update"
        )
    return data


def validate_replay_trace(path, stats):
    """Reconcile each dynamic target triple with per-PC replay counters."""
    counters = {}
    previous_seq, previous_cycle = -1, -1
    with gzip.open(path, "rt") as stream:
        header = json.loads(next(stream))
        if header != {"kind": "header", "format": "h2p-hint-replay",
                      "version": 7, "storage": "finite",
                      "hash_version": 7, "context_encoding":
                      "xs-gem5-spec-startpc-ith128-path128-lhr16-inflight8-v1"}:
            raise ValueError("invalid finite-v3 replay trace header")
        for line in stream:
            row = json.loads(line)
            if row.get("kind") != "branch":
                raise ValueError("replay trace contains a non-branch row")
            pc, actual = address(row["query_pc"]), address(row["actual"])
            if (type(row["seq"]) is not int or row["seq"] <= previous_seq or
                    type(row["commit_cycle"]) is not int or
                    row["commit_cycle"] < previous_cycle):
                raise ValueError("replay trace retirement order differs")
            previous_seq, previous_cycle = row["seq"], row["commit_cycle"]
            baseline = int(row["baseline_target"], 16)
            final = int(row["final_target"], 16)
            if not 0 <= baseline <= (1 << 64) - 1 or not 0 <= final <= (1 << 64) - 1:
                raise ValueError("replay target is outside unsigned 64-bit range")
            before = not row["baseline_valid"] or baseline != actual
            after = not row["final_valid"] or final != actual
            if (row["corrected"] is not (before and not after) or
                    row["harmed"] is not (not before and after)):
                raise ValueError("replay correction/harm differs from target triple")
            if row["tag_hit"] != (row["hint_target"] is not None):
                raise ValueError("replay tag hit lacks a shadow target")
            if row["override"] and (not row["tag_hit"] or row["confidence"] == 0):
                raise ValueError("replay override lacks an enabled Hint target")
            if row["tag_hit"] and (type(row["confidence"]) is not int or
                    not 0 <= row["confidence"] <= 3):
                raise ValueError("replay confidence is out of range")
            count = counters.setdefault(pc, {name: 0 for name in empty_counters()})
            count["executions"] += 1
            count["baseline_misses"] += before
            count["final_misses"] += after
            count["corrected"] += row["corrected"]
            count["harmed"] += row["harmed"]
            count["eligible"] += row["eligible"]
            count["tag_hits"] += row["tag_hit"]
            count["conf_enabled"] += bool(row["tag_hit"] and row["confidence"])
            count["overrides"] += row["override"]
    for stat in stats["pcs"]:
        pc = address(stat["pc"])
        actual = counters.get(pc, {name: 0 for name in empty_counters()})
        if any(actual[name] != stat[name] for name in empty_counters()):
            raise ValueError(f"replay trace and per-PC counters differ at {pc:#x}")
    return {"records": sum(row["executions"] for row in counters.values()),
            "pcs": len(counters)}


def validate_occupancy(data, mode, metadata=None, sample_cycles=None, finite_v3=False):
    """Frozen valid rows occupy storage even while confidence is zero."""
    if mode == "replay":
        if finite_v3:
            from .finite_v3 import contract as v3_contract
            v3_contract.pack_image(metadata)
        else:
            validate_metadata(metadata)
        residents = {
            "cam": sum(row.get("valid", True) for row in metadata["cam"]),
            "context": sum(row.get("valid", True) for row in metadata["entries"]),
            "slots": len(metadata["slots"]),
        }
    else:
        residents = dict.fromkeys(("cam", "context", "slots"), 0)
    if data.get("occupancy_applicable") is not (mode == "replay"):
        raise ValueError(
            "collect hardware occupancy must be marked inapplicable"
        )
    for table, capacity in (("cam", 16), ("context", 4096), ("slots", 32)):
        row = data["occupancy"][table]
        cycles = row["sample_cycles"]
        if type(cycles) is not int or cycles < 0:
            raise ValueError("invalid occupancy sample cycle count")
        expected_cycles = sample_cycles if mode == "replay" else 0
        if expected_cycles is not None and cycles != expected_cycles:
            raise ValueError(
                f"{table} occupancy cycles differ from measure boundaries: {cycles} != {expected_cycles}"
            )
        expected = {
            "resident_entries": residents[table],
            "finite_capacity": capacity,
            "avg_entries": residents[table] if cycles else 0,
            "peak_entries": residents[table] if cycles else 0,
        }
        expected.update(
            {
                field + "_multiple": expected[field + "_entries"] / capacity
                for field in ("resident", "avg", "peak")
            }
        )
        for field, value in expected.items():
            if not math.isclose(
                row[field], value, rel_tol=1e-5, abs_tol=1e-10
            ):
                raise ValueError(
                    f"{table}.{field} differs from resident metadata"
                )
    return data["occupancy"]


def validate_difftest_config(config, command, difftest_ref_so=None,
                             memory_type="DDR4_2400_8x8"):
    """Bind the run's reference model to the audited command and config."""
    enabled = difftest_ref_so is not None
    flags = re.findall(r"^enable_difftest=(true|false)$", config, re.M)
    if len(flags) < 2 or any(flag != ("true" if enabled else "false")
                           for flag in flags):
        raise ValueError("difftest configuration differs")
    memories = [arg for arg in command if arg.startswith("--mem-type=")]
    if (("--enable-difftest" in command) != enabled or
            ("--disable-difftest" in command) == enabled or
            memories != [f"--mem-type={memory_type}"]):
        raise ValueError("difftest or memory command differs")
    references = [arg.split("=", 1)[1] for arg in command
                  if arg.startswith("--difftest-ref-so=")]
    if enabled:
        reference = Path(difftest_ref_so["path"])
        if (references != [str(reference)] or
                f"difftest_ref_so={reference}\n" not in config or
                sha256(reference) != difftest_ref_so["sha256"]):
            raise ValueError("NEMU reference path/SHA differs")
    elif references:
        raise ValueError("disabled difftest unexpectedly has a reference")


def audit_run(
    output,
    returncode,
    warmup,
    measure,
    interval,
    mode,
    candidates,
    ideal=False,
    metadata=None,
    check_occupancy=False,
    finite_v3=False,
    difftest_ref_so=None,
    memory_type="DDR4_2400_8x8",
):
    output = Path(output)
    checker = legacy()
    errors, sim_insts, records = checker.validate(
        output, returncode, warmup, measure, interval
    )
    stats = None
    total = 0
    try:
        parsed = checker.parse_h2p_output(output / "h2p-indirect.txt")
        total = sum(row[0] for row in parsed.values())
        if mode != "off":
            stats = validate_hint_stats(
                json.loads((output / "hint-stats.json").read_text()),
                total,
                mode,
                candidates,
                ideal,
                finite_v3,
            )
            for row in ([] if finite_v3 else stats["pcs"]):
                if (
                    row["final_misses"]
                    != parsed.get(address(row["pc"]), (0,))[0]
                ):
                    raise ValueError(
                        "Hint per-PC final misses differ from H2P aggregate"
                    )
            if not (output / "conf0.txt").is_file():
                raise ValueError("missing conf0.txt")
            transitions = sum(
                bool(line.strip())
                for line in (output / "conf0.txt").read_text().splitlines()
            )
            if transitions != stats["queue"]["zero_transitions"]:
                raise ValueError(
                    "conf0 lines differ from actual confidence transitions"
                )
        if (
            mode == "collect"
            and not (output / "hint-profile.jsonl.gz").is_file()
        ):
            raise ValueError("missing collect profile")
        if mode == "replay" and finite_v3:
            validate_replay_trace(output / "hint-replay.jsonl.gz", stats)
        config = (output / "config.ini").read_text()
        if f"hintMode={mode}\n" not in config:
            raise ValueError("Hint configuration mode mismatch")
        if ideal and "idealHintIBP=true\n" not in config:
            raise ValueError("unlimited Hint flag is not enabled")
        if not ideal and "idealHintIBP=true\n" in config:
            raise ValueError(
                "finite/off run unexpectedly enables unlimited Hint"
            )
        if stats is not None:
            depth = int(
                re.search(r"^hintConfQueueDepth=(\d+)$", config, re.M).group(1)
            )
            if (
                max(stats["queue"]["pending"], stats["queue"]["max_occupancy"])
                > depth
            ):
                raise ValueError(
                    "confidence queue exceeds configured capacity"
                )
        command = shlex.split((output / "cmd.txt").read_text())
        validate_difftest_config(config, command, difftest_ref_so, memory_type)
        if "--raw-cpt" in command:
            raise ValueError("zstd checkpoint was run as raw memory")
    except (OSError, KeyError, ValueError) as error:
        errors.append(str(error))
    phases = {}
    try:
        phases = checker.read_phases(
            (output / "stdout.log").read_text()
            + (output / "stderr.log").read_text()
        )
    except (OSError, ValueError) as error:
        errors.append(str(error))
    if check_occupancy and stats is not None:
        try:
            clock = re.search(
                r"\[system\.cpu_clk_domain\][^\[]*\nclock=(\d+)", config
            )
            if not clock:
                raise ValueError("CPU clock period is absent from config")
            period = int(clock.group(1))
            cycles = (
                phases["measure_complete"]["tick"] // period
                - phases["warmup_complete"]["tick"] // period
            )
            validate_occupancy(stats, mode, metadata, cycles, finite_v3)
            if stats.get("storage") != ("unlimited" if ideal else "finite"):
                raise ValueError("Hint stats storage flag differs")
        except (OSError, KeyError, TypeError, ValueError) as error:
            errors.append(str(error))
    numeric = (
        stats_values(output / "stats.txt")
        if (output / "stats.txt").exists()
        else {}
    )
    if finite_v3 and mode == "replay" and stats is not None:
        for field, key in (("history_window_stalls", "historyWindowStalls"),
                           ("meta_queue_stalls", "metaQueueStalls"),
                           ("lhr_stalls", "lhrStalls")):
            # stats_values excludes the h2pHint group; read these three
            # counters directly to cross-check the JSON measurement dump.
            line = re.search(r"^system\.cpu\.branchPred\.h2pHint\." + key +
                             r"\s+(\d+)\s", (output / "stats.txt").read_text(), re.M)
            if line is None or int(line.group(1)) != stats["queue"][field]:
                errors.append(f"Hint {field} differs between stats.txt and JSON")
    return {
        "result": "pass" if not errors else "fail",
        "errors": errors,
        "returncode": returncode,
        "sim_insts": sim_insts,
        "h2p_pc_count": records,
        "final_misses": total,
        "window_count": len(range(0, measure, interval)) if interval else 0,
        "phases": phases,
        "sim_ticks": numeric.get("simTicks", [None])[0],
        "ipc": numeric.get("system.cpu.ipc", [None])[0],
        "hint": stats,
    }
