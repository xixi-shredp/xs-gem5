"""Fixed-ASLR launcher tests without starting simulator workloads."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("runner", ROOT / "util/run-cpython-h2p.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class AslrRunnerTest(unittest.TestCase):
    def test_default_does_not_probe(self):
        with patch.object(runner.subprocess, "run") as run:
            self.assertEqual(runner.command_prefix(False), [])
            run.assert_not_called()

    def test_supported_prefix(self):
        with patch.object(runner.platform, "system", return_value="Linux"), patch.object(
            runner.platform, "machine", return_value="aarch64"
        ), patch.object(runner.shutil, "which", return_value="/usr/bin/setarch"), patch.object(
            runner.subprocess, "run", return_value=SimpleNamespace(returncode=0)
        ) as run:
            prefix = runner.command_prefix(True)
            self.assertEqual(prefix, ["/usr/bin/setarch", "aarch64", "-R"])
            self.assertEqual(run.call_args.args[0][:3], prefix)

    def test_unsupported_platform(self):
        with patch.object(runner.platform, "system", return_value="Darwin"):
            with self.assertRaisesRegex(RuntimeError, "requires Linux"):
                runner.command_prefix(True)

    def test_missing_setarch(self):
        with patch.object(runner.platform, "system", return_value="Linux"), patch.object(
            runner.shutil, "which", return_value=None
        ):
            with self.assertRaisesRegex(RuntimeError, "setarch in PATH"):
                runner.command_prefix(True)

    def test_denied_and_timeout(self):
        for outcome in (SimpleNamespace(returncode=1, stderr="denied", stdout=""),
                        subprocess.TimeoutExpired("setarch", 30)):
            with patch.object(runner.platform, "system", return_value="Linux"), patch.object(
                runner.shutil, "which", return_value="/usr/bin/setarch"
            ), patch.object(runner.subprocess, "run") as run:
                if isinstance(outcome, Exception):
                    run.side_effect = outcome
                else:
                    run.return_value = outcome
                with self.assertRaisesRegex(RuntimeError, "preflight failed"):
                    runner.command_prefix(True)

    def test_main_failure_precedes_workloads_and_output(self):
        args = SimpleNamespace(disable_aslr=True)
        with patch.object(runner, "parse_args", return_value=args), patch.object(
            runner, "command_prefix", side_effect=RuntimeError("unsupported")
        ), patch.object(runner, "load_tasks") as load, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.main(), 2)
            load.assert_not_called()

    def test_workload_command_uses_preflight_prefix(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = SimpleNamespace(output_root=Path(tmp),
                command_prefix=["/usr/bin/setarch", "x86_64", "-R"],
                gem5=Path("/fake/gem5.opt"), config=Path("/fake/config.py"),
                warmup_insts=1, measure_insts=1, mem_type="DDR4",
                h2p_stat_interval=1, library_path="")
            with patch.object(
                runner.subprocess, "run", return_value=SimpleNamespace(returncode=0)
            ) as run, patch.object(
                runner, "validate", return_value=([], [1], 0)
            ):
                runner.run_one(args, ("bench", "test", "0", Path("/fake/checkpoint"), {}))
            self.assertEqual(run.call_args.args[0][:4], args.command_prefix + [str(args.gem5)])

    def test_dry_run_identity_separates_layouts(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            args = SimpleNamespace(output_root=base, gem5=Path(__file__),
                config=Path(__file__), workload_list=Path(__file__), jobs=1,
                warmup_insts=1, measure_insts=1, h2p_stat_interval=1, mem_type="DDR4")
            identities = []
            for flag, prefix in [(False, []), (True, ["/usr/bin/setarch", "x86_64", "-R"])]:
                args.disable_aslr = flag
                args.dry_run = True
                with patch.object(runner, "parse_args", return_value=args), patch.object(
                    runner, "command_prefix", return_value=prefix
                ), patch.object(runner, "load_tasks", return_value=[]), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(runner.main(), 0)
                identity = json.loads((base / "experiment.json").read_text())
                self.assertEqual(identity["disable_aslr"], flag)
                self.assertEqual(identity["command_prefix"], prefix)
                identities.append(identity)
            (base / "status.json").write_text(json.dumps({"result": "pass", "identity": identities[0]}))
            self.assertIsNone(runner.completed_result(base, identities[1]))


if __name__ == "__main__":
    unittest.main()
