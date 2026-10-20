"""Shared finite-v7-spec profile reader, deterministic builder, and image codec.

xs-gem5 and RTL collect their own gzip JSONL traces. Both use this module to
score fixed-confidence target corrections and build the same packed image.
"""

from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import re
import gzip
import hashlib
import json
from collections import Counter, defaultdict

from .model import address, json_loads


# The fork contract is self-contained; no project-parent manifest dependency.
from . import context_v5_contract as contract
_CONTRACT_PATH = Path(contract.__file__).resolve()


def load_metadata(path, *, expected_context_source=None):
    metadata = json_loads(Path(path).read_text())
    contract.pack_image(metadata)
    source = metadata["source"]
    present = {name for name in ("profile_context_source", "online_context_source")
               if name in source}
    if present and len(present) != 2:
        raise ValueError("metadata has incomplete context provenance")
    context_source = source.get("profile_context_source", "prediction")
    if (context_source not in ("prediction", "commit-pre") or
            source.get("online_context_source", "prediction") != "prediction"):
        raise ValueError("invalid metadata context provenance")
    if expected_context_source is not None:
        if expected_context_source not in ("prediction", "commit-pre"):
            raise ValueError("invalid expected metadata context source")
        if context_source != expected_context_source:
            raise ValueError("metadata context source mismatch")
    return metadata


def packed_image(path):
    return contract.pack_image(load_metadata(path))


def load_candidates(path):
    data = json_loads(Path(path).read_text())
    if (
        not isinstance(data, dict)
        or data.get("format") != "h2p-hint-candidates"
        or type(data.get("version")) is not int
        or data["version"] != 7
        or data.get("storage") != "finite"
        or data.get("context_encoding") != contract.CONTEXT_ENCODING
        or not isinstance(data.get("source"), dict)
        or not isinstance(data.get("pcs"), list)
        or len(data["pcs"]) > 16
    ):
        raise ValueError("candidate file is not finite Hint v7 spec")
    if any(not isinstance(pc, str) or not re.fullmatch(
            r"0x[0-9a-fA-F]{1,16}", pc) for pc in data["pcs"]):
        raise ValueError("candidate PCs must be hexadecimal address strings")
    pcs = [int(pc, 16) for pc in data["pcs"]]
    for pc in pcs:
        contract.address_word(pc)
    if len(set(pcs)) != len(pcs):
        raise ValueError("candidate PCs must be unique")
    return pcs


def digest(path):
    sha = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            sha.update(chunk)
    return sha.hexdigest()


def _validate_profile_context(contexts, history):
    if (not isinstance(contexts, list) or len(contexts) != 4 or
            any(type(x) is not int or not 0 <= x <= contract.MASK28
                for x in contexts)):
        raise ValueError("profile requires four folded contexts")
    if not isinstance(history, dict):
        raise ValueError("fork profile lacks full history evidence")
    try:
        components = contract.context_components(
            int(history["ith_low128"], 16), int(history["lhr_low16"], 16),
            int(history["call_low128"], 16), int(history["ret_low128"], 16))
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError("invalid fork full history evidence") from error
    if list(components) != contexts:
        raise ValueError("fork history and folded contexts differ")


def read_profile(path, *, expected_context_source="prediction"):
    """Read finite-v7 JSONL, rejecting a mismatched context provenance.

    Legacy headers are accepted only for prediction-context callers. Commit-pre
    callers must explicitly opt in and present both provenance header fields.
    """
    if expected_context_source not in ("prediction", "commit-pre"):
        raise ValueError("invalid expected profile context source")
    with gzip.open(path, "rt") as stream:
        header = json_loads(next(stream))
        expected = {"version": 7, "storage": "finite", "hash_version": 7,
                    "context_encoding": contract.CONTEXT_ENCODING,
                    "mask_order": contract.MASK_ORDER, "history": contract.HISTORY,
                    "layout": contract.LAYOUT}
        if (header.get("kind") != "header" or
                header.get("format") != "h2p-hint-profile" or
                any(not contract._same_typed(header.get(k), v)
                    for k, v in expected.items())):
            raise ValueError("profile header does not match finite Hint v7 spec")
        provenance_fields = {name for name in
                             ("profile_context_source", "online_context_source")
                             if name in header}
        if provenance_fields and len(provenance_fields) != 2:
            raise ValueError("profile header has incomplete context provenance")
        source = header.get("profile_context_source", "prediction")
        online = header.get("online_context_source", "prediction")
        if source != expected_context_source or online != "prediction":
            raise ValueError("profile context source mismatch")
        if expected_context_source == "commit-pre" and (
                header.get("profile_context_source") != "commit-pre" or
                header.get("online_context_source") != "prediction"):
            raise ValueError("commit-pre profile lacks explicit context provenance")
        pcs = [contract.address_word(int(pc, 16)) for pc in header["pcs"]]
        if len(pcs) > 16 or len(set(pcs)) != len(pcs):
            raise ValueError("one physical CAM profile needs <=16 unique PCs")
        if type(header.get("clock_period")) is not int or header["clock_period"] <= 0:
            raise ValueError("invalid clock period")
        rows, previous_seq, previous_commit = [], -1, -1
        for line in stream:
            row = json_loads(line)
            if row.get("kind") != "branch":
                raise ValueError("expected profile branch")
            for key in ("pc", "query_pc", "actual", "baseline_pc", "block_start", "block_end"):
                row[key] = address(row[key])
            row["baseline_target"] = int(row["baseline_target"], 16)
            if not 0 <= row["baseline_target"] <= contract.MASK64:
                raise ValueError("baseline target out of 64-bit range")
            if (contract.address_word(row["query_pc"]) not in pcs or
                    type(row.get("seq")) is not int or row["seq"] <= previous_seq or
                    type(row.get("commit_cycle")) is not int or
                    row["commit_cycle"] < previous_commit):
                raise ValueError("profile sequence/order/PC mismatch")
            previous_seq, previous_commit = row["seq"], row["commit_cycle"]
            if (type(row.get("query_valid")) is not bool or
                    type(row.get("eligible")) is not bool or
                    type(row.get("baseline_valid")) is not bool):
                raise ValueError("profile eligibility must be boolean")
            query = row.get("query_cycle")
            if query is not None and (type(query) is not int or query > row["commit_cycle"]):
                raise ValueError("invalid query cycle")
            contexts = row.get("contexts")
            if contexts is not None:
                _validate_profile_context(contexts, row.get("history"))
            if source == "commit-pre":
                if not {"prediction_contexts", "prediction_history"} <= row.keys():
                    raise ValueError("commit-pre row lacks prediction diagnostics")
                predicted = row["prediction_contexts"]
                predicted_history = row["prediction_history"]
                if predicted is not None:
                    _validate_profile_context(predicted, predicted_history)
                elif predicted_history is not None:
                    raise ValueError("null prediction context has non-null history")
                if row["query_valid"] and predicted is None:
                    raise ValueError("query_valid row lacks prediction context evidence")
            if "profile_context_source" in header:
                # Retain provenance on decoded rows, preventing accidental use
                # of commit histories with prediction-labelled metadata.
                if row.get("profile_context_source", source) != source:
                    raise ValueError("profile row/header context source mismatch")
                if row.get("online_context_source", online) != online:
                    raise ValueError("profile row/header online context source mismatch")
                row["profile_context_source"] = source
                row["online_context_source"] = online
            if row["query_pc"] != row["block_start"]:
                raise ValueError("profile query PC differs from window start")
            if row["query_valid"] and (contexts is None or query is None):
                raise ValueError("query_valid profile row lacks a valid query")
            if row["eligible"] and (not row["query_valid"] or
                    not row["baseline_valid"] or row["baseline_pc"] != row["pc"] or
                    not row["block_start"] <= row["pc"] < row["block_end"]):
                raise ValueError("eligible profile row lacks a matching baseline branch")
            if "window_delta" in row:
                delta = (row["pc"] - row["query_pc"]) // 2
                if type(row["window_delta"]) is not int or not -64 <= delta <= 63 or row["window_delta"] != delta:
                    raise ValueError("invalid signed halfword window delta")
            rows.append(row)
    return header, rows


def merge_profiles(paths, expected_pcs, *, expected_context_source="prediction"):
    """Merge independently audited collect groups covering at most 16 start PCs."""
    if len(expected_pcs) > 16 or len(set(expected_pcs)) != len(expected_pcs):
        raise ValueError("Discovery must select at most 16 unique start PCs")
    groups = [read_profile(path, expected_context_source=expected_context_source)
              for path in paths]
    if not groups:
        raise ValueError("no profiles")
    if len({header["clock_period"] for header, _ in groups}) != 1:
        raise ValueError("profile group clock periods differ")
    pcs = [address(pc) for header, _ in groups for pc in header["pcs"]]
    if len(set(pcs)) != len(pcs) or set(pcs) != set(expected_pcs):
        raise ValueError("profile groups must cover top16 exactly once")
    # Independent passes may restart their sequence counter at zero. Table
    # construction scores each committed observation; it does not depend on
    # cross-pass retirement order. Preserve the collector's sequence in each
    # group and use a deterministic group/sequence merge for diagnostics.
    rows = [row for _, group in groups for row in group]
    return {**groups[0][0], "pcs": [hex(pc) for pc in pcs], "merged": True}, rows


def baseline_miss(row):
    return not row["baseline_valid"] or row["baseline_target"] != row["actual"]


def query_eligible(row, masks):
    return (row["query_valid"] and row["eligible"] and row["contexts"] is not None and
            contract.select_cam_pc(row["block_start"], list(masks)) == row["query_pc"])


def build_image(rows, masks, source, *, stable_fill=False,
                stable_fill_min_observations=1):
    """Build the positive-gain table, optionally filling unused ways.

    Stable fill adds only observed single-target zero-gain keys belonging to
    already active CAM PCs and reuses existing slots. Positive entries retain
    their physical positions; the default image and report are unchanged.
    """
    source = dict(source)
    source.setdefault("profile_context_source", "prediction")
    source.setdefault("online_context_source", "prediction")
    if (source["profile_context_source"] not in ("prediction", "commit-pre") or
            source["online_context_source"] != "prediction"):
        raise ValueError("invalid image context provenance")
    if any(row.get("profile_context_source", "prediction") !=
           source["profile_context_source"] for row in rows):
        raise ValueError("profile rows and image context provenance differ")
    if type(stable_fill) is not bool:
        raise ValueError("stable_fill must be boolean")
    if (type(stable_fill_min_observations) is not int or
            stable_fill_min_observations < 1):
        raise ValueError("stable_fill_min_observations must be a positive integer")
    if len(masks) > 16 or any(not 0 <= mask < 16 for mask in masks.values()):
        raise ValueError("finite Hint uses <=16 PCs and four-bit masks")
    masks = dict(sorted(masks.items()))
    keys = defaultdict(lambda: {"targets": Counter(), "correct": 0, "pcs": Counter()})
    own_keys = defaultdict(set)
    coverage = Counter()
    for row in rows:
        pc = row["query_pc"]
        if pc not in masks:
            continue
        coverage[(pc, "executions")] += 1
        coverage[(pc, "baseline_misses")] += baseline_miss(row)
        if not query_eligible(row, masks):
            continue
        coverage[(pc, "queried")] += 1
        key = contract.hash_key(pc, tuple(row["contexts"]), masks[pc])
        keys[key]["targets"][row["actual"]] += 1
        keys[key]["correct"] += not baseline_miss(row)
        keys[key]["pcs"][pc] += 1
        own_keys[pc].add(key)
    from .builder import select_table
    layout = contract.LAYOUT
    selected, slots, steps, _ = select_table(keys, layout)
    mapped = {key for _, key, _ in selected}
    slot_ids = {target: i for i, target in enumerate(slots)}
    by_set = defaultdict(list)
    for gain, key, target in selected:
        by_set[key & 2047].append((key, target))
    entries = [{"set": set_id, "way": way, "tag": key >> 11,
                "slot": slot_ids[target], "conf": 3}
               for set_id, values in sorted(by_set.items())
               for way, (key, target) in enumerate(sorted(values))]
    active = [pc for pc in masks if mapped.intersection(own_keys[pc])]
    fill_report = None
    if stable_fill:
        active_pcs = set(active)
        used_ways = defaultdict(set)
        for entry in entries:
            used_ways[entry["set"]].add(entry["way"])
        candidates = []
        rejected = Counter()
        for key, observations in sorted(keys.items()):
            if key in mapped:
                continue
            if not set(observations["pcs"]) <= active_pcs:
                rejected["inactive_cam_source"] += 1
                continue
            if len(observations["targets"]) != 1:
                rejected["multiple_targets"] += 1
                continue
            target, count = next(iter(observations["targets"].items()))
            if count - observations["correct"] != 0:
                rejected["nonzero_gain"] += 1
                continue
            if count < stable_fill_min_observations:
                rejected["too_few_observations"] += 1
                continue
            if target not in slot_ids:
                rejected["target_not_in_slots"] += 1
                continue
            candidates.append((count, key, target))
        additions = []
        for count, key, target in sorted(candidates, key=lambda x: (-x[0], x[1])):
            set_id = key & 2047
            free = sorted(set(range(layout["ways"])) - used_ways[set_id])
            if not free:
                rejected["set_full"] += 1
                continue
            way = free[0]
            entries.append({"set": set_id, "way": way, "tag": key >> 11,
                            "slot": slot_ids[target], "conf": 3})
            used_ways[set_id].add(way)
            mapped.add(key)
            additions.append({"key": key, "target": hex(target),
                              "observations": count, "set": set_id,
                              "way": way, "slot": slot_ids[target]})
        fill_report = {
            "policy": "stable-single-target-zero-gain-existing-slot-free-way-v1",
            "min_observations": stable_fill_min_observations,
            "candidate_keys": len(candidates), "added_keys": len(additions),
            "added_observations": sum(row["observations"] for row in additions),
            "rejected_keys": dict(sorted(rejected.items())), "entries": additions,
        }
    metadata = {"format": "h2p-hint", "version": 7, "storage": "finite",
                "hash_version": 7, "context_encoding": contract.CONTEXT_ENCODING,
                "mask_order": contract.MASK_ORDER,
                "history": contract.HISTORY, "layout": layout,
                "cam": [{"pc": hex(pc), "mask": masks[pc]} for pc in active],
                "slots": [hex(target) for target in slots],
                "entries": entries, "source": source}
    image = contract.pack_image(metadata)
    report = {"format": "h2p-hint-builder", "version": 7, "storage": "finite",
              "loaded_pcs": len(active), "observed_keys": len(keys),
              "mapped_keys": len(entries), "slot_targets": len(slots),
              "estimated_net_correction_before_confidence": sum(x[0] for x in selected),
              "greedy_steps": steps, "image_sha256": hashlib.sha256(image).hexdigest(),
              "pcs": [{"pc": hex(pc), "mask": masks[pc],
                       "executions": coverage[(pc, "executions")],
                       "baseline_misses": coverage[(pc, "baseline_misses")],
                       "queried": coverage[(pc, "queried")],
                       "mapped_keys": len(mapped.intersection(own_keys[pc])),
                       "loaded": pc in active,
                       "reasons": [] if pc in active else ["no_mapping"]}
                      for pc in masks]}
    if fill_report is not None:
        report["stable_fill"] = fill_report
    return metadata, image, report


def score_image(rows, metadata):
    """Fixed-conf=3 score, not a replay of dynamic confidence/timing."""
    cam = {int(row["pc"], 16): row["mask"] for row in metadata["cam"]}
    slots = [int(target, 16) for target in metadata["slots"]]
    table = {(entry["set"], entry["tag"]): slots[entry["slot"]]
             for entry in metadata["entries"]}
    per_pc = defaultdict(Counter)
    for row in rows:
        pc = row["query_pc"]
        if pc not in cam:
            continue
        counters = per_pc[pc]
        counters["executions"] += 1
        before = baseline_miss(row)
        after = before
        counters["baseline_misses"] += before
        if query_eligible(row, cam):
            key = contract.hash_key(pc, tuple(row["contexts"]), cam[pc])
            target = table.get((key & 2047, key >> 11))
            if target is not None:
                counters["tag_hits"] += 1
                counters["overrides"] += 1
                after = target != row["actual"]
        counters["corrected"] += before and not after
        counters["harmed"] += not before and after
        counters["final_misses"] += after
    return {"pcs": [{"pc": hex(pc), **dict(counts)}
                    for pc, counts in sorted(per_pc.items())]}


def choose_best(scores, cam_limit=16):
    """PC best mask tie: fewer mask bits, then lower numeric mask."""
    choices = {}
    for pc, mask, row in scores:
        rank = (row["corrected"] - row["harmed"], -mask.bit_count(), -mask)
        if pc not in choices or rank > choices[pc][0]:
            choices[pc] = (rank, mask, row)
    ranked = sorted(choices, key=lambda pc: (-choices[pc][0][0], pc))
    selected = [pc for pc in ranked if choices[pc][0][0] > 0][:cam_limit]
    report = [{"pc": hex(pc), "mask": choices[pc][1],
               "corrected": choices[pc][2]["corrected"],
               "harmed": choices[pc][2]["harmed"],
               "net_correction": choices[pc][0][0],
               "reason": ("selected" if pc in selected else
                          "no_positive_gain" if choices[pc][0][0] <= 0 else "cam_capacity")}
              for pc in ranked]
    return {pc: choices[pc][1] for pc in selected}, report


def main(argv=None):
    """Build one finite image from xs-gem5 or RTL independently collected traces."""
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, action="append", required=True)
    parser.add_argument("--profile-context-source",
                        choices=("prediction", "commit-pre"), default="prediction")
    parser.add_argument("--masks", type=Path, required=True,
                        help="JSON object mapping hexadecimal PC to mask 0..15")
    parser.add_argument("--source", type=Path, required=True,
                        help="JSON provenance with workload/checkpoint/profile/builder digests")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    masks = {address(pc): value for pc, value in json_loads(args.masks.read_text()).items()}
    collected = [address(pc) for path in args.profile for pc in read_profile(path, expected_context_source=args.profile_context_source)[0]["pcs"]]
    if not set(masks) <= set(collected):
        raise ValueError("mask PC was not independently profiled")
    _, rows = merge_profiles(args.profile, collected,
                             expected_context_source=args.profile_context_source)
    source = json_loads(args.source.read_text())
    if (source.get("profile_context_source", "prediction") != args.profile_context_source or
            source.get("online_context_source", "prediction") != "prediction"):
        raise ValueError("source file context provenance mismatch")
    expected = hashlib.sha256("\n".join(digest(path) for path in args.profile).encode()).hexdigest()
    if source.get("profile_sha256") != expected or source.get("builder_sha256") != digest(__file__):
        raise ValueError("profile/builder provenance digest mismatch")
    metadata, image, report = build_image(rows, masks, source)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    (args.output / "hint-context-v7.bin").write_bytes(image)
    (args.output / "builder.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    (args.output / "fixed-conf3-estimate.json").write_text(
        json.dumps(score_image(rows, metadata), indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
