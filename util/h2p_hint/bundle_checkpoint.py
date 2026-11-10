#!/usr/bin/env python3
"""Capture an authorized subset of existing SimPoints, without calibration.

Default is read-only dry-run (JSON on stdout). See bundle_checkpoint.md.
Only --execute creates the exclusively new output tree or starts NEMU.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal, InvalidOperation
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

WARMUP = 100_000_000
INTERVAL = 200_000_000
THREAD_ENV = {
    key: "1"
    for key in (
        "ZSTD_NBTHREADS",
        "OMP_NUM_THREADS",
        "OPENBLAS_NUM_THREADS",
        "MKL_NUM_THREADS",
        "XSGEM5_PATCH_ZSTD_THREADS",
    )
}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def load_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate JSON key {key!r}: {path}")
            result[key] = value
        return result

    return json.loads(
        path.read_text(),
        object_pairs_hook=unique,
        parse_constant=lambda value: (_ for _ in ()).throw(
            ValueError(f"nonfinite JSON {value}: {path}")
        ),
    )


def file_info(path):
    require(path.is_file() and path.stat().st_size > 0, f"missing/empty input: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return {
        "path": str(path),
        "sha256": digest.hexdigest(),
        "bytes": path.stat().st_size,
    }


def positive(value):
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return result


def table(path, weights=False):
    result = {}
    lines = path.read_text(encoding="ascii").splitlines()
    require(bool(lines), f"empty metadata: {path}")
    for line in lines:
        fields = line.split()
        require(
            len(fields) == 2 and re.fullmatch(r"[0-9]+", fields[1]),
            f"invalid metadata row: {path}: {line!r}",
        )
        cluster = int(fields[1])
        require(cluster not in result, f"duplicate cluster {cluster}: {path}")
        if weights:
            require(
                bool(
                    re.fullmatch(
                        r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?",
                        fields[0],
                    )
                ),
                f"invalid weight syntax: {line!r}",
            )
            number = Decimal(fields[0])
            require(number.is_finite() and number >= 0, f"invalid weight: {line!r}")
        else:
            require(
                bool(re.fullmatch(r"[0-9]+", fields[0])), f"invalid point: {line!r}"
            )
            number = int(fields[0])
            require(
                number not in [row[0] for row in result.values()],
                f"duplicate point: {path}",
            )
        result[cluster] = (number, line)
    return result


def resources(output, workers):
    cpus = len(os.sched_getaffinity(0))
    load = os.getloadavg()[0]
    mem = next(
        int(line.split()[1]) * 1024
        for line in Path("/proc/meminfo").read_text().splitlines()
        if line.startswith("MemAvailable:")
    )
    ancestor = output.parent
    while not ancestor.exists():
        ancestor = ancestor.parent
    free = shutil.disk_usage(ancestor).free
    return {
        "affinity_cpus": cpus,
        "load1": load,
        "available_memory_bytes": mem,
        "disk_free_bytes": free,
        "workers": workers,
        "threads_per_worker": 1,
        "memory_required_bytes": (32 + 20 * workers) * 1024**3,
        "disk_required_bytes": 192 * 1024**3,
        "memory_budget_source": "serializer.cpp:286-330 malloc(ZSTD_compressBound(PMEM_SIZE)); guest8GiB plus compression buffer approximately8GiB; pilot RSS measurement pending",
        "cpu_budget_ok": workers <= max(0, math.floor(cpus - load)),
        "memory_budget_ok": mem >= (32 + 20 * workers) * 1024**3,
        "disk_budget_ok": free >= 192 * 1024**3,
    }


def build_plan(args):
    output = args.output.absolute().resolve()
    require(
        not output.exists(), f"output already exists (no overwrite/resume): {output}"
    )
    source = args.source_root.resolve()
    require(
        not output.is_relative_to(source),
        "output cannot be inside formal checkpoint source",
    )
    smoke_path = args.smoke.resolve()
    smoke = load_json(smoke_path)
    inputs_path = args.inputs.resolve()
    inputs = load_json(inputs_path)
    require(
        inputs.get("schema") == "bundle-checkpoint-inputs/v1", "invalid inputs schema"
    )
    nemu = args.nemu.resolve()
    restorer = args.restorer.resolve()
    common = {
        "nemu": file_info(nemu),
        "restorer": file_info(restorer),
        "smoke": file_info(smoke_path),
        "inputs": file_info(inputs_path),
        "generator": file_info(Path(__file__).resolve()),
    }
    require(os.access(nemu, os.X_OK), f"NEMU is not executable: {nemu}")
    entries = smoke["workloads"]["cpts"]
    selected = set(args.bench or [entry["bench"] for entry in entries])
    require(
        selected <= {entry["bench"] for entry in entries},
        "bench not authorized by smoke manifest",
    )
    jobs, seen = [], set()
    for entry in entries:
        bench = entry["bench"]
        if bench not in selected:
            continue
        require(bool(re.fullmatch(r"[A-Za-z0-9_]+", bench)), f"unsafe bench: {bench!r}")
        require(bench not in seen, f"duplicate bench: {bench}")
        seen.add(bench)
        workload = bench + "_test"
        old = source / workload
        manifest_path = old / "manifest.json"
        manifest = load_json(manifest_path)
        require(
            manifest.get("workload") == workload
            and manifest.get("scale") == "test"
            and manifest.get("isa_variant") == "no-rvv"
            and manifest.get("warmup_insts") == WARMUP
            and manifest.get("interval_insts") == INTERVAL
            and manifest.get("format") == "zstd",
            f"formal contract mismatch: {manifest_path}",
        )
        loops = manifest.get("accepted_loops")
        require(
            type(loops) is int and loops > 0, f"invalid accepted loops: {manifest_path}"
        )
        points = table(old / "simpoints0")
        weights = table(old / "weights0", True)
        require(
            manifest.get("checkpoint_count") == len(points),
            f"formal checkpoint count mismatch: {bench}",
        )
        require(
            points.keys() == weights.keys(), f"point/weight cluster mismatch: {old}"
        )
        require(
            abs(sum(row[0] for row in weights.values()) - 1) <= Decimal("0.000005"),
            f"formal weights not normalized: {old}",
        )
        requested = entry["cpt_list"]
        require(
            all(isinstance(p, str) and re.fullmatch(r"[0-9]+", p) for p in requested),
            f"invalid smoke points: {bench}",
        )
        wanted = [int(p) for p in requested]
        require(
            len(wanted) == len(set(wanted)) and bool(wanted),
            f"duplicate/empty points: {bench}",
        )
        require(
            set(wanted) <= {row[0] for row in points.values()},
            f"missing original point: {bench}",
        )
        chosen = [(cluster, row) for cluster, row in points.items() if row[0] in wanted]
        image_entry = inputs["benches"][bench]
        image = Path(image_entry["image"]).resolve()
        image_manifest_path = Path(image_entry["image_manifest"]).resolve()
        image_manifest = load_json(image_manifest_path)
        require(
            image_manifest.get("loops") == loops
            and image_manifest.get("isa_variant") == "no-rvv",
            f"new image loops/ISA mismatch: {bench}",
        )
        require(
            bool(
                re.fullmatch(
                    r"[0-9a-f]{64}", str(manifest.get("runtime_manifest_sha256", ""))
                )
            ),
            f"missing original runtime hash: {bench}",
        )
        require(
            image_manifest.get("runtime_manifest_sha256")
            == manifest.get("runtime_manifest_sha256"),
            f"new image changed original runtime: {bench}",
        )
        image_info = file_info(image)
        artifact = image_manifest["artifacts"]["published_image"]
        require(
            Path(artifact["path"]).name == workload + ".bin",
            f"image manifest workload mismatch: {bench}",
        )
        uart = image_manifest.get("uart", {})
        require(
            uart.get("compatible") == "ns16550a"
            and uart.get("clock_frequency") == 10_000_000
            and uart.get("reg_shift") == 2
            and uart.get("reg_io_width") == 4,
            f"image UART10MHz contract mismatch: {bench}",
        )
        require(
            artifact["sha256"] == image_info["sha256"],
            f"new image hash mismatch: {bench}",
        )
        with image.open("rb") as stream:
            image_prefix = stream.read(4096)
        with restorer.open("rb") as stream:
            restorer_prefix = stream.read(4096)
        require(
            len(image_prefix) == len(restorer_prefix) == 4096
            and image_prefix == restorer_prefix,
            f"embedded image/restorer first4096 mismatch: {bench}",
        )
        prefix_hash = hashlib.sha256(image_prefix).hexdigest()
        require(
            image_manifest.get("restorer_prefix_sha256") == prefix_hash,
            f"image manifest restorer prefix mismatch: {bench}",
        )
        raw = output / workload
        command = [
            str(nemu),
            str(image),
            "-D",
            str(raw),
            "-w",
            workload,
            "-b",
            "-C",
            "checkpoint",
            "-S",
            str(raw / "cluster"),
            "--cpt-interval",
            str(INTERVAL),
            "--checkpoint-format",
            "zstd",
            "--warmup-interval",
            str(WARMUP),
        ]
        originals = {
            name: file_info(old / name)
            for name in (
                "manifest.json",
                "simpoints0",
                "weights0",
                "labels0",
                "image-build-manifest.json",
                "tools-manifest.json",
            )
        }
        for name, field in (
            ("image-build-manifest.json", "image_build_manifest_sha256"),
            ("tools-manifest.json", "tools_manifest_sha256"),
        ):
            require(
                originals[name]["sha256"] == manifest.get(field),
                f"formal source hash mismatch: {bench}/{name}",
            )
        jobs.append(
            {
                "bench": bench,
                "workload": workload,
                "accepted_loops": loops,
                "source": originals,
                "image": image_info,
                "image_manifest": file_info(image_manifest_path),
                "restorer_prefix_sha256": prefix_hash,
                "selected": [
                    {
                        "point": row[0],
                        "cluster_id": cluster,
                        "weight_text": weights[cluster][1].split()[0],
                        "requested_capture_roi_insts": max(
                            0, row[0] * INTERVAL - WARMUP
                        ),
                    }
                    for cluster, row in chosen
                ],
                "simpoints0": "\n".join(row[1] for _, row in chosen) + "\n",
                "weights0": "\n".join(weights[c][1] for c, _ in chosen) + "\n",
                "command": command,
                "output": str(raw),
            }
        )
    require(bool(jobs), "no selected workloads")
    return {
        "schema": "bundle-fixed-checkpoint-plan/v1",
        "output": str(output),
        "mode": "execute" if args.execute else "dry-run",
        "inputs": common,
        "resources": resources(output, min(args.workers, len(jobs))),
        "timeout_seconds_per_bench": args.timeout,
        "environment": THREAD_ENV,
        "recluster": False,
        "restorer_command_argument": False,
        "source_nemu_commit": "7c8b842b101c0932eb769f5aa2721770b8690638",
        "source_contract": "serializer.cpp:549-565; path_manager.cpp:60-61,105-109; image_loader.c:276-315",
        "jobs": jobs,
    }


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def _capture(job, plan):
    raw = Path(job["output"])
    cluster = raw / "cluster" / job["workload"]
    cluster.mkdir(parents=True)
    provenance = raw / "original"
    provenance.mkdir()
    for name, info in job["source"].items():
        require(file_info(Path(info["path"])) == info, f"input changed: {info['path']}")
        shutil.copyfile(info["path"], provenance / name)
    for name in ("simpoints0", "weights0"):
        (cluster / name).write_text(job[name], encoding="ascii")
    for info in (
        job["image"],
        job["image_manifest"],
        plan["inputs"]["nemu"],
        plan["inputs"]["restorer"],
    ):
        require(file_info(Path(info["path"])) == info, f"input changed: {info['path']}")
    start = time.monotonic()
    result = {"schema": "bundle-fixed-checkpoint-run/v1", "plan": job, "pass": False}
    try:
        with (raw / "checkpoint.out").open("w") as stdout, (
            raw / "checkpoint.err"
        ).open("w") as stderr:
            process = subprocess.run(
                job["command"],
                stdout=stdout,
                stderr=stderr,
                env={**os.environ, **THREAD_ENV},
                timeout=plan["timeout_seconds_per_bench"],
                check=False,
            )
        result["exit_code"] = process.returncode
        require(process.returncode == 0, f"NEMU exit {process.returncode}")
        out = (raw / "checkpoint.out").read_text(errors="replace")
        err = (raw / "checkpoint.err").read_text(errors="replace")
        require(
            out.count("HIT GOOD TRAP") == 1
            and "trap code:0" in out
            and "NEMU exit with good state" in out,
            "guest GOOD TRAP not established",
        )
        require(
            not any(
                s in out + err for s in ("HIT BAD TRAP", "Kernel panic", "kernel panic")
            ),
            "guest failure marker",
        )
        require(
            len(
                re.findall(
                    r"CPYTHON_RESULT\s+bench="
                    + re.escape(job["bench"])
                    + r"\s+status=0\b",
                    err,
                )
            )
            == 1,
            "guest benchmark result mismatch",
        )
        require(
            re.findall(r"CPYTHON_VERSION=([^\r\n]+)", err) == ["Python 3.14.7"],
            "guest CPython version mismatch",
        )
        checkpoint = raw / "checkpoint" / job["workload"]
        require(checkpoint.is_dir(), "missing checkpoint output")
        actual = {p.name for p in checkpoint.iterdir()}
        require(
            actual == {str(row["point"]) for row in job["selected"]},
            "captured point set mismatch",
        )
        result["checkpoint_files"] = []
        for row in job["selected"]:
            files = list((checkpoint / str(row["point"])).iterdir())
            require(
                len(files) == 1 and files[0].suffix == ".zstd",
                "expected one zstd file per point",
            )
            result["checkpoint_files"].append(file_info(files[0]))
        # Preserve evidence verbatim; thresholds are not exact basic-block capture counts.
        result["capture_log_evidence"] = [
            line
            for line in (out + "\n" + err).splitlines()
            if re.search(
                r"checkpoint|taking.*cpt|simpoint|Writing PC:|Should take cpt now:",
                line,
                re.I,
            )
        ]
        result["observed_capture_roi_counts"] = [
            int(n)
            for n in re.findall(
                r"Taking checkpoint @ instruction count ([0-9]+)", out + "\n" + err
            )
        ]
        result["observed_serialized_pcs"] = re.findall(
            r"Writing PC: (0x[0-9a-fA-F]+) at addr (0x[0-9a-fA-F]+)", out + "\n" + err
        )
        result["capture_pc_and_actual_roi_verified"] = False
        result["pass"] = True
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        result["error"] = str(error)
    result["elapsed_seconds"] = time.monotonic() - start
    for name in ("checkpoint.out", "checkpoint.err"):
        if (raw / name).is_file() and (raw / name).stat().st_size:
            result[name] = file_info(raw / name)
    write_json(raw / "run.json", result)
    return result


def capture(job, plan):
    try:
        return _capture(job, plan)
    except (ValueError, OSError) as error:
        result = {"pass": False, "error": str(error), "plan": job}
        raw = Path(job["output"])
        if raw.is_dir():
            write_json(raw / "run.json", result)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--smoke", type=Path, required=True)
    parser.add_argument(
        "--inputs", type=Path, required=True, help="bundle-checkpoint-inputs/v1 JSON"
    )
    parser.add_argument(
        "--nemu",
        type=Path,
        required=True,
        help="checkpoint-making NEMU, not difftest .so",
    )
    parser.add_argument(
        "--restorer",
        type=Path,
        required=True,
        help="embedded restorer prefix check only; never passed as -r",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--bench", action="append", help="authorized bench subset (repeatable)"
    )
    parser.add_argument("--workers", type=positive, default=1)
    parser.add_argument("--timeout", type=positive, default=3600)
    parser.add_argument(
        "--execute",
        action="store_true",
        help="only after NC layout approval and migration verification",
    )
    args = parser.parse_args()
    try:
        plan = build_plan(args)
        if not args.execute:
            print(json.dumps(plan, indent=2, sort_keys=True))
            return 0
        require(
            socket.gethostname().split(".")[0]
            in ("ps1", "ps2", "iv-yexl3v2u4gwh2ysmqmn1"),
            "execution allowed only on known ps1/ps2 hosts",
        )
        require(
            all(
                plan["resources"][key]
                for key in ("cpu_budget_ok", "memory_budget_ok", "disk_budget_ok")
            ),
            "insufficient current CPU/memory/disk headroom",
        )
        output = Path(plan["output"])
        output.mkdir(parents=True, exist_ok=False)
        write_json(output / "plan.json", plan)
        for name in ("smoke", "inputs", "generator"):
            info = plan["inputs"][name]
            shutil.copyfile(info["path"], output / (name + Path(info["path"]).suffix))
        with ThreadPoolExecutor(max_workers=plan["resources"]["workers"]) as pool:
            results = list(pool.map(lambda job: capture(job, plan), plan["jobs"]))
        passed = all(result["pass"] for result in results)
        write_json(
            output / "summary.json",
            {
                "schema": "bundle-fixed-checkpoint-summary/v1",
                "pass": passed,
                "results": results,
                "validation_limits": [
                    "NC/PMA/layout and migration acceptance are external prerequisites",
                    "No checkpoint restore, no-RVV register, exact capture PC/ROI or BBV equivalence gate executed",
                    "No zstd decompression/GCPT header validation; run is not publication-ready",
                ],
            },
        )
        return 0 if passed else 1
    except (ValueError, KeyError, TypeError, OSError, InvalidOperation) as error:
        parser.exit(2, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
