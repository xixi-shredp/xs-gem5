# 无限容量 Hint 实验工具

`--ideal-hint-ibp` 默认关闭；有限 v2 镜像和原 `util/run-h2p-hint.py` 的
239 次流程继续支持。新流程使用 `util/run-h2p-hint-ideal.py`，正式实验为
185 次运行、9250 个 1M 窗口。完整接口见同目录
`h2p_hint_ideal_contract.md`。

## 数据与算法

v3 candidates 包含新 off baseline 中全部 miss > 0 的 H2P PC，按 miss
降序、PC 升序排名；不限制 20 个候选。每片全部候选同时参加 32 个 mask
的重放，CAM 仲裁仍只查询块内最早的一个候选。

v3 metadata 使用 `storage: "unlimited"`、`hash_version: 2`；layout 仅包含
`conf_queue_depth`，finite_reference 固定为 CAM16、Context4096、ways2、Slot32。
Context 行严格为 valid/key/slot/conf 四个字段，不接受额外完整 PC 或上下文标签。
完整 28-bit key 的别名继续共享同一个 entry 和 confidence。有效项初始 conf 为 3；
无效项不能评分。Slot 保持偶地址、可由 target[47:1] 重建的规范地址并去重。
错误 baseline target 是观察值，可以是任意 uint64，包括非规范或奇地址，不能用
装载地址的规则拒绝原始 profile。

构表先按实际查询资格与 CAM 仲裁过滤，再按 key 合并。每 key 的 target 收益为
该 target 的观察次数减去该 key 的 baseline 正确次数；选择最大正收益，并列选择
最小 target 地址。保存全部正收益 key 和全部引用 target，不裁剪 set ways 或 Slot
数量。C++/Python 的 key 算法、输入 schema、预测时的 entry 标识及 confidence FIFO
规则相同。Python 无限局部历史使用共享不可变路径树，快照不复制所有 PC。

选择联合 mask 时使用各 mask 的实际 replay corrected − harmed；并列优先 context
种类少、mask 值小。全部正收益 PC 参与联合构表，无 CAM16 截断；无映射 PC 删除后
重新构表，直到 CAM 集合不再变化。记录每轮删除原因，不用单 PC 最佳收益之和替代
联合实际重放。

## 在 ICT186 执行

源码在本地修改，gem5 编译/仿真和大规模离线计算均在 ICT186 执行。先完成最终
`gem5-scons build/RISCV/gem5.opt -j25`，保存成功退出、ELF、执行权限及 SHA 的
build-audit.json；定向验证也必须绑定同一 binary/source SHA。

```sh
cd /home/xuejing/sim/workbench/xs-gem5-hint-ideal-heavy-v1
python3 util/run-h2p-hint-ideal.py \
  --gem5 build/RISCV/gem5.opt \
  --config configs/example/kmhv3.py \
  --workload-list /absolute/path/heavy.json \
  --workload-root /home/xuejing/sim/workloads/cpython \
  --source-manifest /absolute/path/source-manifest.json \
  --build-audit /absolute/path/build-audit.json \
  --validation-audit /absolute/path/validation-audit.json \
  --old-root /home/xuejing/sim/workbench/cpython/build/h2p-hint-heavy-v1 \
  --output-root /home/xuejing/sim/workbench/cpython/build/h2p-hint-ideal-heavy-v1 \
  --disable-aslr --jobs 25
```

`--library-path` 可以复用已确认的私有运行库目录。需要检查计划时加 `--dry-run`；
它只读取身份和清单，不启动仿真、不创建输出。五片与 restore 后 100M warm-up +
50M measure + 1M interval 固定，关闭 difftest，DDR4_2400_8x8；zstd 不加 raw-cpt。
`xy-server:` workload-root 必须通过显式本机 workload-root 映射。

阶段顺序：5 baseline → 5 原有限联合 replay 并通过回归门槛 → 5 固定原 PC/mask
无限 replay 与 5 全候选 collect → 160 固定 mask 无限 replay → 5 无限联合 replay。
固定选择容量对照使用原最终 CAM，而非原 top20；原 collect 的缓存、gzip、独立
profile 审计和最终有限镜像均核验 SHA，并通过原 complete-audit 建立身份链。

`--stage baseline|finite|fixed|collect|masks|joint|audit` 支持逐阶段推进；已有实验
需显式 `--resume`，且身份必须完全一致。已有失败/运行中/未完成目录会被拒绝，
没有自动覆盖或择优重跑。进程锁防止同一输出根重复启动。仿真、构表、参考模型和
最终离线审计共享至多 25 个工作槽；池内一个 worker 同时只做一项计算工作。

## 单文件工具与测试

```sh
PYTHONPATH=util python3 -m h2p_hint candidates h2p-indirect.txt candidates.json --ideal-hint-ibp
PYTHONPATH=util python3 -m h2p_hint cache hint-profile.jsonl.gz profile-cache
PYTHONPATH=util python3 -m h2p_hint build profile-cache candidates.json image-dir --mask 14 --ideal-hint-ibp
PYTHONPATH=util python3 -m h2p_hint reference profile-cache image-dir/metadata.txt reference.json --finish-cycle 123456
```

小型 Python 逻辑测试（不调用 gtest 或仿真）：

```sh
python3 tests/gem5/h2p_hint/tools_test.py
python3 tests/gem5/h2p_hint/ideal_tools_test.py
```

生产 C++ 定向测试使用最终 gem5.opt。新的 Python 测试覆盖超过 20 个 PC、5001 个
同原 set 的 key/target、真实跨 PC 别名、所有 32 mask 的有限/无限等价结果、路径
共享/回滚、缺失查询、原始 uint64 target、schema/CLI、占用与损坏证据的拒绝。

## 独立审计与报告

```sh
python3 util/audit-h2p-hint-ideal.py \
  --output-root /home/xuejing/sim/workbench/cpython/build/h2p-hint-ideal-heavy-v1 --jobs 25
```

审计重新读取实际 outputs；`status.json` 的 pass 不是充分证据。检查185次正常退出、
restore/W100M/M50M、9250窗口、排序及每 target/PC 守恒；检查每PC Hint final_misses
与 H2P 一致、每次 committed tag hit 恰好入队或丢弃、队列深度和期末 pending。
conf0 的预测时历史/hash/key/entry/slot、写回单端口顺序、实际 CPU 时钟及 measure
边界独立核对。三表平均/峰值/常驻及有限容量倍数同时核对 stats.txt 与 JSON、镜像。

全部无限镜像重新从 profile 独立计数并再现正收益 target 映射；重新核对全部 baseline
候选、160组mask、实际最佳 mask 的并列规则及联合剪枝固定点。源码 manifest 逐文件
校验内容/执行权限/链接，工具、binary、checkpoint、旧输入与输出保留 SHA。
新 off/collect 及新旧有限 replay 比较所有非宿主/非Hint数值统计和全部50窗口；
新旧 collect 的共同 PC 比较逐条提交与查询上下文/时间/target，不只比较直方图。
发现差异时保留失败证据并停止后续阶段。

report.md 和 summary.json 区分 off→有限、有限→固定选择无限、固定无限→重新搜索
无限的实际 miss/IPC，表格报告三表平均/峰值条目及相对16/4096/32倍数。
`best-masks.json` 为逐PC选择和联合实测，`sweep.csv` 包含实际和参考模型净纠错，
`window-trends.csv` 包含四组1M变化。各镜像目录保留 metadata.txt、builder.json、
reference.json、hard-pred.txt；运行目录保留 conf0.txt 与所有窗口。

冻结表不动态增删，非空 measure 的占用平均和峰值等于常驻有效量；conf=0 仍占空间，
collect 不参与硬件常驻容量比较。结论仅称固定历史、hash、端口、时序及构表规则下
的容量放开收益，属于同片离线特化。参考模型保持 collect 的上下文/周期，不预测
Hint 改变后的错误路径。推荐只在真实 joint miss 优于 off 时启用；所有退化实验照实
保留，IPC 单独报告。
