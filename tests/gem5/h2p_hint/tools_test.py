#!/usr/bin/env python3
"""Host-only contract tests. gem5 runtime tests use the final ICT186 binary."""

import argparse
from collections import Counter
import copy
import gzip
import importlib.util
import json
from pathlib import Path
import random
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "util"))
sys.path.insert(0, str(ROOT / "configs"))
from common.H2PHint import add_h2p_hint_options, h2p_hint_params, validate_h2p_hint_config
from h2p_hint.audit import compare_observer, validate_hint_stats
from h2p_hint.builder import build_image, candidates_from_stage1, select_joint, select_table
from h2p_hint.model import (
    History, LAYOUT, Reference, address, context_key, contract, fold28,
    load_candidates, load_metadata, replay_reference, rotl28, validate_metadata, write_json,
)
from h2p_hint.profile import Cache, make_cache
from h2p_hint.runner import (
    HEAVY, build_command, final_report, image_path, load_workloads, mask_jobs,
    parse_args, planned_run_counts, run_path, validate_runtime_gate,
)


def sample(seq, *, pc=0x20, actual=0x80, baseline=0x40, query=None,
           commit=None, eligible=True, contexts=None, start=None, end=None):
    return {"kind": "branch", "seq": seq, "pc": pc, "actual": actual,
            "baseline_valid": True, "baseline_pc": pc, "baseline_target": baseline,
            "query_cycle": seq * 3 if query is None else query,
            "commit_cycle": seq * 3 + 1 if commit is None else commit,
            "block_start": pc if start is None else start,
            "block_end": pc + 2 if end is None else end,
            "eligible": eligible, "contexts": [0] * 5 if contexts is None else contexts}


def image(*, pcs=(0x20,), target=0x80, layout=None):
    layout = dict(layout or LAYOUT)
    metadata = {"format": "h2p-hint", **contract(layout),
                "cam": [{"pc": hex(pc), "mask": 0, "valid": True} for pc in pcs],
                "slots": [hex(target)], "entries": [], "source": {}}
    sets = layout["context_entries"] // layout["ways"]
    key = context_key(pcs[0], [0] * 5, 0)
    metadata["entries"] = [{"set": key & (sets - 1), "way": 0,
                            "tag": key >> (sets.bit_length() - 1),
                            "slot": 0, "conf": 3, "valid": True}]
    return metadata


class EncodingTests(unittest.TestCase):
    def test_all_masks_fixed_vectors(self):
        # rotl28(1,1), rotl28(2,2), ..., rotl28(5,5).
        components = [2, 8, 24, 64, 160]
        for mask in range(32):
            expected = 0x123456 >> 1
            for bit in range(5):
                if mask & 1 << bit:
                    expected ^= components[bit]
            self.assertEqual(context_key(0x123456, [1, 2, 3, 4, 5], mask), expected)
        self.assertEqual(fold28((0x123 << 56) | (0xabc << 28) | 0x789), 0x123 ^ 0xabc ^ 0x789)
        self.assertEqual(rotl28(1 << 27, 1), 1)

    def test_history_target_bit_encoding_and_lithr_capacity(self):
        history = History(pcs=(0x20,))
        for target in [2, 4, 6, 0, 2]:
            history.update(0x20, target, taken=True, indirect=True)
        self.assertEqual(history.lithr[0x20], 0b10110001)
        self.assertEqual(history.githr, 0b0110110001)
        history.update(0x22, 6, taken=True, indirect=True)
        self.assertNotIn(0x22, history.lithr)

    def test_exact_64_event_expiry_and_call_return_classification(self):
        history = History(pcs=(0x20,))
        history.update(0x20, 6, taken=True, indirect=True, call=True, ret=True)
        self.assertEqual(history.githr, 0)
        self.assertEqual(tuple(history.calls), (0x10,))
        self.assertEqual(tuple(history.returns), (0x10,))
        for index in range(64):
            history.update(0x100 + index * 2, 0, taken=True, call=True)
        self.assertEqual(len(history.calls), 64)
        self.assertNotIn(0x10, history.calls)
        self.assertEqual(history.calls[0], (0x100 + 63 * 2) >> 1)
        history.update(0x20, 6, taken=True, indirect=True, call=True)
        self.assertEqual(history.githr, 3)

    def test_ghr_bound_and_not_taken(self):
        history = History(pcs=(0x20,))
        history.ghr = (1 << 970) - 1
        history.update(0x20, 6, taken=False, conditional=True, indirect=True, call=True)
        self.assertEqual(history.ghr, (1 << 970) - 2)
        self.assertEqual(history.githr, 0)
        self.assertFalse(history.calls)

    def test_snapshot_preupdate_and_squash_restore(self):
        history = History(pcs=(0x20,))
        before = history.snapshot()
        saved_context = history.contexts(0x20)
        history.update(0x20, 6, taken=True, conditional=True, indirect=True, call=True)
        history.update(0x22, 2, taken=True, ret=True)
        self.assertNotEqual(history.contexts(0x20), saved_context)
        history.restore(before)
        self.assertEqual(history.contexts(0x20), saved_context)

    def test_even_canonical_address(self):
        self.assertEqual(address("0xffff800000000002"), 0xffff800000000002)
        for value in [3, -2, 0x800000000002, 1 << 65, 2.0, True]:
            with self.assertRaises(ValueError):
                address(value)


class FiniteTableTests(unittest.TestCase):
    def test_real_cross_pc_alias_and_no_hidden_tag(self):
        first, second = 0x20, 0x20000022
        self.assertEqual(context_key(first, [0] * 5, 0), context_key(second, [0] * 5, 0))
        rows = [sample(1, pc=first, actual=0x80), sample(2, pc=second, actual=0xA0)]
        metadata, report = build_image(rows, {first: 0, second: 0})
        self.assertEqual(len(metadata["entries"]), 1)
        self.assertEqual(metadata["slots"], ["0x80"])
        self.assertEqual(report["cross_pc_alias_keys"], 1)
        replay = replay_reference(rows, metadata)
        self.assertEqual(replay["global"]["tag_hits"], 2)

    def test_net_gain_penalizes_correct_baseline_overrides(self):
        rows = [sample(1, actual=0x80, baseline=0x80),
                sample(2, actual=0x80, baseline=0x80),
                sample(3, actual=0xA0, baseline=0x40)]
        metadata, report = build_image(rows, {0x20: 0})
        self.assertEqual(metadata["entries"], [])
        self.assertEqual(report["estimated_net_correction_before_confidence"], 0)

    def test_two_way_slot_limit_and_tie_order(self):
        layout = {**LAYOUT, "context_entries": 2, "slot_entries": 1}
        keys = {key: {"targets": Counter({0x80: 3, 0xA0: 3}), "correct": 0}
                for key in [3, 1, 2]}
        table, slots, steps, _ = select_table(keys, layout)
        self.assertEqual(slots, [0x80])
        self.assertEqual(sorted(key for _, key, _ in table), [1, 2])
        self.assertEqual(steps[0]["marginal_net_correction"], 6)

    def test_greedy_marginal_matches_bruteforce_finite_objective(self):
        rng = random.Random(1984)
        layout = {**LAYOUT, "context_entries": 8, "slot_entries": 3}
        for _ in range(25):
            keys = {key: {"targets": Counter({target: rng.randrange(9) for target in range(2, 12, 2)}),
                           "correct": rng.randrange(5)} for key in range(12)}
            def objective(targets):
                by_set = [[] for _ in range(4)]
                for key, row in keys.items():
                    gain = max((row["targets"][t] - row["correct"] for t in targets), default=0)
                    by_set[key & 3].append(max(gain, 0))
                return sum(sum(sorted(values, reverse=True)[:2]) for values in by_set)
            selected, expected = [], []
            for _ in range(3):
                scores = [(objective(selected + [target]) - objective(selected), target)
                          for target in range(2, 12, 2) if target not in selected]
                delta, target = sorted(scores, key=lambda pair: (-pair[0], pair[1]))[0]
                if delta <= 0:
                    break
                selected.append(target)
                expected.append({"target": hex(target), "marginal_net_correction": delta})
            _, _, actual, _ = select_table(keys, layout)
            self.assertEqual(actual, expected)

    def test_earliest_cam_candidate_cancels_later(self):
        rows = [sample(1, pc=0x22, start=0x20, end=0x30)]
        metadata, report = build_image(rows, {0x20: 0, 0x22: 0})
        self.assertEqual(metadata["entries"], [])
        self.assertEqual(next(row for row in report["pcs"] if row["pc"] == "0x22")["cam_arbitration_lost"], 1)

    def test_invalid_geometry_versions_pointers_and_validity(self):
        for modify in [lambda m: m.update(hash_version=1),
                       lambda m: m["entries"][0].update(slot=1),
                       lambda m: m["cam"][0].update(valid="true"),
                       lambda m: m["cam"][0].update(mask=True),
                       lambda m: m["layout"].update(context_entries=6),
                       lambda m: m["entries"].append(dict(m["entries"][0])),
                       lambda m: m.update(cam={})]:
            metadata = image()
            modify(metadata)
            with self.assertRaises(ValueError):
                validate_metadata(metadata)

    def test_joint_selection_uses_actual_net_and_mask_ties(self):
        def stats(gain):
            return {"pcs": [{"pc": "0x20", "corrected": gain, "harmed": 0}]}
        selected, report = select_joint([(31, stats(10)), (2, stats(10)),
                                         (1, stats(10)), (0, stats(9))])
        self.assertEqual(selected, {0x20: 1})
        self.assertEqual(report[0]["net_correction"], 10)

    def test_invalid_entries_accept_zero_conf_and_unloaded_slot_but_never_query(self):
        metadata = image()
        metadata["entries"][0].update(valid=False, conf=0, slot=31)
        duplicate_tag = dict(metadata["entries"][0], way=1, conf=2)
        metadata["entries"].append(duplicate_tag)
        validate_metadata(metadata)
        result = replay_reference([sample(1)], metadata, finish_cycle=100)
        self.assertEqual(result["global"]["tag_hits"], 0)
        self.assertEqual(result["queue"]["enqueued"], 0)
        metadata["entries"][0]["valid"] = True
        with self.assertRaises(ValueError):
            validate_metadata(metadata)

    def test_metadata_json_types_and_duplicate_fields_match_cpp_loader(self):
        for modify in [lambda m: m["cam"][0].update(pc=0x20),
                       lambda m: m["slots"].__setitem__(0, 0x80),
                       lambda m: m.update(source=[]),
                       lambda m: m.pop("source"),
                       lambda m: m.update(ghr_bits=970.0),
                       lambda m: m["history"].update(target_shift=True)]:
            metadata = image()
            modify(metadata)
            with self.assertRaises(ValueError):
                validate_metadata(metadata)
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "metadata.json"
            path.write_text('{"version":2,' + json.dumps(image())[1:])
            with self.assertRaises(ValueError):
                load_metadata(path)

    def test_candidate_source_address_types_and_duplicate_fields(self):
        good = {"format": "h2p-hint-candidates", "version": 2,
                "pcs": ["0x20"], "source": {}}
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "candidate.json"
            write_json(path, good)
            self.assertEqual(load_candidates(path), [0x20])
            for modify in [lambda m: m.update(source=[]), lambda m: m.pop("source"),
                           lambda m: m.update(pcs=[0x20]), lambda m: m.update(pcs={}),
                           lambda m: m.update(version=2.0)]:
                data = copy.deepcopy(good)
                modify(data)
                write_json(path, data)
                with self.assertRaises(ValueError):
                    load_candidates(path)
            path.write_text('{"version":2,' + json.dumps(good)[1:])
            with self.assertRaises(ValueError):
                load_candidates(path)


class ConfidenceTests(unittest.TestCase):
    def test_full_queue_drops_new_updates_without_commit_stall(self):
        rows = [sample(i + 1, query=1, commit=10, actual=0xA0) for i in range(9)]
        result = replay_reference(rows, image())
        self.assertEqual(result["queue"]["enqueued"], 8)
        self.assertEqual(result["queue"]["dropped"], 1)
        self.assertEqual(result["queue"]["pending"], 8)
        self.assertEqual(result["queue"]["applied"], 0)
        self.assertEqual(result["global"]["executions"], 9)

    def test_shadow_recovery_and_repeated_zero(self):
        rows = [sample(i, query=i * 3 - 2, commit=i * 3 - 1,
                       actual=0x80 if i == 4 else 0xA0,
                       baseline=0x40 if i == 4 else 0xA0) for i in range(1, 6)]
        result = replay_reference(rows, image(), finish_cycle=15)
        self.assertEqual(result["queue"]["zero_transitions"], 2)
        self.assertEqual(result["queue"]["recoveries"], 1)
        self.assertEqual(result["global"]["conf_enabled"], 4)
        self.assertEqual(result["global"]["harmed"], 4)

    def test_query_confidence_is_immutable_until_later_commit(self):
        model = Reference(image())
        later = sample(4, query=1, commit=20, actual=0xA0, baseline=0xA0)
        model.query(later)
        for seq in range(1, 4):
            row = sample(seq, query=seq * 3, commit=seq * 3 + 1, actual=0xA0)
            model.query(row)
            model.commit(row)
        model.advance(12)
        self.assertEqual(next(iter(model.entries.values()))["conf"], 0)
        model.commit(later)
        self.assertEqual(model.finish()["global"]["harmed"], 1)

    def test_same_cycle_commit_does_not_bypass_query(self):
        rows = [sample(i, query=1, commit=2, actual=0xA0) for i in range(1, 4)]
        rows.append(sample(4, query=2, commit=5, actual=0x80))
        result = replay_reference(rows, image(), finish_cycle=6)
        self.assertEqual(result["global"]["conf_enabled"], 4)
        self.assertEqual(result["queue"]["zero_transitions"], 1)
        self.assertEqual(result["queue"]["recoveries"], 1)

    def test_invalid_entry_never_scores_or_recovers(self):
        metadata = image()
        metadata["entries"][0]["valid"] = False
        result = replay_reference([sample(1)], metadata, finish_cycle=100)
        self.assertEqual(result["queue"]["enqueued"], 0)
        self.assertEqual(result["global"]["final_misses"], 1)

    def test_stats_reset_carries_pending_without_resetting_conf(self):
        model = Reference(image())
        row = sample(1, actual=0xA0)
        model.query(row)
        model.commit(row)
        model.reset_statistics()
        result = model.finish(5)
        self.assertEqual(result["queue"]["carried_in"], 1)
        self.assertEqual(result["queue"]["enqueued"], 0)
        self.assertEqual(result["queue"]["applied"], 1)
        self.assertEqual(next(iter(model.entries.values()))["conf"], 2)


class PipelineToolsTests(unittest.TestCase):
    def test_raw_wrong_baseline_targets_roundtrip_real_profile_cache_reference(self):
        # Actual scimark_fft/26 collect observation: this is a wrong BPU
        # prediction, so canonical-address validation must not discard it.
        observed = {"kind": "branch", "seq": 158854085, "pc": "0x1872d8",
            "actual": "0x64a8a", "baseline_valid": True,
            "baseline_pc": "0x1872d8", "baseline_target": "0x198f9a00198b78",
            "query_cycle": 31762655, "commit_cycle": 31762689,
            "block_start": "0x1872ba", "block_end": "0x1872e0",
            "eligible": True, "contexts": [38440533, 65306740, 85, 44684962, 245303759]}
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            profile = root / "profile.gz"
            targets = [0x198f9a00198b78, 0x64a8b, (1 << 64) - 1]
            with gzip.open(profile, "wt") as stream:
                stream.write(json.dumps({"kind": "header", "format": "h2p-hint-profile",
                    **contract(), "pcs": [observed["pc"]], "clock_period": 1000}) + "\n")
                for index, target in enumerate(targets):
                    row = dict(observed, baseline_target=hex(target),
                               seq=observed["seq"] + index,
                               query_cycle=observed["query_cycle"] + index * 4,
                               commit_cycle=observed["commit_cycle"] + index * 4)
                    stream.write(json.dumps(row) + "\n")
            make_cache(profile, root / "cache")
            cache = Cache(root / "cache")
            restored = list(cache.rows())
            self.assertEqual([row["baseline_target"] for row in restored], targets)
            self.assertEqual([row["baseline_target"] for row in cache.queries()], targets)
            self.assertTrue(all(row["baseline_valid"] for row in restored))
            metadata, report = build_image(cache.rows(), {0x1872d8: 0})
            self.assertEqual(metadata["slots"], ["0x64a8a"])
            self.assertEqual(report["estimated_net_correction_before_confidence"], 3)
            validate_metadata(metadata)
            result = cache.reference(metadata, finish_cycle=31762700)
            self.assertEqual(result["global"]["executions"], 3)
            self.assertEqual(result["global"]["baseline_misses"], 3)
            self.assertEqual(result["global"]["final_misses"], 0)
            self.assertEqual(result["global"]["corrected"], 3)
            self.assertEqual(result["global"]["harmed"], 0)
            self.assertEqual(result, replay_reference(restored, metadata, 31762700))

    def test_profile_raw_target_range_and_metadata_addresses_stay_strict(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            for index, invalid in enumerate((-1, 1 << 64, "-0x1",
                                             "0x10000000000000000", 1.5, True, None)):
                profile = root / f"invalid-{index}.gz"
                with gzip.open(profile, "wt") as stream:
                    stream.write(json.dumps({"kind": "header", "format": "h2p-hint-profile",
                        **contract(), "pcs": ["0x20"], "clock_period": 1000}) + "\n")
                    stream.write(json.dumps(sample(1, baseline=invalid)) + "\n")
                with self.assertRaises(ValueError):
                    make_cache(profile, root / f"cache-{index}")
        for target in ("0x198f9a00198b78", "0x64a8b", "0xffffffffffffffff"):
            for field in ("cam", "slots"):
                metadata = image()
                if field == "cam":
                    metadata["cam"][0]["pc"] = target
                else:
                    metadata["slots"][0] = target
                with self.assertRaises(ValueError):
                    validate_metadata(metadata)

    def test_missing_query_roundtrips_through_gzip_cache_and_reference(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            missing = sample(2, commit=4, eligible=False, start=0, end=0)
            missing.update(query_cycle=None, contexts=None, baseline_valid=False,
                           baseline_pc=0, baseline_target=0)
            rows = [sample(1, query=0, commit=2), missing,
                    sample(3, query=7, commit=9, baseline=0x80)]
            profile = root / "profile.gz"
            with gzip.open(profile, "wt") as stream:
                stream.write(json.dumps({"kind": "header", "format": "h2p-hint-profile",
                    **contract(), "pcs": ["0x20"], "clock_period": 1000}) + "\n")
                for row in rows:
                    stream.write(json.dumps(row) + "\n")
            make_cache(profile, root / "cache")
            cache = Cache(root / "cache")
            restored = list(cache.rows())
            self.assertEqual(restored[0]["query_cycle"], 0)
            self.assertIsNone(restored[1]["query_cycle"])
            self.assertIsNone(restored[1]["contexts"])
            self.assertFalse(restored[1]["eligible"])
            self.assertEqual([row["seq"] for row in cache.queries()], [1, 3])
            result = cache.reference(image(), finish_cycle=10)
            self.assertEqual(result, replay_reference(rows, image(), finish_cycle=10))
            self.assertEqual(result["global"]["executions"], 3)
            self.assertEqual(result["global"]["eligible"], 2)
            self.assertEqual(result["global"]["final_misses"], 1)
            self.assertEqual(result["queue"]["enqueued"], 2)
            identity_path = root / "cache" / "cache.json"
            identity = json.loads(identity_path.read_text())
            identity.pop("record_version")
            write_json(identity_path, identity)
            with self.assertRaises(ValueError):
                make_cache(profile, root / "cache")
            with self.assertRaises(ValueError):
                Cache(root / "cache")

    def test_cache_reference_reorders_query_events_without_future_targets(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            rows = [sample(1, query=3, commit=10), sample(2, query=2, commit=11)]
            profile = root / "profile.gz"
            with gzip.open(profile, "wt") as stream:
                stream.write(json.dumps({"kind": "header", "format": "h2p-hint-profile",
                    **contract(), "pcs": ["0x20"], "clock_period": 1000}) + "\n")
                for row in rows:
                    stream.write(json.dumps(row) + "\n")
            identity = make_cache(profile, root / "cache")
            self.assertFalse(identity["query_times_monotonic"])
            cache = Cache(root / "cache")
            self.assertEqual([row["seq"] for row in cache.queries()], [2, 1])
            self.assertEqual(cache.reference(image(), 12), replay_reference(rows, image(), 12))
            self.assertEqual(make_cache(profile, root / "cache"), identity)

    def test_stage1_ranking_and_all_224_group_masks(self):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "h2p.txt"
            path.write_text("0x24, 10, 0x80(10)\n0x20, 10, 0x80(10)\n0x22, 11, 0x80(11)\n")
            self.assertEqual(candidates_from_stage1(path, None)["pcs"], ["0x22", "0x20", "0x24"])
        tasks = [{"bench": bench, "checkpoint_id": checkpoint, "scale": "test",
                  "candidates": {"pcs": [hex(i * 2) for i in range(
                      20 if bench in ("deltablue", "pickle_pure_python") else 6)]}}
                 for bench, checkpoint in HEAVY]
        jobs = mask_jobs(tasks)
        self.assertEqual(len(jobs), 224)
        self.assertTrue(all(len(job["masks"]) <= 16 for job in jobs))
        self.assertEqual(sum(planned_run_counts(tasks, jobs).values()), 239)
        half_sweep = [job for job in jobs if job["mask"] < 16]
        self.assertEqual(sum(planned_run_counts(tasks, half_sweep).values()), 127)
        for task in tasks:
            for pc in task["candidates"]["pcs"]:
                masks = [job["mask"] for job in jobs if job["task"] == task and address(pc) in job["masks"]]
                self.assertEqual(masks, list(range(32)))

    def test_runner_accepts_both_instruction_windows(self):
        base = ["--gem5", "gem5.opt", "--config", "kmhv3.py",
                "--workload-list", "heavy.json", "--output-root", "out",
                "--source-manifest", "source.json", "--inputs-root", "inputs",
                "--dry-run"]
        self.assertEqual(
            (parse_args(base).warmup_insts, parse_args(base).measure_insts),
            (100_000_000, 50_000_000),
        )
        short = base + ["--warmup-insts", "1000000", "--measure-insts", "1000000"]
        self.assertEqual(parse_args(short).h2p_stat_interval, 1_000_000)
        for invalid in (
            base + ["--warmup-insts", "1000000"],
            short + ["--h2p-stat-interval", "500000"],
        ):
            with patch("sys.stderr"), self.assertRaises(SystemExit) as error:
                parse_args(invalid)
            self.assertEqual(error.exception.code, 2)

    def test_final_report_uses_planned_count_and_short_window(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            task = {"bench": "scimark_fft", "scale": "test", "checkpoint_id": "26"}
            tasks = [task]
            masks = [{"stage": "masks", "task": task, "group": 0,
                      "mask": mask, "masks": {0x20: mask}} for mask in (0, 1)]
            args = SimpleNamespace(output_root=root, warmup_insts=1_000_000,
                measure_insts=1_000_000, h2p_stat_interval=1_000_000,
                run_counts=planned_run_counts(tasks, masks))
            for stage, misses in (("baseline", 1), ("joint", 0)):
                output = run_path(args, task, stage)
                (output / "h2p-indirec").mkdir(parents=True)
                (output / "h2p-indirec" / "0.txt").write_text("")
                (output / "status.json").write_text(json.dumps({
                    "result": "pass", "final_misses": misses, "ipc": 1.0,
                    "hint": {"queue": {}}, "window_count": 1,
                }))
            joint_image = image_path(args, task, "joint")
            joint_image.mkdir(parents=True)
            (joint_image / "metadata.txt").write_text(json.dumps({
                "cam": [], "slots": [], "entries": [],
            }))
            for job in masks:
                output = run_path(args, task, "masks", job["group"], job["mask"])
                output.mkdir(parents=True)
                (output / "status.json").write_text(json.dumps({
                    "result": "pass", "hint": {"pcs": []}, "ipc": 1.0,
                    "window_count": 1,
                }))
                image_dir = image_path(args, task, "masks", job["group"], job["mask"])
                image_dir.mkdir(parents=True)
                (image_dir / "reference.json").write_text('{"pcs": []}')
            with patch("h2p_hint.runner.mask_jobs", return_value=masks):
                with self.assertRaisesRegex(ValueError, "5 successful full runs"):
                    final_report(args, tasks)
                collect = run_path(args, task, "collect")
                collect.mkdir(parents=True)
                (collect / "status.json").write_text(json.dumps({
                    "result": "pass", "window_count": 1,
                }))
                summary = final_report(args, tasks)
            self.assertEqual(summary["full_runs"], 5)
            report = (root / "report.md").read_text()
            self.assertIn("1M warm-up，reset 后 1M measure", report)
            self.assertIn("5 次完整运行", report)
            self.assertIn("2-mask 真实重放", report)

    def test_host_root_mapping_and_default_test_scale(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            manifest = root / "heavy.json"
            manifest.write_text(json.dumps({"workload_root": "xy-server:/not/local",
                "cpts": [{"bench": bench, "cpt_list": [cid]} for bench, cid in HEAVY]}))
            for bench, cid in HEAVY:
                directory = root / "cpts" / "xsgem5" / f"{bench}_test" / cid
                directory.mkdir(parents=True)
                (directory / f"_{cid}_memory_.zstd").write_bytes(b"test checkpoint")
            with self.assertRaises(ValueError):
                load_workloads(manifest)
            tasks = load_workloads(manifest, root)
            self.assertEqual(len(tasks), 5)
            self.assertTrue(all(row["scale"] == "test" for row in tasks))

    def test_observer_comparison_excludes_only_host_and_hint_stats(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            baseline, collect = root / "off", root / "collect"
            baseline.mkdir()
            collect.mkdir()
            common = "simInsts 50000000 # count\nsystem.cpu.ipc 2.00\nvector 1 25% nan\n"
            (baseline / "stats.txt").write_text(common + "hostSeconds 1\n")
            (collect / "stats.txt").write_text(common + "hostSeconds 9\nsystem.cpu.branchPred.h2pHint.lookup 10\n")
            for directory in (baseline, collect):
                (directory / "h2p-indirect.txt").write_text("")
            self.assertTrue(compare_observer(baseline, collect)["pass"])
            (collect / "stats.txt").write_text(common.replace("2.00", "2.01"))
            self.assertFalse(compare_observer(baseline, collect)["pass"])

    def test_cli_rejects_invalid_enabled_configs_and_preserves_off(self):
        parser = argparse.ArgumentParser()
        add_h2p_hint_options(parser)
        args = parser.parse_args([])
        self.assertIsNone(validate_h2p_hint_config(args))
        self.assertEqual(h2p_hint_params(args)["hintMode"], "off")
        args.h2p_hint_mode, args.bp_type = "collect", "DecoupledBPUWithBTB"
        self.assertIn("candidates", validate_h2p_hint_config(args))
        args.smt = True
        self.assertIn("one hardware thread", validate_h2p_hint_config(args))

    def test_final_binary_gate_is_required_and_bound_to_source_identity(self):
        identity = {"gem5": {"sha256": "binary"}, "source_manifest": {"sha256": "source"}}
        gate = {"result": "pass", "gem5_sha256": "binary", "source_manifest_sha256": "source",
                "checks": ["production raw pipeline"]}
        validate_runtime_gate(gate, identity)
        for modify in [lambda g: g.update(result="fail"), lambda g: g.update(checks=[]),
                       lambda g: g.update(gem5_sha256="stale"),
                       lambda g: g.update(source_manifest_sha256="stale")]:
            bad = dict(gate)
            modify(bad)
            with self.assertRaises(ValueError):
                validate_runtime_gate(bad, identity)


if __name__ == "__main__":
    unittest.main()
