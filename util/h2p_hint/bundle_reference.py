"""Observer-only attribution reference and replay images; never runtime input.

Exact refers to logical ownership, not counterfactual history reconstruction.
Cross-table LHR snapshots and unsampled new CAM entries are explicitly uncovered.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import bundle_tools as b


def join_writes(rows, audits, events, container, kind, final_index=None):
    writes = {}
    for audit in audits:
        if audit.get("event") != "write":
            continue
        index = b.number(audit["record_index"])
        b.need(
            index >= 0 and index not in writes,
            "duplicate/negative physical write index",
        )
        writes[index] = audit
    committed = {}
    for event in events:
        seq = b.number(event["seq"])
        b.need(
            seq > 0 and seq not in committed,
            "duplicate/nonpositive committed CFI sequence",
        )
        committed[seq] = event
    result = []
    for approximate, row in b.attributed_rows(rows, container):
        index = row["record_index"]
        b.need(index in writes, "missing physical write audit")
        audit = writes.pop(index)
        b.need(
            bool(audit["is_boundary"]) == row["is_boundary"],
            "raw/audit record type mismatch",
        )
        seq = b.number(audit["seq"])
        b.need(seq in committed, "write audit lacks independent committed CFI")
        event = committed[seq]
        pc, size = b.number(event["pc"]), b.number(event["size"])
        b.address_word(pc)
        b.need(
            size in (2, 4) and pc == b.number(audit["pc"]), "source PC/width mismatch"
        )
        if row["is_boundary"]:
            b.need(
                pc == row["pc"] and b.number(event["target"]) == row["target"],
                "Boundary source/target mismatch",
            )
            continue
        truth = audit["metadata_id"]
        truth = None if truth is None else b.number(truth)
        b.need(
            truth is None
            or (0 <= truth < 32 and container["metadata_valid_mask"] & (1 << truth)),
            "logical metadata absent from container",
        )
        if kind == "discovery":
            c = (row["start_pc"] & ~31) + row["position"] * 2
            b.need(c == pc + size - 2, "Discovery C does not match committed P+size-2")
            b.need(bool(event["call"]) == row["is_call"], "Discovery isCall mismatch")
        else:
            b.need(
                row["pc_id"] == b.number(audit["pc_id"])
                and bool(row["query_valid"]) == bool(audit["query_valid"])
                and row["target"]
                == b.number(event["target"])
                == b.number(audit["target"]),
                "Detail raw/audit/committed mismatch",
            )
        result.append(
            {
                "approximate": approximate,
                "exact": truth,
                "row": row,
                "audit": audit,
                "event": event,
                "pc": pc,
                "size": size,
            }
        )
    if final_index is not None and final_index in writes:
        terminal = writes.pop(final_index)
        b.need(
            not terminal["is_boundary"]
            and b.number(terminal["seq"]) == 0
            and b.number(terminal["pc"]) == 0,
            "unexpected final write audit",
        )
    b.need(not writes, "write audit contains unmatched physical records")
    return result


def select(joined, container, ownership):
    selected = {}
    for mid in range(32):
        if not container["metadata_valid_mask"] & (1 << mid):
            continue
        local = {**container, "initial_metadata_id": mid}
        rows = (x["row"] for x in joined if x[ownership] == mid)
        selected[mid] = b.select_cams(rows, local)[mid]
    return selected


def identity(entry):
    return (
        b.number(entry["start_pc"]),
        b.number(entry["position"]),
        bool(entry["is_call"]),
    )


def matches(entry, pc, size, call):
    q, pos, is_call = identity(entry)
    c = (q & ~31) + pos * 2
    return c == pc + size - 2 and is_call == bool(call)


def semantic_rows(joined, cams, snapshot_cams, ownership):
    grouped = defaultdict(list)
    counts = Counter()
    covered = defaultdict(Counter)
    for sample in joined:
        mid, row, audit = sample[ownership], sample["row"], sample["audit"]
        counts["samples"] += 1
        if mid is None:
            counts["no_owner"] += 1
            continue
        target_entries = cams.get(mid, cams.get(str(mid), []))
        hits = [
            e
            for e in target_entries
            if matches(e, sample["pc"], sample["size"], sample["event"]["call"])
        ]
        b.need(len(hits) <= 1, "ambiguous destination CAM P/C")
        if not hits:
            counts["outside_selected_cam"] += 1
            continue
        entry = hits[0]
        if not row["query_valid"]:
            counts["invalid_snapshot"] += 1
            continue
        snapshot_mid = audit["snapshot_metadata_id"]
        if snapshot_mid is None or b.number(snapshot_mid) != mid:
            counts["cross_table_history_unavailable"] += 1
            continue
        old_entries = snapshot_cams.get(str(mid), snapshot_cams.get(mid, []))
        old = [e for e in old_entries if b.number(e["id"]) == row["pc_id"]]
        b.need(len(old) == 1, "snapshot pc_id absent from original snapshot CAM")
        b.need(
            matches(old[0], sample["pc"], sample["size"], sample["event"]["call"]),
            "snapshot pc_id does not identify committed P/C",
        )
        # Source identity is independently verified before changing pc_id.
        # LHR is reused only within the same logical table and same CFI.
        grouped[mid].append(
            {
                **row,
                "pc_id": entry["id"],
                "start_pc": b.number(entry["start_pc"]),
                "window_delta": 0,
            }
        )
        covered[mid][entry["id"]] += 1
        counts["usable"] += 1
    uncovered = {
        str(mid): [e["id"] for e in entries if not covered[mid][e["id"]]]
        for mid, entries in cams.items()
    }
    return grouped, {
        "counts": dict(counts),
        "unsampled_cam_ids": uncovered,
        "complete_context_coverage": not any(uncovered.values())
        and not counts["cross_table_history_unavailable"]
        and not counts["invalid_snapshot"],
    }


def images(grouped, cams, output, route_container):
    output.mkdir(parents=True, exist_ok=True)
    reports, slots = {}, {
        label: {} for label in ("static-best", "uniform9", "uniform15")
    }
    for mid, entries in cams.items():
        sample = grouped[mid]
        cam = b.legacy.identity("hint-spec-cam")
        cam["entries"] = [
            {
                "id": i,
                "valid": i < len(entries),
                "mask": 0,
                "start_pc": entries[i]["start_pc"] if i < len(entries) else "0x0",
            }
            for i in range(16)
        ]
        best, ranking = b.legacy.rank_masks(sample, cam)
        reports[mid] = {"records": len(sample), "ranking": ranking, "images": {}}
        for label in slots:
            masks = (
                best
                if label == "static-best"
                else {e["id"]: int(label[7:]) for e in entries}
            )
            image = bytearray(b.legacy.build_image(sample, cam, masks))
            image[:144] = b.cam_image(entries, masks)
            image[:8] = b"H2PLD058"
            b.inspect_image(image, 2)
            data = bytes(image)
            path = output / f"{mid}-{label}.bin"
            path.write_bytes(data)
            slots[label][mid] = data
            reports[mid]["images"][label] = {
                "path": str(path.resolve()),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
    containers = {}
    for label, table in slots.items():
        data = b.make_container(
            2,
            route_container["boundaries"],
            route_container["routes"],
            table,
            route_container["initial_metadata_id"],
            route_container["metadata_valid_mask"],
        )
        path = output / f"bundle-{label}.bin"
        path.write_bytes(data)
        containers[label] = {
            "path": str(path.resolve()),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
    return {"tables": reports, "replay_containers": containers}


def contamination(joined):
    count = sum(x["approximate"] != x["exact"] for x in joined)
    by_branch = defaultdict(Counter)
    for x in joined:
        by_branch[hex(x["pc"])]["samples"] += 1
        by_branch[hex(x["pc"])]["misattributed"] += x["approximate"] != x["exact"]
    return {
        "samples": len(joined),
        "misattributed": count,
        "rate": count / len(joined) if joined else None,
        "by_branch": {k: dict(v) for k, v in by_branch.items()},
    }


def load_round(trace, audit, events, container_path, kind, error, capacity):
    container = b.validate_container(Path(container_path).read_bytes())
    b.need(
        container["kind"] == (0 if kind == "discovery" else 1),
        "round/container kind mismatch",
    )
    rows = list(b.decode_trace(trace, kind, error, capacity))
    # A capacity-stopped round may still carry a valid final marker. Determine
    # its physical index from the validated bytes, never from P_Error alone.
    final_index = None
    if Path(trace).stat().st_size:
        with Path(trace).open("rb") as stream:
            stream.seek(-22, 2)
            terminal = int.from_bytes(stream.read(22), "little")
        if terminal == 1 << sum(width for _, width in b.FIELDS[kind]):
            final_index = Path(trace).stat().st_size // 22 - 1
    return container, join_writes(
        rows, b.json_rows(audit), b.json_rows(events), container, kind, final_index
    )


def main():
    from spec_smoke import require_experiment_host

    require_experiment_host()
    parser = argparse.ArgumentParser(description=__doc__)
    for phase in ("discovery", "detail"):
        for field in ("trace", "audit", "events", "container"):
            parser.add_argument(f"--{phase}-{field}", required=True)
        parser.add_argument(f"--{phase}-p-error", type=int, default=0)
        parser.add_argument(f"--{phase}-capacity", type=int)
    parser.add_argument(
        "--snapshot-cams",
        required=True,
        help="CAM identities actually used for Detail profiling",
    )
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    rounds = {}
    for phase in ("discovery", "detail"):
        rounds[phase] = load_round(
            *(
                getattr(args, phase + "_" + f)
                for f in ("trace", "audit", "events", "container")
            ),
            phase,
            getattr(args, phase + "_p_error"),
            getattr(args, phase + "_capacity"),
        )
    discovery_container, discovery = rounds["discovery"]
    detail_container, detail = rounds["detail"]
    for key in ("boundaries", "routes", "initial_metadata_id", "metadata_valid_mask"):
        b.need(
            discovery_container[key] == detail_container[key],
            "Discovery/Detail route plan differs",
        )
    snapshot = json.loads(Path(args.snapshot_cams).read_text())
    cams, products, coverage = {}, {}, {}
    for owner in ("approximate", "exact"):
        cams[owner] = select(discovery, discovery_container, owner)
        grouped, coverage[owner] = semantic_rows(detail, cams[owner], snapshot, owner)
        products[owner] = images(
            grouped, cams[owner], args.output / owner, detail_container
        )
        b.write_json(args.output / owner / "cams.json", cams[owner])
    changes = {}
    for mid in cams["exact"]:
        a, e = cams["approximate"][mid], cams["exact"][mid]
        aa, ee = {identity(x) for x in a}, {identity(x) for x in e}
        changes[mid] = {
            "ordered_equal": a == e,
            "approximate_entries": a,
            "exact_entries": e,
            "removed": sorted(aa - ee),
            "added": sorted(ee - aa),
        }
    differences = {}
    for mid in cams["exact"]:
        differences[mid] = {}
        for label in ("static-best", "uniform9", "uniform15"):
            a = (args.output / "approximate" / f"{mid}-{label}.bin").read_bytes()
            e = (args.output / "exact" / f"{mid}-{label}.bin").read_bytes()
            differences[mid][label] = {
                "equal": a == e,
                "context_words_different": sum(
                    a[i : i + 4] != e[i : i + 4] for i in range(144, 16528, 4)
                ),
                "slot_words_different": sum(
                    a[i : i + 8] != e[i : i + 8] for i in range(16528, 16784, 8)
                ),
            }
    report = {
        "schema": "hint-bundle-observer-reference/v1",
        "discovery_contamination": contamination(discovery),
        "detail_contamination": contamination(detail),
        "top16_changes": changes,
        "coverage": coverage,
        "image_differences": differences,
        "products": products,
        "ipc_measured": False,
        "scope": "Exact logical ownership of observed writes. Cross-table LHR is not reconstructed; "
        "newly selected unsampled CAM branches require independent Detail profiling. "
        "Replay images are finite observed-sample references, not a complete counterfactual IPC result.",
        "inputs": {
            key: {"path": str(Path(value).resolve()), "sha256": b.sha(value)}
            for key, value in vars(args).items()
            if key.endswith(("_trace", "_audit", "_events", "_container"))
            or key == "snapshot_cams"
        },
    }
    b.write_json(args.output / "reference-report.json", report)


if __name__ == "__main__":
    main()
