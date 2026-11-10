"""Ten-slice 1M/1M Hint Off CFI observer run using an explicitly frozen ELF.

No build and no Bundle capability. The additional audit flag only writes
committed-control observations; results must reproduce existing Off windows
and committed prediction counters exactly. Run only on ps1/ps2.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import json
from pathlib import Path
import re
import shutil
import time
import spec_smoke as smoke


def previous_report(root, task):
    base = root / task["tag"]
    choices = (
        base / "baseline/stage.json",
        base / "baseline/report.json",
        base / "report.json",
        base / "stage.json",
    )
    paths = [path for path in choices if path.is_file()]
    smoke.need(paths, f'No completed baseline report for {task["tag"]} under {root}')
    path = paths[0]
    data = smoke.read(path)
    if "checkpoint" in data:
        smoke.need(
            data["checkpoint"] == task["checkpoint"],
            "Comparison checkpoint identity differs",
        )
    report = data.get("report", data)
    smoke.need(
        "window" in report and "prediction_metrics" in report,
        "Incomplete comparison report",
    )
    smoke.need(
        report["window"]["warmup_instructions"] == 1_000_000
        and report["window"]["measure_instructions"] == 1_000_000,
        "Comparison is not the authorized 1M/1M window",
    )
    return path, report


def event_audit(path):
    smoke.need(
        path.is_file() and path.stat().st_size > 0,
        "Missing committed CFI observer stream",
    )
    with path.open("rb") as stream:
        stream.seek(-1, 2)
        smoke.need(stream.read(1) == b"\n", f"CFI JSONL missing final newline: {path}")
    previous = 0
    counts = {
        "all": 0,
        "warmup": 0,
        "measured": 0,
        "taken_call_return": 0,
        "measured_taken_call_return": 0,
    }
    with path.open() as stream:
        for line_number, line in enumerate(stream, 1):
            try:
                event = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(
                    f"Invalid CFI JSON: {path}:{line_number}: {line[:180]!r}"
                ) from error
            seq = event["seq"]
            smoke.need(
                type(seq) is int and seq > previous,
                "CFI sequence is not strictly increasing",
            )
            previous = seq
            smoke.need(
                type(event["size"]) is int and event["size"] in (2, 4),
                "CFI original size invalid",
            )
            for flag in ("call", "return", "indirect", "taken", "measured"):
                smoke.need(type(event[flag]) is bool, "Missing CFI Boolean " + flag)
            for field in ("pc", "target"):
                value = (
                    int(event[field], 0)
                    if isinstance(event[field], str)
                    else event[field]
                )
                smoke.need(
                    type(value) is int and 0 <= value < 1 << 64 and value % 2 == 0,
                    "Invalid aligned CFI " + field,
                )
            counts["all"] += 1
            counts["measured" if event["measured"] else "warmup"] += 1
            if event["taken"] and (event["call"] or event["return"]):
                counts["taken_call_return"] += 1
                counts["measured_taken_call_return"] += event["measured"]
    smoke.need(
        counts["measured"] > 0 and counts["taken_call_return"] > 0,
        "No useful committed CFI population",
    )
    return {
        "file": smoke.identity(path),
        "counts": counts,
        "scope": "All observed committed CFI, including warmup; measured flag retained. No speculative events.",
    }


def inactive_audits(directory):
    result = {}
    for name in ("bundle-controller-audit.jsonl", "bundle-trace-audit.jsonl"):
        path = directory / name
        smoke.need(path.is_file(), f"Missing inactive Bundle observer stream: {path}")
        smoke.need(
            path.stat().st_size == 0,
            f"Off without Bundle capability unexpectedly wrote controller/trace events: {path}",
        )
        result[name] = smoke.identity(path)
    return {
        "files": result,
        "expected_empty": True,
        "scope": "Both streams exist and are empty in Off; no callback flush/close error in audited logs. "
        "Empty files alone cannot prove hardware profiling stream completion.",
    }


class OffObserverRunner(smoke.Runner):
    def run_off(self, task):
        self.check_runtime_inputs()
        smoke.need(
            smoke.identity(task["checkpoint"]["path"]) == task["checkpoint"],
            "Checkpoint changed",
        )
        directory = self.root / task["tag"]
        command, env = smoke.render_command(
            self.manifest,
            task["checkpoint"]["path"],
            directory,
            self.ref,
            self.gem5,
            "off",
            meta_entries=16,
        )
        # Explicit local extension: do not modify the established runner or
        # enable --h2p-hint-bundle, even if the frozen ELF supports it.
        smoke.need("--h2p-hint-bundle" not in command, "Unexpected Bundle capability")
        command.append("--h2p-hint-bundle-audit")
        self.invoke(command, directory, env)
        report = self.audit_simulation(directory)
        # Preserve independently valid simulation evidence even when the
        # observer stream itself later fails its format/identity checks.
        smoke.write(directory / "simulation-only-report.json", report)
        config = (directory / "config.ini").read_text()
        for setting in (
            "hintMode=off",
            "hintBundleEnable=false",
            "hintBundleAudit=true",
        ):
            smoke.need(
                re.search(r"^" + re.escape(setting) + r"$", config, re.M),
                "Observer Off config differs: " + setting,
            )
        logs = (directory / "stdout.log").read_text() + (
            directory / "stderr.log"
        ).read_text()
        smoke.need(
            not re.search(
                r"kernel panic|\bOops:|HIT BAD TRAP|\bERROR\b|Traceback \(", logs, re.I
            ),
            "Guest/program failure in Off observer run",
        )
        comparisons = []
        for root in self.args.compare_baseline_root:
            path, expected = previous_report(root, task)
            equal_window = report["window"] == expected["window"]
            equal_metrics = (
                report["prediction_metrics"] == expected["prediction_metrics"]
            )
            comparisons.append(
                {
                    "baseline_report": smoke.identity(path),
                    "same_window": equal_window,
                    "same_prediction_metrics": equal_metrics,
                }
            )
        report.update(
            {
                "schema": "hint-bundle-off-observer-slice/v1",
                "tag": task["tag"],
                "checkpoint": task["checkpoint"],
                "events": event_audit(directory / "bundle-committed-cfi.jsonl"),
                "inactive_bundle_audits": inactive_audits(directory),
                "comparisons": comparisons,
                "bundle_capability_enabled": False,
            }
        )
        smoke.write(directory / "report.json", report)
        smoke.need(
            all(x["same_window"] and x["same_prediction_metrics"] for x in comparisons),
            "Observer Off cycles/miss differ from completed baseline; see report.json",
        )
        smoke.write(
            directory / "artifacts.json",
            [
                smoke.identity(directory / name)
                for name in (
                    "report.json",
                    "simulation-only-report.json",
                    "command.json",
                    "resource-admission.json",
                    "stdout.log",
                    "stderr.log",
                    "config.ini",
                    "stats.txt",
                    "h2p-target-summary.json",
                    "hint-discovery.json",
                    "bundle-committed-cfi.jsonl",
                    "bundle-controller-audit.jsonl",
                    "bundle-trace-audit.jsonl",
                )
            ],
        )
        return report


def main():
    smoke.require_experiment_host()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--gem5-sha256", required=True)
    parser.add_argument("--ref-so", type=Path, required=True)
    parser.add_argument(
        "--ref-sha256", help="Optional independently frozen reference hash"
    )
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument(
        "--compare-baseline-root",
        "--baseline-root",
        type=Path,
        action="append",
        required=True,
        help="Completed Off pipeline root; repeat to compare several baseline datasets",
    )
    parser.add_argument(
        "--manifest", type=Path, default=smoke.PROJECT / "manifest/smoke-test.json"
    )
    parser.add_argument(
        "--workload-manifest",
        type=Path,
        default=smoke.PROJECT / "workload/cpython-norvv-uart16550-100m200m-heavy.json",
    )
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--jobs", default="auto")
    parser.add_argument("--memory-per-job-gib", type=int, default=12)
    parser.add_argument("--reserve-gib", type=int, default=8)
    args = parser.parse_args()
    smoke.need(
        args.jobs == "auto" or (args.jobs.isdecimal() and int(args.jobs) > 0),
        "Invalid job count",
    )
    smoke.need(
        args.memory_per_job_gib > 0 and args.reserve_gib >= 8,
        "Invalid resource reserve",
    )
    smoke.need(
        len(args.gem5_sha256) == 64 and smoke.sha(args.gem5) == args.gem5_sha256,
        "Explicit frozen ELF SHA256 mismatch",
    )
    if args.ref_sha256:
        smoke.need(
            smoke.sha(args.ref_so) == args.ref_sha256,
            "Frozen reference SHA256 mismatch",
        )
    args.full_test = False
    args.import_baseline_root = None
    args.hint_meta_entries = 16
    runner = OffObserverRunner(args)
    smoke.need(
        not runner.root.exists() or not any(runner.root.iterdir()),
        "Choose a fresh output root; existing experiment input/output will not be overwritten",
    )
    runner.root.mkdir(parents=True, exist_ok=True)
    own_identity = smoke.identity(__file__)
    with (runner.root / ".runner.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        shutil.copy2(__file__, runner.root / "own-script.py")
        prepared = {
            "schema": "hint-bundle-off-observer-inputs/v1",
            "common": runner.common,
            "own_script": own_identity,
            "tasks": runner.tasks,
            "frozen_elf_expected_sha256": args.gem5_sha256,
            "comparison_roots": [
                str(x.resolve(strict=True)) for x in args.compare_baseline_root
            ],
            "mode": "off",
            "bundle_capability_enabled": False,
            "observer_enabled": True,
        }
        # Verify all comparison inputs before launching the first child.
        prepared["comparison_inputs"] = []
        for task in runner.tasks:
            for root in args.compare_baseline_root:
                path, _ = previous_report(root, task)
                prepared["comparison_inputs"].append(smoke.identity(path))
        smoke.write(runner.root / "prepared.json", prepared)
        try:
            runner.plan_resources("off-cfi-observer", len(runner.tasks))
            with ThreadPoolExecutor(max_workers=runner.max_jobs) as pool:
                reports = list(pool.map(runner.run_off, runner.tasks))
            runner.check_runtime_inputs()
            smoke.need(
                smoke.identity(__file__) == own_identity,
                "Observer runner changed while running",
            )
            for frozen in prepared["comparison_inputs"]:
                smoke.need(
                    smoke.identity(frozen["path"]) == frozen,
                    "Comparison input changed during experiment",
                )
            instructions = sum(x["window"]["measure_instructions"] for x in reports)
            cycles = sum(x["window"]["measure_cycles"] for x in reports)
            summary = {
                "schema": "hint-bundle-off-observer-summary/v1",
                "status": "PASS",
                "host": smoke.execution_host(),
                "inputs": prepared,
                "slices": reports,
                "aggregate": {
                    "measure_instructions": instructions,
                    "measure_cycles": cycles,
                    "ipc": instructions / cycles,
                    "nonreturn_target_misses": sum(
                        x["prediction_metrics"]["nonreturn_ittage_plus_hint"]["misses"]
                        for x in reports
                    ),
                },
                "all_baseline_windows_and_prediction_metrics_equal": True,
                "completed_at": time.time(),
            }
            smoke.write(runner.root / "summary.json", summary)
            print(
                json.dumps(
                    {
                        "status": "PASS",
                        "slices": len(reports),
                        "aggregate": summary["aggregate"],
                    }
                ),
                flush=True,
            )
        except Exception as error:
            smoke.write(
                runner.root / "error.json",
                {"status": "FAIL", "error": str(error), "time": time.time()},
            )
            raise


if __name__ == "__main__":
    main()
