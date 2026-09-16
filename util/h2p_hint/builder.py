"""Deterministic slot selection with real 28-bit aliases and finite ways."""

from collections import Counter, defaultdict
import json
from pathlib import Path

from .model import (
    LAYOUT,
    address,
    baseline_miss,
    context_key,
    contract,
    query_eligible,
    sha256,
    validate_metadata,
    write_json,
)


def candidates_from_stage1(h2p_path, destination, *, ideal=False):
    rows = []
    for number, line in enumerate(Path(h2p_path).read_text().splitlines(), 1):
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 3:
            raise ValueError(f"{h2p_path}:{number}: invalid H2P record")
        pc, misses = address(fields[0]), int(fields[1])
        if misses <= 0:
            raise ValueError("stage 1 must contain only positive-miss PCs")
        rows.append((pc, misses))
    if len({pc for pc, _ in rows}) != len(rows):
        raise ValueError("duplicate stage 1 PC")
    rows.sort(key=lambda item: (-item[1], item[0]))
    if not ideal:
        rows = rows[:20]
    data = {
        "format": "h2p-hint-candidates",
        "version": 3 if ideal else 2,
        "pcs": [hex(pc) for pc, _ in rows],
        "source": {
            "h2p_file": str(Path(h2p_path).resolve()),
            "h2p_sha256": sha256(h2p_path),
            "ranking": [
                {"pc": hex(pc), "misses": misses} for pc, misses in rows
            ],
        },
    }
    if ideal:
        data["storage"] = "unlimited"
    if destination is not None:
        write_json(destination, data)
    return data


def histogram(rows, masks):
    """For any target, net correction = its observations - baseline correct.

    No full PC or extra context enters the key. Cross-PC 28-bit collisions
    intentionally share counts and a physical entry.
    """
    keys = {}
    ordered_cam = tuple(sorted(masks))
    pcs = {pc: Counter() for pc in masks}
    per_pc_keys = {pc: Counter() for pc in masks}
    for row in rows:
        pc = row["pc"]
        if pc not in masks:
            continue
        counts = pcs[pc]
        counts["executions"] += 1
        counts["baseline_misses"] += baseline_miss(row)
        counts["eligible"] += row["eligible"]
        if not query_eligible(row, ordered_cam):
            counts["cam_arbitration_lost"] += bool(row["eligible"])
            continue
        counts["queried"] += 1
        key = context_key(pc, row["contexts"], masks[pc])
        entry = keys.setdefault(
            key, {"targets": Counter(), "correct": 0, "pcs": Counter()}
        )
        entry["targets"][row["actual"]] += 1
        entry["correct"] += not baseline_miss(row)
        entry["pcs"][pc] += 1
        per_pc_keys[pc][key] += 1
    return keys, pcs, per_pc_keys


def _best(rows, ways):
    # rows are (score, key, target). Deduplicate keys before ranking.
    best = {}
    for score, key, target in rows:
        old = best.get(key)
        if score > 0 and (
            old is None
            or score > old[0]
            or score == old[0]
            and target < old[2]
        ):
            best[key] = (score, key, target)
    return sorted(best.values(), key=lambda row: (-row[0], row[1], row[2]))[
        :ways
    ]


def select_table(keys, layout):
    """Greedy globally shared targets, scored under the actual ways limit.

    Each candidate target visits only keys where it has a positive gain. A
    marginal score needs the current top ways and that target's improvements;
    no scan of every key in an affected set is necessary. Scores only increase.
    """
    sets = layout["context_entries"] // layout["ways"]
    by_target = defaultdict(lambda: defaultdict(list))
    for key, entry in keys.items():
        for target, observations in entry["targets"].items():
            gain = observations - entry["correct"]
            if gain > 0:
                by_target[target][key & (sets - 1)].append((gain, key, target))
    current_sets, chosen, steps = {}, [], []
    current_values = {}
    remaining = set(by_target)
    for _ in range(layout["slot_entries"]):
        best_target, best_delta = None, 0
        for target in sorted(remaining):
            delta = 0
            for set_id, rows in by_target[target].items():
                current = current_sets.get(set_id, [])
                updated = _best(current + rows, layout["ways"])
                delta += sum(row[0] for row in updated) - sum(
                    row[0] for row in current
                )
            if delta > best_delta:
                best_target, best_delta = target, delta
        if best_target is None:
            break
        remaining.remove(best_target)
        chosen.append(best_target)
        for set_id, rows in by_target[best_target].items():
            current_sets[set_id] = _best(
                current_sets.get(set_id, []) + rows, layout["ways"]
            )
            for score, key, target in rows:
                old = current_values.get(key)
                if (
                    old is None
                    or score > old[0]
                    or score == old[0]
                    and target < old[1]
                ):
                    current_values[key] = (score, target)
        steps.append(
            {"target": hex(best_target), "marginal_net_correction": best_delta}
        )
    entries = [
        row for set_id in sorted(current_sets) for row in current_sets[set_id]
    ]
    # A previously selected target may no longer be referenced after later
    # improvements. Remove it, but never replace it by an unscored extra slot.
    referenced_targets = sorted({target for _, _, target in entries})
    return entries, referenced_targets, steps, current_values


def build_image(
    rows, masks, *, layout=None, source=None, prune_cam=False, ideal=False
):
    if ideal:
        return build_unlimited_image(
            rows, masks, layout=layout, source=source, prune_cam=prune_cam
        )
    layout = dict(layout or LAYOUT)
    if len(masks) > layout["cam_entries"]:
        raise ValueError("group exceeds finite CAM capacity")
    masks = {address(pc): int(mask) for pc, mask in masks.items()}
    if any(not 0 <= mask < 32 for mask in masks.values()):
        raise ValueError("mask exceeds five bits")
    keys, pc_stats, per_pc_keys = histogram(rows, masks)
    table, slots, steps, selected_values = select_table(keys, layout)
    sets = layout["context_entries"] // layout["ways"]
    index_bits = sets.bit_length() - 1
    slot_ids = {target: index for index, target in enumerate(slots)}
    mapped_keys = {key for _, key, _ in table}
    cam_pcs = [
        pc
        for pc in masks
        if not prune_cam or mapped_keys.intersection(per_pc_keys[pc])
    ]
    metadata = {
        "format": "h2p-hint",
        **contract(layout),
        "cam": [
            {"pc": hex(pc), "mask": masks[pc], "valid": True}
            for pc in sorted(cam_pcs)
        ],
        "slots": [hex(target) for target in slots],
        "entries": [],
        "source": dict(source or {}),
    }
    by_set = defaultdict(list)
    for gain, key, target in table:
        by_set[key & (sets - 1)].append((key, target))
    for set_id, rows in sorted(by_set.items()):
        for way, (key, target) in enumerate(sorted(rows)):
            metadata["entries"].append(
                {
                    "set": set_id,
                    "way": way,
                    "tag": key >> index_bits,
                    "slot": slot_ids[target],
                    "conf": 3,
                    "valid": True,
                }
            )
    validate_metadata(metadata)
    hard = []
    for pc, counts in sorted(pc_stats.items()):
        own_keys = set(per_pc_keys[pc])
        positive_keys = {
            key
            for key in own_keys
            if any(
                count > keys[key]["correct"]
                for count in keys[key]["targets"].values()
            )
        }
        selected_keys = own_keys.intersection(selected_values)
        reasons = []
        if counts["queried"] < counts["executions"]:
            reasons.append("query_coverage")
        if not positive_keys:
            reasons.append("no_positive_gain")
        if positive_keys - selected_keys:
            reasons.append(
                "shared_target_capacity"
                if len(steps) == layout["slot_entries"]
                else "set_conflict"
            )
        if selected_keys - mapped_keys:
            reasons.append("set_conflict")
        hard.append(
            {
                "pc": hex(pc),
                "mask": masks[pc],
                **dict(counts),
                "keys": len(own_keys),
                "mapped_keys": len(own_keys & mapped_keys),
                "reasons": sorted(set(reasons)),
                "loaded": pc in cam_pcs,
            }
        )
    report = {
        "format": "h2p-hint-builder",
        "version": 2,
        "qualification": "same-slice offline specialization",
        "source": metadata["source"],
        "layout": layout,
        "candidate_pcs": len(masks),
        "loaded_pcs": len(cam_pcs),
        "observed_keys": len(keys),
        "mapped_keys": len(table),
        "cross_pc_alias_keys": sum(
            len(row["pcs"]) > 1 for row in keys.values()
        ),
        "slot_targets": len(slots),
        "estimated_net_correction_before_confidence": sum(
            row[0] for row in table
        ),
        "greedy_steps": steps,
        "pcs": hard,
    }
    return metadata, report


def select_joint(replays, cam_entries=16):
    """Use actual replay per-PC net correction, never sum group best results."""
    choices = {}
    for mask, stats in replays:
        for row in stats["pcs"]:
            pc = address(row["pc"])
            gain = row["corrected"] - row["harmed"]
            candidate = (gain, -int(mask).bit_count(), -mask)
            if pc not in choices or candidate > choices[pc][0]:
                choices[pc] = (candidate, mask, row)
    ranked = sorted(choices, key=lambda pc: (-choices[pc][0][0], pc))
    selected = [pc for pc in ranked if choices[pc][0][0] > 0][:cam_entries]
    report = [
        {
            "pc": hex(pc),
            "mask": choices[pc][1],
            "net_correction": choices[pc][0][0],
            "loaded_candidate": pc in selected,
            "reason": (
                "selected"
                if pc in selected
                else "no_positive_gain"
                if choices[pc][0][0] <= 0
                else "cam_capacity"
            ),
        }
        for pc in ranked
    ]
    return {pc: choices[pc][1] for pc in selected}, report


def build_unlimited_image(
    rows, masks, *, layout=None, source=None, prune_cam=False
):
    """Best positive mapping for each real 28-bit key, without capacity cuts.

    The CAM arbitration still filters samples before keys merge. All PC and
    context aliases remain shared, including their later confidence updates.
    """
    masks = {address(pc): int(mask) for pc, mask in masks.items()}
    if any(not 0 <= mask < 32 for mask in masks.values()):
        raise ValueError("mask exceeds five bits")
    encoding = contract(layout, ideal=True)
    keys, pc_stats, per_pc_keys = histogram(rows, masks)
    selected = {}
    for key, counts in keys.items():
        target, observations = min(
            counts["targets"].items(), key=lambda item: (-item[1], item[0])
        )
        gain = observations - counts["correct"]
        if gain > 0:
            selected[key] = (target, gain)
    mapped = set(selected)
    slots = sorted({target for target, _ in selected.values()})
    slot_ids = {target: index for index, target in enumerate(slots)}
    cam = [
        pc
        for pc in sorted(masks)
        if not prune_cam or mapped.intersection(per_pc_keys[pc])
    ]
    metadata = {
        "format": "h2p-hint",
        **encoding,
        "cam": [
            {"valid": True, "pc": hex(pc), "mask": masks[pc]} for pc in cam
        ],
        "slots": [hex(target) for target in slots],
        "entries": [
            {"valid": True, "key": key, "slot": slot_ids[target], "conf": 3}
            for key, (target, _) in sorted(selected.items())
        ],
        "source": dict(source or {}),
    }
    validate_metadata(metadata, ideal=True)
    hard = []
    for pc, counts in sorted(pc_stats.items()):
        own_keys = set(per_pc_keys[pc])
        reasons = []
        if counts["queried"] < counts["executions"]:
            reasons.append("query_coverage")
        if not own_keys.intersection(mapped):
            reasons.append("no_positive_gain")
        hard.append(
            {
                "pc": hex(pc),
                "mask": masks[pc],
                **dict(counts),
                "keys": len(own_keys),
                "mapped_keys": len(own_keys & mapped),
                "reasons": reasons,
                "loaded": pc in cam,
            }
        )
    report = {
        "format": "h2p-hint-builder",
        "version": 3,
        "storage": "unlimited",
        "qualification": "same-slice offline specialization; fixed history/hash/ports/timing/builder",
        "source": metadata["source"],
        "layout": encoding["layout"],
        "candidate_pcs": len(masks),
        "loaded_pcs": len(cam),
        "observed_keys": len(keys),
        "mapped_keys": len(mapped),
        "cross_pc_alias_keys": sum(
            len(row["pcs"]) > 1 for row in keys.values()
        ),
        "slot_targets": len(slots),
        "estimated_net_correction_before_confidence": sum(
            gain for _, gain in selected.values()
        ),
        "greedy_steps": [],
        "pcs": hard,
    }
    return metadata, report
