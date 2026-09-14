import argparse

from m5.objects import BTBITTAGE, DecoupledBPUWithBTB
from m5.util import addToPath


addToPath("../../../configs")

from common.H2PStat import add_h2p_stat_option, validate_h2p_stat_config


parser = argparse.ArgumentParser()
add_h2p_stat_option(parser)
parser.add_argument("--num-cpus", type=int, default=1)
parser.add_argument("--bp-type", default="DecoupledBPUWithBTB")
parser.add_argument("--expect-rejection", action="store_true")
args = parser.parse_args()

error = validate_h2p_stat_config(args)
if args.expect_rejection:
    if error is None:
        raise RuntimeError("H2P configuration was expected to be rejected")
    print(f"h2pConfigRejected={error}")
else:
    if error is not None:
        raise RuntimeError(error)

    predictor = DecoupledBPUWithBTB(
        h2pStat=args.h2p_stat,
        h2pStatInterval=args.h2p_stat_interval,
        ittage=BTBITTAGE(h2pStat=args.h2p_stat, h2pStatInterval=args.h2p_stat_interval),
    )
    actual = bool(predictor.h2pStat)
    nested_actual = bool(predictor.ittage.h2pStat)
    if actual != args.h2p_stat or nested_actual != args.h2p_stat:
        raise RuntimeError(
            "DecoupledBPUWithBTB/BTBITTAGE h2pStat does not match the "
            "requested value"
        )
    print(f"h2pStat={str(actual).lower()}")
    print(f"ittageH2pStat={str(nested_actual).lower()}")

    assert int(predictor.ittage.h2pStatInterval) == args.h2p_stat_interval
    print(f"h2pStatInterval={int(predictor.h2pStatInterval)}")
