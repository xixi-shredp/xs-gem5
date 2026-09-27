"""Shared finite-v3 profile reader, deterministic builder, and image codec.

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


_CONTRACT_PATH = Path(__file__).resolve().parents[3] / "manifest" / "hint_v3.py"
_SPEC = spec_from_file_location("kmhv3_hint_v3_contract", _CONTRACT_PATH)
if _SPEC is None or _SPEC.loader is None:
    raise ImportError(f"cannot load Hint v3 contract from {_CONTRACT_PATH}")
contract = module_from_spec(_SPEC)
_SPEC.loader.exec_module(contract)


def load_metadata(path):
    metadata = json_loads(Path(path).read_text())
    contract.pack_image(metadata)
    return metadata


def packed_image(path):
    return contract.pack_image(load_metadata(path))


def load_candidates(path):
    data = json_loads(Path(path).read_text())
    if (
        not isinstance(data, dict)
        or data.get("format") != "h2p-hint-candidates"
        or type(data.get("version")) is not int
        or data["version"] != 3
        or data.get("storage") != "finite"
        or not isinstance(data.get("source"), dict)
        or not isinstance(data.get("pcs"), list)
        or len(data["pcs"]) > 16
    ):
        raise ValueError("candidate file is not finite Hint v3")
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


def read_profile(path):
    """Read the simulator-neutral finite-v3 gzip JSONL profile."""
    with gzip.open(path, "rt") as stream:
        header = json_loads(next(stream))
        expected = {"version": 3, "storage": "finite", "hash_version": 3,
                    "mask_order": contract.MASK_ORDER, "history": contract.HISTORY,
                    "layout": contract.LAYOUT}
        if (header.get("kind") != "header" or
                header.get("format") != "h2p-hint-profile" or
                any(type(header.get(k)) is not type(v) or header[k] != v
                    for k, v in expected.items())):
            raise ValueError("profile header does not match finite Hint v3")
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
            for key in ("pc", "actual", "baseline_pc", "block_start", "block_end"):
                row[key] = address(row[key])
            row["baseline_target"] = int(row["baseline_target"], 16)
            if not 0 <= row["baseline_target"] <= contract.MASK64:
                raise ValueError("baseline target out of 64-bit range")
            if (contract.address_word(row["pc"]) not in pcs or
                    type(row.get("seq")) is not int or row["seq"] <= previous_seq or
                    type(row.get("commit_cycle")) is not int or
                    row["commit_cycle"] < previous_commit):
                raise ValueError("profile sequence/order/PC mismatch")
            previous_seq, previous_commit = row["seq"], row["commit_cycle"]
            if (type(row.get("eligible")) is not bool or
                    type(row.get("baseline_valid")) is not bool):
                raise ValueError("profile eligibility must be boolean")
            query = row.get("query_cycle")
            if query is not None and (type(query) is not int or query > row["commit_cycle"]):
                raise ValueError("invalid query cycle")
            contexts = row.get("contexts")
            if contexts is not None and (not isinstance(contexts, list) or
                    len(contexts) != 4 or any(type(x) is not int or
                    not 0 <= x <= contract.MASK28 for x in contexts)):
                raise ValueError("profile requires four folded contexts")
            if row["eligible"] and (contexts is None or query is None or
                    not row["baseline_valid"] or row["baseline_pc"] != row["pc"] or
                    not row["block_start"] <= row["pc"] < row["block_end"]):
                raise ValueError("eligible profile row lacks a valid query")
            rows.append(row)
    return header, rows


def merge_profiles(paths, expected_pcs):
    """Merge disjoint 16+4 collect groups in committed sequence order."""
    groups = [read_profile(path) for path in paths]
    if not groups:
        raise ValueError("no profiles")
    if len({header["clock_period"] for header, _ in groups}) != 1:
        raise ValueError("profile group clock periods differ")
    pcs = [address(pc) for header, _ in groups for pc in header["pcs"]]
    if len(set(pcs)) != len(pcs) or set(pcs) != set(expected_pcs):
        raise ValueError("profile groups must cover top20 exactly once")
    # Independent passes may restart their sequence counter at zero. Table
    # construction scores each committed observation; it does not depend on
    # cross-pass retirement order. Preserve the collector's sequence in each
    # group and use a deterministic group/sequence merge for diagnostics.
    rows = [row for _, group in groups for row in group]
    return {**groups[0][0], "pcs": [hex(pc) for pc in pcs], "merged": True}, rows


def baseline_miss(row):
    return not row["baseline_valid"] or row["baseline_target"] != row["actual"]


def query_eligible(row, masks):
    return (row["eligible"] and row["contexts"] is not None and
            contract.select_cam_pc(row["block_start"], list(masks)) == row["pc"])


def build_image(rows, masks, source):
    """Greedy shared-slot and two-way finite table, scored on actual trace rows."""
    if len(masks) > 16 or any(not 0 <= mask < 16 for mask in masks.values()):
        raise ValueError("finite Hint uses <=16 PCs and four-bit masks")
    masks = dict(sorted(masks.items()))
    keys = defaultdict(lambda: {"targets": Counter(), "correct": 0, "pcs": Counter()})
    own_keys = defaultdict(set)
    coverage = Counter()
    for row in rows:
        pc = row["pc"]
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
    metadata = {"format": "h2p-hint", "version": 3, "storage": "finite",
                "hash_version": 3, "mask_order": contract.MASK_ORDER,
                "history": contract.HISTORY, "layout": layout,
                "cam": [{"pc": hex(pc), "mask": masks[pc]} for pc in active],
                "slots": [hex(target) for target in slots],
                "entries": entries, "source": source}
    image = contract.pack_image(metadata)
    report = {"format": "h2p-hint-builder", "version": 3, "storage": "finite",
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
    return metadata, image, report


def score_image(rows, metadata):
    """Fixed-conf=3 score, not a replay of dynamic confidence/timing."""
    cam = {int(row["pc"], 16): row["mask"] for row in metadata["cam"]}
    slots = [int(target, 16) for target in metadata["slots"]]
    table = {(entry["set"], entry["tag"]): slots[entry["slot"]]
             for entry in metadata["entries"]}
    per_pc = defaultdict(Counter)
    for row in rows:
        pc = row["pc"]
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
    parser.add_argument("--masks", type=Path, required=True,
                        help="JSON object mapping hexadecimal PC to mask 0..15")
    parser.add_argument("--source", type=Path, required=True,
                        help="JSON provenance with workload/checkpoint/profile/builder digests")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    masks = {address(pc): value for pc, value in json_loads(args.masks.read_text()).items()}
    collected = [address(pc) for path in args.profile for pc in read_profile(path)[0]["pcs"]]
    if not set(masks) <= set(collected):
        raise ValueError("mask PC was not independently profiled")
    _, rows = merge_profiles(args.profile, collected)
    source = json_loads(args.source.read_text())
    expected = hashlib.sha256("\n".join(digest(path) for path in args.profile).encode()).hexdigest()
    if source.get("profile_sha256") != expected or source.get("builder_sha256") != digest(__file__):
        raise ValueError("profile/builder provenance digest mismatch")
    metadata, image, report = build_image(rows, masks, source)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    (args.output / "hint-v3.bin").write_bytes(image)
    (args.output / "builder.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    (args.output / "fixed-conf3-estimate.json").write_text(
        json.dumps(score_image(rows, metadata), indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
