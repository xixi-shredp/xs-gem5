"""Resource-admitted Bundle candidate build, executed only on ps1/ps2."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import spec_smoke as smoke


def main():
    smoke.require_experiment_host()
    root = Path(__file__).resolve().parents[3]
    out = root / "results/model/smoke-test/bundle-20261103/candidate"
    out.mkdir(parents=True, exist_ok=True)
    if (out / "build.log").exists():
        shutil.copy2(out / "build.log", out / f"build-before-{time.time_ns()}.log")
    budget = smoke.resource_snapshot(memory_per_job_gib=4, reserve_gib=32)
    smoke.need(budget["job_capacity"] > 0, "No build headroom")
    smoke.write(out / "resources.json", budget)
    cpus = sorted(os.sched_getaffinity(0))[: budget["job_capacity"]]
    env = dict(os.environ)
    env["PATH"] = str(root / "deps/build-env/bin") + ":" + env["PATH"]
    with (out / "build.log").open("w") as log:
        subprocess.run(
            [
                "taskset",
                "-c",
                ",".join(map(str, cpus)),
                "bash",
                "-c",
                "scons -j$(nproc) build/RISCV/gem5.opt",
            ],
            cwd=root / "xs-gem5",
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    elf = root / "xs-gem5/build/RISCV/gem5.opt"
    smoke.need(
        os.access(elf, os.X_OK) and elf.open("rb").read(4) == b"\x7fELF",
        "Invalid candidate ELF",
    )
    with (out / "elf.log").open("w") as log:
        subprocess.run(["file", str(elf)], stdout=log, check=True)
        subprocess.run(["ls", "-l", str(elf)], stdout=log, check=True)
    frozen = out / f"gem5-bundle-{smoke.sha(elf)}.opt"
    if not frozen.exists():
        shutil.copy2(elf, frozen)
    smoke.write(out / "binary.json", smoke.identity(frozen))
    print("BUNDLE BUILD PASS", flush=True)
    with (out / "helper-tests.log").open("w") as log:
        subprocess.run(
            [
                str(frozen),
                "-d",
                str(out / f"helper-tests-{time.time_ns()}"),
                "src/cpu/pred/btb/test/hint_indirect_test_config.py",
            ],
            cwd=root / "xs-gem5",
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    print("BUNDLE HELPER TESTS PASS", flush=True)
    reference = (
        root
        / "results/model/smoke-test/bundle-20261103/reference/build/riscv64-nemu-interpreter-so"
    )
    bus = out / f"bus-{time.time_ns()}"
    with (out / "bus-tests.log").open("w") as log:
        subprocess.run(
            [
                sys.executable,
                str(root / "xs-gem5/util/h2p_hint/bundle_bus_test.py"),
                "--output-root",
                str(bus),
                "--run",
                "--gem5",
                str(frozen),
                "--binary-sha256",
                smoke.sha(frozen),
                "--ref-so",
                str(reference),
                "--jobs",
                "2",
            ],
            cwd=root / "xs-gem5",
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    print("BUNDLE REAL BUS TESTS PASS", flush=True)
    with (out / "off-startup-test.log").open("w") as log:
        subprocess.run(
            [
                sys.executable,
                str(root / "xs-gem5/util/h2p_hint/bundle_bus_test.py"),
                "--output-root",
                str(bus) + "-off",
                "--case",
                "off-startup",
                "--run",
                "--gem5",
                str(frozen),
                "--binary-sha256",
                smoke.sha(frozen),
                "--ref-so",
                str(reference),
            ],
            cwd=root / "xs-gem5",
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    print("BUNDLE OFF STARTUP TEST PASS", flush=True)


if __name__ == "__main__":
    main()
