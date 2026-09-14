"""Output auditor tests; simulator behavior is tested with gem5.opt separately."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load("runner", ROOT / "util/run-cpython-h2p.py")
analysis = load("analysis", ROOT / "util/h2p-indirect-analysis.py")
config = load("config", ROOT / "configs/common/H2PStat.py")


class IntervalToolsTest(unittest.TestCase):
    def test_windows_preserve_counts_allow_omitted_zero_targets_and_tail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "h2p-indirect.txt").write_text("0x10, 3, 0x20(3), 0x30(0)\n")
            windows = root / "h2p-indirec"
            windows.mkdir()
            (windows / "0.txt").write_text("0x10, 2, 0x20(2)\n")
            (windows / "1000.txt").write_text("")
            (windows / "2000.txt").write_text("0x10, 1, 0x20(1)\n")
            self.assertEqual(runner.validate_windows(root, 1000, 2500), 3)
            (windows / "2000.txt").write_text("")
            with self.assertRaisesRegex(ValueError, "miss mismatch"):
                runner.validate_windows(root, 1000, 2500)

    def test_parser_rejects_miss_sum_and_duplicate_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.txt"
            for row in ("0x10, 2, 0x20(1)\n", "0x10, 2, 0x20(1), 0x20(1)\n"):
                path.write_text(row)
                with self.assertRaises(ValueError):
                    runner.parse_h2p_output(path)

    def test_analysis_legacy_and_counted_top_and_union(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.txt"
            for rows in (
                "0x10, 1, 0x50\n0x20, 3, 0x30, 0x40\n",
                "0x10, 1, 0x50(1)\n0x20, 3, 0x30(3), 0x40(0)\n",
            ):
                path.write_text(rows)
                lines, targets = analysis.top_miss_analysis(path, 1)
                self.assertEqual(len(lines), 1)
                self.assertTrue(lines[0].startswith("0x20, 3,"))
                self.assertEqual(targets, [0x30, 0x40])

    def test_phase_fusion_accounting_is_exact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "stats.txt").write_text("simInsts 2000 # measured\n")
            (root / "config.ini").write_text(
                "h2pStat=true\nh2pStat=true\nh2pStatInterval=0\n"
                "h2pStatInterval=0\ninstructionPhaseControl=true\n"
                "gcptRestoreBase=2147483648\n")
            (root / "cmd.txt").write_text("gem5")
            (root / "stderr.log").write_text("")
            (root / "h2p-indirect.txt").write_text("")
            for split in (0, 1):
                for excess in (0, 1):
                    rows = [
                        ("restore_complete", 10, 0, 0, 0, 0, 0),
                        ("warmup_complete", 1010 + split, 1000, 0, 1000 + split, 0, split),
                        ("measure_complete", 3010 + excess, 1000, 2000, 3000 + excess, excess, split),
                    ]
                    text = "\n".join(
                        f"H2P_PHASE phase={name} tick={committed} committed={committed} "
                        f"warmup={warmup} measure={measure} retired_post_restore={retired} "
                        f"terminal_fusion_excess={extra} warmup_fusion_split={boundary}"
                        for name, committed, warmup, measure, retired, extra, boundary in rows
                    ) + "\na thread reached the max instruction count\n"
                    (root / "stdout.log").write_text(text)
                    self.assertEqual(runner.validate(root, 0, 1000, 2000)[0], [])
                    (root / "stdout.log").write_text(text.replace("measure=2000", "measure=1999"))
                    self.assertTrue(runner.validate(root, 0, 1000, 2000)[0])

    def test_cli_constraints(self):
        import argparse
        parser = argparse.ArgumentParser()
        config.add_h2p_stat_option(parser)
        for argv in (["--h2p-stat-interval=-1"], ["--h2p-stat-interval=1000"]):
            self.assertIsNotNone(config.validate_h2p_stat_config(parser.parse_args(argv)))
        args = parser.parse_args(["--h2p-stat", "--h2p-stat-interval=1000"])
        args.bp_type = "DecoupledBPUWithBTB"
        self.assertIsNone(config.validate_h2p_stat_config(args))


if __name__ == "__main__":
    unittest.main()
