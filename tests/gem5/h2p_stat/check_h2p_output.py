#!/usr/bin/env python3
"""Validate the syntax and ordering of an h2p-indirect.txt file."""

import argparse
from pathlib import Path
import re


HEX_ADDRESS = re.compile(r"0x[0-9a-fA-F]+")
POSITIVE_DECIMAL = re.compile(r"[1-9][0-9]*")


def _parse_address(token, line_number, field):
    if HEX_ADDRESS.fullmatch(token) is None:
        raise ValueError(
            f"line {line_number}: {field} must be a hexadecimal address"
        )
    return int(token, 16)


def parse_h2p_output(path):
    records = {}
    previous_pc = None

    with Path(path).open(encoding="utf-8") as output:
        for line_number, raw_line in enumerate(output, start=1):
            line = raw_line.strip()
            if not line:
                raise ValueError(f"line {line_number}: blank lines are not allowed")

            fields = [field.strip() for field in line.split(",")]
            if len(fields) < 3:
                raise ValueError(
                    f"line {line_number}: expected PC, miss number, and target(s)"
                )

            pc = _parse_address(fields[0], line_number, "PC")
            if previous_pc is not None and pc <= previous_pc:
                raise ValueError(
                    f"line {line_number}: PCs must be strictly increasing"
                )

            if POSITIVE_DECIMAL.fullmatch(fields[1]) is None:
                raise ValueError(
                    f"line {line_number}: miss number must be a positive decimal"
                )
            miss_number = int(fields[1], 10)

            targets = []
            counts = []
            for token in fields[2:]:
                match = re.fullmatch(r"(0x[0-9a-fA-F]+)(?:\(([0-9]+)\))?", token)
                if match is None:
                    raise ValueError(f"line {line_number}: invalid target")
                targets.append(_parse_address(match[1], line_number, "target"))
                counts.append(None if match[2] is None else int(match[2]))
            if any(count is not None for count in counts):
                if None in counts or sum(counts) != miss_number:
                    raise ValueError(f"line {line_number}: target miss sum differs")
            if targets != sorted(set(targets)):
                raise ValueError(
                    f"line {line_number}: targets must be unique and increasing"
                )

            records[pc] = (miss_number, tuple(targets))
            previous_pc = pc

    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="h2p-indirect.txt to validate")
    args = parser.parse_args()
    records = parse_h2p_output(args.output)
    print(f"validated {len(records)} H2P branch record(s)")


if __name__ == "__main__":
    main()
