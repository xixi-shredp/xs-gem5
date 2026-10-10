"""Offline fork contract tests; production C++ checks run in final gem5.opt."""

import copy
import gzip
import json
from pathlib import Path
import struct
import tempfile
import unittest

from . import context_v5_contract as contract
from .finite_v3 import load_candidates, read_profile


class ContextV5Test(unittest.TestCase):
    def metadata(self):
        return json.loads(Path(contract.__file__).with_name(
            "hint-context-v5-metadata.json").read_text())

    def test_frozen_vectors_and_image(self):
        contract.self_test()

    def test_full_width_fold(self):
        for bit in range(128):
            self.assertEqual(contract.fold128(1 << bit), 1 << (bit % 28))
        self.assertEqual(contract.context_components(0, 1 << 15, 0, 0)[1],
                         1 << 15)
        for value in (-1, 1 << 128, True):
            with self.assertRaises(ValueError):
                contract.fold128(value)

    def test_metadata_rejects_old_or_relabeled_contract(self):
        good = self.metadata()
        self.assertEqual(len(contract.pack_image(good)), 16784)
        mutations = [lambda x: x.update(version=3, hash_version=3),
                     lambda x: x.pop("context_encoding"),
                     lambda x: x.update(context_encoding="rtl-length-v4"),
                     lambda x: x["history"].update(ith_use_bits=64),
                     lambda x: x["history"].update(lhr_bits=16),
                     lambda x: x["history"].update(lhr_use_bits=8),
                     lambda x: x["history"].update(path_use_bits=True)]
        for mutate in mutations:
            bad = copy.deepcopy(good)
            mutate(bad)
            with self.assertRaises(ValueError):
                contract.pack_image(bad)

    def test_image_old_magic_and_corruption(self):
        image = contract.pack_image(self.metadata())
        for magic, version in [(b"H2PHINT3", 3), (b"H2PHINT4", 4),
                               (contract.MAGIC, 3)]:
            bad = bytearray(image)
            struct.pack_into("<8sI", bad, 0, magic, version)
            with self.assertRaises(ValueError):
                contract.unpack_image(bad)
        bad = bytearray(image)
        bad[-1] ^= 1
        with self.assertRaises(ValueError):
            contract.unpack_image(bad)

    def test_candidate_encoding_gate(self):
        data = json.loads(Path(contract.__file__).with_name(
            "hint-context-v5-candidates.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.json"
            path.write_text(json.dumps(data))
            self.assertEqual(load_candidates(path), [0x1000])
            for key, value in [("version", 3), ("context_encoding", "old")]:
                bad = dict(data)
                bad[key] = value
                path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError):
                    load_candidates(path)

    def test_profile_full_width_evidence_and_rejection(self):
        header = {"kind": "header", "format": "h2p-hint-profile",
                  "version": 5, "hash_version": 5, "storage": "finite",
                  "context_encoding": contract.CONTEXT_ENCODING,
                  "mask_order": contract.MASK_ORDER, "history": contract.HISTORY,
                  "layout": contract.LAYOUT, "clock_period": 1000,
                  "pcs": ["0x1000"]}
        history = {"ith_low128": hex(1 << 127), "call_low128": hex(1 << 64),
                   "ret_low128": "0x0", "lhr_low16": hex(1 << 15)}
        components = list(contract.context_components(1 << 127, 1 << 15,
                                                     1 << 64, 0))
        row = {"kind": "branch", "seq": 1, "commit_cycle": 3,
               "query_cycle": 1, "pc": "0x1000", "actual": "0x2000",
               "baseline_pc": "0x1000", "baseline_target": "0x3000",
               "block_start": "0x1000", "block_end": "0x1040",
               "eligible": True, "baseline_valid": True,
               "history": history, "contexts": components}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.gz"

            def write(h, r):
                with gzip.open(path, "wt") as stream:
                    stream.write(json.dumps(h) + "\n" + json.dumps(r) + "\n")

            write(header, row)
            self.assertEqual(read_profile(path)[1][0]["contexts"], components)
            write(dict(header, version=3, hash_version=3), row)
            with self.assertRaises(ValueError):
                read_profile(path)
            bad = dict(row, contexts=[0, 0, 0, 0])
            write(header, bad)
            with self.assertRaisesRegex(ValueError, "folded contexts differ"):
                read_profile(path)


if __name__ == "__main__":
    unittest.main()
