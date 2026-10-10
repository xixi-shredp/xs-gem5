"""Targeted command and difftest provenance checks for the finite runner."""

from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

from .audit import validate_difftest_config
from .finite_runner import _command, NO_RVV_SLICES
from .model import sha256


class FiniteRunnerDifftestTest(unittest.TestCase):
    def test_all_stages_share_the_explicit_reference(self):
        args = SimpleNamespace(command_prefix=[], gem5=Path("/tmp/gem5.opt"),
                               config=Path("/tmp/kmhv3.py"), warmup_insts=1_000_000,
                               measure_insts=1_000_000, h2p_stat_interval=1_000_000,
                               enable_difftest=True,
                               difftest_ref_so=Path("/tmp/riscv64-nemu-interpreter-so"))
        task = {"checkpoint": {"path": "/tmp/checkpoint.zstd"}}
        for stage, mode in (("baseline", "off"), ("collect", "collect"),
                            ("joint", "replay")):
            command = _command(args, task, stage, Path("/tmp/output"))
            self.assertIn("--enable-difftest", command)
            self.assertIn("--difftest-ref-so=/tmp/riscv64-nemu-interpreter-so", command)
            self.assertIn("--warmup-insts-no-switch=1000000", command)
            self.assertIn("--maxinsts=2000000", command)
            self.assertIn(f"--h2p-hint-mode={mode}", command)

    def test_audit_rejects_wrong_reference_or_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            ref = Path(temporary) / "nemu.so"
            ref.write_bytes(b"\x7fELF-test-reference")
            identity = {"path": str(ref), "sha256": sha256(ref)}
            command = ["--enable-difftest", f"--difftest-ref-so={ref}",
                       "--mem-type=DDR4_2400_8x8"]
            config = f"enable_difftest=true\nenable_difftest=true\ndifftest_ref_so={ref}\n"
            validate_difftest_config(config, command, identity)
            with self.assertRaisesRegex(ValueError, "NEMU reference"):
                validate_difftest_config(config, command, {**identity, "sha256": "0" * 64})
            with self.assertRaisesRegex(ValueError, "difftest configuration"):
                validate_difftest_config(config.replace("=true", "=false"), command, identity)
            with self.assertRaisesRegex(ValueError, "NEMU reference"):
                validate_difftest_config(config, command[:-2] +
                                         ["--difftest-ref-so=/tmp/wrong.so",
                                          "--mem-type=DDR4_2400_8x8"], identity)
            validate_difftest_config("enable_difftest=false\nenable_difftest=false\n",
                                     ["--disable-difftest", "--mem-type=DDR4_2400_8x8"])

    def test_six_slice_selection_is_explicit(self):
        self.assertEqual(NO_RVV_SLICES,
                         [("scimark_fft", "73"), ("pickle_pure_python", "39"),
                          ("pickle_pure_python", "23"), ("pyflate", "44"),
                          ("deltablue", "44"), ("regex_v8", "49")])

    def test_explicit_backend_memory_type_is_strict(self):
        config = "enable_difftest=false\nenable_difftest=false\n"
        command = ["--disable-difftest", "--mem-type=DRAMsim3"]
        validate_difftest_config(config, command, memory_type="DRAMsim3")
        with self.assertRaisesRegex(ValueError, "memory command"):
            validate_difftest_config(config, command)  # legacy contract stays DDR4
        with self.assertRaisesRegex(ValueError, "memory command"):
            validate_difftest_config(config,
                ["--disable-difftest", "--mem-type=DDR4_2400_8x8"],
                memory_type="DRAMsim3")
        with self.assertRaisesRegex(ValueError, "memory command"):
            validate_difftest_config(config, command + ["--mem-type=DRAMsim3"],
                                     memory_type="DRAMsim3")


if __name__ == "__main__":
    unittest.main()
