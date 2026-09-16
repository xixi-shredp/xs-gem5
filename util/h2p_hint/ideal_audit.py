"""Independent read-only evidence audit for all 185 unlimited Hint runs.

Reopens simulator outputs, images, configuration, transitions, profiles and
window records. A saved status='pass' alone never makes this audit pass.
"""

import argparse
from collections import Counter
import concurrent.futures as futures
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex

from .audit import audit_run, compare_observer
from .model import (
    address,
    baseline_miss,
    context_key,
    fold28,
    load_candidates,
    load_metadata,
    query_eligible,
    rotl28,
    sha256,
    write_json,
)
from .profile import Cache, compare_shared_profiles
from .runner import now, tag


COUNTS = {
    "baseline": 5,
    "finite": 5,
    "fixed": 5,
    "collect": 5,
    "masks": 160,
    "joint": 5,
}


def audit_conf0(
    path, metadata, *, clock_period=None, start_cycle=None, end_cycle=None
):
    ideal = metadata["version"] == 3
    cam = {
        address(row["pc"]): row["mask"]
        for row in metadata["cam"]
        if row["valid"]
    }
    if ideal:
        entries = {
            i: row for i, row in enumerate(metadata["entries"]) if row["valid"]
        }
    else:
        ways = metadata["layout"]["ways"]
        entries = {
            row["set"] * ways + row["way"]: row
            for row in metadata["entries"]
            if row["valid"]
        }
    previous, count = -1, 0
    with Path(path).open() as source:
        for line in source:
            if not line.strip():
                continue
            row = json.loads(line)
            pc = address(row["pc"])
            if cam.get(pc) != row["mask"]:
                raise ValueError("conf0 PC/mask is not installed in CAM")
            key = context_key(pc, row["contexts"], row["mask"])
            if row["hash"] != key or ideal and row.get("key") != key:
                raise ValueError(
                    "conf0 prediction-time hash does not match contexts"
                )
            if ideal:
                entry_id = row["entry_id"]
            else:
                entry_id = row["set"] * ways + row["way"]
                if row.get("entry_id", entry_id) != entry_id:
                    raise ValueError("finite conf0 stable entry id differs")
            entry = entries[entry_id]
            if ideal:
                if entry["key"] != key:
                    raise ValueError("conf0 stable key/entry identity differs")
            else:
                sets = metadata["layout"]["context_entries"] // ways
                if entry["set"] != key & (sets - 1) or entry["tag"] != key >> (
                    sets.bit_length() - 1
                ):
                    raise ValueError("conf0 set/tag identity differs")
            if row["slot"] != entry["slot"] or address(
                row["predicted_target"]
            ) != address(metadata["slots"][entry["slot"]]):
                raise ValueError(
                    "conf0 saved slot/target does not match frozen image"
                )
            if address(row["predicted_target"]) == address(
                row["actual_target"]
            ):
                raise ValueError(
                    "a correct target cannot decrease confidence to zero"
                )
            query, commit, writeback = (
                row[name + "_cycle"]
                for name in ("query", "commit", "writeback")
            )
            if not query <= commit < writeback or writeback <= previous:
                raise ValueError(
                    "conf0 query/commit/writeback order violates single delayed write port"
                )
            if (start_cycle is not None and commit < start_cycle) or (
                end_cycle is not None and writeback > end_cycle
            ):
                raise ValueError(
                    "confidence update was committed/written outside measure"
                )
            previous = writeback
            historical = row["history"]
            for field, width in (("ghr", 970), ("githr", 128), ("lithr", 8)):
                if not 0 <= int(historical[field], 0) < 1 << width:
                    raise ValueError(
                        "conf0 history exceeds its declared hardware width"
                    )
            folded = [
                fold28(int(historical["ghr"], 0)),
                fold28(int(historical["githr"], 0)),
                fold28(int(historical["lithr"], 0)),
            ]
            for kind in ("call", "return"):
                words = historical[kind + "_pc_words_newest_first"]
                if len(words) > 64:
                    raise ValueError("conf0 PC history exceeded 64 entries")
                value = 0
                for age, word in enumerate(words):
                    if not 0 <= int(word, 0) < 1 << 47:
                        raise ValueError("conf0 PC word exceeds pc[47:1]")
                    value ^= rotl28(fold28(int(word, 0)), age)
                folded.append(value)
            if folded != row["contexts"]:
                raise ValueError(
                    "conf0 full history and folded context disagree"
                )
            if clock_period is not None:
                for phase in ("query", "commit", "writeback"):
                    if (
                        row[phase + "_tick"]
                        != row[phase + "_cycle"] * clock_period
                    ):
                        raise ValueError(
                            "conf0 cycle/tick fields differ from configured clock"
                        )
            count += 1
    return count


def reproduce_unlimited(cache, masks, prune=False):
    """Recount positive target gains independently of builder selection code."""
    active, iterations = dict(masks), []
    while True:
        ordered, keys, pc_keys = (
            tuple(sorted(active)),
            {},
            {pc: set() for pc in active},
        )
        for row in cache.rows():
            pc = row["pc"]
            if pc not in active or not query_eligible(row, ordered):
                continue
            key = context_key(pc, row["contexts"], active[pc])
            pc_keys[pc].add(key)
            counts = keys.setdefault(key, [Counter(), 0])
            counts[0][row["actual"]] += 1
            counts[1] += not baseline_miss(row)
        mappings = {}
        for key, (targets, correct) in keys.items():
            target = min(
                targets, key=lambda target: (-targets[target], target)
            )
            if targets[target] > correct:
                mappings[key] = target
        remaining = {
            pc: value
            for pc, value in active.items()
            if not prune or pc_keys[pc].intersection(mappings)
        }
        iterations.append(
            {"input": sorted(active), "remaining": sorted(remaining)}
        )
        if remaining == active:
            targets = sorted(set(mappings.values()))
            slots = {target: index for index, target in enumerate(targets)}
            return {
                "cam": [
                    {"valid": True, "pc": hex(pc), "mask": active[pc]}
                    for pc in sorted(active)
                ],
                "slots": [hex(target) for target in targets],
                "entries": [
                    {
                        "valid": True,
                        "key": key,
                        "conf": 3,
                        "slot": slots[target],
                    }
                    for key, target in sorted(mappings.items())
                ],
            }, iterations
        active = remaining


def actual_best_masks(root, task, candidates):
    best = {}
    for mask in range(32):
        path = (
            root
            / "runs"
            / "masks"
            / tag(task)
            / f"mask-{mask:02d}"
            / "hint-stats.json"
        )
        data = json.loads(path.read_text())
        pcs = {address(row["pc"]): row for row in data["pcs"]}
        if not set(candidates) <= set(pcs):
            raise ValueError("mask sweep misses candidate per-PC rows")
        for pc in candidates:
            row = pcs[pc]
            score = (
                row["corrected"] - row["harmed"],
                -mask.bit_count(),
                -mask,
            )
            if pc not in best or score > best[pc][0]:
                best[pc] = (score, mask)
    ranked = sorted(best, key=lambda pc: (-best[pc][0][0], pc))
    choices = [
        {
            "pc": hex(pc),
            "mask": best[pc][1],
            "net_correction": best[pc][0][0],
            "loaded_candidate": best[pc][0][0] > 0,
            "reason": "selected" if best[pc][0][0] > 0 else "no_positive_gain",
        }
        for pc in ranked
    ]
    return {pc: best[pc][1] for pc in ranked if best[pc][0][0] > 0}, choices


def audit_source_files(root, manifest):
    failures = []
    for name, expected in manifest["files"].items():
        path = Path(root) / name
        try:
            if path.is_symlink():
                raw, kind = os.readlink(path).encode(), "symlink"
                digest = hashlib.sha256(raw).hexdigest()
                size = len(raw)
            else:
                kind, digest, size = "file", sha256(path), path.stat().st_size
            actual = {
                "kind": kind,
                "sha256": digest,
                "size": size,
                "executable": bool(path.lstat().st_mode & 0o111),
            }
            if actual != expected:
                failures.append(name)
        except OSError:
            failures.append(name)
    if failures:
        raise ValueError(
            "source file identity mismatch: " + ", ".join(failures[:20])
        )
    return len(manifest["files"])


def audit_stats_occupancy(path, data):
    values = {}
    with Path(path).open() as source:
        for line in source:
            words = line.split()
            if len(words) >= 2 and ".h2pHint." in words[0]:
                values[words[0].split(".h2pHint.", 1)[1]] = float(words[1])
    for table, fields in data["occupancy"].items():
        for field, expected in fields.items():
            key = f"{table}_{field}"
            if key not in values or not math.isclose(
                values[key], expected, rel_tol=1e-5, abs_tol=1e-6
            ):
                raise ValueError(f"stats.txt/JSON occupancy differ for {key}")
    return len(values)


def audit_one(job):
    path, expected, common = Path(job["path"]), job["expected"], job["common"]
    errors, audit = [], {}
    try:
        status = json.loads((path / "status.json").read_text())
        identity = json.loads((path / "identity.json").read_text())
        if status.get("result") != "pass" or status.get("returncode") != 0:
            raise ValueError(
                "simulator status/exit code is not a completed pass"
            )
        if status.get("identity") != identity:
            raise ValueError(
                "status identity differs from independently saved identity"
            )
        for key, value in common.items():
            if identity.get(key) != value:
                raise ValueError(
                    f"run identity differs from experiment for {key}"
                )
        for key, value in expected.items():
            if identity.get(key) != value:
                raise ValueError(
                    f"run stage/mode/task identity differs for {key}"
                )
        command = shlex.split((path / "cmd.txt").read_text())
        if command != status["command"]:
            raise ValueError("recorded command differs from executed command")
        option = lambda name: next(
            (
                word.split("=", 1)[1]
                for word in command
                if word.startswith(name + "=")
            ),
            None,
        )
        candidates, metadata = [], None
        if expected["mode"] != "off":
            candidate_path = Path(option("--h2p-hint-candidates"))
            if sha256(candidate_path) != identity["candidate_sha256"]:
                raise ValueError("candidate input SHA changed")
            candidates = load_candidates(candidate_path, expected["ideal"])
        if expected["mode"] == "replay":
            metadata_path = Path(option("--h2p-hint-metadata"))
            if sha256(metadata_path) != identity["metadata_sha256"]:
                raise ValueError("metadata input SHA changed")
            metadata = load_metadata(metadata_path, expected["ideal"])
        if ("--ideal-hint-ibp" in command) != expected["ideal"]:
            raise ValueError("command ideal flag differs from expected stage")
        audit = audit_run(
            path,
            status["returncode"],
            100_000_000,
            50_000_000,
            1_000_000,
            expected["mode"],
            candidates,
            expected["ideal"],
            metadata,
            check_occupancy=True,
        )
        errors.extend(audit["errors"])
        if expected["mode"] != "off":
            audit_stats_occupancy(path / "stats.txt", audit["hint"])
        if metadata:
            if expected["stage"] == "fixed":
                actual_masks = {
                    row["pc"]: row["mask"]
                    for row in metadata["cam"]
                    if row["valid"]
                }
                if actual_masks != expected["task"]["old"]["masks"]:
                    raise ValueError(
                        "fixed-capacity control changed original final CAM PC/mask"
                    )
                if (
                    metadata["source"]["profile_sha256"]
                    != expected["task"]["old"]["cache"][
                        "hint-profile.jsonl.gz"
                    ]["sha256"]
                ):
                    raise ValueError(
                        "fixed control did not use original audited profile"
                    )
            if (
                expected["stage"] == "finite"
                and identity["metadata_sha256"]
                != expected["task"]["old"]["image"]["sha256"]
            ):
                raise ValueError(
                    "finite control changed original joint metadata"
                )
            if expected["stage"] == "masks":
                actual_masks = {
                    address(row["pc"]): row["mask"]
                    for row in metadata["cam"]
                    if row["valid"]
                }
                if actual_masks != dict.fromkeys(candidates, expected["mask"]):
                    raise ValueError(
                        "fixed-mask replay omits a baseline candidate or changes mask"
                    )
            if expected["ideal"]:
                stage, task = expected["stage"], expected["task"]
                cache = Cache(
                    task["old"]["cache_directory"]
                    if stage == "fixed"
                    else Path(job["root"]) / "profiles" / tag(task)
                )
                if stage == "joint":
                    masks, choices = actual_best_masks(
                        Path(job["root"]), task, candidates
                    )
                    saved = json.loads(
                        (
                            Path(job["root"])
                            / "inputs"
                            / tag(task)
                            / "best-masks.json"
                        ).read_text()
                    )
                    if saved != choices:
                        raise ValueError(
                            "best masks do not reproduce actual corrected-harmed and tie rules"
                        )
                elif stage == "fixed":
                    masks = {
                        address(pc): mask
                        for pc, mask in task["old"]["masks"].items()
                    }
                else:
                    masks = dict.fromkeys(candidates, expected["mask"])
                regenerated, iterations = reproduce_unlimited(
                    cache, masks, prune=stage == "joint"
                )
                for field in ("cam", "slots", "entries"):
                    if regenerated[field] != metadata[field]:
                        raise ValueError(
                            f"unlimited {field} cannot be independently rebuilt from recorded profile"
                        )
                builder = json.loads(
                    (metadata_path.parent / "builder.json").read_text()
                )
                actual_iterations = builder["prune_iterations"]
                if len(actual_iterations) != len(iterations) or any(
                    recorded["input_pcs"] != len(rebuilt["input"])
                    or recorded["output_pcs"] != len(rebuilt["remaining"])
                    or sorted(
                        address(row["pc"]) for row in recorded["removed"]
                    )
                    != sorted(
                        set(rebuilt["input"]) - set(rebuilt["remaining"])
                    )
                    for recorded, rebuilt in zip(actual_iterations, iterations)
                ):
                    raise ValueError(
                        "joint pruning iterations differ from independent fixed point"
                    )
                audit["independent_image_rebuild"] = {
                    "pass": True,
                    "iterations": len(iterations),
                    "mapped_keys": len(metadata["entries"]),
                }
            config = (path / "config.ini").read_text()
            period = int(
                re.search(
                    r"\[system\.cpu_clk_domain\][^\[]*\nclock=(\d+)", config
                ).group(1)
            )
            start_cycle = audit["phases"]["warmup_complete"]["tick"] // period
            end_cycle = audit["phases"]["measure_complete"]["tick"] // period
            transitions = audit_conf0(
                path / "conf0.txt",
                metadata,
                clock_period=period,
                start_cycle=start_cycle,
                end_cycle=end_cycle,
            )
            if transitions != audit["hint"]["queue"]["zero_transitions"]:
                raise ValueError(
                    "conf0 transition count differs from statistics"
                )
        if expected["mode"] == "collect":
            cache_dir = Path(job["root"]) / "profiles" / tag(expected["task"])
            cache = Cache(cache_dir)
            if (
                sha256(path / "hint-profile.jsonl.gz")
                != cache.identity["profile_sha256"]
            ):
                raise ValueError("collect gzip profile SHA differs from cache")
            for name, digest in cache.identity.get("files", {}).items():
                if sha256(cache_dir / name) != digest:
                    raise ValueError("collect cache payload SHA changed")
            counts = Counter(row["pc"] for row in cache.rows())
            observed = {
                address(row["pc"]): row["executions"]
                for row in audit["hint"]["pcs"]
                if address(row["pc"]) in candidates
            }
            if any(
                counts[pc] != count for pc, count in observed.items()
            ) or set(counts) - set(candidates):
                raise ValueError(
                    "profile rows do not cover every candidate execution"
                )
            comparison = compare_shared_profiles(
                expected["task"]["old"]["cache_directory"], cache_dir
            )
            audit["shared_profile_comparison"] = comparison
            if not comparison["pass"]:
                raise ValueError("shared PC samples differ from old collect")
        names = [
            "status.json",
            "identity.json",
            "cmd.txt",
            "config.ini",
            "stats.txt",
            "h2p-indirect.txt",
            "stdout.log",
            "stderr.log",
        ]
        if expected["mode"] != "off":
            names += ["hint-stats.json", "conf0.txt"]
        artifact_sha = {name: sha256(path / name) for name in names}
        artifact_sha.update(
            {
                "h2p-indirec/" + child.name: sha256(child)
                for child in sorted((path / "h2p-indirec").glob("*.txt"))
            }
        )
        audit["artifact_sha256"] = artifact_sha
    except Exception as error:
        errors.append(f"{type(error).__name__}: {error}")
    return {
        "path": str(path),
        "stage": expected["stage"],
        "task": tag(expected["task"]),
        "mask": expected["mask"],
        **audit,
        "result": "pass" if not errors else "fail",
        "errors": errors,
    }


def audit_experiment(output_root, jobs=25):
    root = Path(output_root)
    plan = json.loads((root / "experiment.json").read_text())
    common, tasks = plan["identity"], plan["tasks"]
    errors, work = [], []
    if (
        plan.get("expected_runs") != 185
        or plan.get("expected_windows") != 9250
        or plan.get("full_runs") != COUNTS
    ):
        errors.append("experiment matrix differs from accepted 185-run plan")
    for expected in (
        common["gem5"],
        common["config"],
        common["source_manifest"],
        common["workload_list"],
        common["old_complete_audit"],
        common["build_audit"],
        common["validation_audit"],
        *(task["checkpoint"] for task in tasks),
    ):
        if sha256(expected["path"]) != expected["sha256"]:
            errors.append(
                "source/binary/checkpoint input SHA changed: "
                + expected["path"]
            )
    for name, digest in common["tools"].items():
        path = Path(__file__).parent / name
        if not path.exists():
            path = Path(__file__).parent.parent / name
        if sha256(path) != digest:
            errors.append(
                "tool source changed during experiment: " + str(path)
            )
    for task in tasks:
        old = task["old"]
        for identity in (
            old["image"],
            old["candidates"],
            old["profile_audit"],
            old["joint_audit"],
            old["joint_status"],
            old["collect_status"],
            *old["cache"].values(),
        ):
            if sha256(identity["path"]) != identity["sha256"]:
                errors.append(
                    "original control/profile input changed: "
                    + identity["path"]
                )
        complete = json.loads(
            Path(common["old_complete_audit"]["path"]).read_text()
        )
        for kind in ("profile", "joint"):
            key = (
                ("profiles/" if kind == "profile" else "runs/joint/")
                + tag(task)
                + "/audit.json"
            )
            if (
                old[kind + "_audit"]["sha256"]
                != complete["audit_artifact_sha256"][key]
            ):
                errors.append(
                    "old control audit is not bound to the complete old audit"
                )
        baseline = root / "runs" / "baseline" / tag(task) / "h2p-indirect.txt"
        if baseline.exists():
            records = []
            for line in baseline.read_text().splitlines():
                columns = [item.strip() for item in line.split(",")]
                records.append((address(columns[0]), int(columns[1])))
            ranked = [
                pc
                for pc, count in sorted(
                    records, key=lambda item: (-item[1], item[0])
                )
                if count > 0
            ]
            actual = load_candidates(
                root / "inputs" / tag(task) / "candidates.json", ideal=True
            )
            if actual != ranked:
                errors.append(
                    "candidate inventory is not every new baseline H2P PC in rank order"
                )
    source_files = 0
    try:
        source_files = audit_source_files(
            common["source_root"],
            json.loads(Path(common["source_manifest"]["path"]).read_text()),
        )
    except ValueError as error:
        errors.append(str(error))
    for task in tasks:
        for stage in COUNTS:
            masks = range(32) if stage == "masks" else (None,)
            for mask in masks:
                path = root / "runs" / stage / tag(task)
                if mask is not None:
                    path = path / f"mask-{mask:02d}"
                mode = (
                    "off"
                    if stage == "baseline"
                    else "collect"
                    if stage == "collect"
                    else "replay"
                )
                expected = {
                    "task": task,
                    "stage": stage,
                    "mask": mask,
                    "mode": mode,
                    "ideal": stage not in ("baseline", "finite"),
                }
                work.append(
                    {
                        "path": str(path),
                        "expected": expected,
                        "common": common,
                        "root": str(root),
                    }
                )
    actual = {path.parent for path in (root / "runs").rglob("status.json")}
    expected = {Path(job["path"]) for job in work}
    if actual != expected:
        errors.append(
            f"run directories differ: {len(expected - actual)} missing, {len(actual - expected)} extra"
        )
    with futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(audit_one, work))
    failed = [row for row in results if row["result"] != "pass"]
    windows = sum(row.get("window_count", 0) for row in results)
    if failed or len(results) != 185 or windows != 9250:
        errors.append(
            f"full-run/window audit failed: {len(results)} runs, {len(failed)} failed, {windows} windows"
        )
    comparisons = []
    for task in tasks:
        for label, before, after in (
            (
                "finite_regression",
                Path(task["old"]["joint_status"]["path"]).parent,
                root / "runs" / "finite" / tag(task),
            ),
            (
                "observer",
                root / "runs" / "baseline" / tag(task),
                root / "runs" / "collect" / tag(task),
            ),
        ):
            try:
                comparison = compare_observer(
                    before, after, check_windows=True
                )
                comparisons.append(
                    {"kind": label, "task": tag(task), **comparison}
                )
                if not comparison["pass"]:
                    errors.append(f"{label} statistics differ for {tag(task)}")
            except Exception as error:
                errors.append(f"{label}: {error}")
    report = {
        "format": "h2p-hint-ideal-complete-audit",
        "version": 3,
        "result": "pass" if not errors else "fail",
        "errors": errors,
        "full_runs": len(results),
        "failed_runs": len(failed),
        "window_count": windows,
        "gem5_sha256": common["gem5"]["sha256"],
        "source_manifest_sha256": common["source_manifest"]["sha256"],
        "experiment_sha256": sha256(root / "experiment.json"),
        "verified_source_files": source_files,
        "expected_sweep_rows": sum(
            len(
                load_candidates(
                    root / "inputs" / tag(task) / "candidates.json", ideal=True
                )
            )
            for task in tasks
        )
        * 32,
        "finished_utc": now(),
        "comparisons": comparisons,
        "runs": results,
    }
    write_json(root / "complete-audit.json", report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=25)
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 25:
        parser.error("--jobs must be within the shared 25-slot ceiling")
    with (args.output_root / "runner.lock").open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError(
                "active runner/audit owns the shared compute budget"
            ) from error
        report = audit_experiment(args.output_root.resolve(), args.jobs)
    print(
        json.dumps(
            {
                key: value
                for key, value in report.items()
                if key not in ("runs", "comparisons")
            },
            indent=2,
        )
    )
    return int(report["result"] != "pass")


if __name__ == "__main__":
    raise SystemExit(main())
