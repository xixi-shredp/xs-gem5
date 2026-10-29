#!/usr/bin/env python3
"""Resume an audited SPEC experiment on ps1/ps2; full requires --full-test.

All compilation, tests and experiments are restricted to ps1/ps2, including Python.
This runner has no build command;
any future build entry must call require_experiment_host before spawning scons.
Local work is limited to editing and reading files.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import threading
import time

GEM5_ROOT = Path(__file__).resolve().parents[2]
PROJECT = GEM5_ROOT.parent
ALLOWED_OUTPUT = PROJECT / "results/model/smoke-test"
sys.path.insert(0, str(GEM5_ROOT))
from util.h2p_hint.audit import legacy
from util.h2p_hint.retirement_boundary import retirement_window
from util.h2p_hint import cam58

STAGES = ("baseline", "discovery", "select-cam", "detail", "build", "replay", "select")
MASK_ORDER = ["ITH", "Call", "Ret", "LHR"]
# Actual OS hostnames verified through SSH by the team.
REMOTE_HOSTS = {"ps1": "ps1", "iv-yexl3v2u4gwh2ysmqmn1": "ps1",
                "ps2": "ps2", "iv-yexl3v2u4gwh2ysms14a": "ps2"}


def execution_host():
    hostname = socket.gethostname()
    return {"hostname": hostname, "fqdn": socket.getfqdn(),
            "ssh_destination": REMOTE_HOSTS.get(hostname.lower().rstrip(".").split(".")[0])}


def require_experiment_host(host=None):
    host = execution_host() if host is None else host
    # Match an actual OS hostname, not an environment variable or a user flag.
    # The first DNS component allows the server's normal FQDN form.
    need(host["hostname"].lower().rstrip(".").split(".")[0] in REMOTE_HOSTS,
         "Compilation, Python checks and experiments are authorized only on ps1/ps2; deploy via SSH. "
         f"Current host: {host['hostname']}. Local work is limited to editing and reading files")


def resource_snapshot(active=0, memory_per_job_gib=12, reserve_cpus=2, reserve_gib=8):
    """Conservative admission estimate; existing RTL/build load counts as occupied."""
    require_experiment_host()
    cpus = len(os.sched_getaffinity(0))
    try:
        quota, period = Path("/sys/fs/cgroup/cpu.max").read_text().split()
        if quota != "max":
            cpus = min(cpus, max(1, int(quota) // int(period)))
    except (OSError, ValueError):
        pass
    mem = {line.split(":")[0]: int(line.split()[1]) * 1024
           for line in Path("/proc/meminfo").read_text().splitlines()}
    available = mem["MemAvailable"]
    try:
        limit = Path("/sys/fs/cgroup/memory.max").read_text().strip()
        if limit != "max":
            available = min(available, max(0, int(limit) - int(Path("/sys/fs/cgroup/memory.current").read_text())))
    except (OSError, ValueError):
        pass
    load = os.getloadavg()[0]
    # Existing runner children are already counted by the stage-wide semaphore;
    # exclude them when estimating unrelated build/RTL CPU occupancy.
    other_load = max(0.0, load - active)
    cpu_slots = max(0, cpus - math.ceil(other_load) - reserve_cpus)
    memory_slots = max(0, (available - reserve_gib * (1 << 30)) // (memory_per_job_gib * (1 << 30)))
    return {"timestamp": time.time(), "host": execution_host(), "affinity_quota_cpus": cpus,
            "load1": load, "runner_active_children": active, "estimated_other_load": other_load,
            "available_memory_bytes": available, "memory_per_job_gib": memory_per_job_gib,
            "reserve_cpus": reserve_cpus, "reserve_gib": reserve_gib,
            "cpu_slots": cpu_slots, "memory_slots": memory_slots,
            "job_capacity": min(cpu_slots, memory_slots)}


def need(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read(path):
    return json.loads(Path(path).read_text())


def write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, sort_keys=True, indent=2) + "\n")
    temporary.replace(path)


def identity(path):
    path = Path(path).resolve(strict=True)
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": sha(path)}


def final_stats(path):
    """Read the last complete measure block without editing warmup evidence."""
    content = Path(path).read_text()
    blocks = re.findall(r"-{5,}\s+Begin Simulation Statistics\s+-{5,}\s*\n(.*?)-{5,}\s+End Simulation Statistics\s+-{5,}",
                        content, re.S)
    need(blocks, "Missing complete statistics block")
    need(content.count("Begin Simulation Statistics") == len(blocks), "Incomplete final statistics block")
    result = {}
    for line in blocks[-1].splitlines():
        fields = line.split("#", 1)[0].split()
        if len(fields) < 2:
            continue
        numbers = []
        for token in fields[1:]:
            try:
                numbers.append(float(token.rstrip("%")))
            except ValueError:
                break
        if numbers:
            need(fields[0] not in result, f"Duplicate statistic in final block: {fields[0]}")
            result[fields[0]] = numbers
    return result, len(blocks)


def committed_metrics(directory, numeric):
    """Public committed counters only; prediction lookups include wrong-path work."""
    def count(value, name):
        need(type(value) is int and value >= 0, f"Invalid committed counter {name}")
        return value

    def scalar(name):
        row = numeric.get("system.cpu.branchPred." + name)
        need(row is not None and len(row) == 1 and math.isfinite(row[0]) and
             row[0] >= 0 and row[0].is_integer(), f"Missing integer BPU statistic {name}")
        return int(row[0])

    def accuracy(total, misses, source, **extra):
        need(0 <= misses <= total, f"Miss counter exceeds population: {source}")
        return {"total": total, "misses": misses, "correct": total - misses,
                "accuracy": (total - misses) / total if total else None,
                "source": source, **extra}

    summary = read(directory / "h2p-target-summary.json")
    need(summary.get("format") == "committed-nonreturn-indirect-targets" and
         summary.get("version") == 1, "Unsupported public nonreturn summary")
    total = count(summary["executions"], "nonreturn.executions")
    absent = count(summary["absent_target"], "nonreturn.absent_target")
    wrong = count(summary["valid_wrong_target"], "nonreturn.valid_wrong_target")
    misses = count(summary["target_misses"], "nonreturn.target_misses")
    need(misses == absent + wrong, "Nonreturn target-miss components disagree")
    pcs = summary["pcs"]
    for field in ("executions", "absent_target", "valid_wrong_target", "target_misses"):
        need(sum(count(p[field], "pc." + field) for p in pcs) == summary[field],
             f"Nonreturn per-PC sum differs: {field}")
    returns, return_misses = scalar("returnNum"), scalar("returnMiss")
    other, other_misses = scalar("otherNum"), scalar("otherMiss")
    need(other == total, "BPU nonreturn population differs from public target summary")
    result = {
        "all_indirect": accuracy(returns + other, return_misses + other_misses,
            "stats.txt:last-complete returnNum+otherNum / returnMiss+otherMiss",
            definition="committed BPU CFI-misprediction counters; return and nonreturn populations",
            return_total=returns, return_misses=return_misses,
            nonreturn_total=other, nonreturn_cfi_misses=other_misses),
        "nonreturn_ittage_plus_hint": accuracy(total, misses, "h2p-target-summary.json",
            definition="committed final fetch target; absent target counts as a miss, including target=fallthrough",
            absent_target=absent, valid_wrong_target=wrong),
        "hint_override": None, "hint_tag_hit": None,
        "counter_scope": "measure-only committed branches; no speculative query counters or private replay trace"}
    path = directory / "hint-stats.json"
    if path.exists():
        hints = read(path)
        need(hints.get("format") == "h2p-hint-stats" and hints.get("version") == 7 and
             hints.get("storage") == "finite", "Unsupported public Hint stats")
        g = hints["global"]
        for name in ("executions", "final_misses", "baseline_misses", "corrected", "harmed", "conf_enabled",
                     "overrides", "override_misses", "tag_hits", "tag_hit_misses"):
            count(g[name], "hint.global." + name)
        need(g["executions"] == total and g["final_misses"] == misses,
             "Public Hint final population/misses differs from target summary")
        need(g["baseline_misses"] + g["harmed"] == g["final_misses"] + g["corrected"],
             "Hint corrected/harmed accounting identity failed")
        need(g["overrides"] <= g["tag_hits"] <= total, "Hint committed subset counters disagree")
        need(g["conf_enabled"] <= g["tag_hits"], "Hint confidence-enabled count exceeds committed tag hits")
        result["hint_override"] = accuracy(g["overrides"], g["override_misses"],
            "hint-stats.json:global.overrides/global.override_misses",
            definition="committed branches actually overridden by Hint")
        result["hint_tag_hit"] = accuracy(g["tag_hits"], g["tag_hit_misses"],
            "hint-stats.json:global.tag_hits/global.tag_hit_misses",
            definition="committed eligible Context hits; includes conf=0 shadow targets")
        result["hint_public_global"] = g
        queue = hints["queue"]
        queue_names = ("recoveries", "zero_transitions", "dropped", "enqueued", "applied", "pending",
                       "canceled", "carried_in", "max_occupancy", "occupancy_sum", "occupancy_samples",
                       "rollback_writes", "rollback_read_conflicts", "rollback_pending", "rollback_fallbacks",
                       "meta_queue_stalls", "lhr_stalls", "history_window_stalls")
        result["hint_public_queue"] = {name: count(queue[name], "queue." + name) for name in queue_names}
        query = hints["query"]
        result["hint_fallbacks"] = {
            "busy_fallback_attempts": count(query["busy_fallbacks"], "query.busy_fallbacks"),
            "frontend_hint_blocked_cycles": count(query["frontend_hint_blocked_cycles"], "query.frontend_hint_blocked_cycles"),
            "metadata_capacity_attempts": queue["meta_queue_stalls"],
            "per_slot_lhr_quota_attempts": queue["lhr_stalls"],
            "history_window_attempts": queue["history_window_stalls"],
            "rollback_attempts": queue["rollback_fallbacks"]}
        result["hint_confidence"] = {
            "committed_zero_confidence_hits": g["tag_hits"] - g["conf_enabled"],
            "applied_zero_to_positive": queue["recoveries"],
            "applied_positive_to_zero": queue["zero_transitions"],
            "definition": "applied transitions use old SRAM value and may include stale 0->3 writes; "
                          "not the count of query-conf0 baseline-miss requests"}
    return result


def tasks_from_manifests(manifest, workload, input_root, full_test=False):
    expected = (100_000_000, 200_000_000) if full_test else (1_000_000, 1_000_000)
    need((manifest.get("warmup_insts"), manifest.get("measure_insts")) == expected,
         f"Instruction window must match authorized {'full' if full_test else 'smoke'} {expected}")
    need(manifest.get("difftest", {}).get("enable") is True, "smoke requires difftest")
    groups = workload["cpts"]
    need(groups == manifest["workloads"]["cpts"], "workload and smoke manifest slices differ")
    pairs = [(g["workload"], g["bench"], str(c)) for g in groups for c in g["cpt_list"]]
    need(all(name == "cpython-norvv-uart16550-100m200m" for name, _, _ in pairs),
         "Only the authorized no-RVV UART16550 100M/200M checkpoint collection is accepted")
    need(len(pairs) == 10 and len({p[1] for p in pairs}) == 7 and len(set(pairs)) == 10,
         "Expected exactly the authorized seven benchmarks and ten distinct slices")
    allowed = {("scimark_fft", "51"), ("scimark_fft", "33"),
               ("pickle_pure_python", "34"), ("pickle_pure_python", "55"),
               ("deltablue", "11"), ("json_dumps", "3"), ("gc_collect", "25"),
               ("gc_collect", "20"), ("pyflate", "43"), ("richards", "17")}
    need({(b, c) for _, b, c in pairs} == allowed, "Unexpected smoke slice selection")
    tasks = []
    for name, bench, point in pairs:
        directory = input_root / (bench + "_test") / point
        files = sorted(directory.glob("*_memory_.zstd"))
        need(len(files) == 1, f"Expected one checkpoint: {directory}")
        tasks.append({"workload": name, "bench": bench, "point": point,
                      "tag": f"{bench}/{point}", "checkpoint": identity(files[0])})
    return tasks


def render_command(manifest, checkpoint, output, ref, gem5, mode, cam=None, image=None, meta_entries=16):
    values = {"baseline_checkpoint": checkpoint, "csr_checkpoint": checkpoint,
              "output_dir": str(output), "ref_so": str(ref)}
    key = "baseline_run_flags" if mode == "off" else "new_run_flags"
    tokens = [token.format_map(values) for fragment in manifest["model_root"][key]
              for token in shlex.split(fragment)]
    env = os.environ.copy()
    while tokens and re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", tokens[0]):
        name, value = tokens.pop(0).split("=", 1)
        env[name] = value
    need(tokens and "gem5" in Path(tokens[0]).name, "Manifest must invoke gem5 directly")
    tokens[0] = str(gem5)
    warmup, measure = manifest['warmup_insts'], manifest['measure_insts']
    need(f"--warmup-insts-no-switch={warmup}" in tokens and
         f"--maxinsts={warmup + measure}" in tokens,
         "Manifest command does not match declared instruction window")
    need(sum(t.startswith("--maxinsts") for t in tokens) == 1 and
         sum(t.startswith("--warmup-insts") for t in tokens) == 1,
         "Duplicate or conflicting instruction-count options are forbidden")
    need(not any(t.startswith(("--h2p-hint-", "--disable-difftest"))
                 for t in tokens), "Manifest must contain only the basic smoke command")
    tokens += ["--enable-difftest", f"--difftest-ref-so={ref}", "--h2p-stat",
               f"--h2p-hint-mode={mode}"]
    tokens.append(f"--h2p-hint-meta-entries={meta_entries}")
    if cam:
        tokens.append(f"--h2p-hint-candidates={cam}")
    if image:
        tokens.append(f"--h2p-hint-metadata={image}")
    env["GCBV_REF_SO"] = str(ref)
    env["GEM5_REQUIRE_DIFFTEST_ABI"] = "1"
    return tokens, env


class Runner:
    def __init__(self, args):
        self.args = args
        self.root = args.output_root.resolve()
        allowed_output = PROJECT / 'results/model/full-test' if args.full_test else ALLOWED_OUTPUT
        need(self.root.is_relative_to(allowed_output.resolve()) and self.root != allowed_output.resolve(),
             f"Choose a dedicated output subdirectory under {allowed_output}")
        self.manifest = read(args.manifest)
        self.tasks = tasks_from_manifests(self.manifest, read(args.workload_manifest),
                                         args.input_root.resolve(), args.full_test)
        need(not args.full_test or not args.import_baseline_root,
             'Full runs must produce an independent baseline')
        self.gem5 = args.gem5.resolve(strict=True)
        self.ref = args.ref_so.resolve(strict=True)
        self.tool = Path(__file__).with_name("spec_trace.py")
        self.common = {"schema": "hint-spec-smoke-inputs/v1", "warmup": self.manifest['warmup_insts'],
                       "measure": self.manifest['measure_insts'], "hint_meta_entries": args.hint_meta_entries, "manifest": identity(args.manifest),
                       "workload_manifest": identity(args.workload_manifest),
                       "gem5": identity(self.gem5), "reference": identity(self.ref),
                       "runner": identity(__file__), "offline_tool": identity(self.tool),
                       "cam58_adapter": identity(cam58.__file__),
                       "config": identity(GEM5_ROOT / "configs/example/backend_v2_norvv_nol3.py")}
        self.common["execution_host"] = execution_host()
        self.common["import_baseline_root"] = (str(args.import_baseline_root.resolve())
                                               if args.import_baseline_root else None)
        need(os.access(self.gem5, os.X_OK), "gem5 is not executable")
        need(self.gem5.open("rb").read(4) == b"\x7fELF", "gem5 must be the completed ELF binary")
        self.active_children = 0
        self.resource_condition = threading.Condition()
        self.child_slots = threading.BoundedSemaphore(1)
        self.max_jobs = 1

    def plan_resources(self, stage, task_count):
        snapshot = resource_snapshot(memory_per_job_gib=self.args.memory_per_job_gib,
                                     reserve_gib=self.args.reserve_gib)
        cap = task_count * (3 if stage == "replay" else 1)
        capacity = min(cap, snapshot["job_capacity"])
        need(capacity > 0, "No remaining CPU/memory budget; wait for build/RTL jobs to finish")
        requested = capacity if self.args.jobs == "auto" else int(self.args.jobs)
        need(requested > 0, "--jobs must be auto or a positive integer")
        need(min(cap, requested) <= capacity, "Requested --jobs exceeds remaining CPU/memory budget")
        self.max_jobs = min(cap, requested)
        self.child_slots = threading.BoundedSemaphore(self.max_jobs)
        write(self.root / "resources" / f"{stage}-{time.time_ns()}.json",
              {**snapshot, "phase": stage, "phase_task_cap": cap, "selected_jobs": self.max_jobs})

    def invoke(self, command, directory, env=None):
        require_experiment_host()
        with self.child_slots:
            with self.resource_condition:
                while True:
                    snapshot = resource_snapshot(self.active_children, self.args.memory_per_job_gib,
                                                 reserve_gib=self.args.reserve_gib)
                    if snapshot["job_capacity"] >= self.active_children + 1:
                        self.active_children += 1
                        break
                    print(f"waiting for CPU/memory admission: {directory}", flush=True)
                    self.resource_condition.wait(timeout=10)
            try:
                directory.mkdir(parents=True, exist_ok=True)
                write(directory / "resource-admission.json", snapshot)
                self._invoke(command, directory, env)
            finally:
                with self.resource_condition:
                    self.active_children -= 1
                    self.resource_condition.notify_all()

    def _invoke(self, command, directory, env=None):
        if Path(command[0]).resolve() == self.gem5:
            require_experiment_host()
        directory.mkdir(parents=True, exist_ok=True)
        write(directory / "command.json", {"argv": command, "cwd": str(GEM5_ROOT),
                                           "GCBV_REF_SO": str(self.ref),
                                           "execution_host": execution_host(),
                                           "ssh_connection": os.environ.get("SSH_CONNECTION"),
                                           "ssh_original_command": os.environ.get("SSH_ORIGINAL_COMMAND")})
        with (directory / "stdout.log").open("w") as stdout, (directory / "stderr.log").open("w") as stderr:
            result = subprocess.run(command, cwd=GEM5_ROOT, env=env, stdout=stdout, stderr=stderr)
        need(result.returncode == 0, f"Command failed ({result.returncode}): {directory}")

    def offline(self, argv, logdir):
        self.invoke([sys.executable, str(self.tool)] + [str(a) for a in argv], logdir)

    def state(self, base, stage):
        path = base / stage / "stage.json"
        need(path.is_file(), f"Missing prerequisite {path}; run its stage first")
        state = read(path)
        need(state["common"] == self.common, f"Input/binary/ref fingerprint changed: {path}")
        need(identity(state["checkpoint"]["path"]) == state["checkpoint"],
             f"Checkpoint identity changed: {path}")
        for name, expected in state.get("prerequisites", {}).items():
            self.state(base, name)
            need(sha(base / name / "stage.json") == expected,
                 f"Prerequisite changed: {base / name}")
        for expected in state["artifacts"]:
            need(identity(expected["path"]) == expected, f"Artifact changed: {expected['path']}")
        return state

    def run_stage(self, task, stage):
        base = self.root / task["tag"]
        directory = base / stage
        if (directory / "stage.json").exists():
            state = self.state(base, stage)
            need(state["checkpoint"] == task["checkpoint"], "Checkpoint identity changed")
            print(f"resume {task['tag']} {stage}", flush=True)
            return state
        need(not directory.exists(), f"Incomplete stage directory: {directory}; inspect it and use a fresh output root")
        dependencies = {"detail": ["select-cam"], "select-cam": ["discovery"],
                        "build": ["detail", "select-cam"], "replay": ["build"],
                        "select": ["baseline", "replay"]}.get(stage, [])
        prerequisite = {name: self.state(base, name) for name in dependencies}
        directory.mkdir(parents=True)
        print(f"start {task['tag']} {stage}", flush=True)
        write(directory / "started.json", {"common": self.common, "checkpoint": task["checkpoint"],
                                            "stage": stage, "time": time.time()})
        artifacts, report = [], {}
        cam = base / "select-cam/cam.bin"
        cam_json = base / "select-cam/cam.json"
        if stage == "baseline" and self.args.import_baseline_root:
            artifacts, report = self.import_baseline(task)
        elif stage in ("baseline", "discovery", "detail"):
            self.check_runtime_inputs()
            mode = {"baseline": "off", "discovery": "discover", "detail": "detail"}[stage]
            command, env = render_command(self.manifest, task["checkpoint"]["path"], directory,
                                           self.ref, self.gem5, mode, cam if stage == "detail" else None, meta_entries=self.args.hint_meta_entries)
            self.invoke(command, directory, env)
            report = self.audit_simulation(directory)
            artifacts += [directory / p for p in ("stdout.log", "stderr.log", "config.ini", "stats.txt", "command.json", "resource-admission.json", "h2p-target-summary.json", "hint-discovery.json")]
            if (directory / "hint-stats.json").exists():
                artifacts.append(directory / "hint-stats.json")
            if stage != "baseline":
                raw = next((directory / p for p in (f"hint-{stage}.bin", f"{stage}.bin")
                            if (directory / p).is_file()), None)
                need(raw is not None, f"Missing SPEC raw {stage} trace")
                size = 7 if stage == "discovery" else 22
                need(raw.stat().st_size >= size and raw.stat().st_size % size == 0,
                     "Trace is empty or contains an incomplete record")
                # Simulator files are compact and always finalize on normal smoke exit.
                # Capacity-terminated traces require an explicit externally supplied capacity;
                # never silently reinterpret a missing terminator as a complete capture.
                meta = {"schema": "hint-spec-trace", "version": 1, "hash_version": 7,
                        "mask_order": MASK_ORDER, "kind": stage, "record_bytes": size,
                        "termination": "final", "capacity_records": raw.stat().st_size // size,
                        "data_sha256": sha(raw), "audit_sha256": sha(str(raw) + ".audit.jsonl")}
                if stage == "detail":
                    meta["cam_sha256"] = sha(base / "select-cam/cam.public.bin")
                sidecar = directory / "trace.json"
                write(sidecar, meta)
                argv = ["validate", "--trace", raw, "--meta", sidecar, "--output", directory / "validation.json"]
                if stage == "detail":
                    argv += ["--cam", cam_json]
                self.offline(argv, directory / "validate")
                cam58.checked_rows(raw, sidecar, stage,
                                   read(cam_json) if stage == "detail" else None)
                artifacts += [raw, Path(str(raw) + ".audit.jsonl"), sidecar, directory / "validation.json"]
                report["trace"] = str(raw)
        elif stage == "select-cam":
            source = prerequisite["discovery"]["report"]["trace"]
            public_image = directory / "cam.public.bin"
            selection = directory / "selection.json"
            cam58.select_cam(source, base / "discovery/trace.json", cam_json,
                             public_image, selection, cam)
            artifacts += [cam, cam_json, public_image, selection, cam.with_suffix(".adapter.json")]
        elif stage == "build":
            source = prerequisite["detail"]["report"]["trace"]
            filtered, filtered_meta = directory / "fixed-cfi.bin", directory / "fixed-cfi.json"
            selection = base / "select-cam/selection.json"
            cam58.filter_detail(source, base / "detail/trace.json", cam_json, selection,
                                filtered, filtered_meta)
            artifacts += [filtered, filtered_meta, Path(str(filtered) + ".audit.jsonl")]
            common = ["--trace", filtered, "--meta", filtered_meta, "--cam", cam_json]
            ranking = directory / "ranking.json"
            self.offline(["rank-masks", *common, "--output", ranking], directory / "rank")
            artifacts.append(ranking)
            candidates = []
            for candidate in read(ranking)["candidates"]:
                label = candidate["label"]
                need(label in ("static-best", "uniform9", "uniform15"), "Unexpected candidate")
                masks, image, metadata = [directory / (label + ext) for ext in (".masks.json", ".bin", ".json")]
                public_image = directory / (label + ".public.bin")
                write(masks, candidate["masks"])
                self.offline(["build", *common, "--masks", masks, "--image", public_image,
                              "--output", metadata], directory / label)
                need(sha(public_image) == candidate["image_sha256"], "Ranking/build image mismatch")
                cam58.adapt_image(public_image, cam_json, selection, image, "load")
                artifacts += [masks, image, metadata, public_image, image.with_suffix(".adapter.json")]
                candidates.append({"label": label, "image": str(image), "sha256": sha(image)})
            need(candidates, "No candidate images")
            report = {"candidates": candidates, "deduplication": "complete image SHA256"}
        elif stage == "replay":
            def run_candidate(candidate):
                self.check_runtime_inputs()
                out = directory / candidate["label"]
                command, env = render_command(self.manifest, task["checkpoint"]["path"], out,
                                               self.ref, self.gem5, "replay", image=candidate["image"],
                                               meta_entries=self.args.hint_meta_entries)
                self.invoke(command, out, env)
                result = {**candidate, **self.audit_simulation(out)}
                files = [out / p for p in ("stdout.log", "stderr.log", "config.ini", "stats.txt", "command.json", "resource-admission.json", "h2p-target-summary.json", "hint-discovery.json")]
                if (out / "hint-stats.json").exists():
                    files.append(out / "hint-stats.json")
                return result, files
            with ThreadPoolExecutor(max_workers=min(3, self.max_jobs)) as pool:
                completed = list(pool.map(run_candidate, prerequisite["build"]["report"]["candidates"]))
            candidates = [result for result, files in completed]
            artifacts += [file for result, files in completed for file in files]
            report = {"candidates": candidates}
        else:
            baseline = prerequisite["baseline"]["report"]
            candidates = prerequisite["replay"]["report"]["candidates"]
            best = min(candidates, key=lambda c: (-c["ipc"], c["label"]))
            report = {"selection_basis": "measured IPC, not trace-order confidence replay",
                      "best_hint_candidate": best, "baseline": baseline,
                      "binary_comparison": {"baseline": baseline.get("binary", self.common["gem5"]),
                                            "new": self.common["gem5"],
                                            "same_binary": baseline.get("binary", self.common["gem5"])["sha256"] == self.common["gem5"]["sha256"]},
                      "speedup": best["ipc"] / baseline["ipc"],
                      "hint_is_faster": best["ipc"] > baseline["ipc"]}
        write(directory / "report.json", report)
        artifacts += [directory / "report.json"]
        state = {"common": self.common, "checkpoint": task["checkpoint"], "stage": stage,
                 "prerequisites": {k: sha(base / k / "stage.json") for k in dependencies},
                 "artifacts": [identity(p) for p in artifacts], "report": report}
        write(directory / "stage.json", state)
        return state

    def check_runtime_inputs(self):
        require_experiment_host()
        for key in ("gem5", "reference", "offline_tool", "config", "runner", "cam58_adapter"):
            expected = self.common[key]
            need(identity(expected["path"]) == expected,
                 f"Input changed while smoke was running: {expected['path']}")

    def import_baseline(self, task):
        root = self.args.import_baseline_root.resolve(strict=True)
        frozen = read(root / "identity.json")
        need(frozen.get("schema") == "hint-baseline-identity/v1", "Unsupported baseline identity schema")
        need(frozen["binary"]["sha256"] == self.common["gem5"]["sha256"],
             "Final A/B requires Hint Off and Hint On from the same revised ELF; "
             "the earlier frozen baseline is preparation evidence only. Run baseline with this runner")
        for key, name in (("binary", "gem5"), ("config", "config"), ("reference", "reference")):
            row = frozen[key]
            need(sha(row["path"]) == row["sha256"], f"Frozen baseline {key} changed")
            if key != "binary":
                need(row["sha256"] == self.common[name]["sha256"], f"Baseline/new {key} differs")
        need(frozen.get("warmup_insts") == frozen.get("measure_insts") == 1_000_000,
             "Imported baseline must use the same 1M + 1M window")
        reference = read(root / "reference-audit.json")
        need(reference["sha256"] == self.common["reference"]["sha256"] and
             Path(reference["ref_path"]).resolve() == self.ref and reference["elf_state_bytes"] == 1368,
             "Baseline reference audit mismatch")
        need(reference.get("runtime_abi_handshake") not in (None, "pending gem5 build completion", "pending", "failed"),
             "Baseline reference runtime ABI handshake is not confirmed")
        name = f"{task['bench']}_{task['point']}"
        records = [r for r in read(root / "run-summary.json") if r["name"] == name]
        need(len(records) == 1 and records[0]["exit_code"] == 0, "Imported baseline run did not pass")
        record = records[0]
        directory = root / name
        need(Path(record["outdir"]).resolve() == directory.resolve() and
             Path(record["checkpoint"]).resolve() == Path(task["checkpoint"]["path"]) and
             record["checkpoint_sha256"] == task["checkpoint"]["sha256"],
             "Imported baseline checkpoint identity mismatch")
        command = read(directory / "command.json")
        argv = command["argv"]
        need(Path(argv[0]).resolve() == Path(frozen["binary"]["path"]).resolve(),
             "Baseline command does not use frozen binary")
        need(command["GCBV_REF_SO"] == str(self.ref), "Baseline command reference differs")
        expected = [argv[0], "-d", str(directory), "configs/example/backend_v2_norvv_nol3.py",
                    "--generic-rv-cpt=" + task["checkpoint"]["path"],
                    "--warmup-insts-no-switch=1000000", "--maxinsts=2000000", "--h2p-stat"]
        need(argv == expected, "Imported baseline command differs from frozen basic smoke command")
        report = self.audit_simulation(directory, combined_log=directory / "run.log")
        report.update({"imported": True, "source_directory": str(directory),
                       "binary": identity(frozen["binary"]["path"]),
                       "new_binary_sha256": self.common["gem5"]["sha256"],
                       "shared_inputs": ["checkpoint", "reference", "config", "warmup", "measure"],
                       "original_outputs_modified": False})
        artifacts = [root / name for name in ("identity.json", "reference-audit.json", "run-summary.json")]
        artifacts += [directory / p for p in ("command.json", "run.log", "config.ini", "stats.txt", "h2p-target-summary.json", "hint-discovery.json")]
        if (directory / "hint-stats.json").exists():
            artifacts.append(directory / "hint-stats.json")
        artifacts += [Path(frozen["binary"]["path"])]
        return artifacts, report

    def audit_simulation(self, directory, combined_log=None):
        config = (directory / "config.ini").read_text()
        need(re.search(rf"^hintMetaEntries={self.args.hint_meta_entries}$", config, re.M),
             "Configured Hint metadata queue capacity differs")
        need(re.search(r"^enable_difftest=true$", config, re.M), "difftest was not enabled in config.ini")
        need(f"difftest_ref_so={self.ref}" in config, "Configured difftest reference differs")
        if combined_log:
            need(re.search(r"^hintMode=off$", config, re.M), "Imported baseline is not Hint Off")
        logs = combined_log.read_text() if combined_log else ((directory / "stdout.log").read_text()
                                                              + (directory / "stderr.log").read_text())
        need(not re.search(r"(?:fatal:|panic:|Assertion .*failed|difftest.*(?:fail|mismatch))", logs, re.I),
             f"Simulation error: {directory}")
        phases = legacy().read_phases(logs)
        numeric, block_count = final_stats(directory / "stats.txt")
        window, boundary = retirement_window(phases, self.common['warmup'], self.common['measure'], numeric)
        mode = re.search(r"^hintMode=(\w+)$", config, re.M)
        need(mode is not None, "Missing configured Hint mode")
        if mode.group(1) in ("detail", "collect", "replay"):
            need((directory / "hint-stats.json").exists(), "Missing public committed Hint stats")
        return {"ipc": window["measure_ipc"], "window": window, "boundary": boundary,
                "binary": self.common["gem5"],
                "prediction_metrics": committed_metrics(directory, numeric),
                "profiling_audit": read(directory / "hint-discovery.json"),
                "stats_block_count": block_count, "stats_block_used": "last-complete",
                "difftest_enabled": True, "reference_sha256": self.common["reference"]["sha256"]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--workload-manifest", type=Path, required=True)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--ref-so", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--full-test", action="store_true",
                        help="Explicitly enable authorized 100M warmup + 200M measure")
    parser.add_argument("--import-baseline-root", type=Path,
                        help="Read original frozen baseline evidence; never copy or alter its outputs")
    parser.add_argument("--stage", choices=("prepare", "all", *STAGES), default="prepare")
    parser.add_argument("--hint-meta-entries", type=int, choices=(16, 32), default=16)
    parser.add_argument("--jobs", default="auto", help="auto or a positive integer bounded by remaining CPU/memory")
    parser.add_argument("--memory-per-job-gib", type=int, default=12, help="Conservative per-child reservation")
    parser.add_argument("--reserve-gib", type=int, default=8, help="Memory left for OS and other jobs")
    parser.add_argument("--slice", action="append", default=[], metavar="BENCH/ID",
                        help="Run a slice group; partial groups never produce a complete smoke summary")
    args = parser.parse_args(argv)
    try:
        require_experiment_host()
        need(args.jobs == "auto" or (args.jobs.isdecimal() and int(args.jobs) > 0), "--jobs must be auto or positive")
        need(args.memory_per_job_gib > 0, "Per-job memory reservation must be positive")
        need(args.reserve_gib >= 8, "Keep at least 8 GiB reserved")
        runner = Runner(args)
        known = {task["tag"] for task in runner.tasks}
        need(set(args.slice) <= known, "Unknown --slice; use manifest benchmark/id")
        selected = [task for task in runner.tasks if not args.slice or task["tag"] in args.slice]
        runner.root.mkdir(parents=True, exist_ok=True)
        with (runner.root / ".runner.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            prepared = {"common": runner.common, "tasks": runner.tasks,
                        "stages": list(STAGES), "full_experiment": args.full_test}
            previous = runner.root / "prepared.json"
            need(not previous.exists() or read(previous) == prepared,
                 "Prepared input identities changed; use a fresh output root")
            write(previous, prepared)
            if args.stage == "prepare":
                print(json.dumps({"slices": len(runner.tasks), "stages": STAGES, "simulations_started": 0}))
                return 0
            stages = STAGES if args.stage == "all" else [args.stage]
            for stage in stages:
                runner.plan_resources(stage, len(selected))
                with ThreadPoolExecutor(max_workers=min(len(selected), runner.max_jobs)) as pool:
                    list(pool.map(lambda task: runner.run_stage(task, stage), selected))
            if args.stage in ("all", "select"):
                if args.slice:
                    group = hashlib.sha256("\n".join(sorted(args.slice)).encode()).hexdigest()[:12]
                    write(runner.root / f"partial-{group}.json", {"schema": "hint-spec-smoke-partial/v1",
                          "complete": False, "selected_slices": args.slice,
                          "required_slices": sorted(known), "reason": "slice group; full ten-slice aggregation required"})
                else:
                    write(runner.root / "summary.json", {"schema": "hint-spec-smoke-results/v1", "complete": True,
                      "slices": [{"tag": t["tag"], **runner.state(runner.root / t["tag"], "select")["report"]}
                                 for t in runner.tasks]})
    except (ValueError, KeyError, OSError, TypeError) as error:
        parser.exit(2, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
