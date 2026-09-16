"""Reproducible 185-run unlimited-capacity Hint experiment.

The runner uses one process pool for simulation and offline build/reference
work. A worker retains its slot across all three, so --jobs is the compute
concurrency ceiling. Existing failed/running outputs are never retried in place.
"""

import argparse
import concurrent.futures as futures
import csv
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time
import traceback

from .audit import audit_run, compare_observer, legacy
from .builder import build_image, candidates_from_stage1, select_joint
from .model import (
    address,
    contract,
    load_candidates,
    load_metadata,
    sha256,
    write_json,
)
from .profile import Cache, compare_shared_profiles, make_cache
from .runner import (
    HEAVY,
    file_identity,
    load_workloads,
    now,
    tag,
    validate_runtime_gate,
)


RUN_COUNTS = {
    "baseline": 5,
    "finite": 5,
    "fixed": 5,
    "collect": 5,
    "masks": 160,
    "joint": 5,
}
FORMAL_RUNS = sum(RUN_COUNTS.values())
FORMAL_WINDOWS = FORMAL_RUNS * 50
QUALIFICATION = (
    "same-slice offline specialization; capacity-unconstrained under "
    "fixed history/hash/ports/timing/builder; not a target oracle"
)


def run_path(args, task, stage, mask=None):
    path = args.output_root / "runs" / stage / tag(task)
    return path / f"mask-{mask:02d}" if stage == "masks" else path


def image_path(args, task, stage, mask=None):
    path = args.output_root / "images" / stage / tag(task)
    return path / f"mask-{mask:02d}" if stage == "masks" else path


def candidates_path(args, task, stage):
    name = {
        "finite": "finite-candidates.json",
        "fixed": "fixed-candidates.json",
    }.get(stage, "candidates.json")
    return args.output_root / "inputs" / tag(task) / name


def read_status(args, task, stage, mask=None):
    status = json.loads(
        (run_path(args, task, stage, mask) / "status.json").read_text()
    )
    if status.get("result") != "pass":
        raise ValueError(f"required {stage} is not a pass: {tag(task)}")
    return status


def candidate_document(pcs, source):
    return {
        "format": "h2p-hint-candidates",
        "version": 3,
        "storage": "unlimited",
        "pcs": [hex(pc) for pc in pcs],
        "source": source,
    }


def freeze_json(path, data):
    if path.exists():
        if json.loads(path.read_text()) != data:
            raise ValueError(f"frozen input differs: {path}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        write_json(path, data)


def verify_file(expected, full=False):
    path = Path(expected["path"])
    observed = path.stat()
    if (observed.st_size, observed.st_mtime_ns) != (
        expected["size"],
        expected["mtime_ns"],
    ):
        raise ValueError(f"input changed since preflight: {path}")
    if full and sha256(path) != expected["sha256"]:
        raise ValueError(f"input content changed: {path}")


def old_inputs(args, task):
    """Anchor original final selection and profiles in the independent audit."""
    root = args.old_root
    old_tag = tag(task)
    image = root / "images" / "joint" / old_tag / "metadata.txt"
    metadata = load_metadata(image, ideal=False)
    cache_dir = root / "profiles" / old_tag
    profile_audit = root / "final-audit" / "profiles" / old_tag / "audit.json"
    complete = json.loads((root / "complete-audit.json").read_text())
    audit_key = "profiles/" + old_tag + "/audit.json"
    if sha256(profile_audit) != complete["audit_artifact_sha256"][audit_key]:
        raise ValueError(
            "old profile audit is not bound to original complete audit"
        )
    audited = json.loads(profile_audit.read_text())
    if audited.get("result") != "pass":
        raise ValueError(f"old profile was not audited: {profile_audit}")
    files = {}
    for name in (
        "cache.json",
        "records.bin",
        "queries.bin",
        "hint-profile.jsonl.gz",
    ):
        path = (
            (root / "runs" / "collect" / old_tag / name)
            if name.endswith(".gz")
            else cache_dir / name
        )
        identity = file_identity(path)
        if identity["sha256"] != audited["sha256"][name]:
            raise ValueError(f"old audited profile content changed: {path}")
        files[name] = identity
    cache = Cache(cache_dir)
    if cache.identity != audited["identity"]:
        raise ValueError(
            "old profile header/cache identity differs from independent audit"
        )
    old_status = root / "runs" / "joint" / old_tag / "status.json"
    joint_audit_path = (
        root / "final-audit" / "runs" / "joint" / old_tag / "audit.json"
    )
    if (
        sha256(joint_audit_path)
        != complete["audit_artifact_sha256"][
            "runs/joint/" + old_tag + "/audit.json"
        ]
    ):
        raise ValueError(
            "old joint audit changed since original complete audit"
        )
    joint_audit = json.loads(joint_audit_path.read_text())
    if (
        joint_audit.get("result") != "pass"
        or joint_audit["key_output_sha256"]["image/metadata.txt"]
        != sha256(image)
        or joint_audit["key_output_sha256"]["status.json"]
        != sha256(old_status)
    ):
        raise ValueError(
            "old joint image/status does not match independent audit"
        )
    status = json.loads(old_status.read_text())
    if status.get("result") != "pass" or status["identity"][
        "metadata_sha256"
    ] != sha256(image):
        raise ValueError("old joint image does not match its successful run")
    for key in ("sha256", "size"):
        if (
            status["identity"]["task"]["checkpoint"][key]
            != task["checkpoint"][key]
        ):
            raise ValueError(
                "old and new experiments use different checkpoints"
            )
    candidate = root / "inputs" / old_tag / "candidates.json"
    load_candidates(candidate)
    return {
        "image": file_identity(image),
        "candidates": file_identity(candidate),
        "profile_audit": file_identity(profile_audit),
        "cache": files,
        "joint_audit": file_identity(joint_audit_path),
        "cache_directory": str(cache_dir),
        "joint_status": file_identity(old_status),
        "collect_status": file_identity(
            root / "runs" / "collect" / old_tag / "status.json"
        ),
        "masks": {
            row["pc"]: row["mask"] for row in metadata["cam"] if row["valid"]
        },
    }


def prepare_baseline_candidates(args, tasks):
    for task in tasks:
        read_status(args, task, "baseline")
        baseline = run_path(args, task, "baseline") / "h2p-indirect.txt"
        candidates = candidates_from_stage1(baseline, None, ideal=True)
        freeze_json(candidates_path(args, task, "collect"), candidates)


def mask_jobs(args, tasks):
    jobs = []
    for task in tasks:
        pcs = load_candidates(
            candidates_path(args, task, "collect"), ideal=True
        )
        jobs.extend(
            {
                "stage": "masks",
                "task": task,
                "mask": mask,
                "masks": {pc: mask for pc in pcs},
            }
            for mask in range(32)
        )
    if len(jobs) != RUN_COUNTS["masks"]:
        raise ValueError("unlimited sweep must have 5 slices x 32 masks")
    return jobs


def joint_jobs(args, tasks):
    jobs = []
    for task in tasks:
        candidates = set(
            load_candidates(candidates_path(args, task, "collect"), ideal=True)
        )
        replays = []
        for mask in range(32):
            status = read_status(args, task, "masks", mask)
            stats = {
                "pcs": [
                    row
                    for row in status["hint"]["pcs"]
                    if address(row["pc"]) in candidates
                ]
            }
            if {address(row["pc"]) for row in stats["pcs"]} != candidates:
                raise ValueError(
                    "actual mask replay misses candidate per-PC counters"
                )
            replays.append((mask, stats))
        masks, choices = select_joint(replays, cam_entries=None)
        freeze_json(
            args.output_root / "inputs" / tag(task) / "best-masks.json",
            choices,
        )
        jobs.append(
            {
                "stage": "joint",
                "task": task,
                "masks": masks,
                "choices": choices,
            }
        )
    return jobs


def build_command(args, task, stage, output, metadata=None, mask=None):
    mode = (
        "off"
        if stage == "baseline"
        else "collect"
        if stage == "collect"
        else "replay"
    )
    ideal = stage not in ("baseline", "finite")
    command = args.command_prefix + [
        str(args.gem5),
        "-d",
        str(output),
        str(args.config),
        f"--generic-rv-cpt={task['checkpoint']['path']}",
        "--warmup-insts-no-switch=100000000",
        "--maxinsts=150000000",
        "--disable-difftest",
        "--mem-type=DDR4_2400_8x8",
        "--h2p-stat",
        "--h2p-stat-interval=1000000",
        f"--h2p-hint-mode={mode}",
    ]
    if mode != "off":
        command.append(
            f"--h2p-hint-candidates={candidates_path(args, task, stage)}"
        )
    if ideal:
        command.append("--ideal-hint-ibp")
    if metadata is not None:
        command.append(f"--h2p-hint-metadata={metadata}")
    return command


def build_task_image(args, job):
    task, stage, mask = job["task"], job["stage"], job.get("mask")
    if stage == "finite":
        output = image_path(args, task, stage)
        output.mkdir(parents=True, exist_ok=True)
        target = output / "metadata.txt"
        source = Path(task["old"]["image"]["path"])
        if (
            target.exists()
            and sha256(target) != task["old"]["image"]["sha256"]
        ):
            raise ValueError("copied finite image changed")
        if not target.exists():
            shutil.copy2(source, target)
        return target
    old = stage == "fixed"
    cache = Cache(
        task["old"]["cache_directory"]
        if old
        else args.output_root / "profiles" / tag(task)
    )
    collect = (
        json.loads(Path(task["old"]["collect_status"]["path"]).read_text())
        if old
        else read_status(args, task, "collect")
    )
    finish = (
        collect["phases"]["measure_complete"]["tick"]
        // cache.identity["header"]["clock_period"]
    )
    masks = (
        {address(pc): value for pc, value in task["old"]["masks"].items()}
        if old
        else job["masks"]
    )
    identity = {
        "profile_sha256": cache.identity["profile_sha256"],
        "profile_header": cache.identity["header"],
        "cache_files": {
            name: sha256(cache.directory / name)
            for name in ("cache.json", "records.bin", "queries.bin")
        },
        "gem5_sha256": args.common_identity["gem5"]["sha256"],
        "source_manifest_sha256": args.common_identity["source_manifest"][
            "sha256"
        ],
        "candidate_sha256": sha256(candidates_path(args, task, stage)),
        "masks": {hex(pc): value for pc, value in sorted(masks.items())},
        "stage": stage,
        "fixed_mask": mask,
        "qualification": QUALIFICATION,
    }
    if old:
        identity["original_joint_image"] = task["old"]["image"]
        identity["original_profile_audit"] = task["old"]["profile_audit"]
    output = image_path(args, task, stage, mask)
    if output.exists():
        status_path = output / "build-status.json"
        if not args.resume or not status_path.exists():
            raise ValueError(f"existing/incomplete image preserved: {output}")
        status = json.loads(status_path.read_text())
        if (
            status.get("result") != "pass"
            or status.get("identity") != identity
            or status["metadata_sha256"] != sha256(output / "metadata.txt")
        ):
            raise ValueError(f"image identity/status differs: {output}")
        return output / "metadata.txt"
    output.mkdir(parents=True)
    try:
        active, iterations = dict(masks), []
        while True:
            metadata, report = build_image(
                cache.rows(),
                active,
                ideal=True,
                source=identity,
                prune_cam=stage == "joint",
            )
            remaining = {
                address(row["pc"]): row["mask"] for row in metadata["cam"]
            }
            removed = sorted(set(active) - set(remaining))
            iterations.append(
                {
                    "iteration": len(iterations),
                    "input_pcs": len(active),
                    "output_pcs": len(remaining),
                    "removed": [
                        {
                            "pc": hex(pc),
                            "reason": "no_positive_mapping_under_current_cam",
                        }
                        for pc in removed
                    ],
                }
            )
            if stage != "joint" or remaining == active:
                break
            active = remaining
        report["prune_iterations"] = iterations
        reference = cache.reference(metadata, finish)
        write_json(output / "metadata.txt", metadata)
        write_json(output / "builder.json", report)
        write_json(output / "reference.json", reference)
        with (output / "hard-pred.txt").open("w") as destination:
            for row in job.get("choices", []):
                if row["reason"] != "selected":
                    destination.write(json.dumps(row, sort_keys=True) + "\n")
            for row in report["pcs"]:
                if row["reasons"]:
                    destination.write(json.dumps(row, sort_keys=True) + "\n")
            for iteration in iterations:
                for row in iteration["removed"]:
                    destination.write(
                        json.dumps(
                            {**row, "iteration": iteration["iteration"]}
                        )
                        + "\n"
                    )
        write_json(
            output / "build-status.json",
            {
                "result": "pass",
                "identity": identity,
                "metadata_sha256": sha256(output / "metadata.txt"),
                "finished_utc": now(),
            },
        )
    except Exception:
        write_json(
            output / "build-status.json",
            {
                "result": "fail",
                "identity": identity,
                "traceback": traceback.format_exc(),
                "finished_utc": now(),
            },
        )
        raise
    return output / "metadata.txt"


def run_task(args, job):
    task, stage, mask = job["task"], job["stage"], job.get("mask")
    for expected in (
        task["checkpoint"],
        args.common_identity["gem5"],
        args.common_identity["config"],
    ):
        verify_file(expected)
    output = run_path(args, task, stage, mask)
    mode = (
        "off"
        if stage == "baseline"
        else "collect"
        if stage == "collect"
        else "replay"
    )
    ideal = stage not in ("baseline", "finite")
    metadata_path = build_task_image(args, job) if mode == "replay" else None
    metadata = load_metadata(metadata_path, ideal) if metadata_path else None
    candidate_path = (
        candidates_path(args, task, stage) if mode != "off" else None
    )
    candidates = (
        load_candidates(candidate_path, ideal) if candidate_path else []
    )
    identity = {
        **args.common_identity,
        "task": task,
        "stage": stage,
        "mode": mode,
        "ideal": ideal,
        "mask": mask,
        "candidate_sha256": sha256(candidate_path) if candidate_path else None,
        "metadata_sha256": sha256(metadata_path) if metadata_path else None,
    }
    if output.exists():
        status_path = output / "status.json"
        if not args.resume or not status_path.exists():
            raise ValueError(f"existing/incomplete run preserved: {output}")
        status = json.loads(status_path.read_text())
        if (
            status.get("result") != "pass"
            or status.get("identity") != identity
        ):
            raise ValueError(
                f"run failed/running or identity differs; preserved: {output}"
            )
        audit = audit_run(
            output,
            0,
            100_000_000,
            50_000_000,
            1_000_000,
            mode,
            candidates,
            ideal,
            metadata,
            check_occupancy=True,
        )
        if audit["result"] != "pass":
            raise ValueError(
                f"previous pass no longer audits: {output}: {audit['errors']}"
            )
        return {"result": "pass", "output": str(output), "reused": True}
    output.mkdir(parents=True)
    command = build_command(args, task, stage, output, metadata_path, mask)
    (output / "cmd.txt").write_text(shlex.join(command) + "\n")
    write_json(output / "identity.json", identity)
    status = {
        "result": "running",
        "identity": identity,
        "started_utc": now(),
        "worker_pid": os.getpid(),
        "command": command,
    }
    write_json(output / "status.json", status)
    environment = os.environ.copy()
    if args.library_path:
        environment["LD_LIBRARY_PATH"] = args.library_path + (
            ":" + environment["LD_LIBRARY_PATH"]
            if environment.get("LD_LIBRARY_PATH")
            else ""
        )
    started = time.monotonic()
    with (output / "stdout.log").open("w") as stdout, (
        output / "stderr.log"
    ).open("w") as stderr:
        process = subprocess.Popen(
            command, stdout=stdout, stderr=stderr, env=environment
        )
        status["gem5_pid"] = process.pid
        write_json(output / "status.json", status)
        returncode = process.wait()
    status.update(
        audit_run(
            output,
            returncode,
            100_000_000,
            50_000_000,
            1_000_000,
            mode,
            candidates,
            ideal,
            metadata,
            check_occupancy=True,
        )
    )
    status.update(
        finished_utc=now(), elapsed_seconds=time.monotonic() - started
    )
    write_json(output / "status.json", status)
    if status["result"] == "pass" and stage == "collect":
        try:
            cache_dir = args.output_root / "profiles" / tag(task)
            cache = make_cache(output / "hint-profile.jsonl.gz", cache_dir)
            expected = sum(
                row["executions"]
                for row in status["hint"]["pcs"]
                if address(row["pc"]) in candidates
            )
            if cache["records"] != expected:
                raise ValueError(
                    "collect profile does not cover every candidate execution"
                )
            comparison = compare_shared_profiles(
                task["old"]["cache_directory"], cache_dir
            )
            write_json(output / "old-profile-comparison.json", comparison)
            if not comparison["pass"]:
                raise ValueError(
                    "new/old shared-PC samples differ; evidence preserved for investigation"
                )
            status["profile"] = cache
        except Exception:
            status.update(result="fail", errors=[traceback.format_exc()])
        write_json(output / "status.json", status)
    return {
        "result": status["result"],
        "output": str(output),
        "errors": status["errors"],
    }


def execute_stage(args, jobs, label):
    iterator = iter(jobs)
    results, failed = [], False
    with futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        active = {}
        for _ in range(min(args.jobs, len(jobs))):
            job = next(iterator)
            active[pool.submit(run_task, args, job)] = job
        while active:
            done, _ = futures.wait(active, return_when=futures.FIRST_COMPLETED)
            for future in done:
                job = active.pop(future)
                try:
                    result = future.result()
                except Exception:
                    result = {
                        "result": "fail",
                        "job": job,
                        "traceback": traceback.format_exc(),
                    }
                results.append(result)
                failed |= result["result"] != "pass"
                print(
                    json.dumps(
                        {
                            "utc": now(),
                            "stage": job["stage"],
                            "completed": len(results),
                            "total": len(jobs),
                            **result,
                        }
                    ),
                    flush=True,
                )
            if not failed:
                for _ in range(args.jobs - len(active)):
                    job = next(iterator, None)
                    if job is None:
                        break
                    active[pool.submit(run_task, args, job)] = job
            write_json(
                args.output_root / "progress.json",
                {
                    "stage": label,
                    "stage_completed": len(results),
                    "stage_total": len(jobs),
                    "active_slots": len(active),
                    "failed": sum(row["result"] != "pass" for row in results),
                    "updated_utc": now(),
                    "runner_pid": os.getpid(),
                    "full_run_target": FORMAL_RUNS,
                    "last_result": results[-1],
                },
            )
    write_json(
        args.output_root / f"{label}-audit.json",
        {
            "result": "fail" if failed else "pass",
            "completed": len(results),
            "total": len(jobs),
            "results": results,
            "finished_utc": now(),
        },
    )
    if failed:
        raise RuntimeError(
            "stage failed; no new jobs or automatic retries scheduled"
        )


def comparison_gates(args, tasks, *, finite=False, observer=False):
    reports = []
    for task in tasks:
        before = (
            args.old_root / "runs" / "joint" / tag(task)
            if finite
            else run_path(args, task, "baseline")
        )
        after = run_path(args, task, "finite" if finite else "collect")
        read_status(args, task, "finite" if finite else "collect")
        report = compare_observer(before, after, check_windows=True)
        reports.append({"task": tag(task), **report})
    name = "finite-regression" if finite else "observer"
    write_json(args.output_root / f"{name}-audit.json", reports)
    if not all(row["pass"] for row in reports):
        raise ValueError(
            f"{name} behavior differs; inspect preserved {name}-audit.json"
        )
    return reports


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in (
        "gem5",
        "config",
        "workload-list",
        "source-manifest",
        "output-root",
        "old-root",
    ):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--workload-root", type=Path)
    parser.add_argument("--build-audit", type=Path)
    parser.add_argument("--validation-audit", type=Path)
    parser.add_argument("--jobs", type=int, default=25)
    parser.add_argument("--library-path", default="")
    parser.add_argument("--disable-aslr", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--stage", choices=("all", *RUN_COUNTS, "audit"), default="all"
    )
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 25:
        parser.error("--jobs must be within the shared 25-slot budget")
    if not args.dry_run and (
        not args.build_audit or not args.validation_audit
    ):
        parser.error(
            "formal runs require final-binary --build-audit and --validation-audit"
        )
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    return args


def preflight(args):
    tasks = load_workloads(args.workload_list, args.workload_root)
    old_audit_path = args.old_root / "complete-audit.json"
    old_audit = json.loads(old_audit_path.read_text())
    if (
        old_audit.get("result") != "pass"
        or old_audit.get("full_runs") != 239
        or old_audit.get("window_count") != 11950
    ):
        raise ValueError(
            "old experiment must have its complete independent 239-run audit"
        )
    for task in tasks:
        task["old"] = old_inputs(args, task)
    args.command_prefix = legacy().command_prefix(args.disable_aslr)
    args.common_identity = {
        "gem5": file_identity(args.gem5),
        "config": file_identity(args.config),
        "source_root": str(args.gem5.parents[2]),
        "source_manifest": file_identity(args.source_manifest),
        "workload_list": file_identity(args.workload_list),
        "old_complete_audit": file_identity(old_audit_path),
        "warmup_insts": 100_000_000,
        "measure_insts": 50_000_000,
        "h2p_stat_interval": 1_000_000,
        "difftest": False,
        "mem_type": "DDR4_2400_8x8",
        "command_prefix": args.command_prefix,
        "library_path": args.library_path,
        "tools": {
            path.name: sha256(path)
            for path in sorted(Path(__file__).parent.glob("*.py"))
        },
    }
    for name in (
        "run-h2p-hint-ideal.py",
        "audit-h2p-hint-ideal.py",
        "run-cpython-h2p.py",
    ):
        args.common_identity["tools"][name] = sha256(
            Path(__file__).resolve().parents[1] / name
        )
    for name in ("build_audit", "validation_audit"):
        if getattr(args, name):
            args.common_identity[name] = file_identity(getattr(args, name))
    plan = {
        "format": "h2p-hint-ideal-experiment",
        "version": 3,
        "identity": args.common_identity,
        "tasks": tasks,
        "jobs": args.jobs,
        "full_runs": RUN_COUNTS,
        "expected_runs": FORMAL_RUNS,
        "expected_windows": FORMAL_WINDOWS,
        "qualification": QUALIFICATION,
    }
    if args.dry_run:
        return tasks, plan
    build = json.loads(args.build_audit.read_text())
    validate_runtime_gate(
        json.loads(args.validation_audit.read_text()), args.common_identity
    )
    if (
        build.get("returncode") != 0
        or not build.get("executable")
        or "ELF" not in build.get("file", "")
        or not build.get("build_finished_utc")
        or build.get("gem5_sha256") != args.common_identity["gem5"]["sha256"]
        or build.get("source_manifest_sha256")
        != args.common_identity["source_manifest"]["sha256"]
        or not os.access(args.gem5, os.X_OK)
    ):
        raise ValueError(
            "build audit must identify a completed successful final binary/source build"
        )
    with args.gem5.open("rb") as binary:
        if binary.read(4) != b"\x7fELF":
            raise ValueError("gem5 is not ELF")
    from .ideal_audit import audit_source_files

    audit_source_files(
        args.common_identity["source_root"],
        json.loads(args.source_manifest.read_text()),
    )
    return tasks, plan


def main(argv=None):
    args = parse_args(argv)
    tasks, plan = preflight(args)
    if args.dry_run:
        print(json.dumps(plan, indent=2, sort_keys=True))
        return 0
    if args.output_root.exists() and not args.resume:
        raise ValueError(
            "output root exists; select a new root or use strictly verified --resume"
        )
    args.output_root.mkdir(parents=True, exist_ok=args.resume)
    with (args.output_root / "runner.lock").open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("this experiment has an active runner") from error
        freeze_json(args.output_root / "experiment.json", plan)
        for name in ("build_audit", "validation_audit", "source_manifest"):
            source = getattr(args, name)
            destination = args.output_root / (name.replace("_", "-") + ".json")
            if destination.exists() and sha256(destination) != sha256(source):
                raise ValueError("frozen gate/source manifest content differs")
            if not destination.exists():
                shutil.copy2(source, destination)
        for task in tasks:
            finite = json.loads(
                Path(task["old"]["candidates"]["path"]).read_text()
            )
            freeze_json(candidates_path(args, task, "finite"), finite)
            fixed = candidate_document(
                [address(pc) for pc in task["old"]["masks"]],
                {"original_joint_image": task["old"]["image"]},
            )
            freeze_json(candidates_path(args, task, "fixed"), fixed)
        (args.output_root / "runner.pid").write_text(str(os.getpid()) + "\n")
        write_json(
            args.output_root / "runner-state.json",
            {
                "state": "running",
                "stage": args.stage,
                "pid": os.getpid(),
                "started_utc": now(),
                "command": sys.argv,
            },
        )
        try:
            if args.stage in ("all", "baseline"):
                execute_stage(
                    args,
                    [{"stage": "baseline", "task": task} for task in tasks],
                    "baseline",
                )
            if args.stage != "baseline":
                prepare_baseline_candidates(args, tasks)
            if args.stage in ("all", "finite"):
                execute_stage(
                    args,
                    [{"stage": "finite", "task": task} for task in tasks],
                    "finite",
                )
                comparison_gates(args, tasks, finite=True)
            if args.stage in ("all", "fixed", "collect"):
                comparison_gates(args, tasks, finite=True)
                stages = (
                    ("fixed", "collect")
                    if args.stage == "all"
                    else (args.stage,)
                )
                execute_stage(
                    args,
                    [
                        {"stage": stage, "task": task}
                        for stage in stages
                        for task in tasks
                    ],
                    "-".join(stages),
                )
            if args.stage in ("all", "collect", "masks", "joint", "audit"):
                comparison_gates(args, tasks, observer=True)
            if args.stage in ("all", "masks"):
                execute_stage(args, mask_jobs(args, tasks), "masks")
            if args.stage in ("all", "joint"):
                execute_stage(args, joint_jobs(args, tasks), "joint")
            if args.stage in ("all", "joint", "audit"):
                from .ideal_audit import audit_experiment
                from .ideal_report import final_report

                complete = audit_experiment(args.output_root, jobs=args.jobs)
                if complete["result"] != "pass":
                    raise ValueError(
                        "independent complete audit failed; results preserved"
                    )
                final_report(args, tasks, complete)
            write_json(
                args.output_root / "runner-state.json",
                {
                    "state": "complete",
                    "stage": args.stage,
                    "pid": os.getpid(),
                    "finished_utc": now(),
                },
            )
            return 0
        except Exception:
            write_json(
                args.output_root / "runner-state.json",
                {
                    "state": "failed",
                    "stage": args.stage,
                    "pid": os.getpid(),
                    "finished_utc": now(),
                    "traceback": traceback.format_exc(),
                },
            )
            raise


if __name__ == "__main__":
    raise SystemExit(main())
