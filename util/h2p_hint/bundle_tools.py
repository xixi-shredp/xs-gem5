#!/usr/bin/env python3
"""Bundle v1 host tools. Frozen CAM58 v10 tools are read-only dependencies.

The production stream contains no metadata id. Observer identity is exclusively
an independent audit input. Iterators keep raw decoding bounded in memory.
"""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

try:
    from util.h2p_hint import spec_trace as legacy
except ModuleNotFoundError:
    import spec_trace as legacy

SIZE = 0x108000
MASK47 = (1 << 47) - 1
FIELDS = {
    "discovery": [("start_pc", 47), ("position", 5), ("is_call", 1), ("final", 1)],
    "detail": [
        ("pc_id", 4),
        ("target", 47),
        ("ITH", 28),
        ("LHR", 28),
        ("Call", 28),
        ("Ret", 28),
        ("baseline_miss", 1),
        ("query_valid", 1),
        ("final", 1),
    ],
}


def need(ok, message):
    if not ok:
        raise ValueError(message)


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def write_json(path, value):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def json_rows(path):
    with Path(path).open() as stream:
        for line in stream:
            if line.strip():
                yield json.loads(line)


def address(word):
    value = word << 1
    return value | (0xFFFF << 48) if value & (1 << 47) else value


def address_word(pc):
    pc = number(pc)
    need(
        0 <= pc < 1 << 64 and pc % 2 == 0 and address((pc >> 1) & MASK47) == pc,
        "unaligned or noncanonical address",
    )
    return (pc >> 1) & MASK47


def route_hash(pc, target):
    value = (address_word(pc) << 47) | address_word(target)
    result = 0
    for offset in range(0, 94, 24):
        result ^= (value >> offset) & 0xFFFFFF
    return result


def inspect_image(data, kind):
    length = 144 if kind == 1 else 16784
    magic = b"H2PCAM58" if kind == 1 else b"H2PLD058"
    need(
        len(data) == length and data[:8] == magic and data[8:16] == bytes(8),
        "invalid inner CAM58 image header/length",
    )
    seen = set()
    for index in range(16):
        word = struct.unpack_from("<Q", data, 16 + index * 8)[0]
        need(word >> 58 == 0, "CAM reserved bits")
        valid = bool(word & (1 << 51))
        need(valid or word == 0, "invalid CAM row is nonzero")
        if not valid:
            continue
        q = address(word & MASK47)
        position = (word >> 52) & 31
        cfi = (q & ~31) + 2 * position
        need(
            q not in seen
            and q <= (1 << 64) - 65
            and q <= cfi < min((q + 64) & ~31, (q & ~4095) + 4096),
            "CAM identity/window invalid",
        )
        seen.add(q)
    if kind == 2:
        for pos in range(4096):
            word = struct.unpack_from("<I", data, 0x90 + 4 * pos)[0]
            need(
                word >> 25 == 0 and (word & (1 << 24) or word == 0),
                "Context reserved/invalid bits",
            )
            if pos % 2 and word & (1 << 24):
                other = struct.unpack_from("<I", data, 0x90 + 4 * (pos - 1))[0]
                need(
                    not (other & (1 << 24)) or other & 0x1FFFF != word & 0x1FFFF,
                    "duplicate Context tag",
                )
        for pos in range(32):
            need(
                struct.unpack_from("<Q", data, 0x4090 + 8 * pos)[0] >> 47 == 0,
                "Slot reserved bits",
            )


def validate_container(data):
    need(
        len(data) == SIZE and data[:8] == b"H2PBND01",
        "invalid Bundle container size/magic",
    )
    kind, initial = data[8], data[9]
    need(
        kind in (0, 1, 2) and not (initial & 0xC0) and data[10:12] == bytes(2),
        "Bundle header reserved/kind",
    )
    initial_id = (initial & 31) if initial & 32 else None
    need(initial & 32 or initial & 31 == 0, "invalid initial id without valid")
    mask = struct.unpack_from("<I", data, 12)[0]
    need(
        initial_id is None or mask & (1 << initial_id),
        "initial references absent metadata",
    )
    boundaries, seen = [], set()
    for pos in range(32):
        word = struct.unpack_from("<Q", data, 16 + 8 * pos)[0]
        need(
            word >> 48 == 0 and (word & 1 or word == 0),
            "Boundary reserved/invalid bits",
        )
        if word & 1:
            pc = address(word >> 1)
            need(pc not in seen, "duplicate Boundary PC")
            boundaries.append(pc)
            seen.add(pc)
    routes = []
    for index in range(256):
        tags = {}
        for way in range(2):
            word = struct.unpack_from("<I", data, 0x110 + 4 * (2 * index + way))[0]
            need(
                word >> 22 == 0 and (word & 1 or word == 0),
                "Bundle Table reserved/invalid bits",
            )
            if not word & 1:
                continue
            tag, mid = (word >> 1) & 0xFFFF, (word >> 17) & 31
            need(mask & (1 << mid), "route references absent metadata")
            need(tag not in tags or tags[tag] == mid, "conflicting same-tag routes")
            tags[tag] = mid
            routes.append({"index": index, "way": way, "tag": tag, "metadata_id": mid})
    need(not any(data[0x910:0x8000]), "nonzero container gap")
    for mid in range(32):
        slot = data[0x8000 + mid * 0x8000 : 0x10000 + mid * 0x8000]
        length = (
            0 if kind == 0 or not (mask & (1 << mid)) else (144 if kind == 1 else 16784)
        )
        if length:
            inspect_image(slot[:length], kind)
        need(not any(slot[length:]), "nonzero absent-slot or slot padding")
    return {
        "schema": "hint-bundle-container/v1",
        "kind": kind,
        "initial_metadata_id": initial_id,
        "metadata_valid_mask": mask,
        "boundaries": [hex(p) for p in boundaries],
        "routes": routes,
    }


def make_container(kind, boundaries, routes, slots, initial=None, valid_mask=None):
    need(kind in (0, 1, 2), "invalid container kind")
    need(
        len(boundaries) <= 32 and len(set(map(number, boundaries))) == len(boundaries),
        "Boundary count/duplicates",
    )
    mask = sum(1 << number(i) for i in slots) if valid_mask is None else valid_mask
    need(0 <= mask <= 0xFFFFFFFF, "invalid metadata mask")
    out = bytearray(SIZE)
    out[:8] = b"H2PBND01"
    out[8] = kind
    if initial is not None:
        need(0 <= initial < 32, "invalid initial metadata")
        out[9] = initial | 32
    struct.pack_into("<I", out, 12, mask)
    for i, pc in enumerate(boundaries):
        struct.pack_into("<Q", out, 16 + 8 * i, (address_word(pc) << 1) | 1)
    occupied = set()
    for row in routes:
        index, way, tag, mid = (
            number(row[k]) for k in ("index", "way", "tag", "metadata_id")
        )
        need(
            0 <= index < 256 and 0 <= way < 2 and 0 <= tag < 65536 and 0 <= mid < 32,
            "route field out of range",
        )
        need((index, way) not in occupied, "duplicate route position")
        occupied.add((index, way))
        struct.pack_into(
            "<I", out, 0x110 + 4 * (2 * index + way), 1 | (tag << 1) | (mid << 17)
        )
    for mid, path in slots.items():
        mid = number(mid)
        need(0 <= mid < 32, "slot id out of range")
        if kind == 0:
            continue
        image = Path(path).read_bytes() if isinstance(path, (str, Path)) else path
        inspect_image(image, kind)
        start = 0x8000 + mid * 0x8000
        out[start : start + len(image)] = image
    validate_container(out)
    return bytes(out)


def route_lookup(container, pc, target):
    if number(pc) not in {number(p) for p in container["boundaries"]}:
        return False, None
    h = route_hash(pc, target)
    rows = sorted(container["routes"], key=lambda r: r["way"])
    for row in rows:
        if row["index"] == h & 255 and row["tag"] == h >> 8:
            return True, row["metadata_id"]
    return True, None


def decode_trace(path, kind, p_error=0, capacity_bytes=None):
    """Streaming decoder; final records are validated and not yielded.

    Fully consume the iterator before using results: late corruption invalidates
    the whole round. P_Error 001 permits ordinary losses; 010 is capacity stop.
    """
    need(kind in FIELDS and p_error in (0, 1, 2), "invalid/failed profiling round")
    total = Path(path).stat().st_size
    need(total % 22 == 0, "partial Bundle record")
    if capacity_bytes is not None:
        need(
            0 <= capacity_bytes and total <= capacity_bytes // 22 * 22,
            "trace exceeds capacity",
        )
    final = False
    with Path(path).open("rb") as stream:
        for index in range(total // 22):
            value = int.from_bytes(stream.read(22), "little")
            need(not final, "record after final")
            if value & 1:
                need(value >> 95 == 0, "Boundary reserved bits")
                yield {
                    "record_index": index,
                    "is_boundary": True,
                    "pc": address((value >> 1) & MASK47),
                    "target": address((value >> 48) & MASK47),
                }
                continue
            row = {"record_index": index, "is_boundary": False}
            pos = 1
            for name, width in FIELDS[kind]:
                row[name] = (value >> pos) & ((1 << width) - 1)
                pos += width
            need(value >> pos == 0, "Hint reserved bits")
            if row["final"]:
                need(value == 1 << (pos - 1), "final has nonzero payload")
                final = True
                continue
            if kind == "discovery":
                q = address(row["start_pc"])
                c = (q & ~31) + row["position"] * 2
                need(
                    q <= (1 << 64) - 65
                    and q <= c < min((q + 64) & ~31, (q & ~4095) + 4096),
                    "Discovery CFI window invalid",
                )
                row["start_pc"] = q
                row["is_call"] = bool(row["is_call"])
            else:
                row["target"] = address(row["target"])
            yield row
    need(
        final
        or (
            p_error == 2
            and capacity_bytes is not None
            and total == capacity_bytes // 22 * 22
        ),
        "missing final without complete capacity termination",
    )


def attributed_rows(rows, container):
    current = container["initial_metadata_id"]
    for row in rows:
        if row["is_boundary"]:
            hit, desired = route_lookup(container, row["pc"], row["target"])
            need(hit, "Boundary trace PC absent from frozen Boundary Table")
            current = desired
            yield current, row
        else:
            yield current, row


def select_cams(rows, container):
    # Keep only per-(Q,position,call) counters rather than all dynamic records.
    groups = defaultdict(Counter)
    for mid, row in attributed_rows(rows, container):
        if mid is not None and not row["is_boundary"]:
            groups[mid][row["start_pc"], row["position"], row["is_call"]] += 1
    result = {
        mid: [] for mid in range(32) if container["metadata_valid_mask"] & (1 << mid)
    }
    for mid, counts in groups.items():
        first = {}
        for q, pos, call in counts:
            first[q] = min(first.get(q, 32), pos)
        candidates = []
        for q, pos in first.items():
            identities = [call for qq, pp, call in counts if qq == q and pp == pos]
            need(len(identities) == 1, "conflicting isCall for fixed CFI")
            call = identities[0]
            candidates.append((counts[q, pos, call], q, pos, call))
        selected = sorted(candidates, key=lambda x: (-x[0], x[1]))[:16]
        result[mid] = [
            {
                "id": i,
                "start_pc": hex(q),
                "position": pos,
                "is_call": call,
                "misses": count,
            }
            for i, (count, q, pos, call) in enumerate(selected)
        ]
    return result


def cam_image(entries, masks=None):
    out = bytearray(b"H2PCAM58" + bytes(136))
    for entry in entries:
        i = entry["id"]
        q = number(entry["start_pc"])
        mask = (masks or {}).get(i, 0)
        need(0 <= i < 16 and 0 <= mask < 16, "CAM id/mask invalid")
        word = (
            address_word(q)
            | (mask << 47)
            | (1 << 51)
            | (entry["position"] << 52)
            | (int(entry["is_call"]) << 57)
        )
        struct.pack_into("<Q", out, 16 + 8 * i, word)
    inspect_image(out, 1)
    return bytes(out)


def build_tables(rows, container, cams, output):
    """New-format->legacy in-memory row conversion, never legacy raw decoding."""
    grouped = defaultdict(list)
    discarded = Counter()
    for mid, row in attributed_rows(rows, container):
        if row["is_boundary"] or mid is None:
            continue
        mapping = {e["id"]: e for e in cams.get(str(mid), cams.get(mid, []))}
        if row["pc_id"] not in mapping:
            discarded[mid] += 1
            continue
        # Legacy hash/table builder consumes semantic fields only. DELTA=0 is
        # an explicit intermediate placeholder, not inferred production data.
        grouped[mid].append(
            {
                **row,
                "start_pc": number(mapping[row["pc_id"]]["start_pc"]),
                "window_delta": 0,
            }
        )
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    reports = {}
    for mid, entries in cams.items():
        mid = int(mid)
        sample = grouped[mid]
        legacy_cam = legacy.identity("hint-spec-cam")
        legacy_cam["entries"] = [
            {
                "id": i,
                "valid": i < len(entries),
                "mask": 0,
                "start_pc": entries[i]["start_pc"] if i < len(entries) else "0x0",
            }
            for i in range(16)
        ]
        best, ranking = legacy.rank_masks(sample, legacy_cam)
        variants = {
            "static-best": best,
            "uniform9": {e["id"]: 9 for e in entries},
            "uniform15": {e["id"]: 15 for e in entries},
        }
        variants_report = []
        seen = set()
        for label, masks in variants.items():
            old = legacy.build_image(sample, legacy_cam, masks)
            image = bytearray(old)
            image[:144] = cam_image(entries, masks)
            image[:8] = b"H2PLD058"
            inspect_image(image, 2)
            digest = hashlib.sha256(image).hexdigest()
            if digest in seen:
                continue
            seen.add(digest)
            path = output / f"{mid}-{label}.bin"
            path.write_bytes(image)
            variants_report.append(
                {
                    "label": label,
                    "image": str(path.resolve()),
                    "sha256": digest,
                    "estimate": legacy.score(sample, masks),
                }
            )
        reports[mid] = {
            "records": len(sample),
            "unknown_pc_id_dropped": discarded[mid],
            "candidates": variants_report,
            "ranking": ranking,
            "conversion": "Bundle semantic rows; placeholder DELTA=0 only in host builder API",
        }
    return reports


def disassembly_coverage(elf, objdump="riscv64-linux-gnu-objdump"):
    result = subprocess.run(
        [objdump, "-d", str(elf)], capture_output=True, text=True, check=True
    )
    coverage = {}
    for line in result.stdout.splitlines():
        match = re.match(
            r"^\s*([0-9a-fA-F]+):\s+([0-9a-fA-F]{4}(?:[0-9a-fA-F]{4})?)\s+\S", line
        )
        if match:
            pc = int(match[1], 16)
            size = len(match[2]) // 2
            address_word(pc)
            need(size in (2, 4), "unsupported disassembly width")
            coverage[pc] = size
    need(coverage, "empty/unrecognized ELF disassembly")
    return coverage


def plan_routes(events, coverage, initial_pc=None):
    # Greedy static address-span regions; holes count toward 800KiB.
    regions = []
    pc_to_id = {}
    for pc in sorted(coverage):
        size = coverage[pc]
        if not regions or pc + size - regions[-1]["start"] > 819200:
            need(
                len(regions) < 32,
                "ELF footprint requires more than 32 address-span Bundles",
            )
            regions.append(
                {
                    "id": len(regions),
                    "start": pc,
                    "end": pc + size,
                    "instruction_count": 0,
                }
            )
        region = regions[-1]
        region["end"] = max(region["end"], pc + size)
        region["instruction_count"] += 1
        pc_to_id[pc] = region["id"]
    boundary_counts = Counter()
    pairs = Counter()
    skipped = Counter()
    previous = 0
    for event in events:
        seq = number(event["seq"])
        need(seq > previous, "retired control events are not strictly ordered")
        previous = seq
        for flag in ("call", "return", "indirect", "taken"):
            need(type(event[flag]) is bool, "missing event Boolean " + flag)
        pc = number(event["pc"])
        size = number(event["size"])
        target = number(event["target"])
        address_word(pc)
        address_word(target)
        need(size in (2, 4), "invalid original instruction width")
        if not (event["call"] or event["return"]) or not event["taken"]:
            continue
        if pc not in coverage or target not in coverage:
            skipped["outside_verified_elf"] += 1
            continue
        need(
            coverage[pc] == size,
            "retirement length disagrees with independent ELF disassembly",
        )
        mid = pc_to_id[target]
        boundary_counts[pc] += 1
        pairs[pc, target, mid] += 1
    boundaries = [
        p for p, _ in sorted(boundary_counts.items(), key=lambda x: (-x[1], x[0]))[:32]
    ]
    hashed = defaultdict(Counter)
    source = defaultdict(list)
    for (pc, target, mid), count in pairs.items():
        if pc not in boundaries:
            skipped["boundary_capacity_events"] += count
            continue
        h = route_hash(pc, target)
        hashed[h][mid] += count
        source[h].append(
            {"pc": hex(pc), "target": hex(target), "metadata_id": mid, "events": count}
        )
    sets = defaultdict(list)
    aliases = []
    for h, counts in hashed.items():
        mid = min(counts, key=lambda m: (-counts[m], m))
        sets[h & 255].append((sum(counts.values()), h >> 8, mid))
        if len(source[h]) > 1:
            aliases.append(
                {
                    "hash": h,
                    "selected_metadata_id": mid,
                    "conflicting_id_events": sum(
                        c for m, c in counts.items() if m != mid
                    ),
                    "sources": source[h],
                }
            )
    routes = []
    evictions = []
    for index, entries in sorted(sets.items()):
        for way, (count, tag, mid) in enumerate(
            sorted(entries, key=lambda x: (-x[0], x[1]))
        ):
            row = {"index": index, "tag": tag, "metadata_id": mid, "events": count}
            if way < 2:
                routes.append({**row, "way": way})
            else:
                evictions.append(row)
    need(
        initial_pc is None or number(initial_pc) in pc_to_id,
        "initial PC outside verified ELF coverage",
    )
    return {
        "schema": "hint-bundle-route-plan/v1",
        "review_required": True,
        "boundary_pcs": [hex(p) for p in boundaries],
        "routes": routes,
        "regions": regions,
        "initial_metadata_id": None
        if initial_pc is None
        else pc_to_id[number(initial_pc)],
        "valid_mask": sum(1 << r["id"] for r in regions),
        "hash_aliases": aliases,
        "two_way_evictions": evictions,
        "skipped_events": dict(skipped),
        "policy": "greedy static ELF instruction address spans; most frequent verified Call/Return boundaries",
    }


def attribution(rows, container, observers):
    indexed = {}
    for item in observers:
        index = number(item["record_index"])
        need(
            index >= 0 and index not in indexed,
            "duplicate/negative observer record index",
        )
        indexed[index] = item
    counts = Counter()
    by_bundle = defaultdict(Counter)
    by_branch = defaultdict(Counter)
    for mid, row in attributed_rows(rows, container):
        if row["is_boundary"]:
            continue
        index = row["record_index"]
        need(index in indexed, "missing independent observer record")
        actual = indexed.pop(index)
        truth = actual["metadata_id"]
        truth = None if truth is None else number(truth)
        need(truth is None or 0 <= truth < 32, "observer metadata id out of range")
        counts["samples"] += 1
        counts["misattributed"] += mid != truth
        key = f"{truth}->{mid}"
        by_bundle[key]["samples"] += 1
        by_bundle[key]["misattributed"] += mid != truth
        branch = actual.get("pc", "unknown")
        by_branch[branch]["samples"] += 1
        by_branch[branch]["misattributed"] += mid != truth
    need(not indexed, "observer includes non-Hint or nonexistent raw records")
    return {
        "schema": "hint-bundle-attribution/v1",
        "counts": dict(counts),
        "by_bundle": {k: dict(v) for k, v in by_bundle.items()},
        "by_branch": {k: dict(v) for k, v in by_branch.items()},
        "scope": "observer-only classification; actual IPC and independent no-mixing table runs remain required",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("validate-container")
    p.add_argument("container")
    p.add_argument("--output", required=True)
    p = sub.add_parser("build-container")
    p.add_argument("plan")
    p.add_argument("--kind", type=int, required=True)
    p.add_argument("--slots", help="JSON id->inner image path")
    p.add_argument("--output", required=True)
    p = sub.add_parser("route-plan")
    p.add_argument("--events", required=True)
    p.add_argument("--elf", required=True)
    p.add_argument("--objdump", default="riscv64-linux-gnu-objdump")
    p.add_argument("--initial-pc")
    p.add_argument("--output", required=True)
    for name in ("decode-trace", "build-cams", "build-tables", "attribution"):
        p = sub.add_parser(name)
        p.add_argument("--trace", required=True)
        p.add_argument("--kind", choices=FIELDS, required=True)
        p.add_argument("--p-error", type=int, default=0)
        p.add_argument("--capacity-bytes", type=int)
        p.add_argument("--output", required=True)
        if name != "decode-trace":
            p.add_argument("--container", required=True)
        if name == "build-tables":
            p.add_argument("--cams", required=True)
        if name == "attribution":
            p.add_argument("--observer", required=True)
    args = parser.parse_args()
    if args.command == "validate-container":
        write_json(args.output, validate_container(Path(args.container).read_bytes()))
        return
    if args.command == "build-container":
        plan = json.loads(Path(args.plan).read_text())
        slots = json.loads(Path(args.slots).read_text()) if args.slots else {}
        out = make_container(
            args.kind,
            plan["boundary_pcs"],
            plan["routes"],
            slots,
            plan["initial_metadata_id"],
            plan["valid_mask"],
        )
        Path(args.output).write_bytes(out)
        return
    if args.command == "route-plan":
        coverage = disassembly_coverage(args.elf, args.objdump)
        report = plan_routes(json_rows(args.events), coverage, args.initial_pc)
        report["inputs"] = {
            "elf": str(Path(args.elf).resolve()),
            "elf_sha256": sha(args.elf),
            "events_sha256": sha(args.events),
            "objdump": args.objdump,
        }
        report["coverage"] = {
            "instructions": len(coverage),
            "method": "objdump -d independent ELF widths; includes holes in static footprint",
        }
        write_json(args.output, report)
        return
    rows = decode_trace(args.trace, args.kind, args.p_error, args.capacity_bytes)
    if args.command == "decode-trace":
        counts = Counter("boundary" if r["is_boundary"] else "hint" for r in rows)
        write_json(args.output, {"counts": dict(counts), "sha256": sha(args.trace)})
        return
    container = validate_container(Path(args.container).read_bytes())
    need(
        container["kind"] == (0 if args.kind == "discovery" else 1),
        "trace/container kind mismatch",
    )
    if args.command == "build-cams":
        need(args.kind == "discovery", "CAM construction requires Discovery")
        cams = select_cams(rows, container)
        out = Path(args.output)
        out.mkdir(parents=True, exist_ok=True)
        for mid, entries in cams.items():
            (out / f"{mid}-cam.bin").write_bytes(cam_image(entries))
        write_json(out / "cams.json", cams)
    elif args.command == "build-tables":
        need(args.kind == "detail", "Context construction requires Detail")
        cams = json.loads(Path(args.cams).read_text())
        report = build_tables(rows, container, cams, args.output)
        write_json(Path(args.output) / "tables.json", report)
    else:
        write_json(args.output, attribution(rows, container, json_rows(args.observer)))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
