"""Host regressions for conservative zero-gain Context fill (no guest required).

Run from util with: python3 -m unittest h2p_hint.stable_fill_test -v
Fixtures construct real 28-bit hash keys and exercise the packed finite image.
"""
import copy
import unittest

from . import finite_v3 as finite
from . import context_v5_contract as contract

PC = 0x1000
OTHER_PC = 0x2000
TARGET = 0x8000
SOURCE = {"workload_id": "stable-fill-regression",
          "checkpoint_sha256": "1" * 64,
          "profile_sha256": "2" * 64,
          "builder_sha256": "3" * 64}


def observation(key, *, pc=PC, target=TARGET, correct=True, eligible=True):
    # Mask 1 selects the first folded component; solve its XOR for the desired
    # physical key instead of mocking the hash or table selector.
    component = key ^ contract.hash_key(pc, (0, 0, 0, 0), 0)
    return {"query_pc": pc, "pc": pc, "block_start": pc,
            "contexts": [component, 0, 0, 0], "query_valid": True,
            "eligible": eligible, "baseline_valid": True,
            "baseline_target": target if correct else target + 2,
            "actual": target}


def make(rows, masks=None, **kwargs):
    return finite.build_image(rows, {PC: 1} if masks is None else masks,
                              copy.deepcopy(SOURCE), **kwargs)


def keyed(metadata):
    return {(entry["tag"] << 11) | entry["set"]: entry
            for entry in metadata["entries"]}


class StableFillTest(unittest.TestCase):
    def test_default_compatibility_and_input_immutability(self):
        rows = [observation(4096, correct=False), observation(0)]
        original = copy.deepcopy(rows)
        default = make(rows)
        explicit = make(rows, stable_fill=False)
        self.assertEqual(default, explicit)
        self.assertEqual(rows, original)
        self.assertEqual(set(keyed(default[0])), {4096})

    def test_fill_preserves_physical_positive_way_slot_and_cam(self):
        # Lower numeric key must not reorder the old positive entry's way.
        rows = [observation(4096, correct=False), observation(0)]
        base, _, _ = make(rows)
        filled, image, _ = make(rows, stable_fill=True)
        self.assertEqual(keyed(filled)[4096], keyed(base)[4096])
        self.assertEqual(keyed(filled)[0]["way"], 1)
        self.assertEqual(filled["cam"], base["cam"])
        self.assertEqual(filled["slots"], base["slots"])
        self.assertEqual(image, contract.pack_image(filled))
        self.assertEqual(keyed(filled)[0]["conf"], 3)
        scores = finite.score_image(rows, filled)["pcs"][0]
        self.assertEqual(scores["harmed"], 0)
        self.assertEqual(scores["corrected"], 1)

    def test_full_set_cannot_evict_positive_keys(self):
        rows = [observation(0, correct=False), observation(2048, correct=False),
                observation(4096)]
        base = make(rows)[0]
        filled = make(rows, stable_fill=True)[0]
        self.assertEqual(base, filled)

    def test_impure_target_key_is_rejected_even_with_zero_gain(self):
        rows = [observation(1, correct=False), observation(2),
                observation(2, target=TARGET + 2)]
        filled = make(rows, stable_fill=True)[0]
        self.assertNotIn(2, keyed(filled))

    def test_new_slot_is_rejected(self):
        rows = [observation(1, correct=False),
                observation(2, target=TARGET + 2)]
        filled = make(rows, stable_fill=True)[0]
        self.assertEqual(filled["slots"], [hex(TARGET)])
        self.assertNotIn(2, keyed(filled))

    def test_no_positive_mapping_does_not_activate_cam(self):
        rows = [observation(2)]
        filled = make(rows, stable_fill=True)[0]
        self.assertEqual(filled["cam"], [])
        self.assertEqual(filled["entries"], [])
        self.assertEqual(filled["slots"], [])

    def test_inactive_pc_cannot_contribute_stable_fill(self):
        rows = [observation(1, correct=False), observation(2, pc=OTHER_PC)]
        filled = make(rows, {PC: 1, OTHER_PC: 1}, stable_fill=True)[0]
        self.assertEqual(filled["cam"], [{"pc": hex(PC), "mask": 1}])
        self.assertNotIn(2, keyed(filled))

    def test_cross_pc_alias_with_inactive_owner_is_rejected(self):
        rows = [observation(1, correct=False), observation(2),
                observation(2, pc=OTHER_PC)]
        filled = make(rows, {PC: 1, OTHER_PC: 1}, stable_fill=True)[0]
        self.assertNotIn(2, keyed(filled))
        self.assertEqual(len(filled["cam"]), 1)

    def test_cross_pc_alias_with_active_owners_same_target_is_safe(self):
        rows = [observation(1, correct=False),
                observation(3, pc=OTHER_PC, correct=False),
                observation(2), observation(2, pc=OTHER_PC)]
        filled = make(rows, {PC: 1, OTHER_PC: 1}, stable_fill=True)[0]
        self.assertIn(2, keyed(filled))
        self.assertEqual(len(filled["cam"]), 2)

    def test_observations_priority_then_key_and_deterministic_order(self):
        # One way remains in set 0: key 4096 beats key 2048 by support.
        rows = [observation(0, correct=False), observation(2048)]
        rows += [observation(4096)] * 3
        filled = make(rows, stable_fill=True)
        reversed_result = make(list(reversed(rows)), stable_fill=True)
        self.assertEqual(filled, reversed_result)
        self.assertIn(4096, keyed(filled[0]))
        self.assertNotIn(2048, keyed(filled[0]))
        tied = make([observation(0, correct=False), observation(2048),
                     observation(4096)], stable_fill=True)[0]
        self.assertIn(2048, keyed(tied))
        self.assertNotIn(4096, keyed(tied))

    def test_min_observations_and_ineligible_support(self):
        rows = [observation(1, correct=False), observation(2),
                observation(2, eligible=False)]
        filled = make(rows, stable_fill=True, stable_fill_min_observations=2)[0]
        self.assertNotIn(2, keyed(filled))
        filled = make(rows + [observation(2)], stable_fill=True,
                      stable_fill_min_observations=2)[0]
        self.assertIn(2, keyed(filled))

    def test_positive_gain_cannot_be_reintroduced_as_fill(self):
        # A positive-gain key excluded by the 32-target limit may not bypass
        # that decision, nor may fill manufacture a target slot.
        rows = [observation(index, target=TARGET + index * 2, correct=False)
                for index in range(33)]
        base = make(rows)[0]
        filled = make(rows, stable_fill=True)[0]
        self.assertEqual(base, filled)
        self.assertEqual(len(filled["slots"]), 32)

    def test_capacity_and_packed_image_validation(self):
        # Every set has two positive ways; no global or per-set over-allocation.
        rows = [observation(key, correct=False) for key in range(4096)]
        rows += [observation(4096)]
        filled, image, _ = make(rows, stable_fill=True)
        self.assertEqual(len(filled["entries"]), 4096)
        self.assertNotIn(4096, keyed(filled))
        self.assertEqual(image, contract.pack_image(filled))
        self.assertEqual(len({(x["set"], x["way"]) for x in filled["entries"]}), 4096)


if __name__ == "__main__":
    unittest.main()
