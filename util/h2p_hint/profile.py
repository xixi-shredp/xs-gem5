"""Compact on-disk profile cache, built from each gzip trace once.

The query index handles non-monotonic query times without retaining millions of
Python objects. Two independent iterators merge predictions and retirements.
"""

import json
import mmap
from pathlib import Path
import sqlite3
import struct

from .model import (
    Reference,
    address,
    read_profile,
    sha256,
    validate_contract,
    write_json,
)


RECORD = struct.Struct("<9Q6I")
OFFSET = struct.Struct("<Q")
RECORD_VERSION = 2
QUERY_PRESENT = 1 << 3
FIELDS = (
    "seq",
    "query_cycle",
    "commit_cycle",
    "pc",
    "actual",
    "baseline_target",
    "baseline_pc",
    "block_start",
    "block_end",
)


def unpack(raw):
    values = RECORD.unpack(raw)
    row = dict(zip(FIELDS, values[:9]))
    flags = values[14]
    row.update(
        eligible=bool(flags & 1),
        baseline_valid=bool(flags & 2),
        contexts=list(values[9:14]) if flags & 4 else None,
    )
    if not flags & QUERY_PRESENT:
        row["query_cycle"] = None
    return row


def make_cache(profile, directory):
    profile, directory = Path(profile), Path(directory)
    profile_digest = sha256(profile)
    identity_path = directory / "cache.json"
    if directory.exists():
        if not identity_path.exists():
            raise ValueError(
                f"incomplete profile cache; preserve and inspect {directory}"
            )
        identity = json.loads(identity_path.read_text())
        if (
            identity.get("profile_sha256") != profile_digest
            or identity.get("record_size") != RECORD.size
            or identity.get("record_version") != RECORD_VERSION
        ):
            raise ValueError("profile cache identity mismatch")
        for name in ("records.bin", "queries.bin"):
            if (
                name in identity.get("files", {})
                and sha256(directory / name) != identity["files"][name]
            ):
                raise ValueError("profile cache content changed")
        validate_contract(identity["header"])
        return identity
    directory.mkdir(parents=True)
    source = iter(read_profile(profile))
    header = next(source)
    previous_query, monotonic, count = -1, True, 0
    with (directory / "records.bin").open("wb") as records, (
        directory / "queries.bin"
    ).open("wb") as queries:
        for row in source:
            query_present = row["query_cycle"] is not None
            if not query_present and (
                row["eligible"] or row["contexts"] is not None
            ):
                raise ValueError(
                    "missing query cannot have eligibility or contexts"
                )
            flags = (
                int(row["eligible"])
                | int(row["baseline_valid"]) << 1
                | int(row["contexts"] is not None) << 2
                | (QUERY_PRESENT if query_present else 0)
            )
            values = [row[key] for key in FIELDS]
            # Zero is only an unused storage value when QUERY_PRESENT is clear.
            # Readers restore None; a real cycle-zero query keeps its own flag.
            if not query_present:
                values[1] = 0
            records.write(
                RECORD.pack(*values, *(row["contexts"] or [0] * 5), flags)
            )
            if row["eligible"]:
                queries.write(OFFSET.pack(count * RECORD.size))
                monotonic &= row["query_cycle"] >= previous_query
                previous_query = row["query_cycle"]
            count += 1
    if not monotonic:
        # A rare reordering costs a disk sort, not an unbounded event heap.
        database_path = directory / "query-sort.sqlite"
        connection = sqlite3.connect(database_path)
        connection.execute(
            "CREATE TABLE queries (cycle INTEGER, seq INTEGER, offset INTEGER)"
        )
        with (directory / "records.bin").open("rb") as records:
            batch = []
            for index in range(count):
                row = unpack(records.read(RECORD.size))
                if row["eligible"]:
                    batch.append(
                        (row["query_cycle"], row["seq"], index * RECORD.size)
                    )
                if len(batch) >= 10000:
                    connection.executemany(
                        "INSERT INTO queries VALUES (?,?,?)", batch
                    )
                    batch.clear()
            connection.executemany("INSERT INTO queries VALUES (?,?,?)", batch)
        connection.commit()
        with (directory / "queries.bin").open("wb") as queries:
            for (offset,) in connection.execute(
                "SELECT offset FROM queries ORDER BY cycle, seq"
            ):
                queries.write(OFFSET.pack(offset))
        connection.close()
        database_path.unlink()
    identity = {
        "format": "h2p-hint-profile-cache",
        "version": 2,
        "profile": str(profile.resolve()),
        "profile_sha256": profile_digest,
        "record_size": RECORD.size,
        "record_version": RECORD_VERSION,
        "records": count,
        "query_times_monotonic": monotonic,
        "header": header,
    }
    identity["files"] = {
        name: sha256(directory / name)
        for name in ("records.bin", "queries.bin")
    }
    write_json(identity_path, identity)
    return identity


class Cache:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.identity = json.loads((self.directory / "cache.json").read_text())
        if (
            self.identity.get("record_version") != RECORD_VERSION
            or self.identity.get("record_size") != RECORD.size
        ):
            raise ValueError("profile cache record encoding mismatch")
        validate_contract(self.identity["header"])

    def rows(self):
        with (self.directory / "records.bin").open("rb") as source:
            while raw := source.read(RECORD.size):
                yield unpack(raw)

    def queries(self):
        if not self.identity["records"]:
            return
        with (self.directory / "records.bin").open("rb") as source, (
            self.directory / "queries.bin"
        ).open("rb") as index:
            with mmap.mmap(
                source.fileno(), 0, access=mmap.ACCESS_READ
            ) as records:
                while raw := index.read(OFFSET.size):
                    offset = OFFSET.unpack(raw)[0]
                    yield unpack(records[offset : offset + RECORD.size])

    def reference(self, metadata, finish_cycle=None):
        queries = iter(self.queries())
        next_query = next(queries, None)
        model = Reference(metadata)
        for committed in self.rows():
            while (
                next_query
                and next_query["query_cycle"] <= committed["commit_cycle"]
            ):
                model.query(next_query)
                next_query = next(queries, None)
            model.commit(committed)
        if next_query is not None:
            raise ValueError("query is later than its retirement")
        return model.finish(finish_cycle)


def compare_shared_profiles(old_directory, new_directory, max_examples=8):
    """Stream the shared PCs in commit order; preserve mismatch diagnostics.

    Compare all prediction-time fields, not just target histograms. Profiles
    may use different headers/candidate sets, but the shared executions must
    still agree for a transparent observer and the same baseline behavior.
    """
    import hashlib
    from itertools import zip_longest

    old, new = Cache(old_directory), Cache(new_directory)
    common = {address(pc) for pc in old.identity["header"]["pcs"]} & {
        address(pc) for pc in new.identity["header"]["pcs"]
    }
    old_rows = (row for row in old.rows() if row["pc"] in common)
    new_rows = (row for row in new.rows() if row["pc"] in common)
    digests = [hashlib.sha256(), hashlib.sha256()]
    counts, differences, examples = [0, 0], 0, []
    for left, right in zip_longest(old_rows, new_rows):
        for index, row in enumerate((left, right)):
            if row is not None:
                counts[index] += 1
                digests[index].update(
                    (json.dumps(row, sort_keys=True) + "\n").encode()
                )
        if left != right:
            differences += 1
            if len(examples) < max_examples:
                examples.append({"old": left, "new": right})
    return {
        "pass": differences == 0,
        "shared_pcs": [hex(pc) for pc in sorted(common)],
        "old_records": counts[0],
        "new_records": counts[1],
        "different_records": differences,
        "examples": examples,
        "old_shared_sha256": digests[0].hexdigest(),
        "new_shared_sha256": digests[1].hexdigest(),
        "old_profile_sha256": old.identity["profile_sha256"],
        "new_profile_sha256": new.identity["profile_sha256"],
    }
