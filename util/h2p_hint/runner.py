"""Persistent finite Hint heavy-workload experiment with a shared process budget."""

import argparse
import concurrent.futures as futures
import csv
import datetime as dt
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
from .model import LAYOUT, address, contract, load_candidates, sha256, write_json
from .profile import Cache, make_cache


HEAVY = [("scimark_fft", "26"), ("pickle_pure_python", "18"),
         ("pyflate", "44"), ("deltablue", "44"), ("regex_v8", "30")]


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def file_identity(path):
    path = Path(path).resolve()
    stat = path.stat()
    return {"path": str(path), "size": stat.st_size, "mtime_ns": stat.st_mtime_ns,
            "sha256": sha256(path)}


def load_workloads(workload_list, workload_root=None):
    specification = json.loads(Path(workload_list).read_text())
    root_text = str(workload_root or specification["workload_root"])
    if ":" in root_text:
        raise ValueError("host:path workload roots require an explicit --workload-root local mapping")
    root = Path(root_text).resolve()
    result = []
    for group in specification["cpts"]:
        if group.get("workload", "cpython") != "cpython":
            raise ValueError("this runner is restricted to the accepted CPython heavy list")
        scale = group.get("scale", "test")
        if scale != "test":
            raise ValueError("the heavy experiment uses scale=test")
        for checkpoint_id in group["cpt_list"]:
            checkpoint_id = str(checkpoint_id)
            checkpoint = legacy().checkpoint_for(root, group["bench"], scale, checkpoint_id)
            result.append({"bench": group["bench"], "scale": scale,
                           "checkpoint_id": checkpoint_id,
                           "checkpoint": file_identity(checkpoint)})
    if [(row["bench"], row["checkpoint_id"]) for row in result] != HEAVY:
        raise ValueError("heavy.json must contain exactly the five accepted slices in order")
    return result


def tag(task):
    return f"{task['bench']}_{task['scale']}/{task['checkpoint_id']}"


def input_candidates(args, task):
    if args.inputs_root:
        path = args.inputs_root / task["bench"] / task["checkpoint_id"] / "candidates.json"
        data = json.loads(path.read_text())
        load_candidates(path)
        return data
    path = args.stage1_root / tag(task) / "h2p-indirect.txt"
    return candidates_from_stage1(path, None)


def geometry_args():
    return [f"--h2p-hint-{name.replace('_', '-')}={value}" for name, value in LAYOUT.items()]


def validate_runtime_gate(gate, identity):
    if (gate.get("result") != "pass" or
            gate.get("gem5_sha256") != identity["gem5"]["sha256"] or
            gate.get("source_manifest_sha256") != identity["source_manifest"]["sha256"] or
            not isinstance(gate.get("checks"), (dict, list)) or not gate["checks"]):
        raise ValueError("validation audit must pass final-binary gates for this exact binary/source")


def build_command(args, task, mode, output, metadata=None):
    command = args.command_prefix + [str(args.gem5), "-d", str(output), str(args.config),
        f"--generic-rv-cpt={task['checkpoint']['path']}",
        f"--warmup-insts-no-switch={args.warmup_insts}",
        f"--maxinsts={args.warmup_insts + args.measure_insts}",
        "--disable-difftest", "--mem-type=DDR4_2400_8x8", "--h2p-stat",
        f"--h2p-stat-interval={args.h2p_stat_interval}", f"--h2p-hint-mode={mode}"]
    if mode != "off":
        command += [f"--h2p-hint-candidates={args.output_root / 'inputs' / tag(task) / 'candidates.json'}"]
        command += geometry_args()
    if metadata:
        command += [f"--h2p-hint-metadata={metadata}"]
    return command


def run_path(args, task, stage, group=None, mask=None):
    output = args.output_root / "runs" / stage / tag(task)
    if stage == "masks":
        output = output / f"group-{group}" / f"mask-{mask:02d}"
    return output


def image_path(args, task, stage, group=None, mask=None):
    output = args.output_root / "images" / stage / tag(task)
    if stage == "masks":
        output = output / f"group-{group}" / f"mask-{mask:02d}"
    return output


def finish_cycle(args, task, cache):
    collected = json.loads((run_path(args, task, "collect") / "status.json").read_text())
    return (collected["phases"]["measure_complete"]["tick"] //
            cache.identity["header"]["clock_period"])


def build_task_image(args, task, stage, masks, *, group=None, mask=None, choices=None):
    output = image_path(args, task, stage, group, mask)
    cache = Cache(args.output_root / "profiles" / tag(task))
    identity = {"profile_sha256": cache.identity["profile_sha256"],
                "gem5_sha256": args.common_identity["gem5"]["sha256"],
                "source_manifest_sha256": args.common_identity["source_manifest"]["sha256"],
                "candidate_sha256": task["candidate_sha256"],
                "masks": {hex(pc): value for pc, value in sorted(masks.items())},
                "fixed_mask": mask, "group": group,
                "qualification": "same-slice offline specialization"}
    if output.exists():
        status_path = output / "build-status.json"
        if not args.resume or not status_path.exists():
            raise ValueError(f"existing/incomplete image is preserved: {output}")
        status = json.loads(status_path.read_text())
        if status.get("result") != "pass" or status.get("identity") != identity:
            raise ValueError(f"image identity/status differs: {output}")
        if sha256(output / "metadata.txt") != status["metadata_sha256"]:
            raise ValueError(f"image content changed: {output}")
        return output / "metadata.txt"
    output.mkdir(parents=True)
    try:
        active_masks = dict(masks)
        # Dropping an unmapped joint PC can expose a later CAM candidate.
        # Rebuild under the final CAM until removing empty PCs reaches a fixed point.
        while True:
            metadata, report = build_image(cache.rows(), active_masks, source=identity,
                                           prune_cam=stage == "joint")
            remaining = {address(row["pc"]): row["mask"] for row in metadata["cam"]}
            if stage != "joint" or remaining == active_masks:
                break
            active_masks = remaining
        reference = cache.reference(metadata, finish_cycle(args, task, cache))
        write_json(output / "metadata.txt", metadata)
        write_json(output / "builder.json", report)
        write_json(output / "reference.json", reference)
        with (output / "hard-pred.txt").open("w") as destination:
            for row in choices or []:
                if row["reason"] != "selected":
                    destination.write(json.dumps(row, sort_keys=True) + "\n")
            for row in report["pcs"]:
                if row["reasons"]:
                    destination.write(json.dumps(row, sort_keys=True) + "\n")
            for pc in sorted(set(masks) - set(active_masks)):
                destination.write(json.dumps({"pc": hex(pc), "reason": "no_mapping_after_joint_capacity"}) + "\n")
        write_json(output / "build-status.json", {"result": "pass", "identity": identity,
                   "metadata_sha256": sha256(output / "metadata.txt"), "finished_utc": now()})
    except Exception:
        write_json(output / "build-status.json", {"result": "fail", "identity": identity,
                   "traceback": traceback.format_exc(), "finished_utc": now()})
        raise
    return output / "metadata.txt"


def run_task(args, job):
    task, stage = job["task"], job["stage"]
    for expected in (task["checkpoint"], args.common_identity["gem5"],
                     args.common_identity["config"]):
        stat = Path(expected["path"]).stat()
        if (stat.st_size, stat.st_mtime_ns) != (expected["size"], expected["mtime_ns"]):
            raise ValueError(f"run input changed after preflight: {expected['path']}")
    group, mask = job.get("group"), job.get("mask")
    output = run_path(args, task, stage, group, mask)
    mode = {"baseline": "off", "collect": "collect", "masks": "replay", "joint": "replay"}[stage]
    metadata = None
    if stage in ("masks", "joint"):
        masks = job["masks"]
        metadata = build_task_image(args, task, stage, masks, group=group, mask=mask,
                                    choices=job.get("choices"))
    identity = {**args.common_identity, "task": task, "stage": stage,
                "mode": mode, "group": group, "mask": mask,
                "metadata_sha256": sha256(metadata) if metadata else None}
    candidates = [address(pc) for pc in task["candidates"]["pcs"]]
    if output.exists():
        status_path = output / "status.json"
        if not args.resume or not status_path.exists():
            raise ValueError(f"existing/incomplete run is preserved: {output}")
        status = json.loads(status_path.read_text())
        if status.get("result") != "pass" or status.get("identity") != identity:
            raise ValueError(f"run failed/running or has different identity; preserved: {output}")
        audit = audit_run(output, 0, args.warmup_insts, args.measure_insts,
                          args.h2p_stat_interval, mode, candidates)
        if audit["result"] != "pass":
            raise ValueError(f"previous pass no longer audits: {output}: {audit['errors']}")
        return {"result": "pass", "output": str(output), "reused": True}
    output.mkdir(parents=True)
    command = build_command(args, task, mode, output, metadata)
    (output / "cmd.txt").write_text(shlex.join(command) + "\n")
    write_json(output / "identity.json", identity)
    status = {"result": "running", "identity": identity, "started_utc": now(),
              "worker_pid": os.getpid(), "command": command}
    write_json(output / "status.json", status)
    environment = os.environ.copy()
    if args.library_path:
        environment["LD_LIBRARY_PATH"] = args.library_path + (
            ":" + environment["LD_LIBRARY_PATH"] if environment.get("LD_LIBRARY_PATH") else "")
    started = time.monotonic()
    with (output / "stdout.log").open("w") as stdout, (output / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=environment)
        status["gem5_pid"] = process.pid
        write_json(output / "status.json", status)
        returncode = process.wait()
    status.update(audit_run(output, returncode, args.warmup_insts, args.measure_insts,
                            args.h2p_stat_interval, mode, candidates))
    status.update(finished_utc=now(), elapsed_seconds=time.monotonic() - started)
    write_json(output / "status.json", status)
    if status["result"] == "pass" and stage == "collect":
        try:
            cache = make_cache(output / "hint-profile.jsonl.gz", args.output_root / "profiles" / tag(task))
            expected = sum(row["executions"] for row in status["hint"]["pcs"]
                           if address(row["pc"]) in candidates)
            if cache["records"] != expected:
                raise ValueError("collect profile does not cover every candidate execution")
            status["profile"] = cache
        except Exception:
            status.update(result="fail", errors=[traceback.format_exc()])
        write_json(output / "status.json", status)
    return {"result": status["result"], "output": str(output), "errors": status["errors"]}


def execute_stage(args, jobs, label=None):
    """One worker owns one slot for either Python build/reference or gem5.

    Only jobs slots are submitted at once. After a failure, already-running
    work finishes and no new jobs start. There is no automatic retry.
    """
    iterator = iter(jobs)
    label = label or jobs[0]["stage"]
    completed, failed, results = 0, False, []
    started = now()
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
                    result = {"result": "fail", "job": job, "traceback": traceback.format_exc()}
                results.append(result)
                completed += 1
                failed |= result["result"] != "pass"
                print(json.dumps({"utc": now(), "stage": job["stage"],
                                  "completed": completed, "total": len(jobs), **result}), flush=True)
            if not failed:
                for _ in range(args.jobs - len(active)):
                    job = next(iterator, None)
                    if job is None:
                        break
                    active[pool.submit(run_task, args, job)] = job
            write_json(args.output_root / "progress.json", {
                "stage": label, "stage_completed": completed,
                "stage_total": len(jobs), "active_slots": len(active),
                "failed": sum(row["result"] != "pass" for row in results),
                "started_utc": started, "updated_utc": now(), "runner_pid": os.getpid(),
                "last_result": results[-1],
                "full_run_target": sum(args.run_counts.values())})
    write_json(args.output_root / f"{label}-audit.json", {
        "result": "fail" if failed else "pass", "completed": completed,
        "total": len(jobs), "results": results, "finished_utc": now()})
    if failed:
        raise RuntimeError("stage failed; evidence preserved, no new jobs or retries scheduled")


def ensure_pass(args, task, stage):
    status = json.loads((run_path(args, task, stage) / "status.json").read_text())
    if status.get("result") != "pass":
        raise ValueError(f"required {stage} run is not a pass: {tag(task)}")
    return status


def audit_observers(args, tasks):
    reports = []
    for task in tasks:
        ensure_pass(args, task, "baseline")
        ensure_pass(args, task, "collect")
        report = compare_observer(run_path(args, task, "baseline"), run_path(args, task, "collect"))
        reports.append({"task": tag(task), **report})
    write_json(args.output_root / "observer-audit.json", reports)
    if not all(report["pass"] for report in reports):
        raise ValueError("off/collect behavior differs; see observer-audit.json")


def mask_jobs(tasks):
    jobs = []
    for task in tasks:
        pcs = [address(pc) for pc in task["candidates"]["pcs"]]
        for group, start in enumerate(range(0, len(pcs), 16)):
            for mask in range(32):
                jobs.append({"stage": "masks", "task": task, "group": group, "mask": mask,
                             "masks": {pc: mask for pc in pcs[start:start + 16]}})
    return jobs


def planned_run_counts(tasks, masks):
    return {"baseline": len(tasks), "collect": len(tasks),
            "masks": len(masks), "joint": len(tasks)}


def joint_jobs(args, tasks, masks):
    jobs = []
    for task in tasks:
        replays = []
        for job in masks:
            if tag(job["task"]) != tag(task):
                continue
            status = json.loads((run_path(args, task, "masks", job["group"], job["mask"]) /
                                 "status.json").read_text())
            if status.get("result") != "pass":
                raise ValueError("mask sweep is incomplete")
            stats = dict(status["hint"])
            stats["pcs"] = [row for row in stats["pcs"] if address(row["pc"]) in job["masks"]]
            replays.append((job["mask"], stats))
        selected, choices = select_joint(replays)
        write_json(args.output_root / "inputs" / tag(task) / "best-masks.json", choices)
        jobs.append({"stage": "joint", "task": task, "masks": selected, "choices": choices})
    return jobs


def final_report(args, tasks):
    masks = mask_jobs(tasks)
    expected_runs = sum(args.run_counts.values())
    rows = []
    for task in tasks:
        baseline, joint = (ensure_pass(args, task, stage) for stage in ("baseline", "joint"))
        image = image_path(args, task, "joint")
        metadata = json.loads((image / "metadata.txt").read_text())
        improved = joint["final_misses"] < baseline["final_misses"]
        recommendation = metadata if improved else {
            "format": "h2p-hint", **contract(), "cam": [], "slots": [], "entries": [],
            "source": {"reason": "joint final misses did not improve baseline",
                       "evaluated_image_sha256": sha256(image / "metadata.txt")}}
        write_json(image / "recommended-metadata.txt", recommendation)
        rows.append({"task": tag(task), "baseline_misses": baseline["final_misses"],
                     "joint_misses": joint["final_misses"],
                     "net_miss_reduction": baseline["final_misses"] - joint["final_misses"],
                     "baseline_ipc": baseline["ipc"], "joint_ipc": joint["ipc"],
                     "recommended": "joint" if improved else "disabled",
                     "metadata": str(image / "metadata.txt"),
                     "recommended_metadata": str(image / "recommended-metadata.txt"),
                     "queue": joint["hint"]["queue"],
                     "loaded_pcs": len(metadata["cam"]), "slots": len(metadata["slots"]),
                     "entries": len(metadata["entries"])})
    completed = list((args.output_root / "runs").rglob("status.json"))
    statuses = [json.loads(path.read_text()) for path in completed]
    if len(statuses) != expected_runs or any(row.get("result") != "pass" for row in statuses):
        raise ValueError(f"final audit requires all {expected_runs} successful full runs")
    summary = {"format": "h2p-hint-experiment-summary", "version": 2,
               "qualification": "same-slice offline specialization",
               "full_runs": len(statuses), "pass": len(statuses), "fail": 0,
               "window_count": sum(row["window_count"] for row in statuses),
               "finished_utc": now(), "results": rows}
    write_json(args.output_root / "summary.json", summary)
    # These are observations, not the sum of independently optimized PC gains.
    with (args.output_root / "sweep.csv").open("w", newline="") as stream:
        fields = ["task", "group", "mask", "pc", "executions", "baseline_misses",
                  "final_misses", "corrected", "harmed", "net_correction", "eligible",
                  "tag_hits", "conf_enabled", "overrides", "reference_net_correction",
                  "ipc", "queue_dropped"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for task in tasks:
            for job in masks:
                if tag(job["task"]) != tag(task):
                    continue
                status = json.loads((run_path(args, task, "masks", job["group"], job["mask"]) /
                                     "status.json").read_text())
                reference = json.loads((image_path(args, task, "masks", job["group"], job["mask"]) /
                                        "reference.json").read_text())
                reference_pcs = {address(row["pc"]): row for row in reference["pcs"]}
                for row in status["hint"]["pcs"]:
                    pc = address(row["pc"])
                    if pc not in job["masks"]:
                        continue
                    ref = reference_pcs.get(pc, {})
                    writer.writerow({"task": tag(task), "group": job["group"], "mask": job["mask"],
                        **{name: row[name] for name in fields if name in row},
                        "net_correction": row["corrected"] - row["harmed"],
                        "reference_net_correction": ref.get("corrected", 0) - ref.get("harmed", 0),
                        "ipc": status["ipc"], "queue_dropped": status["hint"]["queue"]["dropped"]})
    checker = legacy()
    with (args.output_root / "joint-window-trends.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["task", "measure_start_inst", "pc", "baseline_misses",
                         "joint_misses", "net_miss_reduction"])
        for task in tasks:
            for offset in range(0, args.measure_insts, args.h2p_stat_interval):
                base = checker.parse_h2p_output(run_path(args, task, "baseline") /
                                                "h2p-indirec" / f"{offset}.txt")
                joint = checker.parse_h2p_output(run_path(args, task, "joint") /
                                                 "h2p-indirec" / f"{offset}.txt")
                for pc in sorted(base.keys() | joint.keys()):
                    before, after = base.get(pc, (0,))[0], joint.get(pc, (0,))[0]
                    writer.writerow([tag(task), offset, hex(pc), before, after, before - after])
    warmup_m = args.warmup_insts // 1_000_000
    measure_m = args.measure_insts // 1_000_000
    interval_m = args.h2p_stat_interval // 1_000_000
    mask_count = len({job["mask"] for job in masks})
    lines = ["# H2P Hint 第二轮 profiling\n",
             f"本轮为同片离线特化：restore 完成后 {warmup_m}M warm-up，reset 后 {measure_m}M measure；",
             f"interval={interval_m}M，difftest 关闭，DDR4_2400_8x8。{expected_runs} 次完整运行及窗口审计通过。\n",
             "| 切片 | baseline miss | joint miss | miss 减少 | baseline IPC | joint IPC | 推荐 |",
             "|---|---:|---:|---:|---:|---:|---|"]
    for row in rows:
        lines.append(f"| {row['task']} | {row['baseline_misses']} | {row['joint_misses']} | "
                     f"{row['net_miss_reduction']} | {row['baseline_ipc']} | {row['joint_ipc']} | "
                     f"{row['recommended']} |")
    lines += ["", f"每 PC 的 {mask_count}-mask 真实重放、参考模型差异、查询覆盖和队列丢弃见 `sweep.csv`。",
              "每片最佳 mask 与联合选入原因见 `inputs/<slice>/best-masks.json`；",
              "联合容量、target 共享、硬件别名与失败原因见 `images/joint/<slice>/builder.json` 和 `hard-pred.txt`。",
              f"逐 PC 的 {interval_m}M miss 趋势见 `joint-window-trends.csv`；全部 target/计数仍保留在原始运行目录。",
              "reference 使用 collect 的上下文与周期，不预测 Hint 改变后的错误路径与性能。",
              "推荐仅按总最终 miss 判定，IPC 单独展示；disabled 推荐不会删除已完成的退化实验。"]
    (args.output_root / "report.md").write_text("\n".join(lines) + "\n")
    return summary


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("gem5", "config", "workload-list", "output-root", "source-manifest"):
        parser.add_argument("--" + option, required=True, type=Path)
    parser.add_argument("--build-audit", type=Path)
    parser.add_argument("--validation-audit", type=Path)
    parser.add_argument("--workload-root", type=Path)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--inputs-root", type=Path)
    group.add_argument("--stage1-root", type=Path)
    parser.add_argument("--jobs", type=int, default=25)
    parser.add_argument("--warmup-insts", type=int, default=100_000_000)
    parser.add_argument("--measure-insts", type=int, default=50_000_000)
    parser.add_argument("--h2p-stat-interval", type=int, default=1_000_000)
    parser.add_argument("--library-path", default="")
    parser.add_argument("--disable-aslr", action="store_true")
    parser.add_argument("--stage", choices=("all", "baseline", "collect", "masks", "joint"), default="all")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 25:
        parser.error("--jobs must be between 1 and the authorized 25-slot limit")
    if (args.warmup_insts, args.measure_insts, args.h2p_stat_interval) not in (
            (100_000_000, 50_000_000, 1_000_000),
            (1_000_000, 1_000_000, 1_000_000)):
        parser.error("supported runs require 100M/50M or 1M/1M warmup/measure, interval 1M")
    if not args.dry_run and not args.build_audit:
        parser.error("--build-audit is required before starting gem5")
    if not args.dry_run and not args.validation_audit:
        parser.error("--validation-audit is required before starting the formal experiment")
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    return args


def main(argv=None):
    args = parse_args(argv)
    tasks = load_workloads(args.workload_list, args.workload_root)
    for task in tasks:
        task["candidates"] = input_candidates(args, task)
        canonical = (json.dumps(task["candidates"], sort_keys=True, indent=2) + "\n").encode()
        task["candidate_sha256"] = hashlib.sha256(canonical).hexdigest()
    jobs = mask_jobs(tasks)
    args.run_counts = planned_run_counts(tasks, jobs)
    args.command_prefix = legacy().command_prefix(args.disable_aslr)
    args.common_identity = {
        "gem5": file_identity(args.gem5), "config": file_identity(args.config),
        "source_manifest": file_identity(args.source_manifest),
        "workload_list": file_identity(args.workload_list),
        "warmup_insts": args.warmup_insts, "measure_insts": args.measure_insts,
        "h2p_stat_interval": args.h2p_stat_interval, "layout": LAYOUT,
        "difftest": False, "mem_type": "DDR4_2400_8x8",
        "command_prefix": args.command_prefix, "library_path": args.library_path,
        "tools": {path.name: sha256(path) for path in sorted(Path(__file__).parent.glob("*.py"))},
    }
    for filename in ("run-h2p-hint.py", "run-cpython-h2p.py"):
        args.common_identity["tools"][filename] = sha256(Path(__file__).resolve().parents[1] / filename)
    for name in ("build_audit", "validation_audit"):
        if getattr(args, name):
            args.common_identity[name] = file_identity(getattr(args, name))
    plan = {"identity": args.common_identity, "jobs": args.jobs, "tasks": tasks,
            "full_runs": args.run_counts,
            "qualification": "same-slice offline specialization"}
    if args.dry_run:
        print(json.dumps(plan, indent=2, sort_keys=True))
        return 0
    build = json.loads(args.build_audit.read_text())
    validate_runtime_gate(json.loads(args.validation_audit.read_text()), args.common_identity)
    if (build.get("returncode") != 0 or not build.get("executable") or
            "ELF" not in build.get("file", "") or not build.get("build_finished_utc") or
            build.get("gem5_sha256") != args.common_identity["gem5"]["sha256"] or
            build.get("source_manifest_sha256") != args.common_identity["source_manifest"]["sha256"] or
            not os.access(args.gem5, os.X_OK)):
        raise ValueError("build audit does not identify a completed successful build of this binary/source")
    with args.gem5.open("rb") as binary:
        if binary.read(4) != b"\x7fELF":
            raise ValueError("gem5 is not an ELF executable")
    if args.output_root.exists() and not args.resume:
        raise ValueError("output root exists; use a new root or --resume for verified passes")
    args.output_root.mkdir(parents=True, exist_ok=args.resume)
    lock = (args.output_root / "runner.lock").open("a+")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        raise ValueError("this output root already has an active runner") from error
    manifest_path = args.output_root / "experiment.json"
    if manifest_path.exists():
        if json.loads(manifest_path.read_text()) != plan:
            raise ValueError("resume experiment identity differs")
    else:
        write_json(manifest_path, plan)
        shutil.copy2(args.build_audit, args.output_root / "build-audit.json")
        shutil.copy2(args.validation_audit, args.output_root / "validation-audit.json")
        shutil.copy2(args.source_manifest, args.output_root / "source-manifest.json")
        for task in tasks:
            directory = args.output_root / "inputs" / tag(task)
            directory.mkdir(parents=True)
            write_json(directory / "candidates.json", task["candidates"])
    (args.output_root / "runner.pid").write_text(str(os.getpid()) + "\n")
    write_json(args.output_root / "runner-state.json", {"state": "running", "stage": args.stage,
               "pid": os.getpid(), "started_utc": now(), "command": sys.argv})
    try:
        if args.stage == "all":
            execute_stage(args, [{"stage": stage, "task": task}
                for stage in ("baseline", "collect") for task in tasks],
                label="baseline-collect")
            audit_observers(args, tasks)
        if args.stage == "baseline":
            execute_stage(args, [{"stage": "baseline", "task": task} for task in tasks])
        if args.stage == "collect":
            for task in tasks:
                ensure_pass(args, task, "baseline")
            execute_stage(args, [{"stage": "collect", "task": task} for task in tasks])
            audit_observers(args, tasks)
        if args.stage in ("all", "masks"):
            audit_observers(args, tasks)
            execute_stage(args, jobs)
        if args.stage in ("all", "joint"):
            execute_stage(args, joint_jobs(args, tasks, jobs))
            final_report(args, tasks)
        write_json(args.output_root / "runner-state.json", {"state": "complete", "stage": args.stage,
                   "pid": os.getpid(), "finished_utc": now()})
        return 0
    except Exception:
        write_json(args.output_root / "runner-state.json", {"state": "failed", "stage": args.stage,
                   "pid": os.getpid(), "finished_utc": now(), "traceback": traceback.format_exc()})
        raise
    finally:
        lock.close()


if __name__ == "__main__":
    raise SystemExit(main())
