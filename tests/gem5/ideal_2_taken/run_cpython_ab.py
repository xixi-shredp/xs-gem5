#!/usr/bin/env python3
"""Run a resumable baseline/ideal-2-taken CPython SimPoint experiment."""

import argparse
import concurrent.futures
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time


CONFIGS = (("baseline", False), ("ideal2", True))
PANIC_RE = re.compile(r"Kernel panic|---\[ end Kernel panic|\bOops:", re.I)
STAT_RE = re.compile(r"^(\S+)\s+(-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)\s")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_stat(path, name):
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = STAT_RE.match(line)
        if match and match.group(1) == name:
            return float(match.group(2))
    return None


def checkpoint_for(root, bench, scale, cpt_id):
    directory = root / "cpts" / "xsgem5" / f"{bench}_{scale}" / cpt_id
    matches = sorted(directory.glob("*_memory_.zstd"))
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one checkpoint in {directory}, found {len(matches)}"
        )
    return matches[0]


def load_tasks(args, identity):
    spec = json.loads(args.workload_list.read_text(encoding="utf-8"))
    workload_root = Path(spec["workload_root"])
    tasks = []
    for group in spec["cpts"]:
        bench = group["bench"]
        scale = group["scale"]
        for cpt_id in group["cpt_list"]:
            cpt_id = str(cpt_id)
            if args.only and f"{bench}/{cpt_id}" not in args.only:
                continue
            checkpoint = checkpoint_for(workload_root, bench, scale, cpt_id)
            stat = checkpoint.stat()
            for config_name, ideal in CONFIGS:
                task_identity = dict(identity)
                task_identity.update({
                    "bench": bench,
                    "scale": scale,
                    "cpt_id": cpt_id,
                    "checkpoint": str(checkpoint),
                    "checkpoint_size": stat.st_size,
                    "checkpoint_mtime_ns": stat.st_mtime_ns,
                    "config_name": config_name,
                    "ideal_two_taken": ideal,
                })
                tasks.append((bench, scale, cpt_id, checkpoint,
                              config_name, ideal, task_identity))
    return tasks


def valid_completed(output, identity, maxinsts):
    status_path = output / "status.json"
    if not status_path.is_file():
        return False
    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return (
        status.get("result") == "pass"
        and status.get("identity") == identity
        and status.get("sim_insts", 0) >= maxinsts
        and (output / "stats.txt").is_file()
        and (output / "config.ini").is_file()
    )


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


def validate(output, returncode, ideal, maxinsts):
    errors = []
    required = ("stats.txt", "config.ini", "stdout.log", "stderr.log", "cmd.txt")
    for name in required:
        if not (output / name).is_file():
            errors.append(f"missing {name}")
    if returncode != 0:
        errors.append(f"return code {returncode}")
    sim_insts = read_stat(output / "stats.txt", "simInsts") \
        if (output / "stats.txt").is_file() else None
    if sim_insts is None or sim_insts < maxinsts:
        errors.append(f"simInsts {sim_insts!r} < {maxinsts}")
    logs = ""
    for name in ("stdout.log", "stderr.log"):
        path = output / name
        if path.is_file():
            logs += path.read_text(encoding="utf-8", errors="replace")
    if PANIC_RE.search(logs):
        errors.append("guest kernel panic/oops detected")
    if "a thread reached the max instruction count" not in logs:
        errors.append("missing max-instruction exit cause")
    config = (output / "config.ini").read_text(
        encoding="utf-8", errors="replace"
    ) if (output / "config.ini").is_file() else ""
    expected = f"idealTwoTaken={'true' if ideal else 'false'}"
    if expected not in config:
        errors.append(f"missing {expected} in config.ini")
    return errors, sim_insts


def run_one(args, task):
    bench, scale, cpt_id, checkpoint, config_name, ideal, identity = task
    output = args.output_root / f"{bench}_{scale}" / cpt_id / config_name
    if valid_completed(output, identity, args.maxinsts):
        return "skip", bench, cpt_id, config_name, 0.0, []
    archive_stale(output)
    output.mkdir(parents=True)

    command = [
        str(args.gem5), "-d", str(output), str(args.config),
        f"--generic-rv-cpt={checkpoint}",
        "--warmup-insts-no-switch=0",
        f"--maxinsts={args.maxinsts}",
    ]
    if args.disable_difftest:
        command.append("--disable-difftest")
    else:
        command.extend((
            "--enable-difftest",
            f"--difftest-ref-so={args.difftest_ref}",
        ))
    if ideal:
        command.append("--ideal-2-taken")
    (output / "cmd.txt").write_text(shlex.join(command) + "\n", encoding="utf-8")
    (output / "identity.json").write_text(
        json.dumps(identity, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    started = dt.datetime.now(dt.timezone.utc)
    start_monotonic = time.monotonic()
    env = os.environ.copy()
    if args.library_path:
        env["LD_LIBRARY_PATH"] = args.library_path + (
            ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else ""
        )
    with (output / "stdout.log").open("w", encoding="utf-8") as stdout, \
            (output / "stderr.log").open("w", encoding="utf-8") as stderr:
        completed = subprocess.run(command, stdout=stdout, stderr=stderr, env=env)
    elapsed = time.monotonic() - start_monotonic
    errors, sim_insts = validate(output, completed.returncode, ideal, args.maxinsts)
    status = {
        "schema_version": 1,
        "result": "pass" if not errors else "fail",
        "identity": identity,
        "returncode": completed.returncode,
        "sim_insts": sim_insts,
        "elapsed_seconds": elapsed,
        "started_utc": started.isoformat(),
        "finished_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "errors": errors,
    }
    (output / "status.json").write_text(
        json.dumps(status, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return status["result"], bench, cpt_id, config_name, elapsed, errors


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--workload-list", type=Path, required=True)
    difftest = parser.add_mutually_exclusive_group(required=True)
    difftest.add_argument("--difftest-ref", type=Path)
    difftest.add_argument("--disable-difftest", action="store_true")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--maxinsts", type=int, default=50_000_000)
    parser.add_argument("--library-path", default="")
    parser.add_argument(
        "--only", action="append", metavar="BENCH/CPT",
        help="run only selected slice(s); may be repeated",
    )
    args = parser.parse_args()
    if args.jobs < 1 or args.maxinsts < 1:
        parser.error("--jobs and --maxinsts must be positive")
    for name in ("gem5", "config", "workload_list"):
        path = getattr(args, name)
        if not path.is_file():
            parser.error(f"--{name.replace('_', '-')} is not a file: {path}")
    if args.difftest_ref and not args.difftest_ref.is_file():
        parser.error(f"--difftest-ref is not a file: {args.difftest_ref}")
    return args


def main():
    args = parse_args()
    args.output_root.mkdir(parents=True, exist_ok=True)
    identity = {
        "schema_version": 1,
        "gem5": str(args.gem5.resolve()),
        "gem5_sha256": sha256(args.gem5),
        "config": str(args.config.resolve()),
        "config_sha256": sha256(args.config),
        "workload_list": str(args.workload_list.resolve()),
        "workload_list_sha256": sha256(args.workload_list),
        "difftest_enabled": not args.disable_difftest,
        "difftest_ref": (
            str(args.difftest_ref.resolve()) if args.difftest_ref else None
        ),
        "difftest_ref_sha256": (
            sha256(args.difftest_ref) if args.difftest_ref else None
        ),
        "maxinsts": args.maxinsts,
        "warmup_insts_no_switch": 0,
    }
    tasks = load_tasks(args, identity)
    manifest = dict(identity)
    manifest.update({"jobs": args.jobs, "task_count": len(tasks)})
    (args.output_root / "experiment.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    counts = {"pass": 0, "fail": 0, "skip": 0}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_one, args, task) for task in tasks]
        for future in concurrent.futures.as_completed(futures):
            result, bench, cpt_id, config, elapsed, errors = future.result()
            counts[result] += 1
            detail = "; ".join(errors)
            print(
                f"{result.upper():4} {bench}/{cpt_id}/{config} "
                f"{elapsed:.1f}s{': ' + detail if detail else ''}",
                flush=True,
            )
    print(json.dumps(counts, sort_keys=True), flush=True)
    return 1 if counts["fail"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
