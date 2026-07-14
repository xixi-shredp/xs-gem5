#!/usr/bin/env python3
"""Prepare and summarize deterministic MOP-cache SPEC2017 test runs.

The selected checkpoints are resolved from each workload's simpoints0 and
weights0 pair.  This deliberately avoids recursively selecting a similarly
named checkpoint (notably xz's extra checkpoint under point 0).
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import shlex
import sys
from pathlib import Path


BEGIN_STATS = "---------- Begin Simulation Statistics ----------"
DEFAULT_ROOT = Path.home() / "sim/workloads/spec-cpu-2017"
DEFAULT_POINTS = (
    "gcc_r_test_cmd0/0",
    "perlbench_r_test_cmd0/61",
    "xalancbmk_r_test_cmd0/3",
    "xz_r_test_cmd0/90",
)
DEFAULT_FIELDS = (
    "ipc=system.cpu.totalIpc",
    "cycles=system.cpu.numCycles,simTicks",
    "committed_insts=system.cpu.committedInsts::0,system.cpu.committedInsts,system.cpu.commit.committedInsts",
    "mop_lookups=system.cpu.fetch.mopCacheLookups",
    "mop_hits=system.cpu.fetch.mopCacheHits",
    "mop_misses=system.cpu.fetch.mopCacheMisses",
    "mop_supplied=system.cpu.fetch.mopCacheSuppliedInsts",
    "mop_avoided_icache=system.cpu.fetch.mopCacheAvoidedICacheRequests",
    "mop_fallbacks=system.cpu.fetch.mopCacheFallbackRequests",
)


@dataclass(frozen=True)
class Point:
    workload: str
    simpoint: int
    cluster: int
    weight: float
    checkpoint_dir: Path

    @property
    def name(self) -> str:
        return f"{self.workload}_{self.simpoint}"


def parse_point_spec(text: str) -> tuple[str, int]:
    try:
        workload, point_text = text.rsplit("/", 1)
        return workload, int(point_text)
    except (ValueError, TypeError) as exc:
        raise ValueError(f"bad point {text!r}; expected WORKLOAD/SIMPOINT") from exc


def read_two_column(path: Path, first_type, label: str) -> dict[int, object]:
    values: dict[int, object] = {}
    with path.open(encoding="utf-8") as handle:
        for line_no, raw in enumerate(handle, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            if len(fields) != 2:
                raise ValueError(f"{path}:{line_no}: expected two columns")
            value, cluster = first_type(fields[0]), int(fields[1])
            if cluster in values:
                raise ValueError(f"{path}:{line_no}: duplicate cluster {cluster}")
            values[cluster] = value
    if not values:
        raise ValueError(f"{path}: empty {label} file")
    return values


def resolve_point(root: Path, spec: str) -> Point:
    workload, requested = parse_point_spec(spec)
    workload_dir = root / "xsgem5" / "out" / workload
    cluster_dir = workload_dir / "checkpoints" / "cluster" / workload
    simpoints = read_two_column(cluster_dir / "simpoints0", int, "simpoint")
    weights = read_two_column(cluster_dir / "weights0", float, "weight")
    if simpoints.keys() != weights.keys():
        raise ValueError(
            f"{workload}: simpoints0/weights0 cluster IDs differ: "
            f"{sorted(simpoints)} != {sorted(weights)}"
        )
    matches = [cluster for cluster, point in simpoints.items() if point == requested]
    if len(matches) != 1:
        raise ValueError(
            f"{spec}: expected one metadata match, found {len(matches)}"
        )
    cluster = matches[0]
    checkpoint_dir = (
        workload_dir / "checkpoints" / "checkpoint" / workload / str(requested)
    )
    files = sorted(checkpoint_dir.glob("*.zstd"))
    if len(files) != 1:
        raise ValueError(
            f"{spec}: expected exactly one .zstd directly in {checkpoint_dir}, "
            f"found {len(files)}"
        )
    if not files[0].name.startswith(f"_{requested}_"):
        raise ValueError(f"{spec}: checkpoint filename does not match point: {files[0]}")
    return Point(
        workload=workload,
        simpoint=requested,
        cluster=cluster,
        weight=float(weights[cluster]),
        checkpoint_dir=checkpoint_dir,
    )


def selected_specs(args: argparse.Namespace) -> list[str]:
    return args.point if args.point else list(DEFAULT_POINTS)


def manifest_line(point: Point) -> str:
    # The last four fields retain parallel_sim/distributed_sim compatibility:
    # skip, functional warmup, detailed warmup, and measured instructions (M).
    checkpoint_key = point.checkpoint_dir.relative_to(
        point.checkpoint_dir.parents[4]
    )
    return f"{point.name} {checkpoint_key}/ 0 0 1 19"


def cmd_manifest(args: argparse.Namespace) -> int:
    root = args.workload_root.resolve()
    points = [resolve_point(root, spec) for spec in selected_specs(args)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as handle:
        handle.write("# name checkpoint_path skip functional_warmup detailed_warmup sample\n")
        for point in points:
            handle.write(manifest_line(point) + "\n")
    for point in points:
        print(
            f"selected {point.workload}/{point.simpoint}: "
            f"cluster={point.cluster} weight={point.weight:g} "
            f"checkpoint={point.checkpoint_dir}"
        )
    print(f"wrote {args.output} ({len(points)} workloads)")
    return 0


def shell_join(parts: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in parts)


def cmd_commands(args: argparse.Namespace) -> int:
    root = args.workload_root.resolve()
    cpt_root = root / "xsgem5" / "out"
    manifest = args.manifest.resolve()
    archive = args.archive.resolve()
    common_gem5_args = (
        f"--warmup-insts-no-switch={args.warmup} --maxinsts={args.maxinsts}"
    )
    common = [
        "python3",
        args.runner,
        "--servers",
        args.servers,
        "--jobs-per-server",
        str(args.jobs_per_server),
    ]
    for mode, mop_arg in (("off", ""), ("on", args.enable_arg)):
        gem5_args = " ".join(part for part in (common_gem5_args, mop_arg) if part)
        command = common + [
            "--extra-gem5-args",
            gem5_args,
            args.config,
            str(manifest),
            str(cpt_root),
            str(archive / f"{args.tag}_{mode}"),
        ]
        print(f"# {mode.upper()}: warmup={args.warmup} maxinsts={args.maxinsts}")
        print(shell_join(command))
    return 0


def parse_field(text: str) -> tuple[str, tuple[str, ...]]:
    if "=" not in text:
        raise ValueError(f"bad field {text!r}; expected LABEL=STAT[,STAT...]")
    label, names_text = text.split("=", 1)
    names = tuple(name.strip() for name in names_text.split(",") if name.strip())
    if not label.strip() or not names:
        raise ValueError(f"bad field {text!r}; expected LABEL=STAT[,STAT...]")
    return label.strip(), names


def read_last_dump(path: Path) -> dict[str, str]:
    current: dict[str, str] = {}
    saw_begin = False
    with path.open(encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            if raw.startswith(BEGIN_STATS):
                current = {}
                saw_begin = True
                continue
            fields = raw.split()
            if len(fields) >= 2 and not fields[0].startswith("#"):
                current[fields[0]] = fields[1]
    if not saw_begin:
        raise ValueError(f"{path}: no simulation statistics dump found")
    return current


def cmd_stats(args: argparse.Namespace) -> int:
    field_args = args.field if args.field else list(DEFAULT_FIELDS)
    fields = [parse_field(field) for field in field_args]
    writer = csv.writer(sys.stdout)
    writer.writerow(["stats", *(label for label, _ in fields)])
    for path in args.stats:
        values = read_last_dump(path)
        row = [str(path)]
        for label, candidates in fields:
            match = next((values[name] for name in candidates if name in values), None)
            if match is None:
                print(
                    f"warning: {path}: missing {label}; tried "
                    + ", ".join(candidates),
                    file=sys.stderr,
                )
                row.append("MISSING")
            else:
                row.append(match)
        writer.writerow(row)
    return 0


def add_selection_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--workload-root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument(
        "--point",
        action="append",
        help="WORKLOAD/SIMPOINT; repeat to override the four default points",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    manifest = subparsers.add_parser("manifest", help="write the exact workload list")
    add_selection_args(manifest)
    manifest.add_argument("--output", type=Path, required=True)
    manifest.set_defaults(func=cmd_manifest)

    commands = subparsers.add_parser("commands", help="print paired off/on commands")
    commands.add_argument("--workload-root", type=Path, default=DEFAULT_ROOT)
    commands.add_argument("--manifest", type=Path, required=True)
    commands.add_argument("--archive", type=Path, default=Path.cwd())
    commands.add_argument("--tag", default="mop_spec17_test")
    commands.add_argument("--runner", default="util/xs_scripts/distributed_sim.py")
    commands.add_argument("--config", default="configs/example/kmhv3.py")
    commands.add_argument("--servers", default="local")
    commands.add_argument("--jobs-per-server", type=int, default=1)
    commands.add_argument("--warmup", type=int, default=1_000_000)
    commands.add_argument("--maxinsts", type=int, default=20_000_000)
    commands.add_argument("--enable-arg", default="--enable-mop-cache")
    commands.set_defaults(func=cmd_commands)

    stats = subparsers.add_parser("stats", help="extract fields from the last stats dump")
    stats.add_argument("stats", type=Path, nargs="+")
    stats.add_argument(
        "--field",
        action="append",
        help="LABEL=STAT[,FALLBACK_STAT...]; repeat to replace defaults",
    )
    stats.set_defaults(func=cmd_stats)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if getattr(args, "warmup", 0) < 0 or getattr(args, "maxinsts", 1) <= 0:
        raise ValueError("warmup must be >= 0 and maxinsts must be > 0")
    if getattr(args, "warmup", 0) >= getattr(args, "maxinsts", 1):
        raise ValueError("warmup must be smaller than maxinsts")
    return args.func(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(f"mop_cache_spec17.py: error: {exc}", file=sys.stderr)
        raise SystemExit(1)
