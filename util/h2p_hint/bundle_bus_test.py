#!/usr/bin/env python3
"""Real guest CSR + physical timing-DMA directed cases; execute only on ps1/ps2.

The raw image contains instructions only. Guest stores construct the container
in RAM, then fence and launch CSR LOAD. No host file substitutes for a DMA read.
Preparation does not run gem5; --run additionally requires an explicit final
binary SHA from a completed build and a compatible NEMU reference.
"""
import argparse
import configparser
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from util.h2p_hint.spec_smoke import require_experiment_host, resource_snapshot

ROOT = Path(__file__).resolve().parents[2]
BASE, CONTAINER, TRACE, COMPETE = 0x80000000, 0x80400000, 0x80800000, 0x80A00000
UART = 0x310B0000
VALID, BUNDLE, LOAD, BUSY = 1 << 63, 1 << 51, 1 << 59, 1 << 50
CASES = {
    "off-startup": {"bundle_enabled": False},
    "load-full": {"error": 0, "reads": 290 + 2098},
    "bad-magic": {"error": 1, "reads": 2},
    "physical-fault": {"error": 4, "access_faults": 1, "reads": 0},
    "pmp-m-locked-deny": {"error": 4, "access_faults": 1, "reads": 0},
    "pmp-s-deny": {"error": 4, "access_faults": 1, "reads": 0},
    "launch-m-poll-s": {"error": 0, "reads": 290 + 2098},
    "launch-s-mprv-m": {"error": 4, "access_faults": 1, "reads": 290},
    "profile-final-write": {"error": 0, "reads": 290, "writes": 3},
}


def need(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def imm(value, bits):
    need(
        -(1 << (bits - 1)) <= value < (1 << (bits - 1)),
        "instruction immediate overflow",
    )
    return value & ((1 << bits) - 1)


def addi(rd, rs, value):
    return imm(value, 12) << 20 | rs << 15 | rd << 7 | 0x13


def csr(number, rd, rs, op):
    return number << 20 | rs << 15 | op << 12 | rd << 7 | 0x73


def store(rs, base, offset=0, size=8):
    value = imm(offset, 12)
    return (
        (value >> 5) << 25
        | rs << 20
        | base << 15
        | {1: 0, 8: 3}[size] << 12
        | (value & 31) << 7
        | 0x23
    )


def load(rd, base, offset=0, size=8):
    return imm(offset, 12) << 20 | base << 15 | {1: 4, 8: 3}[size] << 12 | rd << 7 | 3


def branch(rs1, rs2, offset, different):
    value = imm(offset, 13)
    need(not offset & 1, "unaligned branch")
    return (
        (value >> 12) << 31
        | ((value >> 5) & 63) << 25
        | rs2 << 20
        | rs1 << 15
        | int(different) << 12
        | ((value >> 1) & 15) << 8
        | ((value >> 11) & 1) << 7
        | 0x63
    )


def jump(offset):
    value = imm(offset, 21)
    need(not offset & 1, "unaligned jump")
    return (
        (value >> 20) << 31
        | ((value >> 1) & 1023) << 21
        | ((value >> 11) & 1) << 20
        | ((value >> 12) & 255) << 12
        | 0x6F
    )


class Guest:
    def __init__(self):
        self.code, self.labels, self.serial = [], {}, 0

    @property
    def pc(self):
        return BASE + 4 * len(self.code)

    def emit(self, word, text):
        self.code.append((word, text))

    def label(self, name):
        need(name not in self.labels, "duplicate guest label")
        self.labels[name] = self.pc

    def unique(self, prefix):
        self.serial += 1
        return f"{prefix}_{self.serial}"

    def li(self, rd, value):
        value &= (1 << 64) - 1
        digits = f"{value:x}"
        self.emit(addi(rd, 0, int(digits[0], 16)), f"li.start x{rd}, 0x{digits[0]}")
        shift = 0
        for digit in digits[1:]:
            shift += 4
            if int(digit, 16):
                self.emit(
                    shift << 20 | rd << 15 | 1 << 12 | rd << 7 | 0x13,
                    f"slli x{rd}, x{rd}, {shift}",
                )
                self.emit(addi(rd, rd, int(digit, 16)), f"addi x{rd}, x{rd}, 0x{digit}")
                shift = 0
        if shift:
            self.emit(
                shift << 20 | rd << 15 | 1 << 12 | rd << 7 | 0x13,
                f"slli x{rd}, x{rd}, {shift}",
            )

    def br(self, rs1, rs2, label, different=True):
        pc = self.pc
        self.emit(
            lambda labels: branch(rs1, rs2, labels[label] - pc, different),
            f"{'bne' if different else 'beq'} x{rs1}, x{rs2}, {label}",
        )

    def j(self, label):
        pc = self.pc
        self.emit(lambda labels: jump(labels[label] - pc), f"j {label}")

    def address(self, rd, label):
        pc = self.pc
        self.emit(
            lambda labels: (((labels[label] - pc + 0x800) >> 12) & 0xFFFFF) << 12
            | rd << 7
            | 0x17,
            f"auipc x{rd}, %pcrel_hi({label})",
        )
        self.emit(
            lambda labels: addi(
                rd,
                rd,
                (labels[label] - pc) - (((labels[label] - pc + 0x800) >> 12) << 12),
            ),
            f"addi x{rd}, x{rd}, %pcrel_lo({label})",
        )

    def equal(self, rs, value, failure):
        okay = self.unique("check")
        self.li(6, value)
        self.br(rs, 6, okay, different=False)
        self.li(31, failure)
        self.j("fail")
        self.label(okay)

    def clear(self, address, size):
        need(size % 8 == 0, "clear size must be a whole beat")
        self.li(5, address)
        self.li(6, size // 8)
        loop = self.unique("clear")
        self.label(loop)
        self.emit(store(0, 5), "sd zero, 0(t0)")
        self.emit(addi(5, 5, 8), "addi t0,t0,8")
        self.emit(addi(6, 6, -1), "addi t1,t1,-1")
        self.br(6, 0, loop)

    def put(self, address, value):
        self.li(5, address)
        self.li(6, value)
        self.emit(store(6, 5), f"sd t1, [{address:#x}]")

    def message(self, text):
        self.li(5, UART)
        for character in text:
            wait = self.unique("tx")
            self.label(wait)
            self.emit(load(6, 5, 20, 1), "lbu t1, UART.LSR")
            self.emit(0x04037313, "andi t1,t1,64")
            self.br(6, 0, wait, different=False)
            self.li(6, ord(character))
            self.emit(store(6, 5, 0, 1), f"sb {character!r}, UART.THR")
        wait = self.unique("tx_done")
        self.label(wait)
        self.emit(load(6, 5, 20, 1), "lbu t1, UART.LSR")
        self.emit(0x04037313, "andi t1,t1,64")
        self.br(6, 0, wait, different=False)
        self.emit(0x0330000F, "fence rw,rw")

    def output(self, directory):
        words = [word(self.labels) if callable(word) else word for word, _ in self.code]
        raw = directory / "guest.bin"
        raw.write_bytes(struct.pack(f"<{len(words)}I", *words))
        (directory / "guest.asm").write_text(
            "\n".join(
                f"{BASE+4*i:016x}: {word:08x}  {self.code[i][1]}"
                for i, word in enumerate(words)
            )
            + "\n"
        )
        return raw


def make_guest(case, directory):
    expected = CASES[case]
    g = Guest()
    g.label("entry")
    if case == "off-startup":
        # No Bundle CSR access: exercise ordinary Off init/tick/termination.
        g.message("BUNDLE_BUS_PASS off-startup\n")
        g.li(10, 0)
        g.emit(0x6B, "nemu_trap # ordinary Off startup negative control")
        g.label("stop")
        g.j("stop")
        directory.mkdir(parents=True, exist_ok=False)
        raw = g.output(directory)
        save(
            directory / "guest.json",
            {
                "schema": "hint-bundle-bus-guest/v1",
                "case": case,
                "raw_sha256": sha(raw),
                "base": hex(BASE),
                "expected": expected,
                "instruction_count": len(g.code),
                "accesses_bundle_csr": False,
                "guest_constructs_container": False,
                "uses_host_dma_image": False,
            },
        )
        return raw
    # Any unexpected trap reports failure rather than silently looping.
    g.address(5, "launch_s_ecall_handler" if case == "launch-s-mprv-m" else "trap_fail")
    g.emit(csr(0x305, 0, 5, 1), "csrw mtvec,t0")
    g.emit(csr(0x304, 0, 0, 1), "csrw mie,zero")
    if case == "launch-s-mprv-m":
        g.emit(csr(0x302, 0, 0, 1), "csrw medeleg,zero # S ECALL must enter M")
    g.li(31, 0)
    g.clear(CONTAINER, 2320)
    g.clear(CONTAINER + 0x8000, 16784)
    g.put(CONTAINER, int.from_bytes(b"H2PBND01", "little"))
    # kind=2, initial_valid=1, initial_id=0, valid_mask=1; zero routing tables.
    g.put(CONTAINER + 8, 2 | (32 << 8) | (1 << 32))
    g.put(CONTAINER + 0x8000, int.from_bytes(b"H2PLD058", "little"))
    g.put(CONTAINER + 0x8010, (BASE >> 1) | (1 << 51) | (1 << 52))
    g.put(CONTAINER + 0x8090, (1 << 24) | (3 << 22))
    g.put(CONTAINER + 0xC090, BASE >> 1)
    g.put(COMPETE, 0)
    if case == "profile-final-write":
        g.put(CONTAINER + 8, 0)  # kind=0, no initial metadata; routes remain empty.
        g.clear(TRACE, 32)
    if case == "bad-magic":
        g.put(CONTAINER, 0xDEAD)

    # CSR zero-source writes must really write zero; read-only CSRRS zero must
    # not trigger a controller write. Test both register and immediate forms.
    g.li(5, CONTAINER >> 20)
    g.emit(csr(0x5C2, 0, 5, 1), "csrw ibphintladdr,t0")
    g.emit(csr(0x5C2, 0, 0, 1), "csrrw zero,ibphintladdr,zero")
    g.emit(csr(0x5C2, 7, 0, 2), "csrr t2,ibphintladdr")
    g.equal(7, 0, 1)
    g.emit(csr(0x5C2, 0, 5, 1), "csrw ibphintladdr,t0")
    g.emit(csr(0x5C2, 0, 0, 5), "csrwi ibphintladdr,0")
    g.emit(csr(0x5C2, 7, 0, 2), "csrr t2,ibphintladdr")
    g.equal(7, 0, 2)
    g.li(5, 0x700 if case == "physical-fault" else CONTAINER >> 20)
    g.emit(csr(0x5C2, 0, 5, 1), "csrw ibphintladdr,t0")
    g.emit(csr(0x5C2, 7, 0, 2), "csrr t2,ibphintladdr")
    g.equal(7, 0x700 if case == "physical-fault" else CONTAINER >> 20, 3)
    g.li(5, TRACE >> 20)
    g.emit(csr(0x5C1, 0, 5, 1), "csrw ibphintpaddr,t0")
    g.emit(csr(0x5C1, 7, 0, 2), "csrr t2,ibphintpaddr")
    g.equal(7, TRACE >> 20, 4)
    g.emit(0x0330000F, "fence rw,rw # publish image stores before DMA")

    restricted = case in (
        "pmp-m-locked-deny",
        "pmp-s-deny",
        "launch-m-poll-s",
        "launch-s-mprv-m",
    )
    if restricted:
        if case == "launch-s-mprv-m":
            g.li(5, ((CONTAINER + 0x8000) >> 2) | ((1 << 9) - 1))
            g.emit(csr(0x3B0, 0, 5, 1), "csrw pmpaddr0,inner image first 4KiB NAPOT")
        else:
            g.li(5, (CONTAINER >> 2) | ((1 << 17) - 1))
            g.emit(csr(0x3B0, 0, 5, 1), "csrw pmpaddr0,container 1MiB NAPOT")
        g.li(5, (1 << 54) - 1)
        g.emit(csr(0x3B1, 0, 5, 1), "csrw pmpaddr1,all memory NAPOT")
        g.li(5, (0x1F << 8) | (0x98 if case == "pmp-m-locked-deny" else 0x18))
        g.emit(csr(0x3A0, 0, 5, 1), "csrw pmpcfg0,deny container then allow others")

    def enter_supervisor():
        g.address(5, "supervisor")
        g.emit(csr(0x341, 0, 5, 1), "csrw mepc,t0")
        g.emit(csr(0x300, 5, 0, 2), "csrr t0,mstatus")
        g.li(6, (~((3 << 11) | (1 << 17))) & ((1 << 64) - 1))
        g.emit(
            6 << 20 | 5 << 15 | 7 << 12 | 5 << 7 | 0x33, "and t0,t0,t1 # clear MPP/MPRV"
        )
        g.li(6, 1 << 11)
        g.emit(6 << 20 | 5 << 15 | 6 << 12 | 5 << 7 | 0x33, "or t0,t0,t1 # MPP=S")
        g.emit(csr(0x300, 0, 5, 1), "csrw mstatus,t0")
        g.emit(0x30200073, "mret")

    if case in ("pmp-s-deny", "launch-s-mprv-m"):
        enter_supervisor()
        g.label("supervisor")
    configuration = (
        VALID | BUNDLE | ((1 << 15) | 1 if case == "profile-final-write" else 0)
    )
    g.li(5, configuration | LOAD)
    g.emit(csr(0x5C0, 0, 5, 1), "csrw ibphintstatus,VALID|BUNDLE|LOAD")
    if case == "launch-s-mprv-m":
        g.label("launch_s_ecall_inst")
        g.emit(0x00000073, "ecall # S launch is already committed; trap to M")
        g.label("launch_s_ecall_handler")
        # Validate this really was our S ECALL, rather than accepting an
        # unrelated trap as evidence of a successful privilege transition.
        g.emit(csr(0x342, 7, 0, 2), "csrr t2,mcause")
        g.equal(7, 9, 12)
        g.emit(csr(0x341, 7, 0, 2), "csrr t2,mepc")
        g.address(6, "launch_s_ecall_inst")
        expected_ecall = g.unique("expected_ecall")
        g.br(7, 6, expected_ecall, different=False)
        g.li(31, 13)
        g.j("fail")
        g.label(expected_ecall)
        g.li(5, (1 << 17) | (3 << 11))
        g.emit(csr(0x300, 0, 5, 2), "csrs mstatus,MPRV|MPP_M # remain in M")
        g.emit(csr(0x300, 7, 0, 2), "csrr t2,mstatus")
        g.emit(
            5 << 20 | 7 << 15 | 7 << 12 | 7 << 7 | 0x33,
            "and t2,t2,t0 # confirm MPRV=1 and MPP=M",
        )
        g.equal(7, (1 << 17) | (3 << 11), 14)
    if case == "launch-m-poll-s":
        enter_supervisor()
        g.label("supervisor")
    g.li(28, 200000)
    g.li(29, LOAD | BUSY)
    g.li(30, COMPETE)
    g.label("poll")
    g.emit(csr(0x5C0, 7, 0, 2), "csrr t2,ibphintstatus")
    g.emit(load(5, 30), "ld t0,0(t5) # genuine Core traffic competing with DMA")
    g.emit(29 << 20 | 7 << 15 | 7 << 12 | 5 << 7 | 0x33, "and t0,t2,t4")
    g.br(5, 0, "done", different=False)
    g.emit(addi(28, 28, -1), "addi t3,t3,-1")
    g.br(28, 0, "poll")
    g.li(31, 5)
    g.j("fail")
    g.label("done")
    g.emit(52 << 20 | 7 << 15 | 5 << 12 | 5 << 7 | 0x13, "srli t0,t2,52")
    g.emit(7 << 20 | 5 << 15 | 7 << 12 | 5 << 7 | 0x13, "andi t0,t0,7")
    g.equal(5, expected["error"], 6)
    if expected["error"] == 0:
        g.emit(63 << 20 | 7 << 15 | 5 << 12 | 5 << 7 | 0x13, "srli t0,t2,63")
        g.equal(5, 1, 7)
    if case == "profile-final-write":
        g.li(5, configuration | (1 << 61))
        g.emit(csr(0x5C0, 0, 5, 1), "csrw ibphintstatus,Discovery_Started")
        g.li(28, 200000)
        g.li(29, (1 << 61) | BUSY)
        g.label("profile_poll")
        g.emit(csr(0x5C0, 7, 0, 2), "csrr t2,ibphintstatus")
        g.emit(load(5, 30), "ld t0,0(t5) # Core competes with trace writes")
        g.emit(29 << 20 | 7 << 15 | 7 << 12 | 5 << 7 | 0x33, "and t0,t2,t4")
        g.br(5, 0, "profile_done", different=False)
        g.emit(addi(28, 28, -1), "addi t3,t3,-1")
        g.br(28, 0, "profile_poll")
        g.li(31, 9)
        g.j("fail")
        g.label("profile_done")
        g.emit(0x0330000F, "fence rw,rw # DMA done before guest buffer reads")
        g.emit(52 << 20 | 7 << 15 | 5 << 12 | 5 << 7 | 0x13, "srli t0,t2,52")
        g.emit(127 << 20 | 5 << 15 | 7 << 12 | 5 << 7 | 0x13, "andi t0,t0,127")
        g.equal(5, 0, 10)
        g.li(30, TRACE)
        for offset, value in ((0, 1 << 54), (8, 0), (16, 0)):
            g.emit(load(5, 30, offset), f"ld t0,trace+{offset}")
            g.equal(5, value, 11)
    g.message(f"BUNDLE_BUS_PASS {case}\n")
    g.li(10, 0)
    g.emit(0x6B, "nemu_trap # stop after successful guest checks")
    g.j("done")
    g.label("trap_fail")
    g.li(31, 8)
    g.label("fail")
    g.message(f"BUNDLE_BUS_FAIL {case}\n")
    g.li(10, 1)
    g.emit(0x6B, "nemu_trap # runner rejects missing PASS or FAIL marker")
    g.j("fail")
    directory.mkdir(parents=True, exist_ok=False)
    raw = g.output(directory)
    manifest = {
        "schema": "hint-bundle-bus-guest/v1",
        "case": case,
        "raw_sha256": sha(raw),
        "base": hex(BASE),
        "container": hex(CONTAINER),
        "trace": hex(TRACE),
        "expected": expected,
        "instruction_count": len(g.code),
        "labels": {k: hex(v) for k, v in g.labels.items()},
        "guest_constructs_container": True,
        "uses_host_dma_image": False,
    }
    save(directory / "guest.json", manifest)
    return raw


def run_case(case, args):
    directory = args.output_root / case
    raw = make_guest(case, directory)
    out = directory / "simulation"
    out.mkdir()
    need(
        sha(args.gem5) == args.binary_sha256,
        "Candidate binary changed before invocation",
    )
    command = [
        str(args.gem5),
        "-d",
        str(out),
        str(ROOT / "configs/example/backend_v2_norvv_nol3.py"),
        "--raw-cpt",
        f"--generic-rv-cpt={raw}",
        "--warmup-insts-no-switch=1",
        "--maxinsts=2000000",
        "--enable-difftest",
        f"--difftest-ref-so={args.ref_so}",
        "--h2p-stat",
    ]
    bundle_enabled = case != "off-startup"
    if bundle_enabled:
        command.extend(
            [
                "--h2p-hint-bundle",
                "--h2p-hint-bundle-audit",
                "--h2p-hint-nc-range=0x80400000:0x800000",
            ]
        )
    environment = os.environ.copy()
    environment["GCBV_REF_SO"] = str(args.ref_so)
    environment["GEM5_REQUIRE_DIFFTEST_ABI"] = "1"
    save(directory / "command.json", command)
    (directory / "command.txt").write_text(shlex.join(command) + "\n")
    with (out / "stdout.log").open("w") as stdout, (out / "stderr.log").open(
        "w"
    ) as stderr:
        process = subprocess.run(
            command,
            cwd=ROOT,
            env=environment,
            stdout=stdout,
            stderr=stderr,
            timeout=args.timeout_seconds,
        )
    logs = (out / "stdout.log").read_text() + (out / "stderr.log").read_text()
    need(process.returncode == 0, f"gem5 failed: {case}")
    need(
        not re.search(
            r"fatal:|panic:|Assertion .*failed|difftest.*(?:fail|mismatch)", logs, re.I
        ),
        f"Simulator error: {case}",
    )
    need(
        f"BUNDLE_BUS_PASS {case}" in logs and "BUNDLE_BUS_FAIL" not in logs,
        f"Guest checks did not pass: {case}",
    )
    need(
        "m5_exit instruction encountered" in logs,
        f"Guest did not terminate itself: {case}",
    )
    config = (out / "config.ini").read_text()
    need(
        "enable_difftest=true" in config and f"difftest_ref_so={args.ref_so}" in config,
        "NEMU difftest was not enabled with the expected reference",
    )
    need(
        f"hintBundleEnable={str(bundle_enabled).lower()}" in config,
        "Bundle capability differs from this directed case",
    )
    if not bundle_enabled:
        need(
            "HINT_BUNDLE_DMA_AUDIT " not in logs,
            "Off startup unexpectedly submitted Bundle DMA",
        )
        need(
            sha(args.gem5) == args.binary_sha256, "Candidate binary changed during test"
        )
        result = {
            "case": case,
            "pass": True,
            "bundle_enabled": False,
            "dma_submitted": False,
            "binary_sha256": args.binary_sha256,
            "reference_sha256": sha(args.ref_so),
        }
        save(directory / "result.json", result)
        return result
    need("hintBundleAudit=true" in config, "Bundle observer audit was not enabled")
    jsonl_rows = {}
    for name in (
        "bundle-committed-cfi.jsonl",
        "bundle-controller-audit.jsonl",
        "bundle-trace-audit.jsonl",
    ):
        data = (out / name).read_bytes()
        need(not data or data.endswith(b"\n"), f"Incomplete final JSONL record: {name}")
        rows = [json.loads(line) for line in data.splitlines() if line.strip()]
        need(all(isinstance(row, dict) for row in rows), f"Invalid JSONL event: {name}")
        jsonl_rows[name] = len(rows)
    parsed_config = configparser.ConfigParser(interpolation=None)
    parsed_config.read_string(config)
    nc_ranges = parsed_config.get("system.cpu.mmu.pma_checker", "uncacheable").split()
    need(
        f"{CONTAINER}:{CONTAINER + 0x800000}" in nc_ranges,
        "Container/Trace/competing Core RAM was not configured as real PMA Uncached",
    )
    audit_rows = re.findall(r"HINT_BUNDLE_DMA_AUDIT (\{[^\n]+\})", logs)
    need(len(audit_rows) == 1, f"Missing unique DMA observer summary: {case}")
    audit = json.loads(audit_rows[0])
    expected = CASES[case]
    need(audit["schema"] == "hint-bundle-dma-audit/v1", "Unexpected DMA audit schema")
    need(
        audit["pending"] == 0 and audit["priority_violations"] == 0,
        "DMA pending/priority invariant failed",
    )
    need(
        audit["shared_uncached_max_outstanding"] <= 1
        and audit["shared_uncached_pending"] == 0,
        "Shared Core/DMA Uncached ownership invariant failed",
    )
    need(
        audit["core_uncached_accepted"] == audit["core_uncached_responses"],
        "Core Uncached request/response accounting failed",
    )
    need(
        audit["accepted"] == audit["responses"] == audit["reads"] + audit["writes"],
        "DMA request/response conservation failed",
    )
    need(
        audit["submitted"] == audit["responses"] + audit["access_faults"],
        "DMA completion accounting failed",
    )
    need(
        audit["response_errors"] == 0
        and audit["reads"] == expected["reads"]
        and audit["writes"] == expected.get("writes", 0)
        and audit["access_faults"] == expected.get("access_faults", 0),
        "Unexpected DMA beats or protection outcome",
    )
    need(sha(args.gem5) == args.binary_sha256, "Candidate binary changed during test")
    downstream_refusals = audit["send_refused"] - audit["tag_conflicts"]
    need(downstream_refusals >= 0, "Tag refusals exceed all DMA send refusals")
    result = {
        "case": case,
        "pass": True,
        "dma": audit,
        "jsonl_rows": jsonl_rows,
        "reserved_ram_pma_uncached": True,
        "core_priority_exercised": audit["core_priority_deferrals"] > 0,
        "retry_exercised": downstream_refusals > 0 and audit["retry_notifications"] > 0,
        "downstream_refusals": downstream_refusals,
        "downstream_retry_exercised": downstream_refusals > 0
        and audit["retry_notifications"] > 0,
        "tag_retry_exercised": audit["tag_conflicts"] > 0,
        "binary_sha256": args.binary_sha256,
        "reference_sha256": sha(args.ref_so),
    }
    save(directory / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--case", action="append", choices=sorted(CASES))
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--gem5", type=Path)
    parser.add_argument("--binary-sha256")
    parser.add_argument("--ref-so", type=Path)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    args = parser.parse_args()
    require_experiment_host()
    args.output_root = args.output_root.resolve()
    allowed = ROOT.parent / "results/model/smoke-test"
    need(
        args.output_root.is_relative_to(allowed) and args.output_root != allowed,
        "Use a dedicated output directory under results/model/smoke-test",
    )
    args.output_root.mkdir(parents=True, exist_ok=False)
    cases = args.case or [
        case for case in CASES if case not in ("off-startup", "launch-s-mprv-m")
    ]
    need(len(set(cases)) == len(cases), "Duplicate test cases")
    if not args.run:
        for case in cases:
            make_guest(case, args.output_root / case)
        save(
            args.output_root / "prepared.json",
            {"cases": cases, "simulations_started": 0},
        )
        return
    need(
        args.gem5 and args.ref_so and args.binary_sha256,
        "--run needs completed ELF, SHA and reference",
    )
    args.gem5, args.ref_so = args.gem5.resolve(strict=True), args.ref_so.resolve(
        strict=True
    )
    need(
        args.gem5.open("rb").read(4) == b"\x7fELF" and os.access(args.gem5, os.X_OK),
        "Invalid candidate ELF",
    )
    budget = resource_snapshot(memory_per_job_gib=12)
    need(
        0 < args.jobs <= budget["job_capacity"],
        "Requested jobs exceed remaining CPU/memory capacity",
    )
    save(args.output_root / "resources.json", budget)
    with ThreadPoolExecutor(max_workers=min(args.jobs, len(cases))) as executor:
        results = list(executor.map(lambda case: run_case(case, args), cases))
    save(
        args.output_root / "summary.json",
        {
            "schema": "hint-bundle-bus-test/v1",
            "pass": True,
            "cases": results,
            "limits": [
                "Downstream retry coverage requires non-tag send refusal and real retry notification; tag retries are reported separately",
                "No external memory response-error injection is configured; physical/PMP errors are directed",
                "Full 10-slice profiling/IPC experiments are separate",
            ],
        },
    )


if __name__ == "__main__":
    main()
