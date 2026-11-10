#!/usr/bin/env python3
"""Create a private GCPT initial-memory overlay; never modify a source checkpoint.

Default dry-run validates provenance and stream contents without emitting memory.
--execute produces a new zstd + .manifest.json. No CSR/DMA is emulated here.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile

BASE, LIMIT = 0x80000000, 0x200000000
SAVED, HEADER_END = 0xECDB0, 0x100000
NC_BASE, NC_SIZE = 0x278000000, 0x08000000
CONTAINER, TRACE, TRACE_SIZE = NC_BASE, 0x278200000, 0x04000000
CONTROL, CONTROL_SIZE = 0x27C200000, 4096
BLOCK = 1024 * 1024


def check(ok, message):
    if not ok:
        raise ValueError(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(BLOCK), b""):
            h.update(block)
    return h.hexdigest()


def info(path):
    check(path.is_file() and path.stat().st_size, f"missing/empty input: {path}")
    return {"path": str(path), "sha256": digest(path), "bytes": path.stat().st_size}


def read_json(path):
    def unique(items):
        result = {}
        for key, value in items:
            check(key not in result, f"duplicate JSON key: {path}: {key}")
            result[key] = value
        return result

    return json.loads(path.read_text(), object_pairs_hook=unique)


def raw_restorer(path, manifest):
    size_file = path.stat().st_size
    check(size_file >= 132, "restorer raw too short")
    with path.open("rb") as stream:
        first = stream.read(8)
        size = struct.unpack_from("<I", first, 4)[0]
        check(
            132 <= size <= SAVED and size % 4 == 0 and size <= size_file,
            "restorer header size outside safe low code range",
        )
        stream.seek(0)
        raw = stream.read(size)
    check(
        manifest.get("schema") == "bundle-restorer/v1"
        and manifest.get("raw", {}).get("sha256") == digest(path),
        "restorer manifest/raw hash mismatch",
    )
    check(
        struct.unpack_from("<I", raw)[0] & 0x7F == 0x6F,
        "restorer first instruction is not JAL",
    )
    matches = []
    for at in range(124, size - 3, 4):
        if struct.unpack_from("<I", raw, at)[0] != 0x30200073:
            continue
        expected = []
        for i in range(31):
            reg = 1 if i == 0 else (2 if i == 30 else i + 2)
            expected.append((reg * 8 << 20) | (2 << 15) | (3 << 12) | (reg << 7) | 3)
        if list(struct.unpack_from("<31I", raw, at - 124)) == expected:
            matches.append(at)
    check(
        len(matches) == 1,
        f"expected unique contiguous31ld+mret tail, found {len(matches)}",
    )
    # A raw GCPT can include its entire payload. Only declared prefix bytes are read/overlaid.
    return raw, {
        "declared_size": size,
        "raw_file_size": size_file,
        "tail_mret_offset": matches[0],
        "overlaid_prefix_sha256": hashlib.sha256(raw).hexdigest(),
    }


def provenance(args):
    proof = read_json(args.reservation_proof)
    p = proof.get("proof", {})
    check(
        proof.get("exit_code") == 0 and p.get("dtb_reserved_memory_proof") is True,
        "reservation build proof is not successful",
    )
    check(
        p.get("reserved_base") == hex(NC_BASE)
        and p.get("reserved_size") == NC_SIZE
        and p.get("trace_base") == hex(TRACE)
        and p.get("trace_size") == TRACE_SIZE,
        "reservation/Trace range does not match approved layout",
    )
    image_manifest = info(args.image_manifest)
    check(
        p.get("image_manifest_sha256") == image_manifest["sha256"],
        "reservation/image manifest hash mismatch",
    )
    im = read_json(args.image_manifest)
    image = p["artifacts"]["published_image"]
    check(
        image.get("sha256")
        == im.get("artifacts", {}).get("published_image", {}).get("sha256"),
        "reservation/image artifact mismatch",
    )
    check(
        info(Path(image["path"]))["sha256"] == image["sha256"],
        "reserved image hash mismatch",
    )
    source = info(args.source)
    run = read_json(args.checkpoint_run)
    check(run.get("pass") is True, "source checkpoint run is not successful")
    check(
        run.get("plan", {}).get("image_manifest", {}).get("sha256")
        == image_manifest["sha256"]
        and run.get("plan", {}).get("image", {}).get("sha256") == image["sha256"],
        "source checkpoint was not generated from the reserved image",
    )
    check(
        any(
            item.get("sha256") == source["sha256"]
            for item in run.get("checkpoint_files", [])
        ),
        "checkpoint source hash is absent from reserved-image checkpoint run",
    )
    check(im.get("isa_variant") == "no-rvv", "expected no-RVV image")
    boot = read_json(args.boot_proof)
    check(
        boot.get("exit_code") == 0
        and boot.get("guest_reserved_memory_nomap_proof") is True
        and boot.get("guest_validation", {}).get("nemu_good_trap") is True,
        "guest reserved no-map/GOOD TRAP proof missing",
    )
    check(
        boot.get("proof", {}).get("image_manifest_sha256") == image_manifest["sha256"],
        "guest reservation proof does not bind same image",
    )
    return source, image_manifest


def segments(raw, container):
    sentinel = b"H2P-NC-CONTROL-v1\0"
    control = sentinel + bytes(CONTROL_SIZE - len(sentinel))
    rows = [
        (0, len(raw), raw, "restorer-prefix"),
        (CONTAINER - BASE, len(container), container, "container"),
        (TRACE - BASE, TRACE_SIZE, None, "zero-trace"),
        (CONTROL - BASE, len(control), control, "control-sentinel"),
    ]
    previous = 0
    for start, size, _, name in rows:
        check(
            start >= previous and size > 0 and start + size <= LIMIT,
            f"overlap/outside DRAM: {name}",
        )
        check(
            start + size <= SAVED or start >= HEADER_END,
            f"overlay touches saved state: {name}",
        )
        previous = start + size
    return rows


def overlay_block(data, offset, rows):
    result = bytearray(data)
    for start, length, payload, _ in rows:
        lo, hi = max(offset, start), min(offset + len(result), start + length)
        if lo < hi:
            result[lo - offset : hi - offset] = (
                bytes(hi - lo) if payload is None else payload[lo - start : hi - start]
            )
    return bytes(result)


def stream_overlay(source_stream, sink, rows, logical_limit=LIMIT):
    """Bounded chunks; pad short resident dumps only through highest written segment."""
    source_bytes = output_bytes = 0
    saved_before, saved_after = hashlib.sha256(), hashlib.sha256()
    metadata = bytearray()
    output_digest = hashlib.sha256()
    while True:
        data = source_stream.read(BLOCK)
        if not data:
            break
        check(
            source_bytes + len(data) <= logical_limit,
            "source dump exceeds logical DRAM limit",
        )
        end = source_bytes + len(data)
        lo, hi = max(source_bytes, SAVED), min(end, HEADER_END)
        patched = overlay_block(data, source_bytes, rows)
        if lo < hi:
            original_part = data[lo - source_bytes : hi - source_bytes]
            patched_part = patched[lo - source_bytes : hi - source_bytes]
            check(original_part == patched_part, "saved state changed")
            saved_before.update(original_part)
            saved_after.update(patched_part)
            metadata.extend(original_part)
        if sink:
            sink.write(patched)
        output_digest.update(patched)
        source_bytes = end
    check(source_bytes >= HEADER_END, "source missing complete GCPT saved-state header")
    check(
        struct.unpack_from("<Q", metadata, 0)[0] == 0xBEEF,
        "source GCPT checkpoint flag mismatch",
    )
    pc = struct.unpack_from("<Q", metadata, 8)[0]
    mode = struct.unpack_from("<Q", metadata, 16)[0]
    check(pc and not pc & 1 and mode in (0, 1), "source saved PC/mode unsupported")
    output_bytes = source_bytes
    target = max(source_bytes, max(start + length for start, length, _, _ in rows))
    check(target <= logical_limit, "overlay outside logical DRAM")
    while output_bytes < target:
        data = bytes(min(BLOCK, target - output_bytes))
        patched = overlay_block(data, output_bytes, rows)
        if sink:
            sink.write(patched)
        output_digest.update(patched)
        output_bytes += len(data)
    return {
        "source_uncompressed_bytes": source_bytes,
        "output_uncompressed_bytes": output_bytes,
        "zero_extended_bytes": output_bytes - source_bytes,
        "preserved_range_offsets": [SAVED, HEADER_END],
        "saved_before_sha256": saved_before.hexdigest(),
        "saved_after_sha256": saved_after.hexdigest(),
        "saved_bytes_equal": True,
        "saved_pc": hex(pc),
        "saved_mode": mode,
        "output_uncompressed_sha256": output_digest.hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in (
        "source",
        "restorer",
        "restorer-manifest",
        "container",
        "reservation-proof",
        "boot-proof",
        "image-manifest",
        "checkpoint-run",
        "output",
    ):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument(
        "--mode", choices=("discovery", "detail", "replay"), required=True
    )
    parser.add_argument("--zstd", default="zstd")
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    temporary = None
    try:
        for name in (
            "source",
            "restorer",
            "restorer_manifest",
            "container",
            "reservation_proof",
            "boot_proof",
            "image_manifest",
            "checkpoint_run",
        ):
            setattr(args, name, getattr(args, name).resolve())
        output = args.output.absolute()
        manifest_path = output.with_name(output.name + ".manifest.json")
        check(
            not output.exists() and not manifest_path.exists(),
            "destination exists; no overwrite/resume",
        )
        check(output.suffix == ".zstd", "destination must be .zstd")
        check(output.resolve() != args.source, "source and destination must differ")
        source, im = provenance(args)
        raw, restorer = raw_restorer(args.restorer, read_json(args.restorer_manifest))
        check(
            args.container.stat().st_size == 0x108000,
            "container must be exactly0x108000 bytes",
        )
        container = args.container.read_bytes()
        check(
            container[:8] == b"H2PBND01"
            and container[8] == {"discovery": 0, "detail": 1, "replay": 2}[args.mode],
            "Bundle magic/kind mismatch",
        )
        rows = segments(raw, container)
        check(
            socket.gethostname().split(".")[0]
            in ("ps1", "ps2", "iv-yexl3v2u4gwh2ysmqmn1"),
            "stream validation/execution allowed only on known ps1/ps2 hosts",
        )
        record = {
            "schema": "bundle-checkpoint-overlay/v1",
            "mode": args.mode,
            "source_scope": "checkpoint generated from hash-bound reserved/no-map image; PMA is external",
            "source": source,
            "image_manifest": im,
            "restorer": info(args.restorer),
            "restorer_validation": restorer,
            "container": info(args.container),
            "input_metadata": {
                name: info(getattr(args, name))
                for name in (
                    "reservation_proof",
                    "boot_proof",
                    "checkpoint_run",
                    "restorer_manifest",
                )
            },
            "logical_dram_base": hex(BASE),
            "logical_dram_bytes": LIMIT,
            "segments": [
                {
                    "physical_address": hex(BASE + start),
                    "bytes": length,
                    "kind": name,
                    "sha256": hashlib.sha256(payload).hexdigest()
                    if payload is not None
                    else None,
                }
                for start, length, payload, name in rows
            ],
            "trace_zero_initialized": True,
            "phase_gate_added": False,
            "dma_completion_emulated": False,
        }
        if args.execute:
            output.parent.mkdir(parents=True, exist_ok=True)
            check(
                shutil.disk_usage(output.parent).free >= 20 * 1024**3,
                "need20GiB free before output generation",
            )
            temporary = Path(
                tempfile.mkdtemp(prefix=".gcpt-overlay-", dir=output.parent)
            )
        with tempfile.TemporaryFile() as errors:
            decoder = subprocess.Popen(
                [args.zstd, "-dc", "--", str(args.source)],
                stdout=subprocess.PIPE,
                stderr=errors,
            )
            encoder = None
            try:
                if args.execute:
                    compressed = (temporary / "memory.zstd").open("wb")
                    encoder = subprocess.Popen(
                        [args.zstd, "-T1", "-3", "-c"],
                        stdin=subprocess.PIPE,
                        stdout=compressed,
                        stderr=errors,
                    )
                record["preservation"] = stream_overlay(
                    decoder.stdout, encoder.stdin if encoder else None, rows
                )
                decoder.stdout.close()
                check(decoder.wait() == 0, "zstd decoder failed")
                if encoder:
                    encoder.stdin.close()
                    check(encoder.wait() == 0, "zstd encoder failed")
                    compressed.close()
            finally:
                for process in (decoder, encoder):
                    if process and process.poll() is None:
                        process.kill()
                        process.wait()
        # Fail closed if any bound input changed while the stream was processed.
        for expected in [
            record["source"],
            record["image_manifest"],
            record["restorer"],
            record["container"],
            *record["input_metadata"].values(),
        ]:
            check(
                info(Path(expected["path"])) == expected,
                f"input changed during overlay: {expected['path']}",
            )
        record["execute"] = args.execute
        if args.execute:
            record["output"] = info(temporary / "memory.zstd")
            record["output"]["path"] = str(output)
            # Preserve supplied metadata bytes alongside the output, without editing original files.
            for name in (
                "reservation_proof",
                "boot_proof",
                "image_manifest",
                "checkpoint_run",
                "restorer_manifest",
            ):
                record.setdefault("original_metadata_base64", {})[
                    name
                ] = base64.b64encode(getattr(args, name).read_bytes()).decode("ascii")
            (temporary / "manifest.json").write_text(
                json.dumps(record, indent=2, sort_keys=True) + "\n"
            )
            # Hard links fail atomically if another caller has claimed either destination.
            os.link(temporary / "manifest.json", manifest_path)
            try:
                os.link(temporary / "memory.zstd", output)
            except OSError:
                manifest_path.unlink()
                raise
        print(json.dumps(record, indent=2, sort_keys=True))
        return 0
    except (ValueError, KeyError, TypeError, OSError, struct.error) as error:
        parser.exit(2, f"error: {error}\n")
    finally:
        if temporary:
            shutil.rmtree(temporary)


if __name__ == "__main__":
    raise SystemExit(main())
