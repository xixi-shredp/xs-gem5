#!/usr/bin/env python3
"""Capacity, compatibility and persistence contracts for unlimited Hint tools."""

import argparse
import copy
import gzip
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "util"))
sys.path.insert(0, str(ROOT / "configs"))
from common.H2PHint import (
    add_h2p_hint_options,
    h2p_hint_params,
    validate_h2p_hint_config,
)
from h2p_hint.audit import (
    compare_observer,
    validate_hint_stats,
    validate_occupancy,
)
from h2p_hint.ideal_audit import (
    audit_conf0,
    audit_source_files,
    reproduce_unlimited,
)
from h2p_hint.builder import build_image, candidates_from_stage1, select_joint
from h2p_hint.ideal_runner import (
    FORMAL_RUNS,
    FORMAL_WINDOWS,
    build_command,
    candidate_document,
    candidates_path,
    mask_jobs,
)
from h2p_hint.model import (
    History,
    LAYOUT,
    Reference,
    context_key,
    contract,
    load_candidates,
    replay_reference,
    validate_metadata,
    write_json,
)
from h2p_hint.profile import Cache, compare_shared_profiles, make_cache
from h2p_hint.runner import HEAVY

spec = importlib.util.spec_from_file_location(
    "finite_hint_tools_tests", Path(__file__).with_name("tools_test.py")
)
finite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(finite)
sample, image = finite.sample, finite.image


def ideal_image(**kwargs):
    original = image(**kwargs)
    sets = original["layout"]["context_entries"] // original["layout"]["ways"]
    bits = sets.bit_length() - 1
    original.update(contract(ideal=True))
    original["entries"] = [
        {
            "key": (entry["tag"] << bits) | entry["set"],
            "slot": entry["slot"],
            "valid": entry["valid"],
            "conf": entry["conf"],
        }
        for entry in original["entries"]
    ]
    return original


class UnlimitedTests(unittest.TestCase):
    def test_more_than_all_finite_capacities_and_many_same_set_keys(self):
        pcs = [0x20 + i * 2 for i in range(40)]
        rows = []
        # More than 4096 distinct keys, all in the same original 2048-set index.
        for index in range(5001):
            key = 16 + index * 2048
            pc = pcs[index % len(pcs)]
            folded = (pc >> 1) ^ key
            context = (folded >> 1) | ((folded & 1) << 27)
            rows.append(
                sample(
                    index + 1,
                    pc=pc,
                    actual=0x10000 + 2 * index,
                    contexts=[context, 0, 0, 0, 0],
                )
            )
        metadata, report = build_image(rows, dict.fromkeys(pcs, 1), ideal=True)
        self.assertEqual(len(metadata["cam"]), 40)
        self.assertEqual(len(metadata["entries"]), 5001)
        self.assertEqual(len(metadata["slots"]), 5001)
        self.assertEqual(
            {entry["key"] & 2047 for entry in metadata["entries"]}, {16}
        )
        self.assertEqual(
            report["estimated_net_correction_before_confidence"], 5001
        )
        validate_metadata(metadata, ideal=True)
        self.assertEqual(
            replay_reference(rows, metadata)["global"]["corrected"], 5001
        )
        with self.assertRaises(ValueError):
            build_image(rows, dict.fromkeys(pcs, 1))

    def test_same_hash_alias_and_tie_target_shared_entry_and_confidence(self):
        first, second = 0x20, 0x20000022
        rows = [
            sample(1, pc=first, actual=0x80),
            sample(2, pc=second, actual=0xA0),
        ]
        metadata, report = build_image(rows, {first: 0, second: 0}, ideal=True)
        self.assertEqual(len(metadata["entries"]), 1)
        self.assertEqual(metadata["slots"], ["0x80"])
        self.assertEqual(report["cross_pc_alias_keys"], 1)
        model = Reference(metadata)
        for seq in range(1, 4):
            row = sample(
                seq,
                pc=first if seq % 2 else second,
                query=seq * 3,
                commit=seq * 3 + 1,
                actual=0xA0,
            )
            model.query(row)
            model.commit(row)
        model.advance(11)
        entry = next(iter(model.entries.values()))
        self.assertEqual(entry["conf"], 0)
        self.assertEqual(entry["entry_id"], 0)
        recovery = sample(4, pc=second, query=12, commit=13, actual=0x80)
        model.query(recovery)
        model.commit(recovery)
        model.advance(14)
        self.assertEqual(entry["conf"], 1)
        self.assertEqual(model.finish()["queue"]["recoveries"], 1)

    def test_all_32_masks_same_finite_capacity_mapping_preserves_queue_results(
        self,
    ):
        rows = [
            sample(
                i,
                query=i * 3,
                commit=i * 3 + 1,
                actual=0x80 if i % 5 else 0xA0,
            )
            for i in range(1, 20)
        ]
        for mask in range(32):
            finite_meta, _ = build_image(rows, {0x20: mask})
            ideal_meta, _ = build_image(rows, {0x20: mask}, ideal=True)
            a, b = replay_reference(rows, finite_meta, 60), replay_reference(
                rows, ideal_meta, 60
            )
            for field in ("global", "pcs", "queue", "conf0"):
                self.assertEqual(a[field], b[field])

    def test_ideal_has_same_earliest_pc_query_arbitration(self):
        row = sample(1, pc=0x24, start=0x20, end=0x30)
        metadata, report = build_image([row], {0x20: 0, 0x24: 0}, ideal=True)
        self.assertEqual(metadata["entries"], [])
        self.assertEqual(report["pcs"][1]["cam_arbitration_lost"], 1)
        self.assertEqual(
            replay_reference([row], metadata)["global"]["eligible"], 0
        )

    def test_ideal_best_target_penalizes_all_correct_baseline_instances(self):
        rows = [
            sample(1, actual=0x80, baseline=0x80),
            sample(2, actual=0x80, baseline=0x80),
            sample(3, actual=0xA0, baseline=0x40),
        ]
        self.assertEqual(
            build_image(rows, {0x20: 0}, ideal=True)[0]["entries"], []
        )

    def test_version_mismatch_invalid_rows_and_hidden_pc_rejected(self):
        metadata = ideal_image()
        validate_metadata(metadata, ideal=True)
        with self.assertRaises(ValueError):
            validate_metadata(metadata, ideal=False)
        with self.assertRaises(ValueError):
            validate_metadata(image(), ideal=True)
        for change in (
            lambda m: m.update(storage="finite"),
            lambda m: m["layout"].update(ways=2),
            lambda m: m["entries"][0].update(pc="0x20"),
            lambda m: m["entries"][0].update(key=True),
            lambda m: m["entries"][0].update(key=1 << 28),
            lambda m: m["entries"][0].update(slot=100),
            lambda m: m["entries"][0].update(conf=0),
            lambda m: m["slots"].append("0x80"),
            lambda m: m["finite_reference"].update(cam_entries=17),
        ):
            bad = copy.deepcopy(metadata)
            change(bad)
            with self.assertRaises(ValueError):
                validate_metadata(bad)
        metadata["entries"][0].update(valid=False, conf=0, slot=10_000_000)
        validate_metadata(metadata)
        self.assertEqual(
            replay_reference([sample(1)], metadata)["global"]["tag_hits"], 0
        )
        metadata["entries"].append(dict(metadata["entries"][0]))
        with self.assertRaises(ValueError):
            validate_metadata(metadata)

    def test_unlimited_history_path_copy_and_rollback(self):
        pcs = tuple(range(0x20, 0x20 + 400, 2))
        history = History(pcs=pcs, ideal=True)
        empty = history.snapshot()
        for index, pc in enumerate(pcs):
            history.update(pc, (index % 4) * 2, taken=True, indirect=True)
        populated = history.snapshot()
        values = {pc: history.contexts(pc)[2] for pc in pcs}
        history.update(pcs[40], 6, taken=True, indirect=True)
        changed = history.snapshot()
        self.assertIsNot(populated[2], changed[2])
        # Updating a PC in the first half shares the immutable right subtree.
        self.assertIs(populated[2][1], changed[2][1])
        for pc in pcs:
            if pc != pcs[40]:
                self.assertEqual(history.contexts(pc)[2], values[pc])
        history.restore(populated)
        for pc in pcs:
            self.assertEqual(history.contexts(pc)[2], values[pc])
        history.restore(empty)
        self.assertTrue(all(history.contexts(pc)[2] == 0 for pc in pcs))
        with self.assertRaises(ValueError):
            History(pcs=pcs)

    def test_all_candidates_and_unbounded_joint_selection(self):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "h2p.txt"
            path.write_text(
                "".join(
                    f"{hex(0x20 + i * 2)}, {100-i}, 0x80({100-i})\n"
                    for i in range(50)
                )
            )
            self.assertEqual(
                len(candidates_from_stage1(path, None)["pcs"]), 20
            )
            data = candidates_from_stage1(
                path, Path(raw) / "candidates.json", ideal=True
            )
            self.assertEqual(len(data["pcs"]), 50)
            self.assertEqual(
                len(
                    load_candidates(Path(raw) / "candidates.json", ideal=True)
                ),
                50,
            )
            with self.assertRaises(ValueError):
                load_candidates(Path(raw) / "candidates.json")
        stats = {
            "pcs": [
                {"pc": hex(0x20 + i * 2), "corrected": 5, "harmed": 0}
                for i in range(50)
            ]
        }
        best, _ = select_joint(
            [(2, stats), (1, stats), (31, stats)], cam_entries=None
        )
        self.assertEqual(len(best), 50)
        self.assertEqual(set(best.values()), {1})
        self.assertEqual(len(select_joint([(1, stats)])[0]), 16)

    def test_occupancy_resident_valid_including_conf_zero_no_cycles_and_collect(
        self,
    ):
        metadata = ideal_image()
        values = {}
        for table, capacity in (("cam", 16), ("context", 4096), ("slots", 32)):
            values[table] = {
                "resident_entries": 1,
                "avg_entries": 1,
                "peak_entries": 1,
                "sample_cycles": 100,
                "finite_capacity": capacity,
                "resident_multiple": 1 / capacity,
                "avg_multiple": 1 / capacity,
                "peak_multiple": 1 / capacity,
            }
        data = {"occupancy_applicable": True, "occupancy": values}
        validate_occupancy(data, "replay", metadata, 100)
        for row in values.values():
            row.update(
                sample_cycles=0,
                avg_entries=0,
                peak_entries=0,
                avg_multiple=0,
                peak_multiple=0,
            )
        validate_occupancy(data, "replay", metadata, 0)
        for row in values.values():
            row.update(resident_entries=0, resident_multiple=0)
        data["occupancy_applicable"] = False
        validate_occupancy(data, "collect", sample_cycles=100)

    def test_v3_profile_keeps_raw_prediction_bits_and_shared_samples(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            rows = [
                sample(1, baseline=0x198F9A00198B78),
                sample(2, baseline=0xFFFFFFFFFFFFFFFF),
            ]
            for ideal, name in ((False, "old"), (True, "new")):
                profile = root / (name + ".gz")
                with gzip.open(profile, "wt") as output:
                    output.write(
                        json.dumps(
                            {
                                "kind": "header",
                                "format": "h2p-hint-profile",
                                **contract(ideal=ideal),
                                "pcs": ["0x20"],
                                "clock_period": 333,
                            }
                        )
                        + "\n"
                    )
                    for row in rows:
                        output.write(json.dumps(row) + "\n")
                make_cache(profile, root / name)
            comparison = compare_shared_profiles(root / "old", root / "new")
            self.assertTrue(comparison["pass"])
            self.assertEqual(comparison["new_records"], 2)
            self.assertEqual(
                next(Cache(root / "new").rows())["baseline_target"],
                0x198F9A00198B78,
            )

    def test_configuration_defaults_geometry_and_queue(self):
        parser = argparse.ArgumentParser()
        add_h2p_hint_options(parser)
        defaults = parser.parse_args([])
        params = h2p_hint_params(defaults)
        self.assertFalse(params["idealHintIBP"])
        self.assertEqual(
            [
                params[name]
                for name in (
                    "hintCamEntries",
                    "hintContextEntries",
                    "hintWays",
                    "hintSlotEntries",
                )
            ],
            [16, 4096, 2, 32],
        )
        self.assertIsNotNone(
            validate_h2p_hint_config(parser.parse_args(["--ideal-hint-ibp"]))
        )
        with tempfile.TemporaryDirectory() as raw:
            file = Path(raw) / "input.json"
            file.write_text("{}")
            for mode in ("collect", "replay"):
                option = "candidates" if mode == "collect" else "metadata"
                argv = [
                    "--ideal-hint-ibp",
                    f"--h2p-hint-mode={mode}",
                    f"--h2p-hint-{option}={file}",
                ]
                args = parser.parse_args(argv)
                args.bp_type = "DecoupledBPUWithBTB"
                self.assertIsNone(validate_h2p_hint_config(args))
                self.assertTrue(h2p_hint_params(args)["idealHintIBP"])
                for field in (
                    "cam_entries",
                    "context_entries",
                    "ways",
                    "slot_entries",
                ):
                    bad = copy.copy(args)
                    setattr(bad, "h2p_hint_" + field, LAYOUT[field] * 2)
                    self.assertIsNotNone(validate_h2p_hint_config(bad))
                args.h2p_hint_conf_queue_depth = 3
                self.assertIsNone(validate_h2p_hint_config(args))

    def test_185_matrix_single_cam_32_masks_and_commands(self):
        self.assertEqual((FORMAL_RUNS, FORMAL_WINDOWS), (185, 9250))
        with tempfile.TemporaryDirectory() as raw:
            args = argparse.Namespace(
                output_root=Path(raw),
                command_prefix=[],
                gem5=Path("/gem5"),
                config=Path("/config.py"),
            )
            tasks = [
                {
                    "bench": bench,
                    "checkpoint_id": cid,
                    "scale": "test",
                    "checkpoint": {"path": "/slice.zstd"},
                }
                for bench, cid in HEAVY
            ]
            for task in tasks:
                path = candidates_path(args, task, "collect")
                path.parent.mkdir(parents=True)
                write_json(path, candidate_document(range(0, 100, 2), {}))
            jobs = mask_jobs(args, tasks)
            self.assertEqual(len(jobs), 160)
            self.assertTrue(all(len(job["masks"]) == 50 for job in jobs))
            for stage in (
                "baseline",
                "finite",
                "fixed",
                "collect",
                "masks",
                "joint",
            ):
                command = build_command(args, tasks[0], stage, Path("/output"))
                self.assertEqual(
                    "--ideal-hint-ibp" in command,
                    stage not in ("baseline", "finite"),
                )
                self.assertIn("--maxinsts=150000000", command)
                self.assertNotIn("--raw-cpt", command)

    def test_audit_rejects_lost_scoring_and_window_only_behavior_change(self):
        reference = replay_reference(
            [sample(1)], ideal_image(), finish_cycle=4
        )
        stats = {
            "format": "h2p-hint-stats",
            "version": 3,
            "mode": "replay",
            **{
                field: reference[field] for field in ("global", "queue", "pcs")
            },
        }
        validate_hint_stats(stats, 0, "replay", [0x20], ideal=True)
        corrupt = copy.deepcopy(stats)
        corrupt["queue"]["enqueued"] = corrupt["queue"]["applied"] = corrupt[
            "queue"
        ]["pending"] = 0
        with self.assertRaisesRegex(ValueError, "committed tag hit"):
            validate_hint_stats(corrupt, 0, "replay", [0x20], ideal=True)
        with tempfile.TemporaryDirectory() as raw:
            left, right = Path(raw) / "left", Path(raw) / "right"
            for directory in (left, right):
                (directory / "h2p-indirec").mkdir(parents=True)
                (directory / "stats.txt").write_text("simInsts 50000000\n")
                (directory / "h2p-indirect.txt").write_text(
                    "0x20, 1, 0x80(1)\n"
                )
                for offset in range(0, 50_000_000, 1_000_000):
                    (directory / "h2p-indirec" / f"{offset}.txt").write_text(
                        ""
                    )
            (left / "h2p-indirec" / "0.txt").write_text("0x20, 1, 0x80(1)\n")
            (right / "h2p-indirec" / "1000000.txt").write_text(
                "0x20, 1, 0x80(1)\n"
            )
            self.assertFalse(
                compare_observer(left, right, check_windows=True)["pass"]
            )

    def test_independent_rebuild_retains_aliases_and_detects_stale_source(
        self,
    ):
        rows = [sample(1), sample(2, pc=0x20000022, actual=0xA0)]

        class TinyCache:
            def rows(self):
                return iter(rows)

        metadata, _ = build_image(rows, {0x20: 0, 0x20000022: 0}, ideal=True)
        rebuilt, _ = reproduce_unlimited(TinyCache(), {0x20: 0, 0x20000022: 0})
        self.assertEqual(
            rebuilt,
            {field: metadata[field] for field in ("cam", "entries", "slots")},
        )
        import hashlib

        with tempfile.TemporaryDirectory() as raw:
            source = Path(raw) / "source.py"
            source.write_bytes(b"first")
            manifest = {
                "files": {
                    "source.py": {
                        "kind": "file",
                        "size": 5,
                        "sha256": hashlib.sha256(b"first").hexdigest(),
                        "executable": False,
                    }
                }
            }
            self.assertEqual(audit_source_files(raw, manifest), 1)
            source.write_bytes(b"other")
            with self.assertRaisesRegex(ValueError, "source file identity"):
                audit_source_files(raw, manifest)

    def test_conf0_rejects_wrong_clock_measure_boundary_and_history_width(
        self,
    ):
        metadata = ideal_image()
        row = {
            "pc": "0x20",
            "mask": 0,
            "seq": 1,
            "contexts": [0] * 5,
            "hash": 16,
            "key": 16,
            "entry_id": 0,
            "slot": 0,
            "predicted_target": "0x80",
            "actual_target": "0xA0",
            "query_cycle": 1,
            "commit_cycle": 5,
            "writeback_cycle": 6,
            "query_tick": 10,
            "commit_tick": 50,
            "writeback_tick": 60,
            "history": {
                "ghr": "0x0",
                "githr": "0x0",
                "lithr": "0x0",
                "call_pc_words_newest_first": [],
                "return_pc_words_newest_first": [],
            },
        }
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "conf0.txt"
            path.write_text(json.dumps(row) + "\n")
            self.assertEqual(
                audit_conf0(
                    path, metadata, clock_period=10, start_cycle=5, end_cycle=6
                ),
                1,
            )
            with self.assertRaises(ValueError):
                audit_conf0(
                    path, metadata, clock_period=11, start_cycle=5, end_cycle=6
                )
            with self.assertRaises(ValueError):
                audit_conf0(
                    path, metadata, clock_period=10, start_cycle=5, end_cycle=5
                )
            bad = copy.deepcopy(row)
            bad["history"]["ghr"] = hex(1 << 970)
            path.write_text(json.dumps(bad) + "\n")
            with self.assertRaisesRegex(ValueError, "width"):
                audit_conf0(
                    path, metadata, clock_period=10, start_cycle=5, end_cycle=6
                )


if __name__ == "__main__":
    unittest.main()
