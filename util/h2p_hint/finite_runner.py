"""Audited 1M+1M finite-v3 Hint experiment for five CPython checkpoints."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import fcntl
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
import traceback

from .audit import audit_run, compare_observer, legacy
from .finite_v3 import (build_image, choose_best, contract, digest,
                        merge_profiles, read_profile, score_image, baseline_miss)
from .model import address, write_json
from .runner import file_identity, load_workloads, now, tag, validate_runtime_gate


def ranked_candidates(baseline):
    """Rank this run's indirect branches by measured target misses."""
    data = legacy().parse_h2p_output(baseline / "h2p-indirect.txt")
    ranked = sorted(((pc, values[0]) for pc, values in data.items() if values[0] > 0),
                    key=lambda item: (-item[1], item[0]))[:20]
    return [pc for pc, _ in ranked], ranked


def group_paths(root, task, group):
    return root / "runs" / "collect" / tag(task) / f"group-{group}"


def run_path(root, task, stage):
    return root / "runs" / stage / tag(task)


def _identity(args, task, stage, *, group=None, candidate=None, metadata=None):
    result = {"common": args.identity, "checkpoint": task["checkpoint"],
              "task": tag(task), "stage": stage, "group": group}
    if candidate is not None:
        result["candidate_sha256"] = digest(candidate)
    if metadata is not None:
        result["metadata_sha256"] = digest(metadata)
    return result


def _command(args, task, stage, output, candidate=None, metadata=None):
    mode = {"baseline": "off", "collect": "collect", "joint": "replay"}[stage]
    command = args.command_prefix + [str(args.gem5), "-d", str(output), str(args.config),
        f"--generic-rv-cpt={task['checkpoint']['path']}",
        "--warmup-insts-no-switch=1000000", "--maxinsts=2000000",
        "--disable-difftest", "--mem-type=DDR4_2400_8x8", "--h2p-stat",
        "--h2p-stat-interval=1000000", f"--h2p-hint-mode={mode}"]
    if candidate is not None:
        command.append(f"--h2p-hint-candidates={candidate}")
        command.extend(f"--h2p-hint-{name.replace('_', '-')}={contract.LAYOUT[name]}"
                       for name in ("cam_entries", "context_entries", "ways",
                                    "slot_entries", "conf_queue_depth"))
    if metadata is not None:
        command.append(f"--h2p-hint-metadata={metadata}")
    return command


def run_one(args, task, stage, *, group=None, candidate=None, metadata=None):
    output = (group_paths(args.output_root, task, group) if stage == "collect"
              else run_path(args.output_root, task, stage))
    identity = _identity(args, task, stage, group=group,
                         candidate=candidate, metadata=metadata)
    if stage == "joint":
        if candidate is None or metadata is None:
            raise ValueError("joint replay requires audited candidate and metadata")
        image_dir = metadata.parent
        image = image_dir / "hint-v3.bin"
        data = json.loads(metadata.read_text())
        source = data["source"]
        groups = sorted((args.output_root / "runs" / "collect" / tag(task)).glob("group-*"))
        profiles = [path / "hint-profile.jsonl.gz" for path in groups]
        if (source != _source(args, task, profile_identity(profiles)) or
                contract.pack_image(data) != image.read_bytes() or
                digest(metadata) != json.loads((image_dir / "build-status.json").read_text())["metadata_sha256"] or
                [entry["pc"] for entry in data["cam"]] != json.loads(candidate.read_text())["pcs"]):
            raise ValueError("joint replay provenance/image/candidates audit failed")
        identity["image_sha256"] = digest(image)
        identity["profile_sha256"] = source["profile_sha256"]
    checkpoint = Path(task["checkpoint"]["path"])
    if digest(checkpoint) != task["checkpoint"]["sha256"]:
        raise ValueError(f"checkpoint changed: {checkpoint}")
    if output.exists():
        if not args.resume:
            raise ValueError(f"existing run is preserved: {output}")
        status_path = output / "status.json"
        if not status_path.exists():
            raise ValueError(f"incomplete run is preserved: {output}")
        status = json.loads(status_path.read_text())
        if status.get("result") != "pass" or status.get("identity") != identity:
            raise ValueError(f"run identity/status differs: {output}")
        candidates = [] if candidate is None else [address(pc) for pc in
            json.loads(candidate.read_text())["pcs"]]
        verified = audit_run(output, 0, 1_000_000, 1_000_000, 1_000_000,
                             {"baseline": "off", "collect": "collect",
                              "joint": "replay"}[stage], candidates,
                             finite_v3=True, metadata=data if stage == "joint" else None,
                             check_occupancy=stage != "baseline")
        if verified["result"] != "pass":
            raise ValueError(f"reused run no longer audits: {output}: {verified['errors']}")
        return status
    output.mkdir(parents=True)
    command = _command(args, task, stage, output, candidate, metadata)
    (output / "cmd.txt").write_text(shlex.join(command) + "\n")
    write_json(output / "identity.json", identity)
    status = {"result": "running", "identity": identity, "command": command,
              "started_utc": now()}
    write_json(output / "status.json", status)
    environment = os.environ.copy()
    if args.library_path:
        environment["LD_LIBRARY_PATH"] = args.library_path + (
            ":" + environment["LD_LIBRARY_PATH"] if environment.get("LD_LIBRARY_PATH") else "")
    start = time.monotonic()
    with (output / "stdout.log").open("w") as stdout, (output / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=environment)
        status["gem5_pid"] = process.pid
        write_json(output / "status.json", status)
        returncode = process.wait()
    candidates = [] if candidate is None else [address(pc) for pc in
        json.loads(candidate.read_text())["pcs"]]
    status.update(audit_run(output, returncode, 1_000_000, 1_000_000, 1_000_000,
                            {"baseline": "off", "collect": "collect",
                             "joint": "replay"}[stage], candidates,
                            finite_v3=True, metadata=data if stage == "joint" else None,
                            check_occupancy=stage != "baseline"))
    status.update(finished_utc=now(), elapsed_seconds=time.monotonic() - start)
    write_json(output / "status.json", status)
    if status["result"] != "pass":
        raise RuntimeError(f"simulation audit failed: {output}: {status['errors']}")
    return status


def candidate_file(args, task, group, pcs, ranking):
    path = args.output_root / "inputs" / tag(task) / f"candidates-{group}.json"
    data = {"format": "h2p-hint-candidates", "version": 3,
            "storage": "finite", "pcs": [hex(pc) for pc in pcs],
            "source": {"baseline_h2p_sha256": digest(run_path(args.output_root, task, "baseline") /
                                                       "h2p-indirect.txt"),
                       "ranking": [{"pc": hex(pc), "misses": misses} for pc, misses in ranking]}}
    if path.exists():
        if json.loads(path.read_text()) != data:
            raise ValueError(f"candidate identity changed: {path}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        write_json(path, data)
    return path


def profile_identity(paths):
    # Order is physical group order. Include each compressed trace digest.
    return hashlib.sha256("\n".join(digest(path) for path in paths).encode()).hexdigest()


def _source(args, task, profile_hash):
    return {"workload_id": f"{task['bench']}_{task['scale']}/{task['checkpoint_id']}",
            "checkpoint_sha256": task["checkpoint"]["sha256"],
            "profile_sha256": profile_hash,
            "builder_sha256": digest(Path(__file__).with_name("finite_v3.py"))}


def build_and_score(args, task, rows, pcs, source):
    """Offline 16-mask scan; all scores are fixed-conf=3 trace estimates."""
    directory = args.output_root / "images" / "joint" / tag(task)
    if directory.exists():
        if not args.resume:
            raise ValueError(f"existing image is preserved: {directory}")
        metadata_path = directory / "metadata.json"
        status = json.loads((directory / "build-status.json").read_text())
        if (status.get("result") != "pass" or
                status.get("source") != source or
                digest(metadata_path) != status.get("metadata_sha256") or
                digest(directory / "hint-v3.bin") != status.get("image_sha256")):
            raise ValueError(f"existing image audit differs: {directory}")
        return metadata_path
    directory.mkdir(parents=True)
    scores = []
    try:
        for start in range(0, len(pcs), 16):
            group = pcs[start:start + 16]
            for mask in range(16):
                metadata, _, _ = build_image(rows, {pc: mask for pc in group}, source)
                observed = {int(entry["pc"], 16): entry for entry in
                            score_image(rows, metadata)["pcs"]}
                for pc in group:
                    score = observed.get(pc, {"corrected": 0, "harmed": 0})
                    scores.append((pc, mask, score))
        selected, choices = choose_best(scores)
        # Removing unmapped PCs can change fetch-window CAM arbitration.
        while True:
            metadata, image, report = build_image(rows, selected, source)
            active = {int(entry["pc"], 16): entry["mask"] for entry in metadata["cam"]}
            if active == selected:
                break
            selected = active
        for choice in choices:
            if choice["reason"] == "selected" and int(choice["pc"], 16) not in active:
                choice["reason"] = "no_mapping_after_joint_capacity"
            choice["loaded_candidate"] = int(choice["pc"], 16) in active
        estimate = score_image(rows, metadata)
        write_json(directory / "metadata.json", metadata)
        (directory / "hint-v3.bin").write_bytes(image)
        write_json(directory / "builder.json", report)
        write_json(directory / "best-masks.json", choices)
        write_json(directory / "fixed-conf3-estimate.json", estimate)
        write_json(directory / "candidates-joint.json", {
            "format": "h2p-hint-candidates", "version": 3, "storage": "finite",
            "pcs": [entry["pc"] for entry in metadata["cam"]],
            "source": {"metadata_sha256": digest(directory / "metadata.json")}})
        with (directory / "mask-sweep.jsonl").open("w") as stream:
            for pc, mask, score in scores:
                stream.write(json.dumps({"pc": hex(pc), "mask": mask, **score},
                                        sort_keys=True) + "\n")
        with (directory / "hard-pred.txt").open("w") as stream:
            for entry in choices:
                if entry["reason"] != "selected":
                    stream.write(json.dumps(entry, sort_keys=True) + "\n")
            for entry in report["pcs"]:
                if entry["reasons"]:
                    stream.write(json.dumps(entry, sort_keys=True) + "\n")
        status = {"result": "pass", "source": source,
                  "metadata_sha256": digest(directory / "metadata.json"),
                  "image_sha256": digest(directory / "hint-v3.bin"),
                  "mask_trials": len(scores), "finished_utc": now()}
        write_json(directory / "build-status.json", status)
    except Exception:
        write_json(directory / "build-status.json", {"result": "fail",
                   "source": source, "traceback": traceback.format_exc()})
        raise
    return directory / "metadata.json"


def report(args, tasks):
    def measure_cycles(task, stage, status):
        config = (run_path(args.output_root, task, stage) / "config.ini").read_text()
        match = re.search(r"\[system\.cpu_clk_domain\][^\[]*\nclock=(\d+)", config)
        if not match:
            raise ValueError(f"CPU clock period absent: {tag(task)} {stage}")
        phases = status["phases"]
        ticks = phases["measure_complete"]["tick"] - phases["warmup_complete"]["tick"]
        if ticks < 0 or ticks % int(match.group(1)):
            raise ValueError("measure ticks do not align to CPU cycles")
        return ticks // int(match.group(1))

    rows = []
    for task in tasks:
        base = json.loads((run_path(args.output_root, task, "baseline") / "status.json").read_text())
        replay = json.loads((run_path(args.output_root, task, "joint") / "status.json").read_text())
        image = args.output_root / "images" / "joint" / tag(task)
        metadata = json.loads((image / "metadata.json").read_text())
        estimate = json.loads((image / "fixed-conf3-estimate.json").read_text())
        pcs = replay["hint"]["pcs"]
        conf0 = run_path(args.output_root, task, "joint") / "conf0.txt"
        trace = run_path(args.output_root, task, "joint") / "hint-replay.jsonl.gz"
        rows.append({"task": tag(task), "baseline_misses": base["final_misses"],
                     "hint_misses": replay["final_misses"],
                     "net_miss_reduction": base["final_misses"] - replay["final_misses"],
                     "baseline_ipc": base["ipc"], "hint_ipc": replay["ipc"],
                     "baseline_cycles": measure_cycles(task, "baseline", base),
                     "hint_cycles": measure_cycles(task, "joint", replay),
                     "baseline_ticks": base["sim_ticks"], "hint_ticks": replay["sim_ticks"],
                     "corrected": sum(row["corrected"] for row in pcs),
                     "harmed": sum(row["harmed"] for row in pcs),
                     "queue": replay["hint"]["queue"],
                     "conf0_transitions": sum(bool(line.strip()) for line in conf0.read_text().splitlines()),
                     "conf0_sha256": digest(conf0),
                     "conf0_path": str(conf0),
                     "replay_trace_path": str(trace),
                     "replay_trace_sha256": digest(trace),
                     "unselected_path": str(image / "hard-pred.txt"),
                     "loaded_pcs": len(metadata["cam"]),
                     "fixed_conf3_estimate": estimate,
                     "metadata_sha256": digest(image / "metadata.json"),
                     "image_sha256": digest(image / "hint-v3.bin")})
    status_paths = sorted((args.output_root / "runs").rglob("status.json"))
    expected_runs = sum(2 + max(1, (len(ranked_candidates(
        run_path(args.output_root, task, "baseline"))[0]) + 15) // 16)
        for task in tasks)
    if (len(status_paths) != expected_runs or any(
            json.loads(path.read_text()).get("result") != "pass" for path in status_paths)):
        raise ValueError(f"formal result audit requires {expected_runs} passed simulations")
    summary = {"format": "h2p-hint-experiment-summary", "version": 3,
               "storage": "finite", "window": "1M warmup + 1M measure",
               "result": "pass", "full_runs": expected_runs, "slices": rows,
               "total_baseline_misses": sum(row["baseline_misses"] for row in rows),
               "total_hint_misses": sum(row["hint_misses"] for row in rows),
               "finished_utc": now()}
    write_json(args.output_root / "summary.json", summary)
    write_json(args.output_root / "result-audit.json", {
        "result": "pass", "full_runs": expected_runs,
        "statuses": [{"path": str(path), "sha256": digest(path)} for path in status_paths],
        "summary_sha256": digest(args.output_root / "summary.json"),
        "finished_utc": now()})
    lines = ["# xs-gem5 Hint finite v3: 1M+1M", "",
             "五片各自按 baseline → collect (最多 16+4) → 离线构表 → Hint replay 执行。",
             "mask-sweep 是基于实际 baseline 目标与原预测的固定 conf=3 离线估计；",
             "下表是含动态置信度与错误路径影响的真实 gem5 replay。", "",
             "| 切片 | baseline miss | Hint miss | 净减少 | corrected | harmed | baseline IPC | Hint IPC | baseline 周期 | Hint 周期 |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in rows:
        lines.append(f"| {row['task']} | {row['baseline_misses']} | {row['hint_misses']} | "
                     f"{row['net_miss_reduction']} | {row['corrected']} | {row['harmed']} | "
                     f"{row['baseline_ipc']} | {row['hint_ipc']} | "
                     f"{row['baseline_cycles']} | {row['hint_cycles']} |")
    (args.output_root / "report.md").write_text("\n".join(lines) + "\n")
    return summary


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("gem5", "config", "workload-list", "output-root", "source-manifest",
                 "build-audit", "validation-audit"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--workload-root", type=Path)
    parser.add_argument("--library-path", default="")
    parser.add_argument("--jobs", type=int, default=5)
    parser.add_argument("--disable-aslr", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--stage", choices=("all", "baseline", "collect", "build", "joint"),
                        default="all")
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 5:
        parser.error("--jobs must be in 1..5")
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.resolve())
    return args


def process_task(args, task):
    baseline = run_path(args.output_root, task, "baseline")
    if args.stage in ("all", "baseline"):
        run_one(args, task, "baseline")
    if args.stage == "baseline":
        return
    if not (baseline / "status.json").exists():
        raise ValueError(f"baseline must pass before profiling: {tag(task)}")
    if json.loads((baseline / "status.json").read_text())["result"] != "pass":
        raise ValueError(f"baseline failed: {tag(task)}")
    pcs, ranking = ranked_candidates(baseline)
    candidates = [candidate_file(args, task, group, pcs[start:start + 16], ranking)
                  for group, start in enumerate(range(0, len(pcs), 16))]
    if not candidates:
        candidates = [candidate_file(args, task, 0, [], ranking)]
    if args.stage in ("all", "collect"):
        for group, candidate in enumerate(candidates):
            run_one(args, task, "collect", group=group, candidate=candidate)
    if args.stage == "collect":
        return
    profiles = []
    for group, candidate in enumerate(candidates):
        collect = group_paths(args.output_root, task, group)
        if not (collect / "status.json").exists() or json.loads(
                (collect / "status.json").read_text())["result"] != "pass":
            raise ValueError("all collect groups must pass before construction")
        observer = compare_observer(baseline, collect)
        if not observer["pass"]:
            raise ValueError(f"collect changed baseline: {collect}")
        profile_path = collect / "hint-profile.jsonl.gz"
        _, group_rows = read_profile(profile_path)
        collected_pcs = {address(pc) for pc in json.loads(candidate.read_text())["pcs"]}
        if {row["pc"] for row in group_rows} - collected_pcs:
            raise ValueError("collect profile contains a PC outside its physical CAM")
        status = json.loads((collect / "status.json").read_text())
        for stat in status["hint"]["pcs"]:
            pc = address(stat["pc"])
            if pc not in collected_pcs:
                continue
            observed = [row for row in group_rows if row["pc"] == pc]
            if (len(observed) != stat["executions"] or
                    sum(baseline_miss(row) for row in observed) != stat["baseline_misses"]):
                raise ValueError(f"collect profile/count mismatch for {pc:#x}")
        profiles.append(profile_path)
    _, rows = merge_profiles(profiles, pcs)
    source = _source(args, task, profile_identity(profiles))
    metadata = build_and_score(args, task, rows, pcs, source)
    if args.stage == "build":
        return
    run_one(args, task, "joint", candidate=metadata.parent / "candidates-joint.json",
            metadata=metadata)


def main(argv=None):
    args = parse_args(argv)
    tasks = load_workloads(args.workload_list, args.workload_root)
    args.command_prefix = legacy().command_prefix(args.disable_aslr)
    args.identity = {name: file_identity(getattr(args, name)) for name in
                     ("gem5", "config", "source_manifest", "workload_list",
                      "build_audit", "validation_audit")}
    args.identity.update({"warmup_insts": 1_000_000, "measure_insts": 1_000_000,
                          "h2p_stat_interval": 1_000_000, "layout": contract.LAYOUT,
                          "difftest": False, "mem_type": "DDR4_2400_8x8",
                          "command_prefix": args.command_prefix,
                          "tools": {path.name: digest(path) for path in
                                    sorted(Path(__file__).parent.glob("*.py"))},
                          "contract_sha256": digest(_contract_path())})
    plan = {"identity": args.identity, "tasks": tasks, "jobs": args.jobs,
            "pipeline": "baseline -> disjoint <=16-PC collect groups -> "
                        "fixed-conf3 16-mask trace scan -> joint replay"}
    if args.dry_run:
        print(json.dumps(plan, indent=2, sort_keys=True))
        return 0
    build = json.loads(args.build_audit.read_text())
    validate_runtime_gate(json.loads(args.validation_audit.read_text()), args.identity)
    if (build.get("returncode") != 0 or not build.get("executable") or
            "ELF" not in build.get("file", "") or
            build.get("gem5_sha256") != args.identity["gem5"]["sha256"] or
            build.get("source_manifest_sha256") != args.identity["source_manifest"]["sha256"]):
        raise ValueError("build audit does not match current executable/source")
    if args.gem5.open("rb").read(4) != b"\x7fELF" or not os.access(args.gem5, os.X_OK):
        raise ValueError("gem5 is not an executable ELF")
    if args.output_root.exists() and not args.resume:
        raise ValueError("output root exists; use --resume for audited passes")
    args.output_root.mkdir(parents=True, exist_ok=args.resume)
    with (args.output_root / "runner.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        manifest = args.output_root / "experiment.json"
        if manifest.exists():
            if json.loads(manifest.read_text()) != plan:
                raise ValueError("experiment identity changed")
        else:
            write_json(manifest, plan)
            for name in ("build_audit", "validation_audit", "source_manifest"):
                shutil.copy2(getattr(args, name), args.output_root / f"{name.replace('_', '-')}.json")
        state = args.output_root / "runner-state.json"
        write_json(state, {"state": "running", "stage": args.stage, "started_utc": now()})
        try:
            with ThreadPoolExecutor(max_workers=args.jobs) as workers:
                pending = {workers.submit(process_task, args, task): task for task in tasks}
                for future in as_completed(pending):
                    task = pending[future]
                    future.result()
                    print(json.dumps({"task": tag(task), "result": "pass",
                                      "stage": args.stage, "utc": now()}), flush=True)
            if args.stage in ("all", "joint"):
                report(args, tasks)
            write_json(state, {"state": "complete", "stage": args.stage,
                               "finished_utc": now()})
            return 0
        except Exception:
            write_json(state, {"state": "failed", "stage": args.stage,
                               "finished_utc": now(), "traceback": traceback.format_exc()})
            raise


def _contract_path():
    return Path(__file__).resolve().parents[3] / "manifest" / "hint_v3.py"


if __name__ == "__main__":
    raise SystemExit(main())
