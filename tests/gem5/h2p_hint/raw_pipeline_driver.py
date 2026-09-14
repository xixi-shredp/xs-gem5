#!/usr/bin/env python3
"""Run and audit off/collect/empty-replay through the real kmhv3 CPU pipeline.

Run only after gem5-scons has completely exited. This driver verifies the ELF,
execute permission and supplied final SHA, and never builds or resumes a run.
"""

import argparse
from collections import Counter
import concurrent.futures
import datetime as dt
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys

from raw_pipeline_program import generate


REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "util"))
from h2p_hint.audit import compare_observer, legacy, stats_values, validate_hint_stats


MODES = {"off": "off", "collect": "collect", "empty-replay": "replay"}
DEBUG_FLAGS = "CommitTrace,RAS,DecoupleBPHist"
COMMIT = re.compile(r"^\s*(\d+):.*CT \[tid:\d+\]: \[sn:(\d+) pc:(0x[0-9a-fA-F]+)\]")
TRAP = re.compile(r"^\s*(\d+):.*Trap retired history restored: FTQ (\d+) "
                  r"last_pc (0x[0-9a-fA-F]+) controls (\d+)")


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify_binary(path, expected):
    require(path.is_file() and os.access(path, os.X_OK), "gem5 is not executable")
    with path.open("rb") as source:
        require(source.read(4) == b"\x7fELF", "gem5 is not an ELF executable")
    require(sha(path) == expected, "gem5 SHA differs from the final build identity")


def run_case(name, args, experiment):
    out = args.output_root / name
    out.mkdir(exist_ok=False)
    assets = args.output_root / "inputs"
    verify_binary(args.gem5, args.binary_sha256)
    command = ["setarch", platform.machine(), "-R", str(args.gem5),
               "-d", str(out), f"--debug-flags={DEBUG_FLAGS}",
               "--debug-file=debug.log", str(args.config), "--disable-difftest",
               "--mem-type=DDR4_2400_8x8", "--raw-cpt",
               f"--generic-rv-cpt={assets / 'raw_pipeline.bin'}",
               f"--warmup-insts-no-switch={args.warmup_insts}",
               f"--maxinsts={args.warmup_insts + args.measure_insts}",
               "--h2p-stat", f"--h2p-stat-interval={args.interval}",
               f"--h2p-hint-mode={MODES[name]}"]
    if name != "off":
        command.append(f"--h2p-hint-candidates={assets / 'candidates.json'}")
    if name == "empty-replay":
        command.append(f"--h2p-hint-metadata={assets / 'empty-metadata.json'}")
    (out / "cmd.txt").write_text(shlex.join(command) + "\n")
    save(out / "command.json", command)
    environment = os.environ.copy()
    if args.library_path:
        environment["LD_LIBRARY_PATH"] = args.library_path
    record = {"started_utc": utc(), "binary_sha256": args.binary_sha256,
              "mode": MODES[name]}
    with (out / "stdout.log").open("w") as stdout, (out / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(command, cwd=REPO, env=environment,
                                   stdout=stdout, stderr=stderr)
        record["pid"] = process.pid
        save(out / "process.json", record)
        record["returncode"] = process.wait()
    record.update(finished_utc=utc(), binary_sha256_after=sha(args.gem5))
    save(out / "process.json", record)
    return name


def audit_case(name, output, experiment):
    directory = output / name
    record = {"result": "fail", "errors": []}
    try:
        process = json.loads((directory / "process.json").read_text())
        require(process.get("returncode") == 0,
                f"process failed or is still running: {process}")
        require(process["binary_sha256"] == process["binary_sha256_after"] ==
                experiment["binary_sha256"], "binary identity changed during run")
        logs = (directory / "stdout.log").read_text(errors="replace") + "\n" + \
            (directory / "stderr.log").read_text(errors="replace")
        require("a thread reached the max instruction count" in logs,
                "normal maximum-instruction exit is missing")
        require(not re.search(r"\bpanic:|\bfatal:|HIT BAD TRAP|\bOops:", logs, re.I),
                "failure marker found in process logs")
        checker = legacy()
        phases = checker.read_phases(logs)
        require("restore_complete" not in phases,
                "raw workload incorrectly waited for a GCPT restore marker")
        warm, measure = phases["warmup_complete"], phases["measure_complete"]
        require(warm["warmup"] == experiment["warmup_insts"] and warm["measure"] == 0,
                "warm-up boundary differs")
        require(measure["warmup"] == experiment["warmup_insts"] and
                measure["measure"] == experiment["measure_insts"], "measure boundary differs")
        split, excess = warm["warmup_fusion_split"], measure["terminal_fusion_excess"]
        require(split in (0, 1) and excess in (0, 1), "invalid fusion boundary markers")
        require(measure["committed"] - warm["committed"] ==
                experiment["measure_insts"] + excess - split,
                "measure committed count does not conserve architectural units")
        require(checker.read_stat_values(directory / "stats.txt", "simInsts") ==
                [float(experiment["measure_insts"])], "stats include warm-up or extra dumps")
        config = (directory / "config.ini").read_text()
        require("instructionPhaseControl=true" in config and "gcptRestoreBase=0\n" in config,
                "raw CPU phase configuration differs")
        require(f"hintMode={MODES[name]}\n" in config and "enable_difftest=true" not in config,
                "Hint or difftest configuration differs")
        windows = checker.validate_windows(directory, experiment["interval"],
                                           experiment["measure_insts"])
        h2p = checker.parse_h2p_output(directory / "h2p-indirect.txt")
        program = experiment["program"]
        labels = {key: int(value, 16) for key, value in program["labels"].items()}
        allowed = {labels["fallthrough_call"], labels["fallthrough_jump"]}
        require(set(h2p) <= allowed, "return, direct or failure-path PC appeared in H2P output")
        debug = (directory / "debug.log").read_text(errors="replace")
        commits = [(int(found[1]), int(found[2]), int(found[3], 16))
                   for line in debug.splitlines() if (found := COMMIT.match(line))]
        traps = [(int(found[1]), int(found[2]), int(found[3], 16), int(found[4]))
                 for line in debug.splitlines() if (found := TRAP.match(line))]
        require(commits and len(traps) >= 8, "insufficient real CPU commit/trap evidence")
        visits = Counter(pc for _, _, pc in commits)
        forbidden = {labels[key] for key in
                     ("bad", "bad_spin", "fail_near_body", "fail_near_handler")}
        require(not visits.keys() & forbidden, "guest failure sentinel path committed")
        require(not visits[labels["ecall"]], "faulting ecall unexpectedly retired")
        required_labels = ("fallthrough_call", "fallthrough_jump", "not_taken",
                           "pretrap_alu", "handler", "handler_mret", "after_ecall",
                           "direct_call", "return", "pop_push")
        require(all(visits[labels[label]] for label in required_labels),
                "one required control/trap path never committed")
        require(all(last_pc == labels["pretrap_alu"] for _, _, last_pc, _ in traps),
                "trap history boundary used a handler/control PC instead of the prior ALU")
        require(max(size for _, _, _, size in traps) >= 3,
                "no real FTQ retained the cold call/jump/not-taken prefix together")
        require("IsCall" in debug and "IsRet" in debug,
                "RAS call/return evidence is missing")
        trace_digest = hashlib.sha256(json.dumps(commits).encode()).hexdigest()
        save(directory / "raw_pipeline_evidence.json", {
            "commit_trace_sha256": trace_digest,
            "label_commits": {key: visits[pc] for key, pc in labels.items()},
            "trap_count": len(traps), "traps": traps})
        if name == "off":
            require(not (directory / "hint-stats.json").exists() and
                    not (directory / "hint-profile.jsonl.gz").exists(),
                    "Hint off created a Hint output")
        else:
            hint = validate_hint_stats(json.loads((directory / "hint-stats.json").read_text()),
                                       sum(value[0] for value in h2p.values()), MODES[name],
                                       [int(pc, 16) for pc in program["candidates"]])
            require(hint["global"]["overrides"] == 0 and hint["queue"]["enqueued"] == 0,
                    "observer/empty image changed predictions or scored an entry")
            require((directory / "conf0.txt").read_text() == "", "empty-image conf0 is nonempty")
            if name == "collect":
                with gzip.open(directory / "hint-profile.jsonl.gz", "rt") as source:
                    rows = [json.loads(line) for line in source]
                observed = Counter(int(row["pc"], 16) for row in rows if row["kind"] == "branch")
                require(set(observed) == allowed,
                        "collect failed to observe both indirects or included a return")
                for pc in allowed:
                    expected = next(row["executions"] for row in hint["pcs"]
                                    if int(row["pc"], 16) == pc)
                    require(observed[pc] == expected, "profile and per-PC execution counts differ")
        numeric = stats_values(directory / "stats.txt")
        record.update(result="pass", phases=phases, window_count=windows,
                      h2p_misses=sum(value[0] for value in h2p.values()),
                      trap_count=len(traps), max_retired_prefix=max(row[3] for row in traps),
                      commit_trace_sha256=trace_digest,
                      numeric_statistics=len(numeric), sim_ticks=numeric.get("simTicks"))
    except (OSError, ValueError, KeyError, StopIteration) as error:
        record["errors"].append(str(error))
    save(directory / "raw_pipeline_status.json", record)
    return record


def audit(output):
    experiment = json.loads((output / "experiment.json").read_text())
    cases = {name: audit_case(name, output, experiment) for name in MODES}
    comparisons = {}
    if all(case["result"] == "pass" for case in cases.values()):
        for name in ("collect", "empty-replay"):
            comparison = compare_observer(output / "off", output / name)
            comparison["commit_trace_equal"] = cases[name]["commit_trace_sha256"] == \
                cases["off"]["commit_trace_sha256"]
            comparison["windows_equal"] = all(
                (output / "off" / "h2p-indirec" / f"{offset}.txt").read_bytes() ==
                (output / name / "h2p-indirec" / f"{offset}.txt").read_bytes()
                for offset in range(0, experiment["measure_insts"], experiment["interval"]))
            comparison["pass"] &= comparison["commit_trace_equal"] and comparison["windows_equal"]
            comparisons[name] = comparison
    passed = all(case["result"] == "pass" for case in cases.values()) and \
        len(comparisons) == 2 and all(result["pass"] for result in comparisons.values())
    result = {"format": "h2p-hint-raw-pipeline-audit", "version": 1,
              "result": "pass" if passed else "fail", "audited_utc": utc(),
              "binary_sha256": experiment["binary_sha256"],
              "program_sha256": experiment["program"]["image_sha256"],
              "cases": cases, "comparisons": comparisons}
    save(output / "audit.json", result)
    print(json.dumps({"result": result["result"], "cases": {
        name: {key: case[key] for key in ("result", "errors", "trap_count") if key in case}
        for name, case in cases.items()}, "audit": str(output / "audit.json")}, indent=2))
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, default=REPO / "build/RISCV/gem5.opt")
    parser.add_argument("--config", type=Path, default=REPO / "configs/example/kmhv3.py")
    parser.add_argument("--binary-sha256")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--library-path", default="")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--warmup-insts", type=int, default=2000)
    parser.add_argument("--measure-insts", type=int, default=10000)
    parser.add_argument("--interval", type=int, default=1000)
    parser.add_argument("--audit-only", action="store_true")
    args = parser.parse_args()
    args.output_root, args.gem5, args.config = (
        path.resolve() for path in (args.output_root, args.gem5, args.config))
    if args.audit_only:
        return 0 if audit(args.output_root) else 1
    if not args.binary_sha256 or not re.fullmatch(r"[0-9a-f]{64}", args.binary_sha256):
        parser.error("--binary-sha256 must identify the fully completed final gem5 build")
    if not 1 <= args.jobs <= 3 or min(args.warmup_insts, args.measure_insts, args.interval) <= 0:
        parser.error("jobs must be 1–3 and warmup, measure and interval must be positive")
    verify_binary(args.gem5, args.binary_sha256)
    args.output_root.mkdir(parents=True, exist_ok=False)
    program = generate(args.output_root / "inputs")
    experiment = {"format": "h2p-hint-raw-pipeline-experiment", "version": 1,
                  "created_utc": utc(), "binary": str(args.gem5),
                  "binary_sha256": args.binary_sha256, "config": str(args.config),
                  "config_sha256": sha(args.config), "driver_sha256": sha(__file__),
                  "generator_sha256": sha(Path(__file__).with_name("raw_pipeline_program.py")),
                  "warmup_insts": args.warmup_insts, "measure_insts": args.measure_insts,
                  "interval": args.interval, "jobs": args.jobs,
                  "debug_flags": DEBUG_FLAGS, "program": program}
    save(args.output_root / "experiment.json", experiment)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        list(pool.map(lambda name: run_case(name, args, experiment), MODES))
    return 0 if audit(args.output_root) else 1


if __name__ == "__main__":
    raise SystemExit(main())
