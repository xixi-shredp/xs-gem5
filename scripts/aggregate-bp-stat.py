#!/usr/bin/env python3
"""Aggregate raw bp-stat histograms without averaging or normalization."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from pathlib import Path


def parse_bp_stat(path: Path) -> dict[str, Counter[int]]:
    sections: dict[str, Counter[int]] = defaultdict(Counter)
    section: str | None = None
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#") or line == "key count":
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            sections.setdefault(section, Counter())
            continue
        if section is None:
            raise ValueError(f"{path}:{line_number}: histogram entry outside a section")
        fields = line.split()
        if len(fields) != 2:
            raise ValueError(f"{path}:{line_number}: expected 'key count'")
        key, count = map(int, fields)
        if key < 0 or count < 0:
            raise ValueError(f"{path}:{line_number}: negative key or count")
        sections[section][key] += count
    return sections


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    inputs = sorted(args.input_root.glob("*/*/bp-stat/bp-stat.txt"))
    if not inputs:
        raise SystemExit(f"no bp-stat files below {args.input_root}")

    aggregate: dict[str, Counter[int]] = defaultdict(Counter)
    schema: set[str] | None = None
    for path in inputs:
        sections = parse_bp_stat(path)
        if schema is None:
            schema = set(sections)
        elif set(sections) != schema:
            raise SystemExit(f"inconsistent bp-stat schema: {path}")
        for section, counts in sections.items():
            aggregate[section].update(counts)

    lines = [
        "# bp-stat: aggregate raw histogram counts",
        f"# inputs: {len(inputs)} files below {args.input_root}",
        "# aggregation: sum each section/key count; no averaging or normalization",
        "",
    ]
    for section in sorted(aggregate):
        lines.extend((f"[{section}]", "key count"))
        lines.extend(f"{key} {aggregate[section][key]}" for key in sorted(aggregate[section]))
        lines.append("")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    print(f"aggregated {len(inputs)} files into {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
