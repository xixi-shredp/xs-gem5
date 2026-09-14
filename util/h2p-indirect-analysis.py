#!/usr/bin/env python3
"""Print h2p-indirect.txt records with the largest miss counts."""

import argparse
import re
from pathlib import Path


def top_miss_analysis(path, limit=20):
    records = []

    with path.open(encoding="utf-8") as input_file:
        for line_number, raw_line in enumerate(input_file, start=1):
            line = raw_line.rstrip("\n")
            fields = [field.strip() for field in line.split(",")]
            if len(fields) < 3:
                raise ValueError(
                    f"line {line_number}: expected PC, miss number, and target(s)"
                )

            try:
                miss_number = int(fields[1], 10)
            except ValueError as error:
                raise ValueError(
                    f"line {line_number}: invalid miss number {fields[1]!r}"
                ) from error

            if miss_number <= 0:
                raise ValueError(
                    f"line {line_number}: miss number must be positive"
                )

            targets = []
            target_misses = []
            for token in fields[2:]:
                match = re.fullmatch(r"(0x[0-9a-fA-F]+)(?:\(([0-9]+)\))?", token)
                if match is None:
                    raise ValueError(f"line {line_number}: invalid target {token!r}")
                target, misses = match.groups()
                target_misses.append(None if misses is None else int(misses))
                if not target.lower().startswith("0x"):
                    raise ValueError(
                        f"line {line_number}: invalid target address {target!r}"
                    )
                try:
                    targets.append(int(target, 16))
                except ValueError as error:
                    raise ValueError(
                        f"line {line_number}: invalid target address {target!r}"
                    ) from error

            if any(value is not None for value in target_misses):
                if None in target_misses or sum(target_misses) != miss_number:
                    raise ValueError(f"line {line_number}: target miss counts do not sum to PC misses")
            records.append((miss_number, line_number, line, targets))

    records.sort(key=lambda record: (-record[0], record[1]))
    top_records = records[:limit]
    lines = [record[2] for record in top_records]
    unique_targets = sorted(
        {target for record in top_records for target in record[3]}
    )
    return lines, unique_targets


def top_miss_lines(path, limit=20):
    return top_miss_analysis(path, limit)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="path to h2p-indirect.txt")
    parser.add_argument(
        "--top",
        type=int,
        default=20,
        metavar="N",
        help="number of records to print (default: 20)",
    )
    args = parser.parse_args()

    if args.top <= 0:
        parser.error("--top must be a positive integer")

    try:
        lines, targets = top_miss_analysis(args.input, args.top)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    for line in lines:
        print(line)
    print("=========== unique targets ===========")
    for target in targets:
        print(f"0x{target:x}")


if __name__ == "__main__":
    main()
