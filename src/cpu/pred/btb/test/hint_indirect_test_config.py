"""Run with the fully built gem5.opt; this is not a gtest binary."""
import argparse
from pathlib import Path

import m5
from m5.objects import HintIndirectTest, Root

parser = argparse.ArgumentParser()
parser.add_argument("--metadata", default="")
parser.add_argument("--candidates", default="")
parser.add_argument("--ideal-hint-ibp", action="store_true")
parser.add_argument("--vectors", default=str(Path(__file__).resolve().parents[5] / "util/h2p_hint/hint-context-v7-vectors.json"))
parser.add_argument("--range-port-reuse", action="store_true")
args = parser.parse_args()
root = Root(full_system=False)
root.test = HintIndirectTest(
    metadata=args.metadata,
    candidates=args.candidates,
    vectors=args.vectors,
    ideal=args.ideal_hint_ibp,
    range_port_reuse=args.range_port_reuse,
)
m5.instantiate()
event = m5.simulate()
print(event.getCause())
if event.getCause() != "Hint self-test passed":
    raise SystemExit(1)
