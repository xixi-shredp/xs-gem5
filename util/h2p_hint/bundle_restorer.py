#!/usr/bin/env python3
"""Generate/build a private no-RVV GCPT restorer with real Bundle CSR startup.

Default prints a read-only plan. --generate-source creates a new private tree;
--build also cross-compiles, only on ps1/ps2 with an explicit toolchain proof.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess

from bundle_checkpoint_overlay import raw_restorer

SOURCES = (
    "restore.lds",
    "src/restore.S",
    "src/restore_rom_addr.h",
    "src/csr.h",
    "src/encoding.h",
    "src/bits.h",
)
RESTORE_SHA = "3cfdbf621c7fc265ba81419dbc60edaf9fbbcba07362fcaddf233fafcbbc989d"
LDS_SHA = "841c92ec8abde30cd7b2442eee7dd6e0eb59d1623e97dc05141fad4d2e8a6d8f"
ANCHOR = "  add sp, sp, s1 # load int section addr\n  ld x1, (1*8)(sp)"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check(ok, message):
    if not ok:
        raise ValueError(message)


def hook(mode, budget, timeout):
    # Registers are not restored yet. sp already points to immutable GPR metadata.
    common = """
  .global bundle_hook_begin
bundle_hook_begin:
  csrr s3, mstatus
  li t0, 0x20008
  csrc mstatus, t0
  csrr s4, mie
  csrw mie, zero
  csrr s5, mtvec
  la t0, bundle_prelude_failure
  csrw mtvec, t0
  li t0, 0x27c200000
  ld t1, 0(t0)
  li t2, 0x432d434e2d503248
  bne t1, t2, bundle_prelude_failure
"""
    if mode != "off":
        base = (1 << 63) | (1 << 51) | (64 << 15) | budget
        start = {"discovery": 1 << 61, "detail": 1 << 60, "replay": 1 << 62}[mode]
        common += f"""  csrr t0, 0x5c0
  li t1, 0x{((15<<59)|(1<<50)):x}
  and t0, t0, t1
  bnez t0, bundle_prelude_failure
  li t0, 0x2780
  csrw 0x5c2, t0
  li t0, 0x2782
  csrw 0x5c1, t0
  fence rw,rw
  li t0, 0x{base|(1<<59):x}
  csrw 0x5c0, t0
  li t3, {timeout}
.Lbundle_load_wait:
  csrr t0, 0x5c0
  li t1, 0x{((1<<59)|(1<<50)):x}
  and t2, t0, t1
  beqz t2, .Lbundle_loaded
  addi t3, t3, -1
  bnez t3, .Lbundle_load_wait
  j bundle_prelude_failure
.Lbundle_loaded:
  bgez t0, bundle_prelude_failure
  li t1, 0x{7<<52:x}
  and t2, t0, t1
  bnez t2, bundle_prelude_failure
  fence rw,rw
  li t0, 0x{base|start:x}
  .global bundle_start_csr
bundle_start_csr:
  csrw 0x5c0, t0
  csrr t0, 0x5c0
  bgez t0, bundle_prelude_failure
  li t1, 0x{start:x}
  and t2, t0, t1
  beqz t2, bundle_prelude_failure
"""
    common += """  .global bundle_control_restore
bundle_control_restore:
  csrw mtvec, s5
  csrw mie, s4
  csrw mstatus, s3
  .global bundle_gpr_tail
bundle_gpr_tail:
"""
    return common


FAILURE = """
  .pushsection ".restore.code","ax",@progbits
  .align 2
  .global bundle_prelude_failure
bundle_prelude_failure:
  li a0, 0xbad
  .insn r 0x6b, 0, 0, x0, x0, x0
  j bundle_prelude_failure
  .popsection
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--source",
        type=Path,
        required=True,
        help="read-only matched gcpt_restore source directory",
    )
    p.add_argument("--output", type=Path, required=True, help="must not exist")
    p.add_argument(
        "--mode", choices=("off", "discovery", "detail", "replay"), required=True
    )
    p.add_argument("--budget-million", type=int, default=3)
    p.add_argument("--poll-iterations", type=int, default=1000000)
    p.add_argument(
        "--cross-prefix",
        default="/home/xuejing/opt/riscv/bin/riscv64-unknown-linux-gnu-",
    )
    p.add_argument(
        "--toolchain-proof",
        type=Path,
        help="JSON schema bundle-restorer-toolchain/v1; tools map absolute paths to SHA256",
    )
    p.add_argument("--generate-source", action="store_true")
    p.add_argument("--build", action="store_true")
    a = p.parse_args()
    try:
        source, output = a.source.resolve(), a.output.absolute()
        check(not output.exists(), "output already exists; no overwrite/resume")
        check(
            not output.resolve().is_relative_to(source),
            "output must not be inside read-only source",
        )
        check(
            1 <= a.budget_million <= 4095 and 1 <= a.poll_iterations <= 1000000,
            "budget1..4095 and poll iterations1..1000000 required (restore watchdog bound)",
        )
        hashes = {name: sha(source / name) for name in SOURCES}
        check(
            hashes["src/restore.S"] == RESTORE_SHA and hashes["restore.lds"] == LDS_SHA,
            "unmatched source assembly/linker; review ABI before adding support",
        )
        original = (source / "src/restore.S").read_text()
        check(original.count(ANCHOR) == 1, "GPR hook anchor drift")
        generated = (
            original.replace(
                ANCHOR,
                "  add sp, sp, s1 # load int section addr\n"
                + hook(a.mode, a.budget_million, a.poll_iterations)
                + "  ld x1, (1*8)(sp)",
            )
            + FAILURE
        )
        # Original no-RVV CSR header selects NOP rather than vector loads under this define.
        check(
            "CHECKPOINT_NO_RVV" in (source / "src/csr.h").read_text(),
            "no-RVV header branch missing",
        )
        tools = {
            name: Path(a.cross_prefix + name).resolve()
            for name in ("gcc", "ld", "objcopy", "objdump", "readelf")
        }
        command = [
            [
                str(tools["gcc"]),
                "-c",
                "-x",
                "assembler-with-cpp",
                "-march=rv64gc",
                "-mabi=lp64d",
                "-DCHECKPOINT_NO_RVV",
                "-fno-pie",
                "-mno-relax",
                "-Isrc",
                "src/restore.S",
                "-o",
                "restore.o",
            ],
            [
                str(tools["ld"]),
                "--no-relax",
                "-T",
                "restore.lds",
                "-o",
                "gcpt.elf",
                "restore.o",
            ],
            [str(tools["objcopy"]), "-O", "binary", "gcpt.elf", "gcpt.bin"],
            [str(tools["objdump"]), "-d", "-M", "no-aliases", "gcpt.elf"],
            [str(tools["readelf"]), "-A", "gcpt.elf"],
        ]
        plan = {
            "schema": "bundle-restorer-source-plan/v1",
            "mode": a.mode,
            "source": str(source),
            "source_sha256": hashes,
            "output": str(output),
            "commands": command,
            "budget_million": a.budget_million,
            "poll_iterations": a.poll_iterations,
            "source_generation_sha256": hashlib.sha256(generated.encode()).hexdigest(),
            "layout": {
                "container": "0x278000000",
                "trace": "0x278200000",
                "trace_bytes": 64 << 20,
                "control": "0x27c200000",
            },
            "temporary_csr_changes": [
                "mstatus MIE/MPRV clear",
                "mie zero",
                "private mtvec",
            ],
            "restore_policy": "restore hook-entry mtvec/mie/mstatus before untouched31ld+mret",
            "profiling_policy": "independent retired budget; includes startup prefix; no phase gate or padding",
            "dma_protocol": "real CSR LOAD polling/start; no emulator or predictor table hooks",
            "off_policy": "same common MIE/MPRV/mtvec/sentinel setup; no Hint CSR instruction",
            "independent_nemu_async_controller_validated": False,
        }
        if not (a.generate_source or a.build):
            print(json.dumps(plan, indent=2))
            return 0
        if a.build:
            check(
                socket.gethostname().split(".")[0]
                in ("ps1", "ps2", "iv-yexl3v2u4gwh2ysmqmn1"),
                "build only on ps1/ps2",
            )
            check(
                a.toolchain_proof is not None,
                "build requires toolchain closure/hash proof",
            )
            proof = json.loads(a.toolchain_proof.read_text())
            check(
                proof.get("schema") == "bundle-restorer-toolchain/v1"
                and proof.get("complete") is True,
                "toolchain is not accepted complete",
            )
            for name, path in tools.items():
                check(
                    proof.get("tools", {}).get(str(path)) == sha(path),
                    f"toolchain hash mismatch: {name}",
                )
            plan["toolchain_proof"] = {
                "path": str(a.toolchain_proof.resolve()),
                "sha256": sha(a.toolchain_proof),
            }
        output.mkdir(parents=True, exist_ok=False)
        (output / "src").mkdir()
        for name in SOURCES:
            shutil.copyfile(source / name, output / name)
        (output / "src/restore.S").write_text(generated)
        (output / "source-plan.json").write_text(json.dumps(plan, indent=2) + "\n")
        if not a.build:
            print(json.dumps(plan, indent=2))
            return 0
        for i, cmd in enumerate(command):
            with (output / f"command-{i}.stdout").open("w") as stdout, (
                output / f"command-{i}.stderr"
            ).open("w") as stderr:
                status = subprocess.run(
                    cmd,
                    cwd=output,
                    stdout=stdout,
                    stderr=stderr,
                    timeout=120,
                    check=False,
                )
                check(
                    status.returncode == 0, f"command{i} failed; private logs retained"
                )
        disasm = (output / "command-3.stdout").read_text()
        # All decoded instructions must be scalar (explicit RV64GC plus CHECKPOINT_NO_RVV).
        instructions = re.findall(
            r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+([^\s]+)", disasm, re.M
        )
        check(
            not any(op.startswith("v") for op in instructions),
            "vector mnemonic in generated restorer",
        )
        attributes = (output / "command-4.stdout").read_text()
        arch = re.search(r"Tag_RISCV_arch: \"([^\"]+)\"", attributes)
        check(
            arch is not None and not re.search(r"(?:^|_)v[0-9]|(?:^|_)zv", arch[1]),
            "ELF ISA attributes include V or are absent",
        )
        raw_path = output / "gcpt.bin"
        manifest = {
            "schema": "bundle-restorer/v1",
            "raw": {"path": str(raw_path), "sha256": sha(raw_path)},
            "elf": {
                "path": str(output / "gcpt.elf"),
                "sha256": sha(output / "gcpt.elf"),
            },
            "disassembly": {
                "path": str(output / "command-3.stdout"),
                "sha256": sha(output / "command-3.stdout"),
            },
            "source_plan": plan,
            "no_rvv_disassembly_gate": True,
            "elf_isa_attributes": arch[1],
            "preserves_saved_state_region": True,
            "runtime_status": "not_executed",
            "requires_control_sentinel": True,
        }
        _, validation = raw_restorer(raw_path, manifest)
        manifest["validation"] = validation
        if a.mode != "off":
            match = re.search(r"^([0-9a-f]+) <bundle_start_csr>:", disasm, re.M)
            check(match is not None, "start symbol absent")
            start = int(match[1], 16)
            manifest["successful_start_to_mret_retired_instructions"] = (
                validation["tail_mret_offset"] - start
            ) // 4
            manifest[
                "prefix_count_policy"
            ] = "static success path after start CSR through mret inclusive; requires runtime confirmation; prefix is inside budget"
        (output / "restorer-manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n"
        )
        print(json.dumps(manifest, indent=2))
        return 0
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        p.exit(2, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
