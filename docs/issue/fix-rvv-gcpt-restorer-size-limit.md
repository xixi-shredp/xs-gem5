# configs: Prevent RVV GCPT restorer truncation

branch: xs-dev
commit: 80035e4f5d6754f50aa9bf2ddffd216216bf997a


`config_xiangshan_inputs()` currently has:

```python
elif args.restore_rvv_cpt:
    sys.gcpt_restorer_size_limit = 0x1000
```

The affected RVV restorer is 1,048,600 bytes. The 4 KiB limit partially loads
it, leaves vector state wrong, and causes a later difftest divergence (observed
at `vl1re8.v`).

## Code

Replace only this RVV branch with:

```python
elif args.restore_rvv_cpt:
    print("Simulating single core with RVV, demanding GCPT restorer size of 2M.")
    sys.gcpt_restorer_size_limit = 2**21
```

The 2 MiB bound covers the current ~1 MiB binary while preserving the guard;
do not reuse the RVH `0x1000` value.

My Solution:

- `configs/common/xiangshan.py`, `config_xiangshan_inputs()`

My Solution (specific xs-dev location):

In `configs/common/xiangshan.py:374-376`, replace the RVV-only message and
`sys.gcpt_restorer_size_limit = 0x1000` with the 2 MiB message and
`sys.gcpt_restorer_size_limit = 2**21`. Do not alter the surrounding
multi-context branch at lines 371-373, the RVH branch at 377-379, or the
non-RVV fallback at 380-382.

## Validation

With `--restore-rvv-cpt --enable-riscv-vector`, the log must show the complete
restorer size and no partial-load warning. The representative checkpoint passed
the former divergence, and the later 78-slice difftest run had no failures.
