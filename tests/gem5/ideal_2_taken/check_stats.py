#!/usr/bin/env python3
"""Validate ideal 2-taken statistics from a completed gem5 run."""

import argparse
import re
from pathlib import Path


DISTRIBUTIONS = (
    "idealTwoTakenQueriesPerCycle",
    "idealTwoTakenEnqueuesPerCycle",
    "idealTwoTakenTakenPerCycle",
)
SECOND_SLOT = "idealTwoTakenSecondSlotQueries"


def read_stats(path):
    values = {}
    line_re = re.compile(r"^(\S+)\s+(-?(?:\d+(?:\.\d*)?|\.\d+))\s")
    for line in path.read_text(encoding="utf-8").splitlines():
        match = line_re.match(line)
        if match:
            values[match.group(1)] = float(match.group(2))
    return values


def matching_stats(values, short_name):
    marker = f".{short_name}"
    return {
        name: value
        for name, value in values.items()
        if marker in name and (
            name.endswith(short_name) or marker + "::" in name
        )
    }


def distribution_bucket(name, short_name):
    suffix = name.split(f".{short_name}::", 1)[1]
    return int(suffix) if suffix.isdigit() else None


def stat_prefix(name, short_name):
    return name.split(f".{short_name}", 1)[0]


def validate(stats, require_second_slot=False, require_two_enqueues=False):
    errors = []
    found = {}

    for short_name in DISTRIBUTIONS:
        matches = matching_stats(stats, short_name)
        if not matches:
            errors.append(f"missing distribution: {short_name}")
            continue
        found[short_name] = matches

        for full_name, value in matches.items():
            bucket = distribution_bucket(full_name, short_name)
            if bucket is not None and bucket > 2 and value != 0:
                errors.append(
                    f"{short_name} observed forbidden bucket {bucket}: {value:g}"
                )
            if full_name.endswith("::overflows") and value != 0:
                errors.append(
                    f"{short_name} overflowed its [0, 2] range: {value:g}"
                )

    second_slot_stats = matching_stats(stats, SECOND_SLOT)
    if not second_slot_stats:
        errors.append(f"missing scalar: {SECOND_SLOT}")
    else:
        if require_second_slot and max(second_slot_stats.values()) <= 0:
            errors.append("no second-slot lookup was observed")

        query_stats = found.get("idealTwoTakenQueriesPerCycle", {})
        for second_name, second_count in second_slot_stats.items():
            prefix = stat_prefix(second_name, SECOND_SLOT)
            query_distribution = {
                name: value
                for name, value in query_stats.items()
                if stat_prefix(name, "idealTwoTakenQueriesPerCycle") == prefix
            }
            if not query_distribution:
                errors.append(
                    f"{prefix}: second-slot scalar has no matching query distribution"
                )
                continue

            weighted_queries = sum(
                bucket * value
                for name, value in query_distribution.items()
                if (bucket := distribution_bucket(
                    name, "idealTwoTakenQueriesPerCycle"
                )) is not None
            )
            two_query_cycles = sum(
                value
                for name, value in query_distribution.items()
                if distribution_bucket(
                    name, "idealTwoTakenQueriesPerCycle"
                ) == 2
            )
            samples = max(
                (
                    value
                    for name, value in query_distribution.items()
                    if name.endswith("::samples")
                ),
                default=sum(
                    value
                    for name, value in query_distribution.items()
                    if distribution_bucket(
                        name, "idealTwoTakenQueriesPerCycle"
                    ) is not None
                ),
            )

            if second_count > weighted_queries:
                errors.append(
                    f"{prefix}: second-slot queries ({second_count:g}) exceed "
                    f"total predictor queries ({weighted_queries:g})"
                )
            if second_count > samples:
                errors.append(
                    f"{prefix}: more than one second-slot query per cycle "
                    f"({second_count:g} over {samples:g} samples)"
                )
            if two_query_cycles > second_count:
                errors.append(
                    f"{prefix}: two-query cycles ({two_query_cycles:g}) exceed "
                    f"second-slot queries ({second_count:g})"
                )

    if require_two_enqueues and "idealTwoTakenEnqueuesPerCycle" in found:
        bucket_two = [
            value
            for name, value in found["idealTwoTakenEnqueuesPerCycle"].items()
            if distribution_bucket(
                name, "idealTwoTakenEnqueuesPerCycle"
            ) == 2
        ]
        if not bucket_two or max(bucket_two) <= 0:
            errors.append("no cycle enqueued two prediction blocks")

    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stats", type=Path, help="gem5 stats.txt to check")
    parser.add_argument(
        "--require-second-slot",
        action="store_true",
        help="require at least one second predictor lookup",
    )
    parser.add_argument(
        "--require-two-enqueues",
        action="store_true",
        help="require at least one cycle that enqueued two prediction blocks",
    )
    args = parser.parse_args()

    errors = validate(
        read_stats(args.stats),
        require_second_slot=args.require_second_slot,
        require_two_enqueues=args.require_two_enqueues,
    )
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1

    print("ideal 2-taken statistics are valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
