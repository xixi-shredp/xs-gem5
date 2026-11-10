# 固定旧 SimPoint 子集 checkpoint generator

`bundle_checkpoint.py` 是独立入口，不调用/修改旧 generator，不 profiling、不校准 loops、不重聚类、不发布/替换正式 checkpoint。默认只读 dry-run，输出 JSON 命令计划。仅在用户 NC 布局确定、迁移验收和预制镜像就绪后才可 `--execute`；本次实现没有启动重制。

每个授权 bench 从旧正式 `manifest.json` 读取 accepted_loops、runtime hash、100M/200M/no-RVV/zstd 契约，从 `manifest/smoke-test.json` 选择点。`simpoints0`/`weights0` 严格检查重复点/cluster、有限非负权重、原完整权重和约1、cluster集合及manifest计数；原文件、labels和原manifest按原字节复制到每bench私有 `original/`，记录SHA256。子集保留原行次序、cluster ID、weight原文本，**不归一化**。一个bench只启动一次NEMU以捕获它的全部授权点。

输入 JSON 使用绝对路径；预制镜像及manifest必须已在私有workflow完成：

```json
{
  "schema": "bundle-checkpoint-inputs/v1",
  "benches": {
    "scimark_fft": {
      "image": "/absolute/private/images/scimark_fft_test.bin",
      "image_manifest": "/absolute/private/image-build-manifest.json"
    }
  }
}
```

七bench全部运行时列出全部七项。镜像manifest沿用原builder schema：检查 `loops` 等于旧accepted_loops、no-RVV、runtime hash相同、published_image路径basename与workload及image SHA相符、UART为ns16550a/10MHz/shift2/width4。新DT/firmware/GCPT不要求与旧镜像hash相同。`--restorer` 必须是**新完整镜像内嵌的对应raw restorer**；核验双方前4096B及manifest的prefix hash。这只核验既有manifest规定的prefix范围，不声称检查整个prelude或其控制流。

CLI（在ps1工作区，先仅dry-run；`--bench`可重复，省略即全部授权bench）：

```bash
python3 xs-gem5/util/h2p_hint/bundle_checkpoint.py \
  --source-root /home/xuejing/workloads/cpython/cpts/no-rvv-uart16550-100m200m \
  --smoke /home/xuejing/workbench/kmhv3-hint-norvv-nol3/manifest/smoke-test.json \
  --inputs /absolute/private/nc-images.json \
  --nemu /absolute/checkpoint-making/NEMU/build/riscv64-nemu-interpreter \
  --restorer /absolute/private/embedded-restorer.bin \
  --output /absolute/private/new-fixed-smoke-run \
  --bench scimark_fft --workers 1 --timeout 3600
```

仅后续获准制作时追加 `--execute`。输出目录必须不存在，无resume/force/覆盖逻辑；不能位于旧正式source内。执行只允许ps1/ps2已知主机名（当前ps1真实名`iv-yexl3v2u4gwh2ysmqmn1`；更换服务器需重新核实）。每bench新建 `cluster/<workload>/{simpoints0,weights0}`，NEMU命令严格为：

```text
NEMU IMAGE -D NEW_BENCH_OUTPUT -w BENCH_test -b -C checkpoint
  -S NEW_BENCH_OUTPUT/cluster --cpt-interval 200000000
  --checkpoint-format zstd --warmup-interval 100000000
```

**不传 `-r`**：checkpoint制作从完整GCPT image启动；`-r`会覆盖前缀，可能破坏未来新NC prelude。`--restorer`只作hash/provenance检查。checkpoint-making NEMU与gem5 difftest `.so`不是同一工具。

读取依据：原制作NEMU commit `7c8b842b101c0932eb769f5aa2721770b8690638`，`serializer.cpp:549–565`按两文件逐行读取location/id、weight/id，只要求配对id相等并以location存map；`path_manager.cpp:60–61,105–109`在-S根后追加workload；`image_loader.c:276–315`仅在cpt_image非空时以restorer覆盖prefix。该源事实由experience_review在ICT原树只读核实，运行工具的实际binary hash另外记录，不把commit字符串当新二进制来源认证。

执行预算：`--workers`限制bench并行，所有NEMU/ZSTD/BLAS线程环境设为1，逐benchwall timeout默认3600秒。启动时核对affinity减1分钟load的CPU余量、至少32GiB+20GiB/worker的MemAvailable及192GiB空闲磁盘；记录快照、完整argv、环境、源/镜像/工具hash、输出hash及耗时。此为pilot前保守预算：制作NEMU `serializer.cpp:286–330` 一次性 `malloc(ZSTD_compressBound(PMEM_SIZE))`，8GiB guest之外还需约8GiB压缩缓冲及额外开销（experience_review只读源码证据）。pilot必须实测峰值RSS后再决定并行度，不能按旧12GiB/worker估计。不是持续资源预留或RSS硬限额；其他作业变化时owner仍需调低workers。超时/错误保留私有产物供审计，不自动删旧目录。

`run.json`的pass仅表示NEMU exit0、GOOD TRAP/CPython result/版本、无panic、选点目录集合和每点一个非空zstd文件这些基础门通过。`summary.json`不是正式发布验收。完整stdout/stderr及capture相关原始行保留，记录请求ROI阈值 `max(0, point×200M−100M)`；基本块边界使实际捕获计数可能不同，精确count/PC仍需独立核验。

尚未验证：真实新NC镜像启动、DT/PMA/PMP/NC区初始化、迁移闭包、checkpoint restore与寄存器/no-RVV/BBV语义接近度、zstd解压/GCPT头、资源峰值、超时后的工具子进程行为。reserved no-map可能改变resident最高页，NEMU采用有效resident长度dump，故**不将解压长度恰好8GiB写死为门槛**；逻辑8GiB和短尾zero-fill需后续loader证明。新的Off/v10/Bundle必须使用同一个新checkpoint重新profiling→构表→测量，不能复用旧IPC作新A/B。
