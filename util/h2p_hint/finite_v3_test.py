"""Focused cross-collector finite-v3 builder tests."""

import gzip
import json
from pathlib import Path
import tempfile
import unittest

from .finite_v3 import (build_image, choose_best, contract, merge_profiles,
                        read_profile, score_image)
from .audit import validate_replay_trace
from .model import empty_counters


class FiniteV3Test(unittest.TestCase):
    def source(self):
        return {"workload_id": "sample/0", "checkpoint_sha256": "0" * 64,
                "profile_sha256": "1" * 64, "builder_sha256": "2" * 64}

    def row(self, pc, seq=1, actual=0x2000, baseline=0x3000):
        return {"kind": "branch", "seq": seq, "pc": hex(pc), "actual": hex(actual),
                "baseline_valid": True, "baseline_pc": hex(pc),
                "baseline_target": hex(baseline), "query_cycle": seq,
                "commit_cycle": seq + 1, "block_start": hex(pc),
                "block_end": hex(pc + 32), "eligible": True,
                "contexts": [0, 0, 0, 0], "history": {"ith_low128": "0x0",
                    "call_low128": "0x0", "ret_low128": "0x0", "lhr_low16": "0x0"}}

    def profile(self, path, pcs, rows):
        header = {"kind": "header", "format": "h2p-hint-profile", "version": 5,
                  "storage": "finite", "context_encoding": contract.CONTEXT_ENCODING, "hash_version": 5,
                  "mask_order": contract.MASK_ORDER, "history": contract.HISTORY,
                  "layout": contract.LAYOUT, "clock_period": 500, "pcs": [hex(pc) for pc in pcs]}
        with gzip.open(path, "wt") as stream:
            for item in [header, *rows]:
                stream.write(json.dumps(item) + "\n")

    def test_disjoint_collect_groups_and_packed_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            first, second = Path(temporary) / "g0.gz", Path(temporary) / "g1.gz"
            pcs = [0x1000 + 0x100 * n for n in range(17)]
            self.profile(first, pcs[:16], [self.row(pcs[0])])
            self.profile(second, pcs[16:], [self.row(pcs[16])])
            _, rows = merge_profiles([first, second], pcs)
            self.assertEqual(len(rows), 2)  # seq may restart in each pass
            masks = {pcs[0]: 0, pcs[16]: 0}
            metadata, image, report = build_image(rows, masks, self.source())
            self.assertEqual(len(image), contract.IMAGE_BYTES)
            self.assertEqual(report["loaded_pcs"], 2)
            self.assertEqual(sum(item["corrected"] for item in score_image(rows, metadata)["pcs"]), 2)
            self.assertEqual(contract.pack_image(metadata), image)

    def test_mask_tie_and_positive_selection(self):
        scores = [(0x1000, 3, {"corrected": 2, "harmed": 1}),
                  (0x1000, 0, {"corrected": 1, "harmed": 0}),
                  (0x2000, 0, {"corrected": 0, "harmed": 1})]
        selected, report = choose_best(scores)
        self.assertEqual(selected, {0x1000: 0})
        self.assertEqual(report[-1]["reason"], "no_positive_gain")

    def test_reject_wrong_context_count(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.gz"
            bad = self.row(0x1000)
            bad["contexts"] = [0] * 5
            self.profile(path, [0x1000], [bad])
            with self.assertRaisesRegex(ValueError, "four folded contexts"):
                read_profile(path)

    def test_dynamic_replay_targets_reconcile_with_pc_counters(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "replay.gz"
            row = {"kind": "branch", "seq": 10, "pc": "0x1000",
                   "actual": "0x2000", "baseline_valid": True,
                   "baseline_pc": "0x1000", "baseline_target": "0x3000",
                   "final_valid": True, "final_target": "0x2000",
                   "eligible": True, "tag_hit": True, "override": True,
                   "hint_target": "0x2000", "confidence": 3,
                   "corrected": True, "harmed": False,
                   "query_cycle": 2, "commit_cycle": 20}
            with gzip.open(path, "wt") as stream:
                stream.write(json.dumps({"kind": "header", "format": "h2p-hint-replay",
                                         "version": 5, "storage": "finite",
                                         "hash_version": 5, "context_encoding": contract.CONTEXT_ENCODING}) + "\n")
                stream.write(json.dumps(row) + "\n")
            counts = empty_counters()
            counts.update(executions=1, baseline_misses=1, corrected=1,
                          eligible=1, tag_hits=1, conf_enabled=1, overrides=1)
            self.assertEqual(validate_replay_trace(path, {"pcs": [{"pc": "0x1000", **counts}]})
                             ["records"], 1)
            row["harmed"] = True
            with gzip.open(path, "wt") as stream:
                stream.write(json.dumps({"kind": "header", "format": "h2p-hint-replay",
                                         "version": 5, "storage": "finite",
                                         "hash_version": 5, "context_encoding": contract.CONTEXT_ENCODING}) + "\n")
                stream.write(json.dumps(row) + "\n")
            with self.assertRaisesRegex(ValueError, "correction/harm"):
                validate_replay_trace(path, {"pcs": [{"pc": "0x1000", **counts}]})


if __name__ == "__main__":
    unittest.main()
