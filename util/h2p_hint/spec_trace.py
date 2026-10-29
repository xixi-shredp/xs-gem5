#!/usr/bin/env python3
"""Standalone SPEC binary trace validator and deterministic offline table builder.

Only Python's standard library is needed. No simulator state or record ordering
is used. Static correction estimates do not predict IPC or dynamic confidence.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct

VERSION = 1
HASH_VERSION = 7
MASK_ORDER = ["ITH", "Call", "Ret", "LHR"]
SIZES = {"discovery": 7, "detail": 22}
MASK47 = (1 << 47) - 1
MASK64 = (1 << 64) - 1
FIELDS = {
    "discovery": [("pc", 47), ("window_delta", 7), ("final", 1)],
    "detail": [("pc_id", 4), ("target", 47), ("ITH", 28), ("LHR", 28),
               ("Call", 28), ("Ret", 28), ("window_delta", 7),
               ("baseline_miss", 1), ("query_valid", 1), ("final", 1)],
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load_json(path):
    return json.loads(Path(path).read_text())


def write_json(path, data):
    Path(path).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def identity(schema):
    return {"schema": schema, "version": VERSION, "hash_version": HASH_VERSION,
            "mask_order": MASK_ORDER}


def check_identity(value, schema):
    require(isinstance(value, dict), "sidecar must be a JSON object")
    for key, expected in identity(schema).items():
        require(type(value.get(key)) is type(expected) and value[key] == expected,
                f"unsupported {key} (expected {expected!r})")


def integer(value, low, high, field):
    require(type(value) is int and low <= value <= high, f"invalid {field}")
    return value


def address(word):
    """Recover the canonical sv48 address from its 47-bit halfword encoding."""
    integer(word, 0, MASK47, "address word")
    low = word << 1
    return low | (MASK64 ^ ((1 << 48) - 1)) if low & (1 << 47) else low


def address_word(pc):
    integer(pc, 0, MASK64, "address")
    require(not pc & 1, "address must be halfword aligned")
    word = (pc >> 1) & MASK47
    require(address(word) == pc, "address must be canonical sv48")
    return word


def fold(value, width):
    result = 0
    while value:
        result ^= value & ((1 << width) - 1)
        value >>= width
    return result


def component(value):
    return ((fold(value, 17) ^ ((fold(value, 16) << 2) & 0x1ffff)) << 11) | fold(value, 11)


def key_for(start_pc, row, mask):
    integer(mask, 0, 15, "mask")
    result = component(address_word(start_pc))
    for bit, name in enumerate(MASK_ORDER):
        if mask & (1 << bit):
            result ^= row[name]
    return result


def read_cam(path):
    cam = load_json(path)
    check_identity(cam, "hint-spec-cam")
    entries = cam.get("entries")
    require(isinstance(entries, list) and len(entries) == 16, "CAM must contain 16 fixed slots")
    seen = set()
    for i, row in enumerate(entries):
        require(isinstance(row, dict) and row.get("id") == i and type(row.get("id")) is int,
                "CAM ids must retain their original slot positions")
        require(type(row.get("valid")) is bool, "invalid CAM valid flag")
        integer(row.get("mask"), 0, 15, "CAM mask")
        pc = int(row.get("start_pc", ""), 16)
        address_word(pc)
        if row["valid"]:
            require(pc not in seen, "duplicate CAM startPC")
            seen.add(pc)
        else:
            require(pc == 0 and row["mask"] == 0, "invalid CAM rows must be zero")
    require(digest(cam_image(cam)) == cam.get("image_sha256"), "CAM mapping/image digest mismatch")
    return cam


def cam_image(cam, masks=None):
    words = []
    for row in cam["entries"]:
        mask = row["mask"] if masks is None else masks.get(row["id"], row["mask"])
        words.append(address_word(int(row["start_pc"], 16)) | (mask << 47) | (1 << 51)
                     if row["valid"] else 0)
    return b"H2PHINTP" + bytes(8) + struct.pack("<16Q", *words)


def window_geometry(meta):
    """Optional sidecar declarations audit the fixed SPEC geometry, not override it."""
    for name, fixed in (("fetch_block_bytes", 64), ("fetch_align_bytes", 32),
                        ("page_bytes", 4096)):
        if name in meta:
            require(type(meta[name]) is int and meta[name] == fixed,
                    f"{name} must equal fixed SPEC value {fixed}")
    return 64, 32, 4096


def read_trace(trace_path, meta_path, cam=None):
    meta = load_json(meta_path)
    check_identity(meta, "hint-spec-trace")
    window_geometry(meta)
    kind = meta.get("kind")
    require(kind in SIZES, "unknown trace kind")
    size = SIZES[kind]
    require(type(meta.get("record_bytes")) is int and meta["record_bytes"] == size,
            "record length differs from trace kind")
    require(meta.get("termination") in ("final", "capacity"), "unknown termination mode")
    capacity = integer(meta.get("capacity_records"), 0, (1 << 63) - 1, "capacity_records")
    data = Path(trace_path).read_bytes()
    layout = meta.get("layout", "compact")
    require(layout in ("compact", "buffer"), "unknown trace file layout")
    if layout == "buffer":
        buffer_bytes = integer(meta.get("buffer_bytes"), 0, (1 << 63) - 1, "buffer_bytes")
        require(len(data) == buffer_bytes, "buffer dump length differs from buffer_bytes")
        require(capacity == buffer_bytes // size, "buffer capacity does not match complete-record limit")
    else:
        require(len(data) % size == 0, "truncated trace record")
        require(len(data) // size <= capacity, "trace exceeds record capacity")
    if "data_sha256" in meta:
        require(meta["data_sha256"] == digest(data), "trace digest mismatch")
    require(meta.get("stop_reason") not in ("memory-fault", "memory_fault"),
            "memory-fault trace cannot be used")
    if "p_error" in meta:
        integer(meta["p_error"], 0, 7, "p_error")
        require(meta["p_error"] in (0, 1, 2, 4), "reserved profiling error encoding")
        require(meta["p_error"] != 4, "memory-fault trace cannot be used")
    if kind == "detail":
        require(cam is not None, "Detail requires frozen CAM mapping")
        require(meta.get("cam_sha256") == cam["image_sha256"], "Detail CAM digest mismatch")
    rows = []
    final_seen = False
    parse_bytes = (len(data) // size) * size
    for offset in range(0, parse_bytes, size):
        value = int.from_bytes(data[offset:offset + size], "little")
        row, position = {}, 0
        for name, bits in FIELDS[kind]:
            row[name] = (value >> position) & ((1 << bits) - 1)
            position += bits
        require(value >> position == 0, f"nonzero reserved bits at record {offset // size}")
        require(not final_seen, "records after final marker")
        if row["final"]:
            require(value == 1 << (position - 1), "final marker contains payload")
            final_seen = True
            if layout == "buffer":
                break
            continue
        delta = row["window_delta"]
        row["window_delta"] = delta - 128 if delta & 64 else delta
        if kind == "discovery":
            pc = address(row["pc"])
            row["start_pc"] = pc - 2 * row["window_delta"]
            row["pc"] = pc
        else:
            entry = cam["entries"][row["pc_id"]]
            require(entry["valid"], "Detail pc_id points to invalid CAM slot")
            row["start_pc"] = int(entry["start_pc"], 16)
            row["target"] = address(row["target"])
        rows.append(row)
    if meta["termination"] == "final":
        require(final_seen, "missing final record")
    else:
        require(not final_seen and len(data) // size == capacity,
                "capacity termination must fill the complete record capacity without final")
    return meta, rows


def valid_window(row, meta=None):
    """Raw window ownership is independent of prediction-snapshot validity.

    Raw records omit length. DELTA=-1 is the SPEC producer-guaranteed 32-bit
    boundary case; nonnegative offsets accept any possible 2/4-byte ownership.
    """
    width, align, page = window_geometry(meta or {})
    start = row["start_pc"]
    pc = row.get("pc", start + 2 * row["window_delta"])
    end = min((start + width) & ~(align - 1), (start & ~(page - 1)) + page)
    delta = row["window_delta"]
    if delta < -1:
        return False
    cfi_end = pc + (2 if delta == -1 else 0)
    if start + width > MASK64 or end <= start or not start <= cfi_end < end:
        return False
    try:
        address_word(start)
        address_word(pc)
        address_word(end - 2)
    except ValueError:
        return False
    return True


def require_windows(rows, meta=None):
    require(all(valid_window(row, meta) is not False for row in rows),
            "invalid prediction window: branch is outside the declared fetch window")


def trace_report(meta, rows):
    valid = sum(row.get("query_valid", 1) for row in rows)
    return {"kind": meta["kind"], "records": len(rows), "query_valid": valid,
            "query_invalid": len(rows) - valid,
            "invalid_window": sum(valid_window(row, meta) is False for row in rows),
            "negative_delta": sum(row["window_delta"] < 0 for row in rows),
            "delta_min": min((row["window_delta"] for row in rows), default=None),
            "delta_max": max((row["window_delta"] for row in rows), default=None),
            "termination": meta["termination"],
            "layout": meta.get("layout", "compact"),
            "p_error": meta.get("p_error", 0),
            "note": "unordered; DELTA=-1 assumes producer-guaranteed 32-bit boundary ownership; snapshot validity is separate"}


def select_cam(rows, meta=None):
    require_windows(rows, meta)
    counts = Counter(row["start_pc"] for row in rows)
    ranked = sorted(counts, key=lambda pc: (-counts[pc], pc))[:16]
    cam = identity("hint-spec-cam")
    cam["entries"] = [{"id": i, "valid": i < len(ranked), "mask": 0,
                       "start_pc": hex(ranked[i]) if i < len(ranked) else "0x0",
                       "discovery_misses": counts[ranked[i]] if i < len(ranked) else 0}
                      for i in range(16)]
    cam["image_sha256"] = digest(cam_image(cam))
    return cam


def observations(rows, masks):
    groups = defaultdict(lambda: {"targets": Counter(), "baseline_correct": 0,
                                  "miss_targets": Counter(), "correct_targets": Counter()})
    for row in rows:
        if not row["query_valid"]:
            continue
        key = key_for(row["start_pc"], row, masks[row["pc_id"]])
        group = groups[key]
        group["targets"][row["target"]] += 1
        group["baseline_correct"] += not row["baseline_miss"]
        group["miss_targets" if row["baseline_miss"] else "correct_targets"][row["target"]] += 1
    return groups


def select_table(groups):
    # Deterministic greedy positive correction ranking with explicit finite
    # capacities. This is a heuristic, not an optimum set/slot allocation solver.
    candidates = []
    for key, group in groups.items():
        for target, count in group["targets"].items():
            gain = count - group["baseline_correct"]
            if gain > 0:
                candidates.append((gain, key, target))
    selected, targets, used_keys, sets = [], [], set(), Counter()
    for gain, key, target in sorted(candidates, key=lambda item: (-item[0], item[1], item[2])):
        if key in used_keys or sets[key & 2047] >= 2:
            continue
        if target not in targets:
            if len(targets) == 32:
                continue
            targets.append(target)
        selected.append((gain, key, target))
        used_keys.add(key)
        sets[key & 2047] += 1
    return selected, targets


def score(rows, masks):
    groups = observations(rows, masks)
    selected, slots = select_table(groups)
    corrected = sum(groups[key]["miss_targets"][target] for _, key, target in selected)
    harmed = sum(groups[key]["baseline_correct"] - groups[key]["correct_targets"][target]
                 for _, key, target in selected)
    return {"corrected": corrected, "harmed": harmed, "net_correction": corrected - harmed,
            "observed_keys": len(groups), "mapped_keys": len(selected), "slot_targets": len(slots),
            "multi_target_keys": sum(len(group["targets"]) > 1 for group in groups.values())}


def rank_masks(rows, cam, meta=None):
    require_windows(rows, meta)
    report, best = [], {}
    for entry in cam["entries"]:
        if not entry["valid"]:
            continue
        pc_id = entry["id"]
        subset = [row for row in rows if row["pc_id"] == pc_id]
        scores = [{"mask": mask, **score(subset, {pc_id: mask})} for mask in range(16)]
        chosen = min(scores, key=lambda result: (-result["net_correction"],
                                                 result["mask"].bit_count(), result["mask"]))
        best[pc_id] = chosen["mask"]
        report.append({"pc_id": pc_id, "start_pc": entry["start_pc"],
                       "records": len(subset), "valid_records": sum(r["query_valid"] for r in subset),
                       "static_best_mask": chosen["mask"], "masks": scores})
    return best, report


def build_image(rows, cam, masks, meta=None):
    require_windows(rows, meta)
    selected, slots = select_table(observations(rows, masks))
    context = [0] * 4096
    used = Counter()
    for _, key, target in sorted(selected, key=lambda item: item[1]):
        set_id = key & 2047
        position = set_id * 2 + used[set_id]
        used[set_id] += 1
        context[position] = (key >> 11) | (slots.index(target) << 17) | (3 << 22) | (1 << 24)
    body = cam_image(cam, masks)[16:] + struct.pack("<4096I", *context)
    body += struct.pack("<32Q", *([address_word(t) for t in slots] + [0] * (32 - len(slots))))
    return b"H2PHINTL" + bytes(8) + body


def inspect_image(data, kind):
    size, magic = (144, b"H2PHINTP") if kind == "cam" else (16784, b"H2PHINTL")
    require(len(data) == size, "incorrect image length")
    require(data[:8] == magic and data[8:16] == bytes(8), "invalid image header/reserved bytes")
    cam = struct.unpack_from("<16Q", data, 16)
    seen = set()
    for word in cam:
        require(word >> 52 == 0, "CAM reserved bits are nonzero")
        if word & (1 << 51):
            pc = word & MASK47
            require(pc not in seen, "duplicate CAM startPC")
            seen.add(pc)
        else:
            require(word == 0, "invalid CAM row must be zero")
    report = {"kind": kind, "bytes": len(data), "sha256": digest(data), "valid_cam": len(seen)}
    if kind == "load":
        context = struct.unpack_from("<4096I", data, 0x90)
        slots = struct.unpack_from("<32Q", data, 0x4090)
        require(all(word >> 47 == 0 for word in slots), "Slot reserved bits are nonzero")
        for i, word in enumerate(context):
            require(word >> 25 == 0, "Context reserved bits are nonzero")
            if not word & (1 << 24):
                require(word == 0, "invalid Context row must be zero")
            if i % 2 and word & (1 << 24) and context[i - 1] & (1 << 24):
                require((word & 0x1ffff) != (context[i - 1] & 0x1ffff), "duplicate set/tag")
        report["valid_context"] = sum(bool(word & (1 << 24)) for word in context)
    return report


def pack_image(metadata):
    """Compatibility JSON-table API; raw trace CLI remains the primary input."""
    require(isinstance(metadata, dict), "table metadata must be an object")
    require(type(metadata.get("version")) is int and metadata["version"] == HASH_VERSION,
            "table metadata must use current SPEC version 7")
    require(type(metadata.get("hash_version")) is int and metadata["hash_version"] == HASH_VERSION,
            "table metadata must use current hash version 7")
    require(metadata.get("mask_order") == MASK_ORDER, "table metadata has wrong mask order")
    cams, entries, targets = metadata.get("cam"), metadata.get("entries"), metadata.get("slots")
    require(isinstance(cams, list) and len(cams) <= 16, "invalid CAM table")
    require(isinstance(entries, list) and len(entries) <= 4096, "invalid Context table")
    require(isinstance(targets, list) and len(targets) <= 32, "invalid Slot table")
    def number(value):
        return int(value, 16) if isinstance(value, str) and value.startswith("0x") else value
    cam_words, context, slots = [0] * 16, [0] * 4096, [0] * 32
    for i, row in enumerate(cams):
        require(type(row.get("valid", True)) is bool, "invalid CAM valid flag")
        if row.get("valid", True):
            pc = number(row.get("start_pc", row.get("pc")))
            mask = integer(number(row.get("mask")), 0, 15, "mask")
            cam_words[i] = address_word(pc) | (mask << 47) | (1 << 51)
    for i, target in enumerate(targets):
        slots[i] = address_word(number(target))
    require(len(set(slots[:len(targets)])) == len(targets), "duplicate Slot target")
    used = set()
    for row in entries:
        require(type(row.get("valid", True)) is bool, "invalid Context valid flag")
        if not row.get("valid", True):
            continue
        set_id = integer(number(row.get("set")), 0, 2047, "set")
        way = integer(number(row.get("way")), 0, 1, "way")
        tag = integer(number(row.get("tag")), 0, 0x1ffff, "tag")
        slot = integer(number(row.get("slot")), 0, len(targets) - 1, "slot")
        conf = integer(number(row.get("conf")), 3, 3, "initial confidence")
        position = set_id * 2 + way
        require(position not in used, "duplicate Context position")
        used.add(position)
        context[position] = tag | (slot << 17) | (conf << 22) | (1 << 24)
    data = (b"H2PHINTL" + bytes(8) + struct.pack("<16Q", *cam_words)
            + struct.pack("<4096I", *context) + struct.pack("<32Q", *slots))
    inspect_image(data, "load")
    return data


def unpack_image(data):
    inspect_image(data, "load")
    return (struct.unpack_from("<16Q", data, 0x10),
            struct.unpack_from("<4096I", data, 0x90),
            struct.unpack_from("<32Q", data, 0x4090))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    for name in ("validate", "select-cam", "rank-masks", "build"):
        sub = commands.add_parser(name)
        sub.add_argument("--trace", required=True, type=Path)
        sub.add_argument("--meta", required=True, type=Path)
        sub.add_argument("--cam", type=Path)
        sub.add_argument("--output", required=name != "validate", type=Path)
        if name in ("select-cam", "build"):
            sub.add_argument("--image", required=True, type=Path)
        if name == "build":
            sub.add_argument("--masks", type=Path, help="JSON object mapping fixed CAM ids to masks")
    inspect = commands.add_parser("inspect")
    inspect.add_argument("--kind", choices=("cam", "load"), required=True)
    inspect.add_argument("--image", type=Path, required=True)
    inspect.add_argument("--meta", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.action == "inspect":
            data = args.image.read_bytes()
            report = inspect_image(data, args.kind)
            if args.meta:
                meta = load_json(args.meta)
                check_identity(meta, "hint-spec-cam" if args.kind == "cam" else "hint-spec-image")
                require(meta.get("image_sha256") == digest(data), "image sidecar digest mismatch")
        else:
            cam = read_cam(args.cam) if args.cam else None
            meta, rows = read_trace(args.trace, args.meta, cam)
            if args.action == "validate":
                report = trace_report(meta, rows)
                if args.output:
                    write_json(args.output, report)
            elif args.action == "select-cam":
                require(meta["kind"] == "discovery", "select-cam requires Discovery")
                report = select_cam(rows, meta)
                args.image.write_bytes(cam_image(report))
                write_json(args.output, report)
            else:
                require(meta["kind"] == "detail", "mask selection/build requires Detail")
                best, rankings = rank_masks(rows, cam, meta)
                candidates, seen = [], set()
                for label, masks in (("static-best", best),
                                     ("uniform9", {i: 9 for i in best}),
                                     ("uniform15", {i: 15 for i in best})):
                    image = build_image(rows, cam, masks, meta)
                    image_sha = digest(image)
                    if image_sha not in seen:
                        seen.add(image_sha)
                        candidates.append({"label": label, "masks": masks, "image_sha256": image_sha,
                                           **score(rows, masks)})
                report = {**identity("hint-spec-mask-ranking"), "pcs": rankings,
                          "candidates": candidates,
                          "trace_report": trace_report(meta, rows),
                          "selection_basis": "unordered fixed-confidence static estimate; IPC must be measured",
                          "capacity_policy": "greedy positive gain; 2048 sets x 2 ways; 32 shared targets"}
                if args.action == "build":
                    masks = best
                    if args.masks:
                        value = load_json(args.masks)
                        require(isinstance(value, dict), "mask file must be an object")
                        masks = {int(k): integer(v, 0, 15, "mask") for k, v in value.items()}
                        require(set(masks) == set(best), "masks must cover every valid frozen CAM id")
                    image = build_image(rows, cam, masks, meta)
                    args.image.write_bytes(image)
                    report.update(identity("hint-spec-image"))
                    report.update({"kind": "load", "image_sha256": digest(image), "masks": masks,
                                   "cam_sha256": cam["image_sha256"], "trace_sha256": digest(args.trace.read_bytes()),
                                   "estimate": score(rows, masks)})
                    inspect_image(image, "load")
                write_json(args.output, report)
        print(json.dumps(report, sort_keys=True))
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(2, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
