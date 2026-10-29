"""Independent binary fixture checks; run directly from any directory."""
import copy
import importlib.util
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("spec_trace", HERE / "spec_trace.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
FIXTURE = HERE / "testdata" / "spec_trace"


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.cam = tool.read_cam(FIXTURE / "cam.json")

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, kind):
        return tool.read_trace(FIXTURE / f"{kind}.bin", FIXTURE / f"{kind}.json",
                               self.cam if kind == "detail" else None)

    def test_discovery_independent_bit_positions(self):
        _, rows = self.read("discovery")
        self.assertEqual([r["window_delta"] for r in rows], [0, 1, 15, 30, 31])
        self.assertTrue(all(r["start_pc"] == 0x1000 for r in rows))
        self.assertEqual(tool.cam_image(self.cam), (FIXTURE / "cam.bin").read_bytes())

    def test_encoding_range_is_distinct_from_window_semantics(self):
        meta, rows = tool.read_trace(FIXTURE / "encoding.bin", FIXTURE / "encoding.json")
        self.assertEqual([r["window_delta"] for r in rows], [-64, -1, 0, 31, 63])
        self.assertEqual(tool.trace_report(meta, rows)["invalid_window"], 2)
        with self.assertRaises(ValueError):
            tool.select_cam(rows, meta)
        _, detail = self.read("detail")
        detail[0]["window_delta"] = -2
        with self.assertRaises(ValueError):
            tool.build_image(detail, self.cam, {0: 0})
        detail[0]["query_valid"] = 0
        self.assertFalse(tool.valid_window(detail[0]))
        detail[0]["window_delta"] = -1
        self.assertTrue(tool.valid_window(detail[0]))
        tool.build_image(detail, self.cam, {0: 0})

    def test_cross_boundary_minus_one_and_page_end(self):
        meta, discovery = tool.read_trace(FIXTURE / "boundary-discovery.bin",
                                         FIXTURE / "boundary-discovery.json")
        self.assertEqual([row["start_pc"] for row in discovery], [0x1000, 0xff0])
        tool.select_cam(discovery, meta)
        meta, boundary = tool.read_trace(FIXTURE / "boundary-detail.bin",
                                        FIXTURE / "boundary-detail.json", self.cam)
        self.assertEqual([row["window_delta"] for row in boundary], [-1, -1])
        self.assertEqual([row["query_valid"] for row in boundary], [1, 0])
        tool.build_image(boundary, self.cam, {0: 0}, meta)
        _, rows = self.read("detail")
        for valid in (0, 1):
            row = dict(rows[0], query_valid=valid, window_delta=-1)
            self.assertTrue(tool.valid_window(row))
            self.assertEqual(tool.trace_report({"kind": "detail", "termination": "final"}, [row])["invalid_window"], 0)
            tool.build_image([row], self.cam, {0: 0})
        # Q=page-16 has a nominal 48-byte window but ends at the page boundary.
        for delta, expected in ((-1, True), (7, True), (8, False), (-2, False)):
            row = {"start_pc": 0xff0, "window_delta": delta}
            self.assertEqual(tool.valid_window(row), expected)
        self.assertFalse(tool.valid_window({"start_pc": 0xfffffffffffffffe, "window_delta": 0}))

    def test_hardware_memory_buffer_layout(self):
        for kind in ("discovery", "detail"):
            meta, rows = tool.read_trace(FIXTURE / f"{kind}-buffer.bin",
                                         FIXTURE / f"{kind}-buffer.json",
                                         self.cam if kind == "detail" else None)
            _, compact = self.read(kind)
            self.assertEqual(rows, compact)
            self.assertEqual(tool.trace_report(meta, rows)["layout"], "buffer")
        meta = json.loads((FIXTURE / "discovery.json").read_text())
        data = (FIXTURE / "discovery.bin").read_bytes()[:-7] + b"xx"
        meta.update(layout="buffer", buffer_bytes=len(data), capacity_records=5, termination="capacity")
        path, sidecar = self.root / "buffer.bin", self.root / "buffer.json"
        path.write_bytes(data)
        sidecar.write_text(json.dumps(meta))
        self.assertEqual(len(tool.read_trace(path, sidecar)[1]), 5)
        self.bad_trace(data, dict(meta, capacity_records=6))
        self.bad_trace(data, dict(meta, buffer_bytes=len(data) + 1))

    def test_sidecar_cannot_override_fixed_window_geometry(self):
        declared = dict(fetch_block_bytes=64, fetch_align_bytes=32, page_bytes=4096)
        for kind in ("discovery", "detail"):
            meta, rows = self.read(kind)
            meta.update(declared)
            path, sidecar = self.root / "fixed.bin", self.root / "fixed.json"
            data = (FIXTURE / f"{kind}.bin").read_bytes()
            path.write_bytes(data)
            sidecar.write_text(json.dumps(meta))
            self.assertEqual(tool.read_trace(path, sidecar, self.cam if kind == "detail" else None)[1], rows)
            for field, value in (("page_bytes", 8192), ("fetch_block_bytes", 128),
                                 ("fetch_align_bytes", 64), ("page_bytes", 2048),
                                 ("fetch_block_bytes", True)):
                self.bad_trace(data, dict(meta, **{field: value}), kind)
        # These rows would become legal if the forged geometry were honored.
        for row, forged in (({"start_pc": 0xff0, "window_delta": 8}, {"page_bytes": 8192}),
                            ({"start_pc": 0x1000, "window_delta": 32}, {"fetch_block_bytes": 128})):
            for query_valid in (0, 1):
                row = dict(row, query_valid=query_valid)
                self.assertFalse(tool.valid_window(row))
                with self.assertRaisesRegex(ValueError, "fixed SPEC value"):
                    tool.valid_window(row, forged)

    def test_hash_order_all_masks(self):
        _, rows = self.read("detail")
        row = rows[0]
        # Independent golden: startPC>>1=0x800 gives I=1, T=0x2800.
        base = 0x1400001
        self.assertEqual(tool.component(0x800), base)
        self.assertEqual(tool.component(1), 0x2801)
        self.assertEqual(tool.component(0x10000), 0x8002020)
        # Trace order is ITH,LHR,Call,Ret, whereas mask is ITH,Call,Ret,LHR.
        for mask in range(16):
            expected = base
            for bit, component in enumerate([1, 4, 8, 2]):
                if mask & (1 << bit):
                    expected ^= component
            self.assertEqual(tool.key_for(0x1000, row, mask), expected)

    def test_unordered_invalid_and_freeze(self):
        _, rows = self.read("detail")
        masks, report = tool.rank_masks(rows, self.cam)
        self.assertEqual(len(report[0]["masks"]), 16)
        image = tool.build_image(rows, self.cam, masks)
        random.Random(7).shuffle(rows)
        self.assertEqual(tool.build_image(rows, self.cam, masks), image)
        self.assertEqual(tool.build_image([r for r in rows if r["query_valid"]], self.cam, masks), image)
        info = tool.inspect_image(image, "load")
        self.assertEqual(info["bytes"], 16784)
        self.assertEqual(info["valid_cam"], 1)
        self.assertTrue(image[0x4090 + 5] & 0x80 == 0)  # no legacy Slot valid bit

    def bad_trace(self, data, meta=None, kind="discovery"):
        path, sidecar = self.root / "bad.bin", self.root / "bad.json"
        path.write_bytes(data)
        meta = copy.deepcopy(meta or json.loads((FIXTURE / f"{kind}.json").read_text()))
        meta.pop("data_sha256", None)
        sidecar.write_text(json.dumps(meta))
        with self.assertRaises(ValueError):
            tool.read_trace(path, sidecar, self.cam if kind == "detail" else None)

    def test_bad_wire_data(self):
        data = (FIXTURE / "discovery.bin").read_bytes()
        self.bad_trace(data[:-1])
        self.bad_trace(bytes([data[0] | 1]) + data[1:-7] + ((1 << 54) | 1).to_bytes(7, "little"))
        self.bad_trace((1 << 55).to_bytes(7, "little") + data[7:])
        self.bad_trace(data + bytes(7))
        self.bad_trace(data[:-7])
        meta = json.loads((FIXTURE / "detail.json").read_text())
        detail = bytearray((FIXTURE / "detail.bin").read_bytes())
        detail[0] = (detail[0] & 0xf0) | 15
        self.bad_trace(detail, meta, "detail")

    def test_versions_capacity_and_fault(self):
        data = (FIXTURE / "discovery.bin").read_bytes()
        meta = json.loads((FIXTURE / "discovery.json").read_text())
        for key, value in (("version", 0), ("record_bytes", 8), ("hash_version", 3),
                           ("mask_order", ["ITH", "LHR", "Call", "Ret"]),
                           ("stop_reason", "memory-fault")):
            changed = dict(meta, **{key: value})
            self.bad_trace(data, changed)
        capacity = dict(meta, termination="capacity", capacity_records=5)
        path, sidecar = self.root / "capacity.bin", self.root / "capacity.json"
        path.write_bytes(data[:-7])
        sidecar.write_text(json.dumps(capacity))
        self.assertEqual(len(tool.read_trace(path, sidecar)[1]), 5)
        self.bad_trace(data[:-14], capacity)
        detail_meta = json.loads((FIXTURE / "detail.json").read_text())
        detail_meta["cam_sha256"] = "0" * 64
        self.bad_trace((FIXTURE / "detail.bin").read_bytes(), detail_meta, "detail")

    def test_addresses_and_signed_contract(self):
        for pc in (0, 0x7ffffffffffe, 0xffff800000000000, 0xfffffffffffffffe):
            self.assertEqual(tool.address(tool.address_word(pc)), pc)
        for pc in (-2, 1, 0x800000000000, 1 << 64):
            with self.assertRaises(ValueError):
                tool.address_word(pc)

    def test_finite_capacity_and_score(self):
        groups = {}
        for i in range(40):
            # Distinct tags, same set: only two entries may be admitted.
            groups[i << 11] = {"targets": {i * 2: 3}, "baseline_correct": 0}
        selected, slots = tool.select_table(groups)
        self.assertEqual((len(selected), len(slots)), (2, 2))
        groups = {i: {"targets": {i * 2: 3}, "baseline_correct": 0} for i in range(40)}
        selected, slots = tool.select_table(groups)
        self.assertEqual((len(selected), len(slots)), (32, 32))
        _, rows = self.read("detail")
        self.assertEqual(tool.score(rows, {0: 0})["net_correction"], 2)

    def test_all_cli_from_unrelated_cwd(self):
        def run(*arguments):
            result = subprocess.run([sys.executable, str(HERE / "spec_trace.py"), *map(str, arguments)],
                                    cwd=self.root, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        run("validate", "--trace", FIXTURE / "discovery.bin", "--meta", FIXTURE / "discovery.json")
        run("select-cam", "--trace", FIXTURE / "discovery.bin", "--meta", FIXTURE / "discovery.json",
            "--image", self.root / "cam.bin", "--output", self.root / "cam.json")
        common = ["--trace", FIXTURE / "detail.bin", "--meta", FIXTURE / "detail.json",
                  "--cam", self.root / "cam.json"]
        run("rank-masks", *common, "--output", self.root / "rank.json")
        run("build", *common, "--image", self.root / "load.bin", "--output", self.root / "load.json")
        run("inspect", "--kind", "load", "--image", self.root / "load.bin", "--meta", self.root / "load.json")
        run("inspect", "--kind", "cam", "--image", self.root / "cam.bin", "--meta", self.root / "cam.json")

    def test_manifest_compatibility_entry(self):
        manifest = HERE.parents[2] / "manifest" / "hint_v3.py"
        for arguments in (["self-test"], ["validate", "--trace", FIXTURE / "discovery.bin",
                                           "--meta", FIXTURE / "discovery.json"]):
            result = subprocess.run([sys.executable, str(manifest), *map(str, arguments)],
                                    cwd=self.root, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        metadata = {"version": 7, "hash_version": 7, "mask_order": tool.MASK_ORDER,
                    "cam": [{"pc": "0x1000", "mask": 9}], "slots": ["0x0"],
                    "entries": [{"set": 0, "way": 0, "tag": 1, "slot": 0, "conf": 3}]}
        data = tool.pack_image(metadata)
        self.assertEqual(tool.unpack_image(data)[2][0], 0)
        metadata["version"] = 3
        with self.assertRaises(ValueError):
            tool.pack_image(metadata)


if __name__ == "__main__":
    unittest.main()
