# 私有 reserved-NC GCPT overlay

`bundle_checkpoint_overlay.py` 默认只读 dry-run；`--execute` 才创建新的 `.zstd` 和 `.manifest.json`。不运行NEMU、不写CSR、不向预测器/总线注入完成，不修改旧checkpoint或原CPython/source。所有流处理仅ps1/ps2已知主机；工具在ps1别名的真实hostname也已登记。

```bash
python3 xs-gem5/util/h2p_hint/bundle_checkpoint_overlay.py \
  --source /private/new-nc-cpts/scimark_fft_test/51/memory.zstd \
  --checkpoint-run /private/new-nc-cpts/scimark_fft_test/run.json \
  --reservation-proof /private/nc-images/build-result.json \
  --boot-proof /private/nc-images/boot/result.json \
  --image-manifest /private/nc-images/scimark_fft_test/image-build-manifest.json \
  --restorer /private/prelude/gcpt.bin \
  --restorer-manifest /private/prelude/restorer-manifest.json \
  --container /private/bundle/container.bin --mode replay \
  --output /private/new-overlay/fft51-replay.zstd
```

仅后续获准实际overlay才追加`--execute`。不存在输出及sidecar才运行；发布用独占临时目录和不覆盖hardlink，源路径/输出路径不可相同，失败不发布部分压缩文件。执行前至少20GiB磁盘余量，zstd使用`-T1 -3`，1MiB chunk处理；不分配整份8GiB guest数组。本工具不是checkpoint制作NEMU，20GiB磁盘检查不等于NEMU的20GiB/worker内存预算。

输入来源必须是**新reserved镜像制作的checkpoint**：

- `bundle_image`的build-result要求exit_code0、`proof.dtb_reserved_memory_proof=true`，reserved `[0x278000000,+0x08000000)`，Trace `[0x278200000,+0x04000000)`；绑定image_manifest SHA、published_image SHA，现场校验镜像文件hash。
- 同一镜像boot/result要求exit_code0、`guest_reserved_memory_nomap_proof=true`和`guest_validation.nemu_good_trap=true`。
- 固定点generator的run.json必须pass，plan的image/image_manifest SHA与上述一致，checkpoint_files中必须包含source的实际压缩SHA。
- reservation不是PMA证据；PMA NC仍由模拟器/RTL配置另验。本工具不会接受旧正式checkpoint仅因“尾部看起来空闲”而写入它。

restorer-manifest的通用最小接口：

```json
{"schema":"bundle-restorer/v1","raw":{"sha256":"64位raw文件SHA256"}}
```

其余构建命令、ELF/hash/源码/符号字段可由prelude builder附加并原样保留。raw offset4长度必须132..0xECDB0、4B对齐且不超过raw文件；只读取/覆盖声明长度，**不覆盖完整raw文件**（raw可能附带GCPT payload）。校验首指令JAL及范围内唯一连续31条固定编码GPR load+mret；不验证prelude的CSR操作或profiling方法，不添加隐式phase gate。将来的restorer builder必须生成真实代码/manifest；此最小schema不是已完成prelude编译的声明。

固定布局已获授权：container0x278000000、长度0x108000，Trace0x278200000、64MiB全零。选择余量中的0x27C200000作为4KiB control/sentinel，紧接Trace尾端且仍在128MiB reserved内；开头为`H2P-NC-CONTROL-v1\0`，其余显式零。所有段不重叠，低prefix不触及`[0xECDB0,0x100000)`保存区；mode仅核对容器magic H2PBND01和kind，完整表语义仍需原构表工具/硬件校验。

zstd source流逐块读取，源长度不得超过逻辑DRAM8GiB。将短resident dump显式补零到最高写段，保留原有更长尾部；不得越过0x280000000物理上限。逐块比较保存区原/新字节并记录相等SHA，以及boot flag0xbeef、saved PC和U/S mode。保留全部原保存字段，包括CSR/GPR/FPR等，不改变PC/mode。sidecar记录完整input hash、段地址/长度/hash、保存区证明及输出raw/压缩hash；五份输入JSON原字节以base64保留，不重新序列化充当原文件。流处理后再次校验输入hash，防止处理期间漂移。

ps1有限验证报告：`results/model/smoke-test/bundle-20261103/environment-migration/checkpoint-overlay-validation.json`。合成用例覆盖大raw附带payload但仅写声明prefix、唯一尾段、短尾补零、保存字段相等、边界/元数据重叠拒绝、坏header、zstd roundtrip和未保留来源拒绝。真实旧fft51仅只读前1MiB头部、保存区相等，特意停止解压；它不是全流CRC验证，也不构成reserved授权。实际新NC checkpoint不存在时不伪造该来源，真实完整dry-run/新overlay/恢复可执行性仍待后续。

限制：未验证实际新prelude执行、真实NC boot/恢复、独立NEMU CSR控制器、profiling预算方法或尾部DMA排空；本工具仅准备真实初始物理内存。generic GCPT loader将此overlay载入后，所有容器LOAD和Trace写仍必须通过真实DMA请求/响应。本轮没有生成十片overlay或运行真实workload。
