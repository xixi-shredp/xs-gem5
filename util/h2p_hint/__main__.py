"""Single-artifact operations: candidates, cache, build, reference and vectors."""

import argparse
import json
from pathlib import Path

from .builder import build_image, candidates_from_stage1
from .model import (
    address,
    context_key,
    load_candidates,
    load_metadata,
    sha256,
    write_json,
)
from .profile import Cache, make_cache


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    candidates = commands.add_parser("candidates")
    candidates.add_argument("h2p", type=Path)
    candidates.add_argument("output", type=Path)
    candidates.add_argument(
        "--ideal-hint-ibp",
        action="store_true",
        help="Write v3 candidates including every positive-miss PC",
    )
    cache = commands.add_parser("cache")
    cache.add_argument("profile", type=Path)
    cache.add_argument("output", type=Path)
    build = commands.add_parser("build")
    build.add_argument("cache", type=Path)
    build.add_argument("candidates", type=Path)
    build.add_argument("output", type=Path)
    build.add_argument("--mask", type=int, required=True)
    build.add_argument("--group", type=int, default=0)
    build.add_argument(
        "--ideal-hint-ibp",
        action="store_true",
        help="Build unlimited v3 tables from all candidates in one CAM",
    )
    reference = commands.add_parser("reference")
    reference.add_argument("cache", type=Path)
    reference.add_argument("metadata", type=Path)
    reference.add_argument("output", type=Path)
    reference.add_argument("--finish-cycle", type=int)
    vectors = commands.add_parser("check-vectors")
    vectors.add_argument("vectors", type=Path)
    args = parser.parse_args(argv)
    if args.command == "candidates":
        candidates_from_stage1(
            args.h2p, args.output, ideal=args.ideal_hint_ibp
        )
    elif args.command == "cache":
        print(
            json.dumps(make_cache(args.profile, args.output), sort_keys=True)
        )
    elif args.command == "build":
        if not 0 <= args.mask < 32 or args.group not in (0, 1):
            parser.error("--mask must be 0..31 and --group 0 or 1")
        if args.ideal_hint_ibp and args.group != 0:
            parser.error(
                "unlimited capacity uses one complete CAM; --group must be 0"
            )
        pcs = load_candidates(args.candidates, ideal=args.ideal_hint_ibp)
        if not args.ideal_hint_ibp:
            pcs = pcs[args.group * 16 : args.group * 16 + 16]
        if not pcs and not args.ideal_hint_ibp:
            parser.error("selected group has no candidates")
        cache = Cache(args.cache)
        metadata, report = build_image(
            cache.rows(),
            {pc: args.mask for pc in pcs},
            source={
                "profile_sha256": cache.identity["profile_sha256"],
                "candidate_sha256": sha256(args.candidates),
                "fixed_mask": args.mask,
                "group": args.group,
            },
            ideal=args.ideal_hint_ibp,
        )
        args.output.mkdir(parents=True, exist_ok=False)
        write_json(args.output / "metadata.txt", metadata)
        write_json(args.output / "builder.json", report)
    elif args.command == "reference":
        metadata = load_metadata(args.metadata)
        write_json(
            args.output,
            Cache(args.cache).reference(metadata, args.finish_cycle),
        )
    elif args.command == "check-vectors":
        data = json.loads(args.vectors.read_text())
        rows = data if isinstance(data, list) else data.get("vectors", [data])
        for row in rows:
            actual = [
                context_key(address(row["pc"]), row["contexts"], mask)
                for mask in range(32)
            ]
            if actual != row["keys"]:
                raise ValueError(f"C++/Python hash mismatch for {row['pc']}")
        print(f"PASS {len(rows)} vectors, all 32 masks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
