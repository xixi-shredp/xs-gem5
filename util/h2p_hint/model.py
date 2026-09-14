"""Independent reference for the version 2 Hint encoding and finite hardware.

The trace reference keeps the original prediction-time contexts and cycles. It
does not predict changes in speculative history or timing caused by replay.
Only actual gem5 replay determines the benefit of a frozen image.
"""

from collections import deque
from bisect import bisect_left
from dataclasses import dataclass, field
import gzip
import hashlib
import json
from pathlib import Path
import re


BITS = 28
MASK28 = (1 << BITS) - 1
MASK47 = (1 << 47) - 1
MASK64 = (1 << 64) - 1
MASK_ORDER = ["GHR", "GITHR", "LITHR", "Call", "Return"]
HISTORY = {"githr_targets": 64, "lithr_targets": 4, "call_pcs": 64,
           "return_pcs": 64, "target_shift": 1}
LAYOUT = {"cam_entries": 16, "context_entries": 4096, "ways": 2,
          "slot_entries": 32, "conf_queue_depth": 8}


def integer(value):
    if type(value) not in (str, int):
        raise ValueError("expected integer or address string")
    return int(value, 0) if isinstance(value, str) else value


def address(value):
    value = integer(value)
    if value < 0 or value > MASK64 or value & 1:
        raise ValueError(f"invalid even 64-bit address {value}")
    low = value & ((1 << 48) - 1)
    canonical = low | (MASK64 ^ ((1 << 48) - 1)) if low >> 47 else low
    if value != canonical:
        raise ValueError(f"address cannot reconstruct from bits [47:1]: {value:#x}")
    return value


def prediction_target(value):
    """Keep all raw predictor bits, including wrong odd/noncanonical targets."""
    value = integer(value)
    if not 0 <= value <= MASK64:
        raise ValueError(f"prediction target is not an unsigned 64-bit value: {value}")
    return value


def json_address(value):
    if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-fA-F]{1,16}", value):
        raise ValueError("address must be a hexadecimal JSON string")
    return address(value)


def json_loads(text):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON field {key}")
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=unique_object)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def fold28(value):
    if value < 0:
        raise ValueError("history must be unsigned")
    result = 0
    while value:
        result ^= value & MASK28
        value >>= BITS
    return result


def rotl28(value, shift):
    shift %= BITS
    return ((value << shift) | (value >> ((BITS - shift) % BITS))) & MASK28


def context_key(pc, contexts, mask):
    if not 0 <= mask < 32 or len(contexts) != 5:
        raise ValueError("five contexts and a 5-bit mask are required")
    result = fold28((pc >> 1) & MASK47)
    for index, value in enumerate(contexts):
        if not 0 <= value <= MASK28:
            raise ValueError("profile context is not an unsigned 28-bit fold")
        if mask & (1 << index):
            result ^= rotl28(value, index + 1)
    return result


@dataclass
class History:
    """Unbounded Python integers represent exactly the declared hardware bits."""

    pcs: tuple = ()
    ghr: int = 0
    githr: int = 0
    lithr: dict = field(default_factory=dict)
    calls: deque = field(default_factory=lambda: deque(maxlen=64))
    returns: deque = field(default_factory=lambda: deque(maxlen=64))

    def __post_init__(self):
        if len(set(self.pcs)) != len(self.pcs) or len(self.pcs) > 20:
            raise ValueError("history needs at most 20 distinct PCs")
        self.lithr = {pc: self.lithr.get(pc, 0) for pc in self.pcs}

    def snapshot(self):
        return (self.ghr, self.githr, dict(self.lithr),
                tuple(self.calls), tuple(self.returns))

    def restore(self, snapshot):
        self.ghr, self.githr, lithr, calls, returns = snapshot
        self.lithr = dict(lithr)
        self.calls = deque(calls, maxlen=64)
        self.returns = deque(returns, maxlen=64)

    def update(self, pc, target, *, taken, conditional=False,
               indirect=False, call=False, ret=False):
        if conditional:
            self.ghr = ((self.ghr << 1) | bool(taken)) & ((1 << 970) - 1)
        if not taken:
            return
        if indirect and not ret:
            code = (target >> 1) & 3
            self.githr = ((self.githr << 2) | code) & ((1 << 128) - 1)
            if pc in self.lithr:
                self.lithr[pc] = ((self.lithr[pc] << 2) | code) & 255
        if call:
            self.calls.appendleft((pc >> 1) & MASK47)
        if ret:
            self.returns.appendleft((pc >> 1) & MASK47)

    def contexts(self, pc):
        def fold_pcs(pcs):
            value = 0
            for age, historical_pc in enumerate(pcs):
                value ^= rotl28(fold28(historical_pc), age)
            return value
        return (fold28(self.ghr), fold28(self.githr),
                fold28(self.lithr.get(pc, 0)), fold_pcs(self.calls),
                fold_pcs(self.returns))


def validate_contract(data):
    if not isinstance(data, dict):
        raise ValueError("Hint JSON root must be an object")
    for key, expected in (("version", 2), ("hash_version", 2), ("ghr_bits", 970)):
        if type(data.get(key)) is not int or data[key] != expected:
            raise ValueError(f"incompatible {key}: {data.get(key)!r}")
    for key, expected in (("mask_order", MASK_ORDER),):
        if data.get(key) != expected:
            raise ValueError(f"incompatible {key}: {data.get(key)!r}")
    history = data.get("history")
    if not isinstance(history, dict) or any(
            type(history.get(key)) is not int or history[key] != value
            for key, value in HISTORY.items()):
        raise ValueError("incompatible history encoding")
    layout = data.get("layout", {})
    if not isinstance(layout, dict) or any(
            type(layout.get(key)) is not int or layout[key] < 1 for key in LAYOUT):
        raise ValueError("invalid finite hardware layout")
    entries, ways = layout["context_entries"], layout["ways"]
    sets = entries // ways
    if entries % ways or sets & (sets - 1) or sets >= 1 << BITS:
        raise ValueError("context set count must be a power of two below 2^28")
    if layout["cam_entries"] > 20:
        raise ValueError("CAM capacity exceeds candidate history capacity")
    return layout


def contract(layout=None):
    return {"version": 2, "hash_version": 2, "ghr_bits": 970,
            "mask_order": MASK_ORDER, "history": dict(HISTORY),
            "layout": dict(layout or LAYOUT)}


def validate_metadata(data):
    if not isinstance(data, dict) or data.get("format") != "h2p-hint":
        raise ValueError("not a Hint metadata image")
    layout = validate_contract(data)
    if not isinstance(data.get("source"), dict):
        raise ValueError("metadata source must be a JSON object")
    sets = layout["context_entries"] // layout["ways"]
    tag_limit = 1 << (BITS - (sets.bit_length() - 1))
    cam = data.get("cam", [])
    slots = data.get("slots", [])
    entries = data.get("entries", [])
    if any(not isinstance(data.get(name), list) for name in ("cam", "slots", "entries")):
        raise ValueError("cam, slots, and entries must be JSON arrays")
    if len(cam) > layout["cam_entries"] or len(slots) > layout["slot_entries"]:
        raise ValueError("CAM or target slot capacity exceeded")
    pcs = set()
    for row in cam:
        if not isinstance(row, dict):
            raise ValueError("CAM entry must be a JSON object")
        pc = json_address(row["pc"])
        if pc in pcs or type(row["valid"]) is not bool:
            raise ValueError("duplicate CAM PC or invalid validity field")
        pcs.add(pc)
        if type(row["mask"]) is not int or not 0 <= row["mask"] < 32:
            raise ValueError("mask exceeds five bits")
    targets = [json_address(value) for value in slots]
    if len(targets) != len(set(targets)):
        raise ValueError("shared target slots contain duplicate targets")
    locations, keys = set(), set()
    for row in entries:
        if not isinstance(row, dict) or any(type(row.get(name)) is not int for name in
                                           ("set", "way", "tag", "slot", "conf")):
            raise ValueError("context entry must contain integer position/tag/slot/confidence")
        if type(row.get("valid")) is not bool:
            raise ValueError("entry validity must be a JSON boolean")
        location = (row["set"], row["way"])
        key = (row["set"], row["tag"])
        if location in locations or row["valid"] and key in keys:
            raise ValueError("duplicate SRAM location or set/tag key")
        locations.add(location)
        if row["valid"]:
            keys.add(key)
        if (not 0 <= row["set"] < sets or
                not 0 <= row["way"] < layout["ways"] or
                not 0 <= row["tag"] < tag_limit or not 0 <= row["conf"] <= 3 or
                not 0 <= row["slot"] < layout["slot_entries"] or
                row["valid"] and (row["conf"] != 3 or row["slot"] >= len(slots))):
            raise ValueError("invalid SRAM entry, pointer, or initial confidence")
    return data


def load_metadata(path):
    return validate_metadata(json_loads(Path(path).read_text()))


def load_candidates(path):
    data = json_loads(Path(path).read_text())
    if (not isinstance(data, dict) or data.get("format") != "h2p-hint-candidates" or
            type(data.get("version")) is not int or data["version"] != 2):
        raise ValueError("incompatible candidate file")
    if not isinstance(data.get("source"), dict):
        raise ValueError("candidate source must be a JSON object")
    if not isinstance(data.get("pcs"), list):
        raise ValueError("candidate pcs must be a JSON array")
    pcs = [json_address(pc) for pc in data["pcs"]]
    if len(pcs) > 20 or len(set(pcs)) != len(pcs):
        raise ValueError("candidate file needs at most 20 unique PCs")
    return pcs


def read_profile(path):
    """Read and validate the gzip JSONL format, including correct instances."""
    with gzip.open(path, "rt") as source:
        header = json_loads(next(source))
        if (header.get("kind") != "header" or
                header.get("format") != "h2p-hint-profile"):
            raise ValueError("invalid profile header")
        validate_contract(header)
        if type(header.get("clock_period")) is not int or header["clock_period"] <= 0:
            raise ValueError("profile needs a positive CPU clock period")
        yield header
        previous_seq, previous_commit = -1, -1
        candidates = {address(pc) for pc in header["pcs"]}
        for number, line in enumerate(source, 2):
            row = json_loads(line)
            if row.get("kind") != "branch":
                raise ValueError(f"profile line {number}: expected branch")
            for key in ("pc", "actual", "baseline_pc",
                        "block_start", "block_end"):
                row[key] = address(row[key])
            # A wrong baseline target is an observation of the existing BPU,
            # not an address being installed into the finite Hint tables.
            row["baseline_target"] = prediction_target(row["baseline_target"])
            if row["pc"] not in candidates:
                raise ValueError(f"profile line {number}: non-candidate PC")
            if row["seq"] <= previous_seq or row["commit_cycle"] < previous_commit:
                raise ValueError("profile is not in committed instruction order")
            previous_seq, previous_commit = row["seq"], row["commit_cycle"]
            if type(row["eligible"]) is not bool or type(row["baseline_valid"]) is not bool:
                raise ValueError("profile validity fields must be booleans")
            if row["eligible"]:
                if (row["contexts"] is None or not row["baseline_valid"] or
                        row["baseline_pc"] != row["pc"] or
                        row["query_cycle"] > row["commit_cycle"] or
                        not row["block_start"] <= row["pc"] < row["block_end"]):
                    raise ValueError("eligible sample lacks a valid query snapshot")
                context_key(row["pc"], row["contexts"], 31)
            elif row["contexts"] is not None:
                context_key(row["pc"], row["contexts"], 31)
            yield row


def selected_pc(row, cam_pcs):
    ordered = cam_pcs if isinstance(cam_pcs, tuple) else tuple(sorted(cam_pcs))
    position = bisect_left(ordered, row["block_start"])
    return (ordered[position] if position < len(ordered) and
            ordered[position] < row["block_end"] else None)


def query_eligible(row, cam_pcs):
    return row["eligible"] and selected_pc(row, cam_pcs) == row["pc"]


def baseline_miss(row):
    return not row["baseline_valid"] or row["baseline_target"] != row["actual"]


def empty_counters():
    return dict.fromkeys(("executions", "baseline_misses", "final_misses",
                          "corrected", "harmed", "eligible", "tag_hits",
                          "conf_enabled", "overrides"), 0)


class Reference:
    """One write per cycle, no bypass, bounded FIFO and query-time snapshots."""

    def __init__(self, metadata):
        validate_metadata(metadata)
        self.layout = metadata["layout"]
        self.sets = self.layout["context_entries"] // self.layout["ways"]
        self.index_bits = self.sets.bit_length() - 1
        self.cam = {address(row["pc"]): row["mask"] for row in metadata["cam"]
                    if row["valid"]}
        self.ordered_cam = tuple(sorted(self.cam))
        self.slots = [address(value) for value in metadata["slots"]]
        self.entries = {(row["set"], row["tag"]): dict(row)
                        for row in metadata["entries"] if row["valid"]}
        self.pending = deque()
        self.snapshots = {}
        self.last_write = -1
        self.last_cycle = -1
        self.queue_stats = dict.fromkeys(("enqueued", "applied", "dropped",
                                         "pending", "carried_in", "zero_transitions",
                                         "recoveries", "max_occupancy"), 0)
        self.pcs = {}
        self.transitions = []

    def reset_statistics(self):
        """Stats reset preserves predictor confidence and pending writes."""
        self.queue_stats = dict.fromkeys(self.queue_stats, 0)
        self.queue_stats["carried_in"] = len(self.pending)
        self.queue_stats["max_occupancy"] = len(self.pending)
        self.pcs.clear()
        self.transitions.clear()

    def advance(self, cycle):
        if cycle < self.last_cycle:
            raise ValueError("reference events are not in cycle order")
        while self.pending:
            ready, entry, correct, row = self.pending[0]
            write_cycle = max(ready, self.last_write + 1)
            if write_cycle > cycle:
                break
            self.pending.popleft()
            old = entry["conf"]
            entry["conf"] = min(3, old + 1) if correct else max(0, old - 1)
            self.last_write = write_cycle
            self.queue_stats["applied"] += 1
            if old == 1 and entry["conf"] == 0:
                self.queue_stats["zero_transitions"] += 1
                self.transitions.append({"pc": hex(row["pc"]), "seq": row["seq"],
                                         "commit_cycle": row["commit_cycle"],
                                         "write_cycle": write_cycle})
            if old == 0 and entry["conf"] == 1:
                self.queue_stats["recoveries"] += 1
        self.last_cycle = cycle

    def query(self, row):
        self.advance(row["query_cycle"])
        if not query_eligible(row, self.ordered_cam):
            return
        key = context_key(row["pc"], row["contexts"], self.cam[row["pc"]])
        entry = self.entries.get((key & (self.sets - 1), key >> self.index_bits))
        target = self.slots[entry["slot"]] if entry else None
        self.snapshots[row["seq"]] = (entry, target,
                                      entry["conf"] > 0 if entry else False)

    def commit(self, row):
        self.advance(row["commit_cycle"])
        counters = self.pcs.setdefault(row["pc"], empty_counters())
        counters["executions"] += 1
        base_miss = baseline_miss(row)
        counters["baseline_misses"] += base_miss
        snapshot = self.snapshots.pop(row["seq"], None)
        counters["eligible"] += snapshot is not None
        final_miss = base_miss
        if snapshot:
            entry, target, enabled = snapshot
            if entry:
                counters["tag_hits"] += 1
                counters["conf_enabled"] += enabled
                counters["overrides"] += enabled
                if enabled:
                    final_miss = target != row["actual"]
                if len(self.pending) == self.layout["conf_queue_depth"]:
                    self.queue_stats["dropped"] += 1
                else:
                    self.pending.append((row["commit_cycle"] + 1, entry,
                                         target == row["actual"], row))
                    self.queue_stats["enqueued"] += 1
                    self.queue_stats["max_occupancy"] = max(
                        self.queue_stats["max_occupancy"], len(self.pending))
        counters["final_misses"] += final_miss
        counters["corrected"] += base_miss and not final_miss
        counters["harmed"] += not base_miss and final_miss

    def finish(self, cycle=None):
        if cycle is not None:
            self.advance(cycle)
        self.queue_stats["pending"] = len(self.pending)
        if (self.queue_stats["carried_in"] + self.queue_stats["enqueued"] !=
                self.queue_stats["applied"] + len(self.pending)):
            raise AssertionError("reference confidence queue lost an update")
        total = empty_counters()
        for counters in self.pcs.values():
            for key, value in counters.items():
                total[key] += value
        return {"format": "h2p-hint-reference", "version": 2,
                "qualification": "fixed collect contexts and cycles; not a timing prediction",
                "global": total, "queue": dict(self.queue_stats),
                "pcs": [{"pc": hex(pc), **counters}
                        for pc, counters in sorted(self.pcs.items())],
                "conf0": self.transitions}


def replay_reference(rows, metadata, finish_cycle=None):
    """Convenience driver for short traces; production uses streamed disk data."""
    rows = list(rows)
    events = [(row["commit_cycle"], 1, row["seq"], row) for row in rows]
    events.extend((row["query_cycle"], 0, row["seq"], row)
                  for row in rows if row["eligible"])
    model = Reference(metadata)
    for _, phase, _, row in sorted(events):
        (model.commit if phase else model.query)(row)
    return model.finish(finish_cycle)
