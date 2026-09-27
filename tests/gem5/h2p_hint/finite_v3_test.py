"""Finite v3 loader checks against the shared manifest vectors."""

import json
from pathlib import Path
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from util.h2p_hint import finite_v3  # noqa: E402


class FiniteV3LoaderTest(unittest.TestCase):
    def test_shared_image_and_legacy_rejection(self):
        source = {key: "0" * 64 for key in (
            "checkpoint_sha256", "profile_sha256", "builder_sha256")}
        source["workload_id"] = "hint-self-test"
        metadata = {
            "format": "h2p-hint",
            "version": 3,
            "storage": "finite",
            "hash_version": finite_v3.contract.HASH_VERSION,
            "mask_order": finite_v3.contract.MASK_ORDER,
            "history": finite_v3.contract.HISTORY,
            "layout": finite_v3.contract.LAYOUT,
            "cam": [{"pc": "0x1000", "mask": 15}],
            "slots": ["0x2000"],
            "entries": [{"set": 0, "way": 0, "tag": 1,
                         "slot": 0, "conf": 3}],
            "source": source,
        }
        expected = json.loads(
            (finite_v3._CONTRACT_PATH.parent / "hint-v3-vectors.json")
            .read_text()
        )["sample_image"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "metadata.json"
            path.write_text(json.dumps(metadata))
            self.assertEqual(
                len(finite_v3.packed_image(path)), expected["length"]
            )
            import hashlib

            self.assertEqual(
                hashlib.sha256(finite_v3.packed_image(path)).hexdigest(),
                expected["sha256"],
            )
            del metadata["storage"]
            path.write_text(json.dumps(metadata))
            with self.assertRaises(ValueError):
                finite_v3.load_metadata(path)

    def test_candidate_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.json"
            path.write_text(json.dumps({
                "format": "h2p-hint-candidates", "version": 3,
                "storage": "finite", "source": {},
                "pcs": ["0x1000", "0x1002"],
            }))
            self.assertEqual(finite_v3.load_candidates(path),
                             [0x1000, 0x1002])
            bad = json.loads(path.read_text())
            bad["pcs"] = ["0x1000"] * 17
            path.write_text(json.dumps(bad))
            with self.assertRaises(ValueError):
                finite_v3.load_candidates(path)


if __name__ == "__main__":
    unittest.main()
