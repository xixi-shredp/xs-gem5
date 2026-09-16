# Hint finite replay 独立 timing 审查

本文件由独立 timing reviewer 编写。审查使用同目录 `source-manifest.json`、`evidence/config/pickle_pure_python_test/18/config.ini` 和 `evidence/rtl/` 原始文件快照。下文保留源码与目标接口预审，并在末节对主审冻结的 Candidate A 给出最终独立 gate。候选调度讨论不构成已验证 capability。

范围是 finite replay 的查询、S3 覆盖、训练、历史保存与恢复。没有构建、仿真、STA 或修改生产代码。`frequency_closure` 为 `UNPROVEN`，`area_closure` 为 `UNPROVEN`，`signoff_status` 为 `NOT_PERFORMED`。

## 已确认的周期观察点

| 项目 | 现场源码与配置证据 | 对映射的约束 |
|---|---|---|
| 配置 | `evidence/config/pickle_pure_python_test/18/config.ini:279-310` | FTQ 64，CAM 16，Context 4096 总项、2 ways，Slot 32，更新 FIFO 8，GHR 970；**numStages 为 4**，Hint 启用 replay、ideal 关闭。 |
| Hint 生效 stage | `src/cpu/pred/btb/hint_types.hh:16-24`；`decoupled_bpred.cc:580-607` | Hint 从数组 index 2 起修改匹配的间接跳转 target，也修改 index 3；不能把 source 配置称为字面的 3-stage pipeline。 |
| 请求上限 | `hint_indirect.cc:714-743`；`decoupled_bpred.cc:348-356,409-452` | 每 CPU cycle 至多一次有序请求，空／回绕范围也消费机会。实际启动还受 active、squash、redirect、已有 prediction 和 FTQ 满状态限制。 |
| 查询观察版本 | `hint_indirect.cc:725,796-826` | 调用 tick 后依次读取冻结 CAM／Context／Slot，并把 entry identity、confidence、target 保存到 query；后续写不能改变已经保存的查询结果。 |
| 完成选择 | `hint_indirect.cc:830-884`；`decoupled_bpred.cc:568-607` | 一次完成；必须保存与 baseline 相同 PC 的 S3 间接非 return 身份；取消条件和 saved confidence 决定是否覆盖。 |
| FTQ 可见延迟 | `decoupled_bpred.cc:695-708,737-749,754-813,488-493` | first matching stage 决定 override bubbles。没有 bubbles 时可在发起的同一次 BPU tick 入队；有 bubbles 则逐 cycle 推迟。不能把一个 helper 回调算成一个 RTL cycle，也不能把所有查询 latency 写死为 2。 |
| 同 cycle 顺序 | `src/cpu/o3/cpu.cc:581-590`；`hint_indirect.cc:997-1005` | 已就绪 confidence writeback 先于 commit，再早于 fetch/query。重复 tick 同 cycle 无动作。 |
| 更新排队 | `hint_indirect.cc:974-994`；配置 `config.ini:132` | commit width 8 可产生有序突发；FIFO 深度 8，满时丢新，不反压 commit，最早写周期为提交后的下一 CPU cycle。 |
| 更新完成率 | `hint_indirect.cc:1016-1061` | 单 CPU cycle 最多一次 confidence 写；在实际写时读取当前值再饱和加减，FIFO 顺序，不合并同 entry 更新。 |
| 预测历史 | `hint_history.cc:155-183,230-243`；`decoupled_bpred.cc:554-562,1593-1611` | 查询使用本 block speculative update 前的历史；后续 block 使用前一个已入队预测的 target 所更新历史。 |
| 恢复 | `hint_history.cc:187-206`；`decoupled_bpred.cc:1675-1743`；`common.hh:386-395,467-477` | 先恢复 block-entry snapshot，再有序重放此前退休的 control，必要时再加入纠正的 control；不能只恢复最新一条 taken。 |

这里的文件名若未带 `evidence/`，均相对源码根 `/opt/data/simulator/xs-gem5-hint-ittage`；RTL 引用相对此审查目录。

## 候选流水必须保留的行为

`CAM → context selection/hash → Context → way/tag match → Slot → eligibility/override` 是数据依赖链，而非源码行数的 timing 计数。CAM 输出的 PC 与 mask 参与 hash，Context 输出的 slot 参与下一 lookup。把 host 函数拆成流水需要明确 launch/capture 边界，而不能仅标注“s1/s2/s3”。

如果提案把 CAM、Context 和 Slot 都放在同一个架构周期完成，必然命中技能固定的 `same_cycle_dependent_secondary_lookup` timing FAIL。把 Context 或 Slot 声称为 generic register 数组且组合／零周期读，也命中 `asynchronous_or_below_minimum_read_latency` timing FAIL；这两条固定规则不靠未知工艺来解除。它们证明这种具体提案违规，不能证明所有有限 RTL 方案均不可行。

如果请求在边界 C 稳定，CAM/key 在 C+1 capture、同步 Context response 在 C+2 capture、同步 Slot response 在 C+3 capture，则依赖存储链跨三个 capture 边界；要在“两个完整周期”内输出，必须进一步给出预查找、并行读取、合法的额外 launch 时间或改变存储表示的具体方案。不能隐含删除一个边界。kmhv2 提供 s0 PC 和 s1/s2/s3 fire，但这些可用信号并不能自行证明 source→RTL 观察点映射。

kmhv2 ITTAGE 在 `evidence/rtl/ITTAGE.scala:531-535` 由 s1 fire 发读，在 `:544-601` 完成 s2 provider/target 选择，再在 `:449-458` 注册 s3 target，`:605-609` 输出。因此在此路径旁新增 Hint，需要单独固定 Hint 的最早 launch 和必须可见的 capture。`evidence/rtl/BPU.scala:382-440` 同时说明 s1 受 FTQ ready 影响、s2/s3 各有 valid/fire/flush；元数据必须和相应事务一起停留或取消。

### 查询 confidence 版本反例

设 entry E 在 cycle C 的 query 前 confidence 为 1，较老错误提交在 C 入 FIFO，C+1 应把 E 减到 0。source query 在 C 保存 1，晚些时候 `complete()` 仍可覆盖。若候选方案在 C 只做 CAM，C+1 才读取更新后的 confidence，则读到 0，丢失 source 覆盖。证据是 `hint_indirect.cc:816-821,874-884,974-987,1032-1041` 以及 CPU tick 顺序。

流水映射必须证明 launch 时版本保存。例如把 immutable tag/slot 和 mutable confidence 分开，维护有界版本记录，或以其他已计入资源的方案重建请求时值；还要覆盖 stall 导致的延迟。只写“旁路”不足：source 不允许把尚未到 readyCycle 的队列更新前馈到新查询，但允许且要求新查询看到本 cycle 已执行的写。提前应用未来更新会成为另一处功能错误。

### 吞吐与 update RMW

source 的理想服务上限是每 cycle 一次查询、一次队列 writeback；这只是 source obligation，不能直接填入 candidate `rtl_value`。候选 Context 把查询读和更新 read-modify-write 放在同一 SRAM 时，可能同 cycle 需要两次 read 和一次 write。即使表分为两 ways，两笔访问也可能落到同一 bank，bank 数不能自动证明足够端口。

若使用 1RW 且照搬目标 ITTAGE 的“写压掉读”语义，Hint 查询会变成假 miss。目标证据：`evidence/rtl/ITTAGE.scala:266-287,329-339`；source 没有对照的 query 丢弃或 replay。若候选实际采用这种复用，则 timing/function 发生已证明冲突；如果存储组织尚未选择，诚实状态是相关 bridge `NEEDS_INPUT`。

若使用 1R1W，需要区分 query read 与更新旧值读取，或者证明更新旧值预取／转发可以保持一写每 cycle 和对连续同 entry 更新的顺序。FIFO 本身也需支持同 cycle 有序接收至多 8 项、先 drain 再接收、满时只丢后续项。只画一个“8-entry FIFO”并不能覆盖这些端口和仲裁需求。

### 历史与恢复路径

当前 GHR fold 在 `hint_history.cc:217-237` 处理固定 970 bit；finite 其他上下文也是有界的。常数 rotate、mask、XOR 可构造有限组合逻辑，或以已证明等价的增量 fold 维护；但不能声称源码循环会自动获得这种硬件。计算 DAG 要列出实际 XOR/mux/decode 节点和寄存边界，不能把动态容器当作一个零延迟操作。

call/return 各维护 64 个 PC word，窗口满时须取最老事件、移除 fold、旋转、写入新事件（`hint_history.cc:47-67`）。若窗口用同步存储，oldest-word 读取 latency 与当前预测更新间隔需要一个明确预取／前馈方案。GITHR 更新是固定 128-bit shift；LITHR lookup/update 仍要对 PC 匹配。

`predictWidth` 为 64，因此 `maxInstsNum` 为 32（`decoupled_bpred.cc:70-71`），retired control prefix 最长 32 个。恢复函数在一个回调中遍历不等于允许恢复串行花 32 RTL cycles，也不等于必须把 32 次更新串成同一个组合锥。需要证明在下一允许 query 前完成的边界，或者明确并行 prefix、checkpoint sharing、undo log 等有限实现。普通运行上的 II 不能覆盖此 rare path。

## 可审计数字及 profile 条件

对于一份 CAM、每周期一次 search、16 entries、每 entry PC key 47 bit 的结构声明，v1 规范的 CAM 两项为：

| 量 | 重算 |
|---|---:|
| `cam_parallel_entries` | 16 × 1 bank × 1 search = **16** |
| `cam_compare_bits` | 16 × 1 bank × 1 search × 47 tag bits = **752** |

地址合法性来自 `hint_indirect.cc:188-205`，容量来自冻结配置；匹配是 `[start,end)` 范围搜索（`:744-780`）。两次 bound comparison 应在 timing DAG 与 logic points 中显式出现，不额外乘入规范 CAM aggregate。query 范围是完整 Addr 的 unsigned 比较；缩窄为 PC[47:1] 的电路还需保持非 canonical query、半开区间和 wrap 的行为，不能仅凭 metadata 合法性忽略 query 边界高位。

内置 policy 的 compare-bit 上限依次为 tightest 128、tight 256、normal 512、relaxed 1024。因此如果随后 exact profile 选择落到前三档，752 会构成该 CAM 提案已知 timing FAIL；relaxed 仅在这两个 aggregate 上未超限。当前未给工艺与目标时钟，不能提前选择 relaxed。

加载器按 PC 对有效 CAM 排序（`hint_indirect.cc:554-570`），因此一个明确保持该 preload 表示的候选可用 16-entry priority。其 `priority_max_entries` 为 16，只在内置 relaxed 的该单项上未超限。未明确选择这种结构前，不能直接把此数字写为整个 mapping 的权威 DAG 结果。

一个未经拆分的 32:1 Slot data mux，其 mux input 数为 32，会超过四个 builtin band 的最大上限 16。但此发现仅针对明确声明这种 selector 的候选，不能据此断言 32-entry 同步 Slot SRAM 或其他已划分结构也失败。改为 generic register 组合读亦不能绕过 memory policy。

没有完整、覆盖查询与训练及所有恢复路径的 edge-to-edge DAG，以下六项必须保留 null / `NEEDS_INPUT`：`serial_op_nodes`、`serial_arithmetic_nodes`、`mux_max_inputs`、`mux_serial_levels`、`priority_max_entries`、`fanout_nonclock_control_sinks`。特别是 snapshot 大范围 enable/reset、FTQ metadata 选择与恢复写入 fanout 不能漏计。

## 供主审采用的 gate 输入

1. 缺失 `process_node_nm`、`clock.period_ns`、`clock.frequency_mhz` 和 exact `profile`，产生 timing `NEEDS_INPUT`。
2. 缺失带 boundary、所有 operation instance 和 data/control edges 的完整 timing DAG，产生 timing `NEEDS_INPUT`；已知 CAM aggregate 保留 16 与 752，limit 留空。
3. 查询 latency/II、history recurrence、恢复带宽、confidence 版本、queue RMW 和读写冲突必须由 capability 的 schedule/capacity/service 重算；未实现的 bridge 不给 `rtl_value`。
4. 只在 canonical candidate 实际声明零周期 generic memory、同周期依赖二次 lookup、过量端口 demand 或照搬 read-suppression 时，针对该根因发出精确 FAIL；不要对未选的坏方案发明整体 FAIL。
5. 无构建或仿真并不是 blocker：缺少 ASIC 设计决策按 `NEEDS_INPUT` 处理，已有 source/target 证据可完成静态 review。

## Canonical mapping 复核

已独立读取 `candidate-mapping.md` 的完整原始字节，SHA-256 为 `89966f0fb46f9fdea46bde8de47c553e24afbb9a316b07e32c0a7bc8e820daf3`。其受审 source manifest 为 `sha256:2cfd621a9329aece46e072654414fb14260e15beaa1b523d894be50dde009e25`。本角色审查的是这份有限且不完整的 Candidate A；canonical YAML 的最终字节对应关系另由 cross/evidence review 核验。

**最终独立 timing gate：`NEEDS_INPUT`。** 没有发现已声明的 partial inventory 本身必须导出 timing `FAIL` 的已知冲突。此判断成立于以下明确边界：Candidate A 不主张全链在两周期完成，不采用零周期 generic memory，不采用现有 ITTAGE 的写压读，也不把上述 late-confidence 反例纳入已接受设计。其未完成的 schedule、snapshot preservation 和 recovery obligations 被保留为缺口，因此不能签署 `PASS`。

逐项复核结果如下：

- 六项 DAG metric 全部 null，coverage 为 needs_input，与没有完整 operation/data/control 图的事实一致。不能以 primitive latency 都为 1 来替代缺失的端到端 latency。
- CAM layout 为一份 16-entry、47-bit tag，最大一 search／cycle；两个 aggregate 独立重算为 16 和 752。其全 Addr 范围比较明确单列为 32 个 unsigned 64-bit 比较，未错误缩窄 query bounds 或重复扩大 CAM 规范 aggregate。
- `2R1W` Context 的声明 read 2、write 1、total 3 不超过其声明物理端口。它能表达 query 读与 writer RMW 读，但宏/profile 与写后读兼容性尚未有证据；不因未选择 builtin profile 就直接判定已知超限。
- `1R8W` FIFO 的声明 read 1、write 8、total 9 与物理端口数字自洽。一个成熟旧项先释放、随后八个新项填入空容量是合法最坏情况。它的 admission/位置 routing、lane 实现和 profile calibration 仍未证明，因而没有可接受的 II/rate capability。
- 所有 generic read latency 都是正值 1，CAM search latency 1。候选要求 dependent lookup 在不同边界展开；“每个 primitive”不能单独证明没有跨 storage 的 same-cycle dependency，所以这里仅视为待完成 schedule 必须遵守的限制，不能作为 PASS 证据。
- 已明示的逻辑位数可复算，但临时查询版本、流水 registers、写前馈和恢复资源未分配。尤其不得用未计入面积的 shadow confidence 表或全状态副本，暗中解除时序缺口。

新增的首要 schedule 义务是**空 FIFO 接收后首次 RMW 的最早写回**：source 在 cycle C 接收成熟于 C+1 的更新，并要求 C+1 query 看到已应用结果；如果候选严格串行执行 FIFO read 1 → Context read 1 → saturating update → Context write 1，就不会保持该边界。Candidate A 未承诺这种串行路线，因此目前是需要选择并证明 bypass／prefetch／当前值保存等实现的缺口，而非对已接受路线的 FAIL。连续同 entry 更新时还必须保持 read-current-before-update、FIFO 次序和饱和行为，不能仅优化空队列案例。

最小解除条件是固定 exact process/clock/profile；提交能够连接这些 storage primitives 与所有 source 观察点的完整能力表；证明 mature-write-before-query、launch-confidence preservation、下游 stall 和 FTQ reuse 的生命周期；补齐有序八路 enqueue、一写／cycle、预测历史 recurrence 和最长 retired-prefix 恢复；最后对完整 DAG 与端口 demand 重新计算。若完成映射后出现固定 policy 违规或周期差异，gate 应相应升为 `FAIL`，不能继续使用当前 `NEEDS_INPUT` 作为豁免。
