#!/usr/bin/env python3
"""CAM58 adapter checks. Run only on ps1/ps2, like all project tests."""
from collections import Counter
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from util.h2p_hint import cam58, spec_trace as public
from util.h2p_hint.spec_smoke import require_experiment_host


class Cam58Test(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def capture(self, kind, rows, cam=None, misses=None):
        width, final = (7, 54) if kind == "discovery" else (22, 172)
        trace = self.root / (kind + ".bin")
        raw, audit = [], [{"kind": "header", "version": 1, "producer": "gem5-retired-cfi",
                           "cam_identity": "checked_pc_width_classification", "record_bytes": width}]
        for i, source in enumerate(rows):
            row = {"kind": "record", "index": i, "retired_ordinal": i + 1,
                   "is_rvc": False, "is_call": False, "is_return": False,
                   "is_indirect": True, "baseline_miss": True, "query_valid": True, **source}
            q, pc = int(row["start_pc"], 16), int(row["pc"], 16)
            delta = ((pc - q) // 2) & 127
            if kind == "discovery":
                value = public.address_word(pc) | delta << 47
            else:
                value = row.pop("pc_id", 0) | public.address_word(int(row["target"], 16)) << 4
                value |= delta << 163 | int(row["baseline_miss"]) << 170 | int(row["query_valid"]) << 171
            raw.append(value.to_bytes(width, "little"))
            audit.append(row)
            if kind == "discovery":
                audit.append({**row, "kind": "window_miss"})
        audit.extend(misses or [])
        audit.append({"kind": "footer", "complete": True, "records": len(rows), "overflow": False, "p_error": 0})
        trace.write_bytes(b"".join(raw) + (1 << final).to_bytes(width, "little"))
        audit_path = Path(str(trace) + ".audit.jsonl")
        audit_path.write_text("".join(json.dumps(row) + "\n" for row in audit))
        meta = {**public.identity("hint-spec-trace"), "kind": kind, "record_bytes": width,
                "termination": "final", "capacity_records": len(rows) + 1,
                "data_sha256": cam58.sha(trace), "audit_sha256": cam58.sha(audit_path)}
        if cam:
            meta["cam_sha256"] = public.load_json(cam)["image_sha256"]
        meta_path = trace.with_suffix(".json")
        cam58.write(meta_path, meta)
        return trace, meta_path

    def selection(self, rows):
        trace, meta = self.capture("discovery", rows)
        cam, intermediate, selection, image = [self.root / name for name in
                                             ("cam.json", "public.bin", "selection.json", "cam.bin")]
        cam58.select_cam(trace, meta, cam, intermediate, selection, image)
        return cam, selection, image

    def test_true_window_total_ranking_and_fixed_cfi_tie(self):
        totals = Counter({0x1000 + i * 64: 50 - i for i in range(17)})
        rows = [{"q": q, "pc": q + offset, "position": offset // 2 + 1,
                 "window_totals": totals, "row": {"is_rvc": False, "is_call": False, "is_return": False}}
                for q in totals for offset in (4, 0)]
        chosen = cam58.choices(rows)
        self.assertEqual([int(row["start_pc"], 16) for row in chosen], list(totals)[:16])
        self.assertTrue(all(row["pc"] == row["start_pc"] for row in chosen))
        # The highest-miss window contains only return: do not backfill window 17.
        for row in rows:
            if row["q"] == 0x1000:
                row["row"]["is_return"] = True
        self.assertEqual(len(cam58.choices(rows)), 15)

    def test_boundary_identity_and_digest_rejection(self):
        trace, meta = self.capture("discovery", [{"start_pc": "0x1020", "pc": "0x101e"}])
        self.assertEqual(cam58.checked_rows(trace, meta, "discovery")[0]["position"], 0)
        audit = Path(str(trace) + ".audit.jsonl")
        audit.write_text(audit.read_text().replace('"is_rvc": false', '"is_rvc": true'))
        with self.assertRaisesRegex(ValueError, "digest"):
            cam58.checked_rows(trace, meta, "discovery")
        metadata = public.load_json(meta)
        metadata["audit_sha256"] = cam58.sha(audit)
        cam58.write(meta, metadata)
        with self.assertRaisesRegex(ValueError, "outside|window-miss sample"):
            cam58.checked_rows(trace, meta, "discovery")

    def test_filter_before_build_and_context_slot_preservation(self):
        cam, selection, image = self.selection([
            {"start_pc": "0x1000", "pc": "0x1000", "is_call": True},
            {"start_pc": "0x1000", "pc": "0x1004"}])
        self.assertEqual(image.read_bytes()[:8], b"H2PCAM58")
        word = struct.unpack_from("<Q", image.read_bytes(), 16)[0]
        self.assertEqual((word >> 52) & 31, 1)
        self.assertEqual((word >> 57) & 1, 1)
        trace, meta = self.capture("detail", [
            {"start_pc": "0x1000", "pc": "0x1000", "target": "0x2000", "is_call": True},
            {"start_pc": "0x1000", "pc": "0x1004", "target": "0x3000"}], cam)
        filtered, filtered_meta = self.root / "filtered.bin", self.root / "filtered.json"
        cam58.filter_detail(trace, meta, cam, selection, filtered, filtered_meta)
        mapping = public.read_cam(cam)
        metadata, rows = public.read_trace(filtered, filtered_meta, mapping)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["target"], 0x2000)
        public_image, final = self.root / "load.public.bin", self.root / "load.bin"
        public_image.write_bytes(public.build_image(rows, mapping, {0: 0}, metadata))
        cam58.adapt_image(public_image, cam, selection, final, "load")
        self.assertEqual(final.read_bytes()[:8], b"H2PLD058")
        self.assertEqual(final.read_bytes()[144:], public_image.read_bytes()[144:])
        self.assertEqual(len(cam58.checked_rows(filtered, filtered_meta, "detail", mapping)), 1)

    def test_explicit_index_join_accepts_reordering_rejects_conflicting_miss(self):
        trace, meta = self.capture("discovery", [
            {"start_pc": "0x1000", "pc": "0x1000"},
            {"start_pc": "0x1000", "pc": "0x1004"}])
        audit = Path(str(trace) + ".audit.jsonl")
        rows = [json.loads(line) for line in audit.read_text().splitlines()]
        rows[1:-1] = reversed(rows[1:-1])

        def save():
            audit.write_text("".join(json.dumps(row) + "\n" for row in rows))
            metadata = public.load_json(meta)
            metadata["audit_sha256"] = cam58.sha(audit)
            cam58.write(meta, metadata)

        save()
        self.assertEqual([item["pc"] for item in cam58.checked_rows(trace, meta, "discovery")],
                         [0x1000, 0x1004])
        next(row for row in rows if row.get("kind") == "window_miss")["is_call"] = True
        save()
        with self.assertRaisesRegex(ValueError, "identity disagree"):
            cam58.checked_rows(trace, meta, "discovery")


if __name__ == "__main__":
    require_experiment_host()
    unittest.main()
