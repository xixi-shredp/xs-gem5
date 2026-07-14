#!/usr/bin/env python3

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "mop_cache_spec17.py"
SPEC = importlib.util.spec_from_file_location("mop_cache_spec17", SCRIPT)
assert SPEC and SPEC.loader
MOP = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MOP
SPEC.loader.exec_module(MOP)


class MopCacheSpec17Test(unittest.TestCase):
    def make_point(self, root: Path, workload: str, point: int, extra=False):
        cluster = root / "xsgem5/out" / workload / "checkpoints/cluster" / workload
        checkpoint = root / "xsgem5/out" / workload / "checkpoints/checkpoint" / workload
        cluster.mkdir(parents=True)
        (cluster / "simpoints0").write_text(f"{point} 7\n0 9\n", encoding="utf-8")
        (cluster / "weights0").write_text("0.75 7\n0.25 9\n", encoding="utf-8")
        (checkpoint / str(point)).mkdir(parents=True)
        (checkpoint / str(point) / f"_{point}_0.750000_memory_.zstd").touch()
        if extra:
            (checkpoint / "0").mkdir(exist_ok=True)
            (checkpoint / "0" / "_5_memory_.zstd").touch()

    def test_metadata_resolution_ignores_extra_checkpoint(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_point(root, "xz_r_test_cmd0", 90, extra=True)
            point = MOP.resolve_point(root, "xz_r_test_cmd0/90")
            self.assertEqual(point.cluster, 7)
            self.assertEqual(point.weight, 0.75)
            self.assertEqual(point.checkpoint_dir.name, "90")
            self.assertIn("xz_r_test_cmd0/90/", MOP.manifest_line(point))

    def test_last_stats_dump_and_missing_report(self):
        with tempfile.TemporaryDirectory() as temp:
            stats = Path(temp) / "stats.txt"
            stats.write_text(
                f"{MOP.BEGIN_STATS}\nsystem.cpu.totalIpc 1.0\n"
                f"{MOP.BEGIN_STATS}\nsystem.cpu.totalIpc 2.0\n",
                encoding="utf-8",
            )
            args = type("Args", (), {
                "field": ["ipc=system.cpu.totalIpc", "hits=missing.stat"],
                "stats": [stats],
            })()
            stdout, stderr = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                MOP.cmd_stats(args)
            self.assertIn(",2.0,MISSING", stdout.getvalue())
            self.assertIn("missing hits", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
