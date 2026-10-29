"""Gem5 retirement-audited CAM58 adapter; public raw/builder formats stay fixed."""
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct

from util.h2p_hint import spec_trace as public


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def checked_rows(trace, meta, kind, cam=None):
    """Join by explicit raw index, never by assumed retirement execution order."""
    trace = Path(trace)
    metadata = public.load_json(meta)
    _, decoded = public.read_trace(trace, meta, cam)
    public.require(metadata.get("kind") == kind, "CAM58 trace kind mismatch")
    audit_path = Path(str(trace) + ".audit.jsonl")
    public.require(metadata.get("data_sha256") == sha(trace) and
                   metadata.get("audit_sha256") == sha(audit_path), "CAM58 source digest mismatch")
    audit = [json.loads(line) for line in audit_path.read_text().splitlines()]
    public.require(len(audit) >= 2, "CAM58 missing audit header/footer")
    header, footer = audit[0], audit[-1]
    width = 7 if kind == "discovery" else 22
    public.require(header.get("kind") == "header" and header.get("version") == 1 and
                   header.get("producer") == "gem5-retired-cfi" and
                   header.get("cam_identity") == "checked_pc_width_classification" and
                   header.get("record_bytes") == width, "unsupported CAM58 producer identity")
    records = [row for row in audit[1:-1] if row.get("kind") == "record"]
    window_misses = [row for row in audit[1:-1] if row.get("kind") == "window_miss"]
    public.require(len(records) + len(window_misses) == len(audit) - 2,
                   "unknown CAM58 audit row kind")
    totals, miss_ordinals = Counter(), {}
    for row in window_misses:
        ordinal = row.get("retired_ordinal")
        public.require(type(ordinal) is int and ordinal > 0 and ordinal not in miss_ordinals,
                       "duplicate/invalid window-miss retirement identity")
        miss_ordinals[ordinal] = row
        for name in ("is_rvc", "is_call", "is_return", "is_indirect"):
            public.require(type(row.get(name)) is bool, f"missing window-miss identity {name}")
        q, pc = int(row["start_pc"], 16), int(row["pc"], 16)
        public.address_word(q)
        public.address_word(pc)
        end = pc + (0 if row["is_rvc"] else 2)
        public.address_word(end)
        public.require(row.get("baseline_miss") is True and q <= (1 << 64) - 65 and
                       q <= end < min((q + 64) & ~31, (q & ~4095) + 4096) and
                       (pc >= q or (not row["is_rvc"] and pc == q - 2)),
                       "invalid all-control window-miss sample")
        totals[q] += 1
    data = trace.read_bytes()
    final_bit = 54 if kind == "discovery" else 172
    public.require(footer.get("kind") == "footer" and footer.get("complete") is True and
                   footer.get("overflow") is False and footer.get("p_error") == 0 and
                   footer.get("records") == len(records) == len(decoded) and
                   len(data) == width * (len(records) + 1) and
                   int.from_bytes(data[-width:], "little") == 1 << final_bit,
                   "incomplete CAM58 audit/raw capture")
    indexed = {}
    ordinals = set()
    for row in records:
        index = row.get("index")
        ordinal = row.get("retired_ordinal")
        public.require(row.get("kind") == "record" and type(index) is int and
                       0 <= index < len(records) and index not in indexed and
                       type(ordinal) is int and ordinal > 0 and ordinal not in ordinals,
                       "CAM58 duplicate/invalid raw index or retirement identity")
        indexed[index] = row
        ordinals.add(ordinal)
    result = []
    for index, decoded_row in enumerate(decoded):
        row = indexed[index]
        for name in ("is_rvc", "is_call", "is_return", "is_indirect", "baseline_miss", "query_valid"):
            public.require(type(row.get(name)) is bool, f"CAM58 missing Boolean {name}")
        public.require(row["is_indirect"], "CAM58 raw record must be indirect")
        q, pc = int(row["start_pc"], 16), int(row["pc"], 16)
        public.address_word(q)
        public.address_word(pc)
        public.require(q <= (1 << 64) - 65, "CAM58 window calculation wraps")
        end = pc + (0 if row["is_rvc"] else 2)
        public.address_word(end)
        effective_end = min((q + 64) & ~31, (q & ~4095) + 4096)
        public.require(q <= end < effective_end < 1 << 64 and
                       (pc >= q or (not row["is_rvc"] and pc == q - 2)), "CAM58 CFI outside original window")
        position = (end - (q & ~31)) // 2
        public.require(0 <= position < 32 and decoded_row["start_pc"] == q and
                       decoded_row["window_delta"] * 2 == pc - q,
                       "CAM58 sidecar differs from raw window/DELTA")
        if kind == "discovery":
            public.require(decoded_row["pc"] == pc and row["baseline_miss"] is True,
                           "CAM58 Discovery PC/miss mismatch")
            public.require(row["retired_ordinal"] in miss_ordinals,
                           "Discovery miss missing from all-control window evidence")
            evidence = miss_ordinals[row["retired_ordinal"]]
            public.require(all(row[field] == evidence[field] for field in
                               ("start_pc", "pc", "is_rvc", "is_call", "is_return", "is_indirect")),
                           "Discovery and all-control miss identity disagree")
        else:
            public.require(decoded_row["target"] == int(row["target"], 16) and
                           decoded_row["baseline_miss"] == row["baseline_miss"] and
                           decoded_row["query_valid"] == row["query_valid"], "CAM58 Detail identity mismatch")
        result.append({"row": row, "raw": data[index * width:(index + 1) * width],
                       "q": q, "pc": pc, "position": position, "window_totals": totals})
    return result


def choices(rows):
    totals = rows[0]["window_totals"] if rows else Counter()
    counts, identities = defaultdict(Counter), {}
    for item in rows:
        row = item["row"]
        key = item["q"], item["pc"]
        identity = item["position"], row["is_rvc"], row["is_call"], row["is_return"]
        public.require(key not in identities or identities[key] == identity, "unstable CAM58 CFI identity")
        identities[key] = identity
        if not row["is_return"]:
            counts[item["q"]][item["pc"]] += 1
    selected = []
    # Rank windows BEFORE filtering/selecting the unique eligible CFI.
    for q in sorted(totals, key=lambda q: (-totals[q], q))[:16]:
        if not counts[q]:
            continue
        pc = min(counts[q], key=lambda pc: (-counts[q][pc], pc))
        position, is_rvc, is_call, _ = identities[q, pc]
        selected.append({"id": len(selected), "start_pc": hex(q), "pc": hex(pc),
                         "position": position, "is_rvc": is_rvc, "is_call": is_call,
                         "window_misses": totals[q], "cfi_misses": counts[q][pc]})
    return selected


def select_cam(trace, meta, cam_path, public_image, selection_path, image):
    entries = choices(checked_rows(trace, meta, "discovery"))
    cam = public.identity("hint-spec-cam")
    cam["entries"] = [{"id": i, "valid": i < len(entries), "mask": 0,
                       "start_pc": entries[i]["start_pc"] if i < len(entries) else "0x0",
                       "discovery_misses": entries[i]["window_misses"] if i < len(entries) else 0}
                      for i in range(16)]
    data = public.cam_image(cam)
    cam["image_sha256"] = public.digest(data)
    write(cam_path, cam)
    Path(public_image).write_bytes(data)
    write(selection_path, {"schema": "gem5-hint-cam58-selection", "version": 1,
                          "source_raw": str(Path(trace).resolve()), "source_meta": str(Path(meta).resolve()),
                          "source_raw_sha256": sha(trace), "source_meta_sha256": sha(meta),
                          "source_audit_sha256": sha(str(trace) + ".audit.jsonl"),
                          "public_cam_sha256": sha(cam_path), "entries": entries})
    adapt_image(public_image, cam_path, selection_path, image, "cam")


def selection_entries(selection_path, cam_path):
    selection = public.load_json(selection_path)
    public.require(selection.get("schema") == "gem5-hint-cam58-selection" and selection.get("version") == 1,
                   "unsupported CAM58 selection")
    public.require(selection["public_cam_sha256"] == sha(cam_path) and
                   selection["source_raw_sha256"] == sha(selection["source_raw"]) and
                   selection["source_meta_sha256"] == sha(selection["source_meta"]) and
                   selection["source_audit_sha256"] == sha(selection["source_raw"] + ".audit.jsonl"),
                   "CAM58 frozen source changed")
    expected = choices(checked_rows(selection["source_raw"], selection["source_meta"], "discovery"))
    public.require(selection["entries"] == expected, "CAM58 selection differs from source window/CFI ranking")
    cam = public.read_cam(cam_path)
    for i, row in enumerate(cam["entries"]):
        public.require(row["valid"] == (i < len(expected)) and
                       row["start_pc"] == (expected[i]["start_pc"] if i < len(expected) else "0x0"),
                       "CAM58 pc_id mapping changed")
    return {row["id"]: row for row in expected}


def filter_detail(trace, meta, cam_path, selection_path, output, output_meta):
    cam = public.read_cam(cam_path)
    entries = selection_entries(selection_path, cam_path)
    rows = checked_rows(trace, meta, "detail", cam)
    chosen = {int(row["start_pc"], 16): row for row in entries.values()}
    selected = []
    for item in rows:
        expected = chosen.get(item["q"])
        if expected and item["pc"] == int(expected["pc"], 16):
            public.require(not item["row"]["is_return"] and
                           all(item["row"][key] == expected[key] for key in ("is_rvc", "is_call")) and
                           item["position"] == expected["position"], "CAM58 fixed CFI changed during Detail")
            selected.append(item)
    output = Path(output)
    output.write_bytes(b"".join(item["raw"] for item in selected) + (1 << 172).to_bytes(22, "little"))
    header = {"kind": "header", "version": 1, "producer": "gem5-retired-cfi",
              "cam_identity": "checked_pc_width_classification", "record_bytes": 22,
              "source_raw_sha256": sha(trace), "source_audit_sha256": sha(str(trace) + ".audit.jsonl")}
    audit = [header]
    for index, item in enumerate(selected):
        audit.append({**item["row"], "source_index": item["row"]["index"], "index": index})
    audit.append({"kind": "footer", "complete": True, "records": len(selected), "overflow": False, "p_error": 0})
    audit_path = Path(str(output) + ".audit.jsonl")
    audit_path.write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in audit))
    metadata = public.load_json(meta)
    metadata.update(data_sha256=sha(output), audit_sha256=sha(audit_path),
                    capacity_records=len(selected) + 1, termination="final", selection_sha256=sha(selection_path))
    write(output_meta, metadata)


def adapt_image(public_image, cam_path, selection_path, output, kind):
    data = bytearray(Path(public_image).read_bytes())
    public.inspect_image(data, kind)
    entries = selection_entries(selection_path, cam_path)
    for i in range(16):
        word = struct.unpack_from("<Q", data, 16 + 8 * i)[0]
        public.require(bool(word & (1 << 51)) == (i in entries), "CAM58 image validity mapping differs")
        if i in entries:
            row = entries[i]
            public.require(public.address(word & public.MASK47) == int(row["start_pc"], 16),
                           "CAM58 image startPC mapping differs")
            word |= row["position"] << 52 | int(row["is_call"]) << 57
        struct.pack_into("<Q", data, 16 + 8 * i, word)
    data[:8] = b"H2PCAM58" if kind == "cam" else b"H2PLD058"
    Path(output).write_bytes(data)
    write(Path(output).with_suffix(".adapter.json"), {
        "schema": "gem5-hint-cam58-image", "version": 1, "kind": kind,
        "public_image_sha256": sha(public_image), "public_cam_sha256": sha(cam_path),
        "selection_sha256": sha(selection_path), "image_sha256": sha(output),
        "adapter_sha256": sha(__file__),
        "context_slot_bytes_unchanged": bytes(data[144:]) == Path(public_image).read_bytes()[144:]})
