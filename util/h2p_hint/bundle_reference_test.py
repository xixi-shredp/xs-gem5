"""Independent physical-write/CFI identity contracts; execute only on ps1/ps2."""
import tempfile
import json
from pathlib import Path
import unittest
import bundle_tools as b
import bundle_reference as r


class ObserverReference(unittest.TestCase):
    def container(self):
        h = b.route_hash(0x2000, 0x3000)
        return b.validate_container(
            b.make_container(
                0,
                [0x2000],
                [{"index": h & 255, "way": 0, "tag": h >> 8, "metadata_id": 1}],
                {},
                0,
                3,
            )
        )

    def test_physical_write_join_ignores_accept_and_reordered_reservation(self):
        rows = [
            {"record_index": 0, "is_boundary": True, "pc": 0x2000, "target": 0x3000},
            {
                "record_index": 1,
                "is_boundary": False,
                "start_pc": 0x1000,
                "position": 2,
                "is_call": False,
            },
        ]
        audits = [
            {"event": "accept", "record_index": 17},
            {
                "event": "write",
                "record_index": 0,
                "reservation_index": 9,
                "seq": 8,
                "pc": 0x2000,
                "is_boundary": True,
            },
            {"event": "overwrite", "record_index": 1},
            {
                "event": "write",
                "record_index": 1,
                "reservation_index": 2,
                "seq": 7,
                "pc": 0x1002,
                "is_boundary": False,
                "metadata_id": 0,
            },
        ]
        events = [
            {"seq": 7, "pc": 0x1002, "size": 4, "call": False},
            {"seq": 8, "pc": 0x2000, "size": 2, "target": 0x3000},
        ]
        joined = r.join_writes(rows, audits, events, self.container(), "discovery")
        self.assertEqual((joined[0]["approximate"], joined[0]["exact"]), (1, 0))
        self.assertEqual(r.contamination(joined)["rate"], 1)
        with self.assertRaises(ValueError):
            r.join_writes(
                rows, audits + [audits[-1]], events, self.container(), "discovery"
            )
        events[0]["size"] = 2
        with self.assertRaises(ValueError):
            r.join_writes(rows, audits, events, self.container(), "discovery")

    def test_detail_rekeys_by_cfi_not_new_cam_id(self):
        old = [{"id": 0, "start_pc": "0x1000", "position": 2, "is_call": False}]
        destination = [{"id": 1, "start_pc": "0x1000", "position": 2, "is_call": False}]
        sample = {
            "exact": 0,
            "pc": 0x1002,
            "size": 4,
            "event": {"call": False},
            "audit": {"snapshot_metadata_id": 0},
            "row": {
                "pc_id": 0,
                "query_valid": 1,
                "ITH": 3,
                "LHR": 5,
                "Call": 7,
                "Ret": 11,
                "target": 0x2000,
                "baseline_miss": 1,
            },
        }
        rows, report = r.semantic_rows([sample], {0: destination}, {"0": old}, "exact")
        self.assertEqual(rows[0][0]["pc_id"], 1)
        self.assertEqual(rows[0][0]["LHR"], 5)
        self.assertTrue(report["complete_context_coverage"])
        wrong = [{"id": 0, "start_pc": "0x1000", "position": 3, "is_call": False}]
        with self.assertRaises(ValueError):
            r.semantic_rows([sample], {0: destination}, {"0": wrong}, "exact")

    def test_cross_table_history_and_missing_new_cam_are_uncovered(self):
        cam = [{"id": 0, "start_pc": "0x1000", "position": 2, "is_call": False}]
        sample = {
            "exact": 1,
            "pc": 0x1002,
            "size": 4,
            "event": {"call": False},
            "audit": {"snapshot_metadata_id": 0},
            "row": {"pc_id": 0, "query_valid": 1},
        }
        rows, report = r.semantic_rows([sample], {1: cam}, {0: cam}, "exact")
        self.assertFalse(rows)
        self.assertEqual(report["counts"]["cross_table_history_unavailable"], 1)
        self.assertEqual(report["unsampled_cam_ids"], {"1": [0]})
        self.assertFalse(report["complete_context_coverage"])
        sample["row"]["query_valid"] = 0
        rows, report = r.semantic_rows([sample], {1: cam}, {}, "exact")
        self.assertEqual(report["counts"]["invalid_snapshot"], 1)
        self.assertFalse(report["complete_context_coverage"])

    def test_replay_images_finite_and_valid(self):
        container = self.container()
        cam = [{"id": 0, "start_pc": "0x1000", "position": 2, "is_call": False}]
        sample = {
            "pc_id": 0,
            "query_valid": 1,
            "ITH": 3,
            "LHR": 5,
            "Call": 7,
            "Ret": 11,
            "target": 0x2000,
            "baseline_miss": 1,
            "start_pc": 0x1000,
            "window_delta": 0,
        }
        with tempfile.TemporaryDirectory() as directory:
            reports = r.images(
                {0: [sample], 1: []}, {0: cam, 1: []}, Path(directory), container
            )
            for item in reports["replay_containers"].values():
                self.assertEqual(
                    b.validate_container(Path(item["path"]).read_bytes())["kind"], 2
                )

    def test_capacity_round_with_final_write_is_joined(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "container").write_bytes(b.make_container(0, [], [], {}, 0, 1))
            (root / "trace").write_bytes((1 << 54).to_bytes(22, "little"))
            (root / "audit").write_text(
                json.dumps(
                    {
                        "event": "write",
                        "record_index": 0,
                        "seq": 0,
                        "pc": 0,
                        "is_boundary": False,
                    }
                )
                + "\n"
            )
            (root / "events").write_text("")
            _, joined = r.load_round(
                root / "trace",
                root / "audit",
                root / "events",
                root / "container",
                "discovery",
                2,
                22,
            )
            self.assertEqual(joined, [])


if __name__ == "__main__":
    unittest.main()
