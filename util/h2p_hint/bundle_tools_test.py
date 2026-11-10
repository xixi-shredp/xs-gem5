"""Directed negative/positive Bundle host contracts; run only on ps1/ps2."""
import tempfile
from pathlib import Path
import unittest
import bundle_tools as b
import bundle_guest as guest


def record(kind, **fields):
    value = fields.pop("is_boundary", False)
    if value:
        word = (
            1
            | (b.address_word(fields["pc"]) << 1)
            | (b.address_word(fields["target"]) << 48)
        )
    else:
        word = 0
        offset = 1
        for key, width in b.FIELDS[kind]:
            word |= fields.get(key, 0) << offset
            offset += width
    return word.to_bytes(22, "little")


class BundleContracts(unittest.TestCase):
    def parse(self, data, kind="discovery", **kwargs):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "trace"
            path.write_bytes(data)
            return list(b.decode_trace(path, kind, **kwargs))

    def test_hash_order_and_canonical_high(self):
        self.assertEqual(b.route_hash(2, 0), 1 << 23)
        self.assertEqual(b.route_hash(0, 2), 1)
        with self.assertRaises(ValueError):
            b.route_hash(3, 2)
        with self.assertRaises(ValueError):
            b.address_word(1 << 48)

    def test_trace_layout_final_and_errors(self):
        raw = record("discovery", start_pc=0x1000 >> 1, position=3, is_call=1)
        end = record("discovery", final=1)
        row = self.parse(raw + end)[0]
        self.assertEqual(
            (row["start_pc"], row["position"], row["is_call"]), (0x1000, 3, True)
        )
        for data, kwargs in (
            (raw, {}),
            (end + raw, {}),
            (end, {"p_error": 3}),
            (raw + end[:-1], {}),
            ((1 << 175).to_bytes(22, "little") + end, {}),
        ):
            with self.assertRaises(ValueError):
                self.parse(data, **kwargs)
        self.assertEqual(len(self.parse(raw, p_error=2, capacity_bytes=22)), 1)
        with self.assertRaises(ValueError):
            self.parse(raw, p_error=2, capacity_bytes=44)

    def test_container_reference_padding_and_kind(self):
        image = b.cam_image(
            [{"id": 0, "start_pc": "0x1000", "position": 2, "is_call": True}]
        )
        routes = [{"index": 0, "way": 0, "tag": 0, "metadata_id": 0}]
        data = b.make_container(1, [0x1000], routes, {0: image}, 0)
        self.assertEqual(b.validate_container(data)["initial_metadata_id"], 0)
        bad = bytearray(data)
        bad[0x910] = 1
        with self.assertRaises(ValueError):
            b.validate_container(bad)
        with self.assertRaises(ValueError):
            b.make_container(2, [0x1000], routes, {0: image}, 0)
        with self.assertRaises(ValueError):
            b.make_container(0, [0x1000], routes, {}, 0, 0)
        conflict = routes + [{"index": 0, "way": 1, "tag": 0, "metadata_id": 1}]
        with self.assertRaises(ValueError):
            b.make_container(0, [0x1000], conflict, {}, 0, 3)

    def test_boundary_order_and_minimum_position_selection(self):
        h = b.route_hash(0x2000, 0x3000)
        container = b.validate_container(
            b.make_container(
                0,
                [0x2000],
                [{"index": h & 255, "way": 0, "tag": h >> 8, "metadata_id": 1}],
                {},
                0,
                3,
            )
        )
        rows = [
            {
                "record_index": 0,
                "is_boundary": False,
                "start_pc": 0x1000,
                "position": 5,
                "is_call": False,
            }
        ] * 10
        rows += [
            {
                "record_index": 1,
                "is_boundary": False,
                "start_pc": 0x1000,
                "position": 2,
                "is_call": True,
            },
            {"record_index": 2, "is_boundary": True, "pc": 0x2000, "target": 0x3000},
            {
                "record_index": 3,
                "is_boundary": False,
                "start_pc": 0x4000,
                "position": 0,
                "is_call": False,
            },
        ]
        selected = b.select_cams(iter(rows), container)
        self.assertEqual(selected[0][0]["position"], 2)
        self.assertEqual(selected[0][0]["misses"], 1)
        self.assertEqual(selected[1][0]["start_pc"], "0x4000")

    def test_static_footprint_and_event_length(self):
        events = [
            {
                "seq": 1,
                "pc": "0x1000",
                "size": 4,
                "target": "0x2000",
                "call": True,
                "return": False,
                "indirect": False,
                "taken": True,
            }
        ]
        plan = b.plan_routes(events, {0x1000: 4, 0x2000: 2, 0x1000 + 819200: 4}, 0x1000)
        self.assertEqual(len(plan["regions"]), 2)
        self.assertTrue(all(r["end"] - r["start"] <= 819200 for r in plan["regions"]))
        with self.assertRaises(ValueError):
            b.plan_routes(events, {0x1000: 2, 0x2000: 2})

    def test_real_guest_csr_and_overlap(self):
        code = guest.assembly(0x90000000, 0x90400000, 2, 1, "detail")
        for text in (
            "csrw 0x5c2",
            "csrw 0x5c1",
            "csrw 0x5c0",
            "fence rw,rw",
            "csrr t0, 0x5c0",
        ):
            self.assertIn(text, code)
        with self.assertRaises(ValueError):
            guest.assembly(0x90000000, 0x90100000, 2, 1, "detail")


if __name__ == "__main__":
    unittest.main()
