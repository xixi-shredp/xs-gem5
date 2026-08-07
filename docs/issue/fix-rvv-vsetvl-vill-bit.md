# arch-riscv: Treat VTYPE[XLEN-1] as illegal in vsetvl

branch: xs-dev
commit: 80035e4f5d6754f50aa9bf2ddffd216216bf997a

Both vector configuration templates calculate `new_vill` from LMUL, SEW, and reserved bits, but neither tests RV64 VTYPE bit 63 (`vill`). A `vsetvl` request with that bit set can be accepted as legal.

## Code

`src/arch/riscv/isa/vector/base/vector_conf.isa:110-125`:
```cpp
        VTYPE new_vtype = requested_vtype;
        {
            vlmax = getVlmax(new_vtype, vlen);
            float vflmul = getVflmul(new_vtype.vlmul);
            uint32_t sew = getSew(new_vtype.vsew);
            uint32_t new_vill =
                !(vflmul >= 0.125 && vflmul <= 8) ||
                sew > std::min(vflmul, 1.0f) * ELEN ||
                bits(requested_vtype, 30, 8) != 0;

            if (new_vill) {
                vlmax = 0;
                new_vtype = 0;
                new_vtype.vill = 1;
            }
        }
```

My Solution: add `bits(requested_vtype, 63, 63) ||` after `uint32_t new_vill =`

- `src/arch/riscv/isa/vector/simple/vector_conf.isa:110-125`: make the same insertion at the corresponding `new_vill` expression.

## Validation

I encountered a "difftest failed" error when testing with custom RVV checkpoints, and the issue was resolved after the fix. However, I haven't tested it against a wider range of checkpoints yet, so I am unsure if this is the correct approach to the fix. Please let me know if there are any errors with my understanding of the code or the changes I made.
