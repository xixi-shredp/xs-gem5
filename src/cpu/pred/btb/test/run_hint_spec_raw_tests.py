#!/usr/bin/env python3
"""Run current-SPEC production helpers in the final gem5.opt, only on ps1/ps2.

Invoke remotely after the final build. The script records hostname, resource
headroom, binary/source hashes, commands and output. It never compiles or uses
gtest. All gem5 subprocesses are sequential.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time


def sha(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--remote-alias", choices=("ps1", "ps2"), required=True,
                        help="Audited SSH destination; invoke only on that remote host")
    args = parser.parse_args()
    args.gem5 = args.gem5.resolve()
    args.outdir = args.outdir.resolve()
    here = Path(__file__).resolve().parent
    root = here.parents[4]
    project = root.parent
    config = here / "hint_indirect_test_config.py"
    tool = root / "util/h2p_hint/spec_trace.py"
    args.outdir.mkdir(parents=True, exist_ok=False)
    memory = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        name, value = line.split(":", 1)
        memory[name] = int(value.split()[0]) * 1024
    cpus = len(os.sched_getaffinity(0))
    headroom = {"affinity_cpus": cpus, "loadavg": os.getloadavg(),
                "mem_available_bytes": memory["MemAvailable"]}
    if memory["MemAvailable"] < 512 * 1024 * 1024 or os.getloadavg()[0] >= cpus:
        raise SystemExit(f"Insufficient headroom for one sequential helper: {headroom}")
    report = {"schema": "hint-spec-final-binary-validation", "version": 1,
              "hostname": socket.gethostname(), "ssh_alias": args.remote_alias,
              "cwd": str(Path.cwd()), "resources_before": headroom,
              "gem5": str(args.gem5), "gem5_sha256": sha(args.gem5),
              "source_sha256": {str(path.relative_to(project)): sha(path) for path in
                                 (here / "hint_indirect_test.cc", config, Path(__file__), tool)},
              "cases": [], "passed": False}

    def run(name, command, expected=True, reason=None):
        started = time.monotonic()
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        stdout, stderr = args.outdir / f"{name}.stdout", args.outdir / f"{name}.stderr"
        stdout.write_text(result.stdout)
        stderr.write_text(result.stderr)
        passed = result.returncode == 0 if expected else result.returncode != 0
        if expected and command[0] == args.gem5:
            passed = passed and "Hint self-test passed" in result.stdout
        if not expected:
            passed = passed and any(word in (result.stdout + result.stderr).lower()
                                    for word in ("fatal", "panic", "error"))
            passed = passed and reason is not None and reason.lower() in (result.stdout + result.stderr).lower()
        report["cases"].append({"name": name, "command": list(map(str, command)),
                                "returncode": result.returncode, "expected_success": expected,
                                "expected_diagnostic": reason,
                                "passed": passed, "seconds": round(time.monotonic() - started, 3),
                                "stdout": str(stdout), "stderr": str(stderr)})
        (args.outdir / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
        if not passed:
            raise RuntimeError(f"{name} failed: see {stdout} and {stderr}")

    def gem5_case(name, fixture=None, collect=False, expected=True):
        command = [args.gem5, "--outdir", args.outdir / f"gem5-{name}", config]
        if fixture:
            command += ["--candidates" if collect else "--metadata", fixture]
        reason = None
        if not expected:
            reasons = {"truncated": "incorrect raw Hint image length",
                       "header-reserved": "raw Hint header reserved bytes must be zero",
                       "legacy-magic": "expected value",
                       "cam-reserved": "raw CAM reserved bits must be zero",
                       "invalid-cam-payload": "invalid raw CAM row must be zero",
                       "duplicate-cam": "duplicate raw CAM startPC",
                       "context-reserved": "raw Context reserved bits must be zero",
                       "invalid-context-payload": "invalid raw Context row must be zero",
                       "legacy-slot-valid": "raw Slot reserved bits must be zero",
                       "json-old-version": "finite Hint requires fork metadata/hash/storage version 7 and encoding"}
            reason = next((value for suffix, value in reasons.items() if name.endswith(suffix)), None)
        run(name, command, expected, reason)

    gem5_case("production-helper")

    # Independent bit-by-bit index/tag oracle, no production builder import.
    def component(word):
        index = tag = 0
        for bit in range(word.bit_length()):
            if word & (1 << bit):
                index ^= 1 << (bit % 11)
                tag ^= 1 << (bit % 17)
                if bit % 16 + 2 < 17:
                    tag ^= 1 << (bit % 16 + 2)
        return (tag << 11) | index

    load = bytearray(b"H2PHINTL" + bytes(16784 - 8))
    for slot, pc in ((7, 0x1000), (2, 0x1080)):
        struct.pack_into("<Q", load, 16 + slot * 8, (pc >> 1) | (1 << 51))
        key = component(pc >> 1)
        struct.pack_into("<I", load, 0x90 + (key & 2047) * 8,
                         (key >> 11) | (3 << 22) | (1 << 24))
    cam = bytearray(b"H2PHINTP" + load[8:144])
    fixtures = args.outdir / "fixtures"
    fixtures.mkdir()

    def binary_case(name, image, collect=False, expected=False):
        path = fixtures / f"{name}.bin"
        path.write_bytes(image)
        gem5_case(name, path, collect, expected)

    binary_case("load-valid-sparse-zero-target", load, expected=True)
    binary_case("cam-valid-sparse", cam, collect=True, expected=True)
    for label, original, collect in (("load", load, False), ("cam", cam, True)):
        binary_case(label + "-truncated", original[:-1], collect)
        corrupt = bytearray(original)
        corrupt[8] = 1
        binary_case(label + "-header-reserved", corrupt, collect)
        corrupt = bytearray(original)
        corrupt[0:8] = b"H2PHINT3"
        binary_case(label + "-legacy-magic", corrupt, collect)
        corrupt = bytearray(original)
        struct.pack_into("<Q", corrupt, 16, 1 << 52)
        binary_case(label + "-cam-reserved", corrupt, collect)
        corrupt = bytearray(original)
        struct.pack_into("<Q", corrupt, 16, 1)
        binary_case(label + "-invalid-cam-payload", corrupt, collect)
        corrupt = bytearray(original)
        corrupt[16 + 2 * 8:24 + 2 * 8] = corrupt[16 + 7 * 8:24 + 7 * 8]
        binary_case(label + "-duplicate-cam", corrupt, collect)
    for name, offset, width, value in (("context-reserved", 0x90, 4, 1 << 25),
                                        ("invalid-context-payload", 0x90, 4, 1),
                                        ("legacy-slot-valid", 0x4090, 8, 1 << 47)):
        corrupt = bytearray(load)
        corrupt[offset:offset + width] = value.to_bytes(width, "little")
        binary_case(name, corrupt)
    legacy_metadata = json.loads((args.outdir / "gem5-production-helper" /
                                 "spec-v7-timing-metadata.json").read_text())
    legacy_metadata["version"] = 3
    legacy_path = fixtures / "json-old-version.json"
    legacy_path.write_text(json.dumps(legacy_metadata))
    gem5_case("json-old-version", legacy_path, expected=False)

    # Consume bytes actually exported by the production helper, using the
    # exact same standalone file-only parser that RTL/hardware will invoke.
    output = args.outdir / "gem5-production-helper"
    raw = output / "spec-raw-detail/hint-detail.bin"
    cam_path = output / "spec-raw-detail-cam.bin"
    mapping = {"schema": "hint-spec-cam", "version": 1, "hash_version": 7,
               "mask_order": ["ITH", "Call", "Ret", "LHR"],
               "image_sha256": sha(cam_path), "entries": []}
    for i in range(16):
        pcs = {7: 0x1000, 2: 0x1080, 12: 0x1ff0, 15: (1 << 64) - 2}
        pc = pcs.get(i, 0)
        mapping["entries"].append({"id": i, "valid": i in pcs,
                                   "start_pc": hex(pc), "mask": 0})
    mapping_path = fixtures / "detail-cam.json"
    mapping_path.write_text(json.dumps(mapping))
    sidecar = {"schema": "hint-spec-trace", "version": 1, "hash_version": 7,
               "mask_order": mapping["mask_order"], "kind": "detail", "record_bytes": 22,
               "termination": "final", "capacity_records": raw.stat().st_size // 22,
               "data_sha256": sha(raw), "cam_sha256": sha(cam_path)}
    sidecar_path = fixtures / "detail.json"
    sidecar_path.write_text(json.dumps(sidecar))
    run("production-detail-offline-validate", ["python3", tool, "validate", "--trace", raw,
                                               "--meta", sidecar_path, "--cam", mapping_path])
    run("production-detail-offline-build", ["python3", tool, "build", "--trace", raw,
                                            "--meta", sidecar_path, "--cam", mapping_path,
                                            "--image", fixtures / "rebuilt-load.bin",
                                            "--output", fixtures / "rebuilt-load.json"])
    report["passed"] = all(row["passed"] for row in report["cases"])
    report["fixture_sha256"] = {str(path.relative_to(args.outdir)): sha(path)
                                 for path in fixtures.iterdir() if path.is_file()}
    report["resources_after"] = {"loadavg": os.getloadavg()}
    (args.outdir / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "hostname": report["hostname"],
                      "cases": len(report["cases"]), "report": str(args.outdir / "validation.json")}))


if __name__ == "__main__":
    main()
