import argparse

from m5.objects import DecoupledBPUWithBTB


parser = argparse.ArgumentParser()
parser.add_argument("--ideal-2-taken", action="store_true")
args = parser.parse_args()

predictor = (
    DecoupledBPUWithBTB(idealTwoTaken=True)
    if args.ideal_2_taken
    else DecoupledBPUWithBTB()
)
actual = bool(predictor.idealTwoTaken)

if actual != args.ideal_2_taken:
    raise RuntimeError(
        "DecoupledBPUWithBTB.idealTwoTaken does not match the requested value"
    )

print(f"idealTwoTaken={str(actual).lower()}")
