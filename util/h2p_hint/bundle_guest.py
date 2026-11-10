#!/usr/bin/env python3
"""Emit supervisor guest routines for real Bundle CSR writes/fence/poll.

The caller supplies mapped, physically contiguous NC memory and invokes prepare,
begin, wait/disable around its workload. These are guest instructions, not host
configuration substitutions. No guest memory placement or checkpoint is guessed.
"""
import argparse
from pathlib import Path


def assembly(container, trace, trace_mib, instructions_million, mode, timeout=10000000):
    if mode not in ("discovery", "detail", "replay"):
        raise ValueError("invalid Bundle mode")
    if container < 0 or trace < 0 or container % (1 << 20) or trace % (1 << 20):
        raise ValueError("physical buffers must be nonnegative and 1MiB aligned")
    if max(container + 0x108000, trace + (trace_mib << 20)) > 1 << 64:
        raise ValueError("physical range wraps")
    if not (
        1 <= trace_mib <= 4095 and 1 <= instructions_million <= 4095 and timeout > 0
    ):
        raise ValueError("CSR budget fields or poll timeout out of range")
    if max(container, trace) < min(container + 0x108000, trace + (trace_mib << 20)):
        raise ValueError("Trace must not overlap any part of container")
    base = (1 << 63) | (1 << 51) | (trace_mib << 15) | instructions_million
    started = (
        (1 << 61)
        if mode == "discovery"
        else ((1 << 60) if mode == "detail" else (1 << 62))
    )
    # Preserve only software configuration, never copy hardware error bits back.
    return f"""# Generated Bundle guest CSR routines. a0=0 success, -1 timeout,
# -2 hardware error/rejected write, -3 busy on prepare. t0-t4 clobbered.
# Caller privilege: supervisor or machine; NC physical buffers must be fixed.
.option norvc
.section .text
.align 2
.globl hint_bundle_prepare
hint_bundle_prepare:
  csrr t0, 0x5c0
  li t1, 0x{((15<<59)|(1<<50)):016x}
  and t0, t0, t1
  bnez t0, .Lbusy
  li t0, 0x{container>>20:x}
  csrw 0x5c2, t0
  li t0, 0x{trace>>20:x}
  csrw 0x5c1, t0
  fence rw,rw
  li t0, 0x{base|(1<<59):016x}
  csrw 0x5c0, t0
  li t3, {timeout}
.Lload_wait:
  csrr t0, 0x5c0
  li t1, 0x{((1<<59)|(1<<50)):016x}
  and t2, t0, t1
  beqz t2, .Lload_done
  addi t3, t3, -1
  bnez t3, .Lload_wait
  j .Ltimeout
.Lload_done:
  li t1, 0x{7<<52:016x}
  and t2, t0, t1
  bnez t2, .Lerror
  bgez t0, .Lerror
  fence rw,rw
  li a0, 0
  ret
.globl hint_bundle_begin
hint_bundle_begin:
  fence rw,rw
  li t0, 0x{base|started:016x}
  csrw 0x5c0, t0
  csrr t0, 0x5c0
  bgez t0, .Lerror
  li t1, 0x{started:016x}
  and t2, t0, t1
  beqz t2, .Lerror
  li a0, 0
  ret
.globl hint_bundle_wait
hint_bundle_wait:
  li t3, {timeout}
.Lprofile_wait:
  csrr t0, 0x5c0
  li t1, 0x{((1<<61)|(1<<60)|(1<<59)|(1<<50)):016x}
  and t2, t0, t1
  beqz t2, .Lprofile_done
  addi t3, t3, -1
  bnez t3, .Lprofile_wait
  j .Ltimeout
.Lprofile_done:
  # Return raw P_Error/L_Error in a1. Caller distinguishes usable 001/010
  # from fatal 011/100 and checks final/capacity before offline construction.
  srli a1, t0, 52
  andi a1, a1, 0x7f
  li t1, 0x{7<<52:016x}
  and t2, t0, t1
  bnez t2, .Lerror
  srli t2, t0, 56
  andi t2, t2, 7
  li t1, 3
  bgeu t2, t1, .Lerror
  fence rw,rw
  li a0, 0
  ret
.globl hint_bundle_disable
hint_bundle_disable:
  # Legal while Bundle_Busy only if no outer Started and only Enabled clears.
  csrr t0, 0x5c0
  li t1, 0x{7<<59:016x}
  and t2, t0, t1
  bnez t2, .Lbusy
  li t0, 0x{base:016x}
  csrw 0x5c0, t0
  li t3, {timeout}
.Ldisable_wait:
  csrr t0, 0x5c0
  li t1, 0x{((1<<62)|(1<<50)):016x}
  and t2, t0, t1
  beqz t2, .Ldisabled
  addi t3, t3, -1
  bnez t3, .Ldisable_wait
  j .Ltimeout
.Ldisabled:
  fence rw,rw
  li a0, 0
  ret
.Ltimeout:
  li a0, -1
  ret
.Lerror:
  li a0, -2
  ret
.Lbusy:
  li a0, -3
  ret
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--container", type=lambda s: int(s, 0), required=True)
    p.add_argument("--trace", type=lambda s: int(s, 0), required=True)
    p.add_argument("--trace-mib", type=int, required=True)
    p.add_argument("--instructions-million", type=int, required=True)
    p.add_argument("--mode", choices=("discovery", "detail", "replay"), required=True)
    p.add_argument("--timeout", type=int, default=10000000)
    p.add_argument("--output", required=True)
    a = p.parse_args()
    Path(a.output).write_text(
        assembly(
            a.container, a.trace, a.trace_mib, a.instructions_million, a.mode, a.timeout
        )
    )


if __name__ == "__main__":
    main()
