# H2P Hint：有限硬件模型与第二轮 profiling

本实现把同一 checkpoint 的 50M measure 数据用于离线构表，再使用冻结表重放。
结果属于**同片离线特化收益**，不能据此宣称跨输入泛化能力。第一轮最终 target miss
仅用于选择 top20 PC；本轮 off、collect、replay 必须使用同一已编译完成的二进制。
正确 BTB fallback 不计 H2P miss，return 和 squash 指令不参与提交统计。

## 模型合同

默认硬件为 CAM16、2048 sets × 2 ways Context SRAM、32 个共享 target，以及深度 8
的 confidence FIFO。CAM 每项包含 valid、PC[47:1]、5-bit mask，共 53 bit；Context
每项包含 valid、17-bit tag、5-bit slot、2-bit confidence，共 25 bit，合计 12.5 KiB。
Slot Table 存 32 个 target[47:1]。所有表稀疏装载，未装载项无效。

每周期至多启动一个预测块查询。S1 在块范围内选择最早的 CAM PC，S2 查两路 context，
S3（已有预测向量的 index 2）读取 target。只有原 BTB 在该级已识别该 PC，且该 PC
也是原最终预测选中的非 return 间接分支，Hint 才能接管。较早 taken、PC 不符、return
等取消接管。多个候选共用一次查询机会。命中且查询时 conf > 0 才覆盖原 target；
conf = 0 时保留影子预测供提交评分。原 BTB/ITTAGE 预测结果另存用于净纠错统计。

模型沿用现有 BPU 查询快照和分级可见性抽象，显式保留查询机会、override bubbles、
confidence 写口吞吐、队列满丢弃及下一周期可见性。它不验证 SRAM 电路时序。仅支持
单核单线程，拒绝需要第二预测查询通路的 PairTAGE / idealTwoTaken 配置。

## 历史和 mask

mask bit 0～4 依次为 GHR、GITHR、LITHR、Call、Return，mask 0 仅使用 PC。

| 分量 | 精确定义 |
|---|---|
| GHR | 复用原 BPU 的 970-bit 条件分支方向历史，最新位最低 |
| GITHR | 最近 64 个 taken 非 return 间接 target，每个取 target[2:1]，共 128 bit |
| LITHR | 每候选 PC 最近 4 个自身 taken 非 return 间接 target，共 8 bit |
| Call | 最近 64 个 taken call 的 PC[47:1]，最新 PC 的年龄为 0 |
| Return | 最近 64 个 taken return 的 PC[47:1]，最新 PC 的年龄为 0 |

indirect call 同时更新 GITHR 和 Call。兼具 call/return 属性的指令更新 Call、Return，
但不写 GITHR。collect 最多维护 20 份 LITHR，正式默认 CAM 最多维护 16 份；只按
自身 PC 更新，因此 collect 额外候选不会改变其他 PC 的 LITHR。

统一在预测块入口、该块历史更新之前采样。新增历史必须随 control、memory、trap
squash 恢复；提交不能使用当前历史补造查询。stats reset 不清历史。

`fold28` 按 28-bit 分块 XOR。PC 取 PC[47:1] 再 fold28。GHR/GITHR/LITHR 直接折叠；
Call/Return 对每个 PC 的折叠值按年龄循环左移后 XOR，严格淘汰第 65 个事件。
五个分量分别循环左移 1～5 bit，按 mask 选择后与 PC fold XOR。默认低 11 bit 为
set、高 17 bit 为 tag。同一个 28-bit key 的不同 PC/context 必须共享物理表项，
不能用软件隐藏 tag 消除真实 alias。

## Confidence 时序和统计

有效项初始 conf=3。仅在 measure 中，对已提交、查询合格且 tag 命中的间接非 return
分支评分，包括 conf=0 的影子查询。评分使用保存的 target 和物理表项身份：正确 +1、
错误 −1，实际写回时饱和至 0～3。不能在提交时重新 hash 当前历史或重新选择 target。

更新按提交顺序入 FIFO，最早提交后下一周期写回，每周期至多一项。队列满丢弃新更新，
不反压提交。无待更新旁路，同周期先写回、后查询。CPU 没有前端请求时也按相同逻辑
周期推进；跨空闲周期补写的循环至多处理 FIFO 深度项，不引入隐含多写口。
measure 结束不延长仿真排空队列。额外 stats reset 保留 confidence 和 pending 更新，
以 carried_in 记录继承项，满足 `carried_in + enqueued = applied + pending`。

每次实际 1→0 写一行 `conf0.txt`，包含 PC、mask、查询上下文、hash、set/way、保存的
target、实际 target、提交与逻辑写回时间。影子正确使 0→1；再次归零会再次记录。

`hint-stats.json` 使用 `format=h2p-hint-stats, version=2`，包括：

- `global`：全部已提交非 return 间接分支，记录 executions、baseline_misses、
  final_misses、corrected、harmed、eligible、tag_hits、conf_enabled、overrides。
- `pcs`：至少包含候选文件与 CAM 的 PC，每 PC 使用上述同名计数。
- `queue`：enqueued、applied、dropped、pending、carried_in、zero_transitions、
  recoveries、max_occupancy，以及占用采样计数。
- `query`：查询、CAM 争用、tag/conf 覆盖、override 和各类取消原因。

必须满足 `baseline_misses - final_misses = corrected - harmed`，全局 final_misses
必须等于 `h2p-indirect.txt` 的 miss 总和。各 PC 执行次数是分析错误率的分母，target
miss 分布仍然不能代替完整 target 执行频率。off 不输出 Hint 文件。

## Python 配置和文件接口

`configs/common/H2PHint.py` 接入 XiangShan 配置：

```text
--h2p-hint-mode=off|collect|replay
--h2p-hint-candidates=<candidate.json>
--h2p-hint-metadata=<metadata.txt>
--h2p-hint-cam-entries=16
--h2p-hint-context-entries=4096
--h2p-hint-ways=2
--h2p-hint-slot-entries=32
--h2p-hint-conf-queue-depth=8
```

默认 off。collect 要求候选文件；replay 要求 metadata，候选文件可扩大报告集合。
off 不接受候选或 metadata 文件。对应 SimObject 参数为 hintMode、hintCandidates、
hintMetadata、hintCamEntries、hintContextEntries、hintWays、hintSlotEntries、
hintConfQueueDepth。fixed mask 属于离线 builder/runner 的 `--mask` 和镜像来源字段，
不允许 replay 启动时静默改写已构建镜像的 mask。

候选 JSON：`format=h2p-hint-candidates, version=2, pcs=["0x..."], source={...}`。
保留第一轮 miss 降序、PC 升序排名，最多 20 个。CAM 查询使用地址顺序，不使用排名。

`metadata.txt` 是版本化 JSON：

```json
{
  "format": "h2p-hint", "version": 2, "hash_version": 2, "ghr_bits": 970,
  "mask_order": ["GHR", "GITHR", "LITHR", "Call", "Return"],
  "history": {"githr_targets": 64, "lithr_targets": 4,
              "call_pcs": 64, "return_pcs": 64, "target_shift": 1},
  "layout": {"cam_entries": 16, "context_entries": 4096,
             "ways": 2, "slot_entries": 32, "conf_queue_depth": 8},
  "cam": [{"pc": "0x20", "mask": 0, "valid": true}],
  "slots": ["0x80"],
  "entries": [{"set": 16, "way": 0, "tag": 0,
               "slot": 0, "conf": 3, "valid": true}],
  "source": {}
}
```

地址必须为偶数且可由 48-bit 符号扩展重建；拒绝重复 PC、重复 set/tag、重复物理位置、
越界 pointer、几何不符、history/hash/version 不符和错误 JSON 标量类型。
明确装载的 invalid Context 项允许 conf=0～3、slot 指向未装载但未超硬件容量的位置，
相同 tag 的 invalid 项可以共存；它们不能查询或评分。valid 项必须 conf=3，且引用已
装载 target，同 set 的 valid tag 必须唯一。物理 set/way 始终不能重复。

collect 写 gzip JSONL `hint-profile.jsonl.gz`。header 包含上述合同、候选列表 pcs 和
CPU `clock_period`（ticks）；每个候选的每次已提交非 return 间接实例输出一条 branch：
seq、pc、actual、baseline_valid、baseline_pc、baseline_target、query_cycle、commit_cycle、
block_start、block_end、eligible、contexts。contexts 为未做分量旋转的五个 28-bit fold；
缺失查询必须 eligible=false，缺失快照必须 contexts=null。正确预测同样采集。
collect eligibility 表示原 BPU 查询资格；每组的 CAM 最早候选仲裁在离线模型中额外应用。

`baseline_target` 保存原 BPU 的完整 unsigned 64-bit 预测值。错误预测可能是非 canonical
地址，也可能是奇数；采集解析和 cache 必须原样保留，并直接与 actual 比较计 miss，
不能截断、符号扩展或清除低位来“修正”预测。负数、超过 64 bit 的值及非整数仍拒绝。
例如本轮 scimark_fft 的实际 target 为 `0x64a8a`，原预测为 `0x198f9a00198b78`，
这是保留在 profile 中的错误预测，不是可装载的 Hint target。metadata 的 CAM PC 和
Slot target 仍须满足偶数、canonical、可由 [47:1] 重建的原合同；仅 baseline_target
采用原始预测值口径，提交 PC/actual 等地址字段不放宽。

## 构表和参考模型

`util/h2p_hint` 只依赖 Python 标准库。gzip trace 转换一次为 96-byte 固定记录的磁盘
cache，另建按 query_cycle/seq 排序的 offset 索引。罕见的 query 时间乱序通过 SQLite
磁盘排序解决，不把整段 trace 的 Python 对象常驻内存。之后每 mask 在 cache 上构造
histogram，查询/提交双流参考模型也读取 cache，不反复解压原文件。

对所有硬件可查询样本按 28-bit key 合并。每个 key 对 target 的净收益为
`该 target 的实际观察次数 - baseline 正确次数`，等价于纠错减错误覆盖。
逐步选择最多 32 个共享 target，每一步精确计算加入该 target 后、各 set 仅保留最好
两项的边际收益。并列取 target 地址小者。每 key 选收益最高的已选 target，收益并列
取 target 小者；每 set 按收益降序、key/target 升序保留两个正收益项。

实现用 target→set→正收益 key 的稀疏索引；单次贪心迭代只访问这些边与当前各 set
前两项，避免按候选 target 反复扫描所有 context。复杂度主要为至多 32 次稀疏边遍历，
内存随离线 distinct key/target 关系增长，硬件查询仍只有参数化 CAM 和两路 SRAM。

reference 保存查询时 confidence/target，使用实际 collect 的预测与提交周期驱动
单写口 FIFO，覆盖 CAM 仲裁、conf=0 影子恢复、同周期查询/提交、丢弃与尾部 pending。
reference 不会重建 Hint 改变后的错误路径或周期；它提供固定采样轨迹下的有限表估计。
正式收益以实际 gem5 replay 为准，并与 reference 的偏差同时记录。

每片全部 32 mask：pickle/deltablue 的 top20 分 16+4 两组，其余三片各一组，总计
224 次重放。按实际每 PC corrected−harmed 选 mask，并列按 popcount、mask 数值。
正收益 PC 按收益降序、PC 升序取至多 16 个，联合重新构表后再跑五片；不能相加分组
最佳收益。联合删除无映射 CAM PC 后重新构表，直到 CAM 集合稳定，避免删除候选改变
仲裁却仍沿用过期的构表资格。

`hard-pred.txt` 为 JSONL，区分 no_positive_gain、cam_capacity、shared_target_capacity、
set_conflict、query_coverage，不把容量或覆盖不足统称为天然不可预测。若联合最终 miss
不优于 baseline，保留实验并生成全禁用 recommended-metadata.txt；IPC 单独报告。

## 运行与审计

所有 gem5 编译/仿真在 ICT186，本地仅编辑与 Python 测试。正式运行前，最终
`gem5-scons -j25` 必须结束成功，并由 build-audit 证明 ELF、执行权限、二进制 SHA 和
source-manifest SHA。`.zstd` 不能加 raw-cpt。每片在 restore 完成后开始 100M warm-up，
reset 后只测量 50M，H2P interval 为 1M；关闭 difftest，DDR4_2400_8x8。

远端源码目录下的执行入口：

```bash
python3 util/run-h2p-hint.py \
  --gem5 build/RISCV/gem5.opt --config configs/example/kmhv3.py \
  --workload-list results/h2p-hint-heavy-v1/inputs/heavy.json \
  --inputs-root results/h2p-hint-heavy-v1/inputs \
  --workload-root /home/xuejing/sim/workloads/cpython \
  --source-manifest results/h2p-hint-heavy-v1/source-manifest.json \
  --build-audit results/h2p-hint-heavy-v1/build-audit.json \
  --validation-audit results/h2p-hint-heavy-v1/validation-audit.json \
  --output-root /home/xuejing/sim/workbench/cpython/build/h2p-hint-heavy-v1 \
  --library-path /home/xuejing/sim/workbench/xs-gem5-hint-heavy-v1/.build-deps/root/usr/lib/x86_64-linux-gnu \
  --jobs 25 --disable-aslr
```

`--validation-audit` 必须包含 result=pass、精确 gem5/source-manifest SHA 和非空 checks，
证明最终二进制的 production raw pipeline 等定向 gate 已完成；runner 不替代这些 gate。

`--stage all` 默认先并行 5 off + 5 collect，再执行 off/collect 非宿主数值统计与 H2P
输出一致性检查，通过后放行 224 mask，最后 5 joint，共 239 次完整运行。新 Hint
自身 `.h2pHint.` 计数器不参与 observer 一致性比较。空表 replay 与 baseline 的一致性
由最终二进制定向验证另外执行。`--stage baseline|collect|masks|joint` 可分阶段调度。

一个 ProcessPool 统一限制 Python 构表、reference 和 gem5 的计算槽数为 25；每槽
在离线计算与仿真之间串行切换。输出根目录独占且有 runner.lock，不能重复启动。
`--resume` 只复用身份相同且重新审计通过的 pass；失败、运行中或不完整目录明确拒绝，
不会覆盖、归档或自动重跑。失败停止提交新作业，已在运行的作业保留日志并正常收尾。

runner 记录二进制、配置、源码 manifest、工具、checkpoint（SHA/size/mtime）、候选、
profile、metadata 身份；每次检查退出原因、restore/W/M 边界、单次 50M stats dump、
50 个窗口、逐 PC/target 守恒、Hint 净纠错和 FIFO 守恒、conf0 实际行数。
生成 experiment.json、progress.json、各阶段审计、每 run 的 status.json、summary.json
及推荐镜像。跨阶段差异和失败保留原始证据，不挑选相等的运行替代失败实验。

单项工具：

```bash
PYTHONPATH=util python3 -m h2p_hint candidates h2p-indirect.txt candidates.json
PYTHONPATH=util python3 -m h2p_hint cache hint-profile.jsonl.gz profile-cache
PYTHONPATH=util python3 -m h2p_hint build profile-cache candidates.json mask-03 --mask 3 --group 0
PYTHONPATH=util python3 -m h2p_hint reference profile-cache mask-03/metadata.txt reference.json --finish-cycle 12345
PYTHONPATH=util python3 -m h2p_hint check-vectors hint-hash-vectors.json
python3 tests/gem5/h2p_hint/tools_test.py
```

最后一项为 host-only contract tests，不编译或运行 gem5；C++ loader、历史/squash、S3
可见性和 confidence 端口行为仍须使用 ICT186 的最终 gem5.opt 定向验证，不使用 gtest。
