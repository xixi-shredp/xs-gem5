#!/usr/bin/env python3
"""Run resumable H2P detection for every CPython checkpoint slice."""

import argparse
import concurrent.futures
import datetime as dt
import hashlib
import json
import os
import platform
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time


PANIC_RE = re.compile(r"Kernel panic|---\[ end Kernel panic|\bOops:", re.I)
STAT_RE = re.compile(
    r"^(\S+)\s+(-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)\s"
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_stat_values(path, name):
    values = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = STAT_RE.match(line)
        if match and match.group(1) == name:
            values.append(float(match.group(2)))
    return values


def parse_h2p_output(path):
    """Read sorted new-format output, enforcing per-target miss conservation."""
    records = {}
    previous_pc = -1
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 3 or not re.fullmatch(r"0x[0-9a-fA-F]+", fields[0]):
            raise ValueError(f"line {line_number}: invalid PC/record")
        pc, misses = int(fields[0], 16), int(fields[1])
        if pc <= previous_pc or misses <= 0:
            raise ValueError(f"line {line_number}: invalid PC ordering/misses")
        targets = {}
        for token in fields[2:]:
            match = re.fullmatch(r"(0x[0-9a-fA-F]+)\(([0-9]+)\)", token)
            if not match:
                raise ValueError(f"line {line_number}: target(count) required")
            target, count = int(match[1], 16), int(match[2])
            if targets and target <= next(reversed(targets)):
                raise ValueError(f"line {line_number}: target ordering/duplicate")
            targets[target] = count
        if sum(targets.values()) != misses:
            raise ValueError(f"line {line_number}: target miss sum differs")
        records[pc] = (misses, targets)
        previous_pc = pc
    return records


def validate_h2p_output(path):
    return len(parse_h2p_output(path))


def validate_windows(output, interval, measure_insts):
    total = parse_h2p_output(output / "h2p-indirect.txt")
    directory = output / "h2p-indirec"
    if not interval:
        if directory.exists():
            raise ValueError("interval disabled but window directory exists")
        return 0
    expected = {f"{offset}.txt" for offset in range(0, measure_insts, interval)}
    actual = {path.name for path in directory.iterdir()}
    if actual != expected:
        raise ValueError(f"window names differ: missing={expected-actual}, extra={actual-expected}")
    accumulated = {}
    for name in sorted(expected, key=lambda name: int(name[:-4])):
        for pc, (misses, targets) in parse_h2p_output(directory / name).items():
            if pc not in total or not targets.keys() <= total[pc][1].keys():
                raise ValueError(f"{name}: PC/target absent from total")
            dst = accumulated.setdefault(pc, {})
            for target, count in targets.items():
                dst[target] = dst.get(target, 0) + count
    # Miss-free PCs are omitted per window, so their zero-count targets need
    # not appear in the window union. All positive counts must match exactly.
    for pc, (misses, targets) in total.items():
        counts = accumulated.get(pc, {})
        if sum(counts.values()) != misses or any(
            counts.get(target, 0) != count for target, count in targets.items()
        ):
            raise ValueError(f"window/total miss mismatch for {pc:#x}")
    return len(expected)


def read_phases(logs):
    phases = {}
    for line in logs.splitlines():
        if "H2P_PHASE " not in line:
            continue
        fields = dict(re.findall(r"(\w+)=(\w+)", line))
        name = fields.pop("phase", None)
        if name in phases:
            raise ValueError(f"duplicate phase {name}")
        phases[name] = {key: int(value) for key, value in fields.items()}
    return phases


def checkpoint_for(root, bench, scale, checkpoint_id):
    directory = root / "cpts" / "xsgem5" / f"{bench}_{scale}" / checkpoint_id
    matches = sorted(directory.glob("*_memory_.zstd"))
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one checkpoint in {directory}, found {len(matches)}"
        )
    return matches[0]


def load_tasks(workload_list, common_identity):
    specification = json.loads(workload_list.read_text(encoding="utf-8"))
    workload_root = Path(specification["workload_root"])
    tasks = []
    for group in specification["cpts"]:
        bench = group["bench"]
        scale = group.get("scale", "test")
        for raw_id in group["cpt_list"]:
            checkpoint_id = str(raw_id)
            checkpoint = checkpoint_for(
                workload_root, bench, scale, checkpoint_id
            )
            checkpoint_stat = checkpoint.stat()
            identity = dict(common_identity)
            identity.update(
                {
                    "bench": bench,
                    "scale": scale,
                    "checkpoint_id": checkpoint_id,
                    "checkpoint": str(checkpoint),
                    "checkpoint_size": checkpoint_stat.st_size,
                    "checkpoint_mtime_ns": checkpoint_stat.st_mtime_ns,
                }
            )
            tasks.append((bench, scale, checkpoint_id, checkpoint, identity))
    return tasks


def archive_stale(output):
    if not output.exists():
        return
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    target = output.with_name(f"{output.name}.stale-{stamp}")
    suffix = 1
    while target.exists():
        target = output.with_name(f"{output.name}.stale-{stamp}-{suffix}")
        suffix += 1
    shutil.move(str(output), str(target))


def completed_result(output, identity):
    status_path = output / "status.json"
    if not status_path.is_file():
        return None
    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if status.get("result") != "pass" or status.get("identity") != identity:
        return None
    required = ("stats.txt", "config.ini", "h2p-indirect.txt")
    if not all((output / name).is_file() for name in required):
        return None
    try:
        validate_windows(output, identity["h2p_stat_interval"], identity["measure_insts"])
    except (KeyError, OSError, ValueError):
        return None
    return status


def validate(output, returncode, warmup_insts, measure_insts, interval=0):
    errors = []
    required = (
        "stats.txt",
        "config.ini",
        "h2p-indirect.txt",
        "stdout.log",
        "stderr.log",
        "cmd.txt",
    )
    for name in required:
        if not (output / name).is_file():
            errors.append(f"missing {name}")
    if returncode != 0:
        errors.append(f"return code {returncode}")

    stdout = (
        (output / "stdout.log").read_text(encoding="utf-8", errors="replace")
        if (output / "stdout.log").is_file()
        else ""
    )
    stderr = (
        (output / "stderr.log").read_text(encoding="utf-8", errors="replace")
        if (output / "stderr.log").is_file()
        else ""
    )
    logs = stdout + stderr
    if PANIC_RE.search(logs):
        errors.append("guest kernel panic/oops detected")
    if "a thread reached the max instruction count" not in logs:
        errors.append("maximum-instruction exit was not observed")
    try:
        phases = read_phases(logs)
        restore, warmup, measure = (
            phases[name] for name in
            ("restore_complete", "warmup_complete", "measure_complete")
        )
        if not restore["committed"] < warmup["committed"] < measure["committed"]:
            errors.append("phase commit ordering is invalid")
        split = warmup["warmup_fusion_split"]
        excess = measure["terminal_fusion_excess"]
        if split not in (0, 1) or excess not in (0, 1):
            errors.append("invalid fusion boundary markers")
        if (restore["warmup"] != 0 or restore["measure"] != 0
                or restore["retired_post_restore"] != 0):
            errors.append("restore instructions entered measurement counters")
        if (warmup["warmup"] != warmup_insts or warmup["measure"] != 0
                or measure["warmup"] != warmup_insts
                or measure["measure"] != measure_insts):
            errors.append("phase instruction counters differ")
        if warmup["committed"] - restore["committed"] != warmup_insts + split:
            errors.append("warm-up retirement delta differs from declared fusion split")
        if measure["committed"] - warmup["committed"] != measure_insts + excess - split:
            errors.append("measure retirement delta differs from declared fusion boundaries")
        if (warmup["retired_post_restore"] != warmup_insts + split
                or measure["retired_post_restore"] != warmup_insts + measure_insts + excess
                or warmup["terminal_fusion_excess"] != 0
                or measure["warmup_fusion_split"] != split):
            errors.append("post-restore retirement/fusion conservation failed")
        if not restore["tick"] <= warmup["tick"] <= measure["tick"]:
            errors.append("phase tick ordering is invalid")
    except (KeyError, ValueError) as error:
        errors.append(f"invalid/missing lifecycle phases: {error}")

    sim_insts = []
    if (output / "stats.txt").is_file():
        sim_insts = read_stat_values(output / "stats.txt", "simInsts")
    if sim_insts != [float(measure_insts)]:
        errors.append(f"expected only exact measure stats, got {sim_insts!r}")

    config = (
        (output / "config.ini").read_text(encoding="utf-8", errors="replace")
        if (output / "config.ini").is_file()
        else ""
    )
    if config.count(f"h2pStatInterval={interval}\n") < 2:
        errors.append("top-level and ITTAGE interval configuration differs")
    if "instructionPhaseControl=true" not in config or "gcptRestoreBase=2147483648" not in config:
        errors.append("restore-aware instruction phase configuration missing")
    if config.count("h2pStat=true") < 2:
        errors.append("top-level and ITTAGE h2pStat are not both true")

    h2p_records = None
    if (output / "h2p-indirect.txt").is_file():
        try:
            h2p_records = validate_h2p_output(output / "h2p-indirect.txt")
            validate_windows(output, interval, measure_insts)
        except (OSError, ValueError) as error:
            errors.append(f"invalid h2p-indirect.txt: {error}")
    return errors, sim_insts, h2p_records


def write_status(path, status):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(
        json.dumps(status, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def command_prefix(disable_aslr):
    """Check fixed-address execution support before any workload starts."""
    if not disable_aslr:
        return []
    if platform.system() != "Linux":
        raise RuntimeError("--disable-aslr requires Linux setarch support")
    setarch = shutil.which("setarch")
    if setarch is None:
        raise RuntimeError("--disable-aslr requires setarch in PATH")
    prefix = [setarch, platform.machine(), "-R"]
    try:
        probe = subprocess.run(
            prefix + [sys.executable, "-c", "pass"],
            capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"--disable-aslr preflight failed: {error}") from error
    if probe.returncode:
        detail = probe.stderr.strip() or probe.stdout.strip()
        raise RuntimeError(
            f"--disable-aslr preflight failed (exit {probe.returncode}): {detail}"
        )
    return prefix


def run_one(args, task):
    bench, scale, checkpoint_id, checkpoint, identity = task
    output = args.output_root / f"{bench}_{scale}" / checkpoint_id
    completed = completed_result(output, identity)
    if completed is not None:
        return "skip", bench, checkpoint_id, 0.0, [], completed

    archive_stale(output)
    output.mkdir(parents=True)
    total_insts = args.warmup_insts + args.measure_insts
    command = args.command_prefix + [
        str(args.gem5),
        "-d",
        str(output),
        str(args.config),
        f"--generic-rv-cpt={checkpoint}",
        f"--warmup-insts-no-switch={args.warmup_insts}",
        f"--maxinsts={total_insts}",
        "--disable-difftest",
        f"--mem-type={args.mem_type}",
        "--h2p-stat",
        f"--h2p-stat-interval={args.h2p_stat_interval}",
    ]
    (output / "cmd.txt").write_text(
        shlex.join(command) + "\n", encoding="utf-8"
    )
    (output / "identity.json").write_text(
        json.dumps(identity, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    started = dt.datetime.now(dt.timezone.utc)
    write_status(
        output / "status.json",
        {
            "schema_version": 1,
            "result": "running",
            "identity": identity,
            "started_utc": started.isoformat(),
        },
    )

    environment = os.environ.copy()
    if args.library_path:
        environment["LD_LIBRARY_PATH"] = args.library_path + (
            ":" + environment["LD_LIBRARY_PATH"]
            if environment.get("LD_LIBRARY_PATH")
            else ""
        )
    start_time = time.monotonic()
    with (output / "stdout.log").open("w", encoding="utf-8") as stdout, (
        output / "stderr.log"
    ).open("w", encoding="utf-8") as stderr:
        process = subprocess.run(
            command, stdout=stdout, stderr=stderr, env=environment
        )
    elapsed = time.monotonic() - start_time
    errors, sim_insts, h2p_records = validate(
        output, process.returncode, args.warmup_insts, args.measure_insts,
        args.h2p_stat_interval
    )
    try:
        phases = read_phases((output / "stdout.log").read_text() +
                             (output / "stderr.log").read_text())
    except ValueError:
        phases = {}
    status = {
        "schema_version": 2,
        "result": "pass" if not errors else "fail",
        "identity": identity,
        "returncode": process.returncode,
        "phases": phases,
        "measure_sim_insts": sim_insts[-1] if sim_insts else None,
        "h2p_records": h2p_records,
        "elapsed_seconds": elapsed,
        "started_utc": started.isoformat(),
        "finished_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "errors": errors,
    }
    write_status(output / "status.json", status)
    return status["result"], bench, checkpoint_id, elapsed, errors, status


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--workload-list", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=24)
    parser.add_argument("--warmup-insts", type=int, default=100_000_000)
    parser.add_argument("--measure-insts", type=int, default=50_000_000)
    parser.add_argument("--h2p-stat-interval", type=int, default=1_000_000)
    parser.add_argument("--mem-type", default="DDR4_2400_8x8")
    parser.add_argument("--library-path", default="")
    parser.add_argument(
        "--disable-aslr", action="store_true",
        help="run gem5 through Linux setarch -R after a support preflight",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1 or args.warmup_insts < 1 or args.measure_insts < 1:
        parser.error("--jobs and instruction counts must be positive")
    if args.h2p_stat_interval < 0:
        parser.error("--h2p-stat-interval must be nonnegative")
    for name in ("gem5", "config", "workload_list"):
        path = getattr(args, name)
        if not path.is_file():
            parser.error(f"--{name.replace('_', '-')} is not a file: {path}")
    return args


def main():
    args = parse_args()
    try:
        args.command_prefix = command_prefix(args.disable_aslr)
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 2
    args.output_root.mkdir(parents=True, exist_ok=True)
    common_identity = {
        "schema_version": 1,
        "runner": str(Path(__file__).resolve()),
        "runner_sha256": sha256(Path(__file__)),
        "gem5": str(args.gem5.resolve()),
        "gem5_sha256": sha256(args.gem5),
        "config": str(args.config.resolve()),
        "config_sha256": sha256(args.config),
        "workload_list": str(args.workload_list.resolve()),
        "workload_list_sha256": sha256(args.workload_list),
        "warmup_insts": args.warmup_insts,
        "measure_insts": args.measure_insts,
        "maxinsts": args.warmup_insts + args.measure_insts,
        "h2p_stat": True,
        "h2p_stat_interval": args.h2p_stat_interval,
        "disable_aslr": args.disable_aslr,
        "command_prefix": args.command_prefix,
        "difftest_enabled": False,
        "mem_type": args.mem_type,
    }
    tasks = load_tasks(args.workload_list, common_identity)
    manifest = dict(common_identity)
    manifest.update({"jobs": args.jobs, "task_count": len(tasks)})
    (args.output_root / "experiment.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if args.dry_run:
        print(json.dumps(manifest, indent=2, sort_keys=True))
        return 0

    counts = {"pass": 0, "fail": 0, "skip": 0}
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        future_to_task = {
            pool.submit(run_one, args, task): task for task in tasks
        }
        for future in concurrent.futures.as_completed(future_to_task):
            task = future_to_task[future]
            try:
                result, bench, checkpoint_id, elapsed, errors, status = (
                    future.result()
                )
            except Exception as error:
                _, bench, checkpoint_id, _, _ = task
                result, elapsed, errors, status = (
                    "fail",
                    0.0,
                    [f"runner exception: {error}"],
                    None,
                )
            counts[result] += 1
            detail = "; ".join(errors)
            print(
                f"{result.upper():4} {bench}/{checkpoint_id} {elapsed:.1f}s"
                f"{': ' + detail if detail else ''}",
                flush=True,
            )
            progress = {
                "schema_version": 1,
                "started_utc": started,
                "updated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "counts": counts,
                "completed": sum(counts.values()),
                "total": len(tasks),
                "last_status": status,
            }
            write_status(args.output_root / "progress.json", progress)
    print(json.dumps(counts, sort_keys=True), flush=True)
    return 1 if counts["fail"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
