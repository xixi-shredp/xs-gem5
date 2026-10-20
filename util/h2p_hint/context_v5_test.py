"""Spec v7 contract regression tests; C++ checks use the final production binary."""
import copy
import gzip
import json
from pathlib import Path
import tempfile
import unittest
from . import context_v5_contract as c
from .finite_v3 import read_profile


class ContextV6Test(unittest.TestCase):
    def metadata(self):
        return json.loads(Path(c.__file__).with_name('hint-context-v7-metadata.json').read_text())

    def test_independent_oracle_vectors(self):
        c.self_test()

    def test_mask_and_exact_cam(self):
        self.assertEqual(c.MASK_ORDER, ['ITH', 'Call', 'Ret', 'LHR'])
        self.assertIsNone(c.select_cam_pc(0x1000, [0x1002]))
        self.assertEqual(c.select_cam_pc(0x1000, [0x1000, 0x1002]), 0x1000)
        self.assertNotEqual(c.context_components(0, 0x8000, 0, 0)[3], 0)
        with self.assertRaises(ValueError): c.context_components(0, 0x10000, 0, 0)

    def test_strict_contract_and_header(self):
        metadata = self.metadata()
        image = c.pack_image(metadata)
        self.assertEqual(len(image), 16784)
        self.assertEqual(image[:16], b'H2PHINTL'+bytes(8))
        c.unpack_image(image)
        for magic in (b'H2PHCF05', b'H2PHINT3'):
            with self.assertRaises(ValueError): c.unpack_image(magic+image[8:])
        bad = bytearray(image); bad[8] = 1
        with self.assertRaises(ValueError): c.unpack_image(bad)
        for key, value in [('version',5), ('version',6), ('hash_version',5), ('hash_version',6), ('context_encoding','old')]:
            bad = copy.deepcopy(metadata); bad[key] = value
            with self.assertRaises(ValueError): c.pack_image(bad)
        # Payload has no CRC per Spec; structural corruption is still rejected.
        bad = bytearray(image); bad[16+7] = 0x80
        with self.assertRaises(ValueError): c.unpack_image(bad)

    def test_lhr_contract_rejects_old_physical_read_head_and_limit(self):
        for field, value in [('lhr_bits',16), ('lhr_use_bits',8), ('lhr_head_bits',4)]:
            bad = self.metadata(); bad['history'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                c.pack_image(bad)
        bad = self.metadata(); del bad['history']['lhr_head_bits']
        with self.assertRaises(ValueError): c.pack_image(bad)
        bad = self.metadata(); bad['layout']['lhr_max_uncommitted'] = 4
        with self.assertRaises(ValueError): c.pack_image(bad)

    def test_all_sixteen_lhr_bits_and_mask_independence(self):
        base = c.context_components(0, 0, 0, 0)
        for bit in range(16):
            components = c.context_components(0, 1 << bit, 0, 0)
            self.assertEqual(components[:3], base[:3])
            self.assertNotEqual(c.hash_key(0x1000, components, 8), c.hash_key(0x1000, base, 8))
            self.assertEqual(c.hash_key(0x1000, components, 7), c.hash_key(0x1000, base, 7))
