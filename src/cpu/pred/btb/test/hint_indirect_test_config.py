"""Run with the fully built gem5.opt; this is not a gtest binary."""
import argparse

import m5
from m5.objects import HintIndirectTest, Root

parser = argparse.ArgumentParser()
parser.add_argument("--metadata", default="")
parser.add_argument("--candidates", default="")
args = parser.parse_args()
root = Root(full_system=False)
root.test = HintIndirectTest(metadata=args.metadata, candidates=args.candidates)
m5.instantiate()
event = m5.simulate()
print(event.getCause())
if event.getCause() != "Hint self-test passed":
    raise SystemExit(1)
