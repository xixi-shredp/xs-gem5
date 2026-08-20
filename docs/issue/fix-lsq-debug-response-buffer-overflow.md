# cpu-o3: Bound the LSQ debug response dump to one word

branch: xs-dev
commit: 80035e4f5d6754f50aa9bf2ddffd216216bf997a


`LSQ::SingleDataRequest::recvTimingResp()` has this `debug::LSQ` path:

```cpp
char buffer[8];
std::memcpy(buffer, pkt->getPtr<char>(), pkt->getSize());
... *((uint64_t *)buffer)
```

`pkt->getSize()` may exceed eight bytes, so debug logging alone overflows the
stack buffer; the final cast also relies on potentially unaligned storage.

`pkt->getSize()` 由发起访存的 request 决定, 向量访存可能会超过 8B:
- `src/arch/riscv/isa/vector/base/vector_mem.temp.isa:224-229` 计算 `uint32_t mem_size` 并以它构造 byte-enable；在响应路径的 `:284-286`，代码 `memcpy(..., pkt->getSize())` 并断言 `mem_size == pkt->getSize()`
- 同一模板的 `:224-226` 注释还明确指出 whole register load/store 的 `mem_size = VLENB`
所以只要 vector-memory micro-op 的 `mem_size` 或 `VLENB` 大于 8（例如 `vl1re8.v` 等 whole-register load），这个 debug path 就会把超过 8B 的 response 拷入 `char buffer[8]`

## Code

Replace it with a zero-padded one-word sample:

```cpp
uint64_t first_word = 0;
const size_t copy_size = std::min<size_t>(pkt->getSize(), sizeof(first_word));
std::memcpy(&first_word, pkt->getPtr<uint8_t>(), copy_size);
```

Print `first_word` with `%#lx`. This preserves debug visibility while bounding
accesses and handling short packets safely.

My Solution:

- `src/cpu/o3/lsq.cc`, `LSQ::SingleDataRequest::recvTimingResp()`

My Solution (specific xs-dev location):

At `src/cpu/o3/lsq.cc:3304-3311`, replace all of the `if (debug::LSQ)` body:
lines 3305-3306 are the unsafe buffer and copy, and line 3310 dereferences it.
Use the `uint64_t first_word`/bounded-copy snippet in the proposal, then change
the format token on line 3308 from `data: %d` to `data: %#lx` and pass
`first_word`. Leave the response bookkeeping starting at line 3313 unchanged.

## Validation

Run an LSQ-debug workload with responses larger than eight bytes (preferably
under ASan). The reference patch full-build compiled on latest xs-dev.
