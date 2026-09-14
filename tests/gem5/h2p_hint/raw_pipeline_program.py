#!/usr/bin/env python3
"""Generate a small RV64 bare-metal trap/indirect-control pipeline workload.

No cross compiler is required. The instruction words, labels, readable assembly
and a small independent architectural execution check are emitted together.
The checker validates this test's program, not gem5 or predictor timing.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct


BASE = 0x80000000
REG_MASK = (1 << 64) - 1
NOP = 0x00000013


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def immediate(value, bits):
    if not -(1 << (bits - 1)) <= value < 1 << (bits - 1):
        raise ValueError(f"immediate {value} does not fit signed {bits} bits")
    return value & ((1 << bits) - 1)


def addi(rd, rs1, value):
    return immediate(value, 12) << 20 | rs1 << 15 | rd << 7 | 0x13


def jalr(rd, rs1, value=0):
    return immediate(value, 12) << 20 | rs1 << 15 | rd << 7 | 0x67


def jal(rd, offset):
    value = immediate(offset, 21)
    if offset & 1:
        raise ValueError("JAL destination is not halfword aligned")
    return ((value >> 20) << 31 | ((value >> 1) & 0x3ff) << 21 |
            ((value >> 11) & 1) << 20 | ((value >> 12) & 0xff) << 12 |
            rd << 7 | 0x6f)


def bne(rs1, rs2, offset):
    value = immediate(offset, 13)
    if offset & 1:
        raise ValueError("branch destination is not halfword aligned")
    return ((value >> 12) << 31 | ((value >> 5) & 0x3f) << 25 |
            rs2 << 20 | rs1 << 15 | 1 << 12 |
            ((value >> 1) & 0xf) << 8 | ((value >> 11) & 1) << 7 | 0x63)


def csr(number, rd, rs1, funct3):
    return number << 20 | rs1 << 15 | funct3 << 12 | rd << 7 | 0x73


class Program:
    def __init__(self):
        self.pc = BASE
        self.labels = {}
        self.instructions = []

    def at(self, offset):
        if BASE + offset < self.pc:
            raise ValueError("overlapping program regions")
        self.pc = BASE + offset

    def label(self, name):
        if name in self.labels:
            raise ValueError(f"duplicate label {name}")
        self.labels[name] = self.pc

    def emit(self, word, text):
        self.instructions.append((self.pc, word, text))
        self.pc += 4

    def jump(self, rd, label):
        pc = self.pc
        self.emit(lambda labels: jal(rd, labels[label] - pc),
                  f"jal x{rd}, {label}")

    def branch_ne(self, rs1, rs2, label):
        pc = self.pc
        self.emit(lambda labels: bne(rs1, rs2, labels[label] - pc),
                  f"bne x{rs1}, x{rs2}, {label}")

    def address(self, rd, label):
        pc = self.pc

        def high(labels):
            return (labels[label] - pc + 0x800) >> 12

        self.emit(lambda labels: (immediate(high(labels), 20) << 12 |
                                  rd << 7 | 0x17),
                  f"auipc x{rd}, pcrel_hi({label})")
        self.emit(lambda labels: addi(rd, rd, labels[label] - pc -
                                     (high(labels) << 12)),
                  f"addi x{rd}, x{rd}, pcrel_lo({label})")

    def words(self):
        return {pc: word(self.labels) if callable(word) else word
                for pc, word, _ in self.instructions}


def make_program():
    p = Program()
    p.label("entry")
    p.address(5, "handler")
    p.emit(csr(0x305, 0, 5, 1), "csrw mtvec, x5")
    p.emit(csr(0x300, 0, 8, 7), "csrci mstatus, 8")
    p.emit(csr(0x304, 0, 0, 1), "csrw mie, zero")
    for reg, purpose in ((9, "iterations"), (18, "traps"),
                         (19, "pre-trap ALU"), (20, "subroutine calls"),
                         (31, "failure sentinel")):
        p.emit(addi(reg, 0, 0), f"addi x{reg}, zero, 0 # {purpose}")
    p.jump(0, "body")

    p.at(0x100)
    p.label("body")
    p.emit(addi(9, 9, 1), "addi s1, s1, 1")
    p.address(10, "after_fallthrough_call")
    p.label("fallthrough_call")
    p.emit(jalr(1, 10), "jalr ra, a0, 0 # indirect call to next instruction")
    p.label("after_fallthrough_call")
    p.address(11, "after_fallthrough_jump")
    p.label("fallthrough_jump")
    p.emit(jalr(0, 11), "jalr zero, a1, 0 # indirect jump to next instruction")
    p.label("after_fallthrough_jump")
    p.label("not_taken")
    p.branch_ne(0, 0, "fail_near_body")
    p.label("pretrap_alu")
    p.emit(addi(19, 19, 1), "addi s3, s3, 1 # last retired PC before ecall")
    p.label("ecall")
    p.emit(0x00000073, "ecall")
    p.label("after_ecall")
    p.branch_ne(9, 18, "fail_near_body")
    p.label("direct_call")
    p.jump(1, "subroutine")
    p.address(5, "after_pop_push")
    p.label("pop_push")
    p.emit(jalr(1, 5), "jalr ra, t0, 0 # rs1=x5 and rd=x1: pop then push")
    p.label("after_pop_push")
    p.jump(0, "body")
    if p.pc != BASE + 0x140:
        raise ValueError("body no longer fits the intended 64-byte block")

    p.at(0x200)
    p.label("fail_near_body")
    p.jump(0, "bad")
    p.at(0x1000)
    p.label("subroutine")
    p.emit(addi(20, 20, 1), "addi s4, s4, 1")
    p.label("return")
    p.emit(jalr(0, 1), "jalr zero, ra, 0 # ordinary return")

    p.at(0x2000)
    p.label("handler")
    p.emit(csr(0x342, 7, 0, 2), "csrr t2, mcause")
    p.emit(addi(28, 0, 11), "addi t3, zero, 11 # M-mode ecall")
    p.branch_ne(7, 28, "fail_near_handler")
    p.emit(addi(18, 18, 1), "addi s2, s2, 1")
    p.emit(csr(0x341, 7, 0, 2), "csrr t2, mepc")
    p.emit(addi(7, 7, 4), "addi t2, t2, 4")
    p.emit(csr(0x341, 0, 7, 1), "csrw mepc, t2")
    p.label("handler_mret")
    p.emit(0x30200073, "mret")
    p.at(0x2080)
    p.label("fail_near_handler")
    p.jump(0, "bad")
    p.at(0x3000)
    p.label("bad")
    p.emit(addi(31, 0, -1), "addi t6, zero, -1 # visible failure sentinel")
    p.label("bad_spin")
    p.jump(0, "bad_spin")
    return p


def architectural_check(program, steps=20000):
    """Execute the small emitted instruction subset to check control flow."""
    words, registers, csrs = program.words(), [0] * 32, {}
    pc, visited, traps = BASE, Counter(), 0
    forbidden = {program.labels[name] for name in
                 ("bad", "bad_spin", "fail_near_body", "fail_near_handler")}
    for _ in range(steps):
        if pc in forbidden or pc not in words:
            raise ValueError(f"architectural program check reached {pc:#x}")
        visited[pc] += 1
        word = words[pc]
        opcode, rd, funct3, rs1, rs2 = (word & 0x7f, word >> 7 & 31,
                                        word >> 12 & 7, word >> 15 & 31,
                                        word >> 20 & 31)
        next_pc = pc + 4
        if opcode == 0x13 and funct3 == 0:
            registers[rd] = registers[rs1] + signed(word >> 20, 12)
        elif opcode == 0x17:
            registers[rd] = pc + signed(word & 0xfffff000, 32)
        elif opcode == 0x67 and funct3 == 0:
            next_pc = (registers[rs1] + signed(word >> 20, 12)) & ~1
            registers[rd] = pc + 4
        elif opcode == 0x6f:
            imm = ((word >> 31) << 20 | ((word >> 21) & 0x3ff) << 1 |
                   ((word >> 20) & 1) << 11 | ((word >> 12) & 0xff) << 12)
            registers[rd], next_pc = pc + 4, pc + signed(imm, 21)
        elif opcode == 0x63 and funct3 == 1:
            imm = ((word >> 31) << 12 | ((word >> 25) & 0x3f) << 5 |
                   ((word >> 8) & 0xf) << 1 | ((word >> 7) & 1) << 11)
            if registers[rs1] != registers[rs2]:
                next_pc = pc + signed(imm, 13)
        elif word == 0x00000073:
            csrs[0x341], csrs[0x342] = pc, 11
            next_pc = csrs[0x305] & ~3
            traps += 1
        elif word == 0x30200073:
            next_pc = csrs[0x341]
        elif opcode == 0x73:
            number, old = word >> 20, csrs.get(word >> 20, 0)
            if funct3 == 1:
                csrs[number] = registers[rs1]
            elif funct3 == 2:
                csrs[number] = old | registers[rs1]
            elif funct3 == 7:
                csrs[number] = old & ~rs1
            else:
                raise ValueError(f"unsupported CSR instruction {word:#x}")
            registers[rd] = old
        else:
            raise ValueError(f"unsupported instruction {word:#x}")
        registers[rd] &= REG_MASK
        registers[0] = 0
        pc = next_pc & REG_MASK
    if traps < 20 or registers[31] != 0:
        raise ValueError("program did not repeatedly exercise the trap path")
    return {"steps": steps, "traps": traps, "failure_sentinel": registers[31],
            "visited": {name: visited[address] for name, address in program.labels.items()}}


def generate(output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    program = make_program()
    words = program.words()
    image = bytearray(struct.pack("<I", NOP) * ((max(words) - BASE + 4) // 4))
    for pc, word in words.items():
        struct.pack_into("<I", image, pc - BASE, word)
    (output / "raw_pipeline.bin").write_bytes(image)
    label_at = {}
    for name, pc in program.labels.items():
        label_at.setdefault(pc, []).append(name)
    assembly = [".section .text", ".option norvc", ".globl entry"]
    previous = BASE
    for pc, _, text in program.instructions:
        if pc != previous:
            assembly.append(f".org {pc - BASE:#x}")
        assembly.extend(name + ":" for name in label_at.get(pc, []))
        assembly.append(f"    .4byte {words[pc]:#010x} # {pc:#x}: {text}")
        previous = pc + 4
    (output / "raw_pipeline.S").write_text("\n".join(assembly) + "\n")
    candidates = [program.labels[name] for name in
                  ("fallthrough_call", "fallthrough_jump", "pop_push", "return")]
    source = {"purpose": "raw CPU pipeline trap recovery gate",
              "image_sha256": hashlib.sha256(image).hexdigest()}
    (output / "candidates.json").write_text(json.dumps({
        "format": "h2p-hint-candidates", "version": 2,
        "pcs": [hex(pc) for pc in candidates], "source": source}, indent=2) + "\n")
    metadata = {
        "format": "h2p-hint", "version": 2, "hash_version": 2,
        "ghr_bits": 970, "mask_order": ["GHR", "GITHR", "LITHR", "Call", "Return"],
        "history": {"githr_targets": 64, "lithr_targets": 4, "call_pcs": 64,
                    "return_pcs": 64, "target_shift": 1},
        "layout": {"cam_entries": 16, "context_entries": 4096, "ways": 2,
                   "slot_entries": 32, "conf_queue_depth": 8},
        "cam": [], "slots": [], "entries": [], "source": source}
    (output / "empty-metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    manifest = {"format": "h2p-hint-raw-pipeline-program", "version": 1,
                "base": hex(BASE), **source,
                "labels": {name: hex(pc) for name, pc in program.labels.items()},
                "candidates": [hex(pc) for pc in candidates],
                "architectural_check": architectural_check(program)}
    (output / "program.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(generate(args.output_dir), indent=2))
