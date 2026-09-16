# kmhv2 目标接口审查记录

本记录审查 ICT186 `/home/xuejing/rtl/XiangShan-kmhv2` 的现有接口，以及接入新的 finite Hint 模块需要补齐的边界。目标 HEAD 为 `4b9ddb8a6311bff6f5e29cb3722d1f4236d66292`；utility 子模块为 `6ed7394ef85887ea360f33b3be97bb13fb31e57c`。远端身份记录显示工作树干净。以下行号均指本目录下 `evidence/rtl/` 的原始字节快照，和远端对应源码行号一致。

审查者：`target_interface_complete`，负责目标接口及存储拓扑的独立静态审查。没有构建、仿真或修改目标 RTL。全部 11 份 Scala 快照的 SHA256 已逐项核对，与 `provenance.json` 一致。

## 目标事实与含义

| 编号 | 已核实事实及源码行号 | 对 Hint 集成的含义 |
|---|---|---|
| T01 | `Parameters.scala:111–117` 的 ITTAGE 表为 256、256、512、512、512 项，tag 为 9 位；`ITTAGE.scala:244–253` 每项包含 valid、tag、2 位 ctr、20 位 offset、4 位 region pointer、usePCRegion 和 useful，共 38 位。 | 现有 ITTAGE 的 tagged-history 表和 RegionWays 不是 Hint 的 CAM16、Context4096、Slot32。直接把 metadata 装进现有 ITTAGE 表不能保持键、仲裁和目标表示。五表逻辑位数 77,824 只描述基线，不能算成新增 Hint 的已获预算或可回收面积。 |
| T02 | `Parameters.scala:394–395` 由最长 119 位历史、两分支、FTQ64 和 9 位余量推得 HistoryLength 为 256；`BPU.scala:122–131` 的输入只有 PC、folded histories、ghist 和预测响应。`ITTAGE.scala:70–73,87–101` 请求/更新接口亦无 Hint 历史。 | 无法直接复用该历史接口表达 970 位 Hint GHR、GITHR、各候选 LITHR 以及 Call/Return 窗口。可新增独立 Hint 历史模块，也可扩展共享历史；后者必须证明事件定义和恢复语义一致。 |
| T03 | `Parameters.scala:66,74–75,92,620–631` 默认启用 H 和 Sv48，内部 VAddrBits 为 50。`Configs.scala:41–46,443–448` 的 DefaultConfig 从该 core 参数构造。 | Hint metadata 的 PC/target 与 PC[47:1] 计算需要明确到内部 50 位地址的规范化、扩展与适用地址域。不能仅凭信号宽度近似就把高位丢弃；新增模块的命中比较与目标输出必须保持模拟器允许的地址语义。 |
| T04 | `Parameters.scala:123–142` 当前组件顺序为 uFTB→TAGE→FTB→ITTAGE→RAS。`Composer.scala:24–25,37–60,64–77` 分发阶段控制并拼接/拆分 meta，`69` 行要求总 meta 不超过 MaxMetaLength。`BPU.scala:27–28` 上限是非 FPGA 512、FPGA 256，再按 H 扩展加 4。 | 新 Hint 组件存在明确接入点；应放在 ITTAGE 结果后、保留 RAS 的 return 优先级。需要扩展输入并重新计算总 meta。仅新增一个 meta 字段不足以证明 FTQ 能容纳全部查询/恢复状态，可用独立 FTQ-indexed 存储，但必须纳入容量、端口和生命周期。 |
| T05 | `BPU.scala:238–239` 是三阶段。`ITTAGE.scala:533–535` 在 s1 fire 发请求，`544–601` 在 s2 选择，`449–458,605–609` 寄存到 s3 jalr_target。 | 可以在 s3 形成 Hint 覆盖点，但还没有实现 Hint 的 CAM→hash→Context→Slot 调度。目标有三阶段不等于该完整依赖链已在三阶段内实现，更不能把模拟器一次回调等同于一段组合逻辑已闭合时序。 |
| T06 | `BPU.scala:376–441` 将 redirect 逐级传播 flush；s1 fire 受 FTQ ready 和下游 ready 限制（`396`），s3 fire 等于 s3 valid（`428`）。`886–894` 在送 FTQ 时用 fire 和 flush 限定 valid。 | Hint 查询、候选元数据、历史快照必须随相同 token 停顿与冲刷，避免重发、重复训练或混用相邻 block。不得为了隐藏新增 Hint 查询冲突而引入模拟器不存在的丢查询或额外停顿。 |
| T07 | `ITTAGE.scala:266–274` 使用 holdRead、singlePort 的 FoldedSRAMTemplate；`284–287` 对同周期 update.valid 与 req.valid 的 tagged 请求屏蔽下一周期响应；`329–339` 仅 power-on reset 会使请求不 ready，普通写冲突不会阻塞整个 BPU。 | 原样复用现有 ITTAGE 存储仲裁会丢失 Hint 在该周期应得到的查询结果。这是直接复用方案的功能反例，并非所有新 Hint RTL 映射均不可实现。需要独立端口、分离静态字段/置信度、复制或其他明确保序机制，并逐一计入代价。 |
| T08 | `ITTAGE.scala:341–356` 用 WrBypass 的最新 ctr 或 update.oldCtr 更新计数，分配写 2；`WrBypass.scala:68–82,95–124` 以索引/可选 tag 的 CAM 和 Mem 保存最近写数据；ITTAGE 构造时未传 tagWidth。 | 它是当前 ITTAGE 更新旁路，并不是 Hint 的深度 8、满丢最新、提交后一周期成熟、每周期一次写回的 FIFO。不能借用该旁路而把未成熟 Hint 更新提前暴露给预测；也不能用提交时重新读出的 confidence 替代 launch 时保存的 confidence。 |
| T09 | `FrontendBundle.scala:667–676` 的 SpeculativeInfo 只有历史指针及 RAS 信息；`Bundle.scala:96–123` 的 CfiUpdateInfo 只有现有分支恢复字段。`NewFtq.scala:631–650` 在最后预测阶段保存 redirect info 和 meta。 | 当前没有 Hint 查询身份、候选 PC、context/slot 身份、launch confidence/target 或 Hint 历史恢复状态。需新增或另存这些状态，并保证 FTQ entry 重用/重定向时旧 token 无法命中新 entry。 |
| T10 | `NewFtq.scala:189–194` 对 BPU 只有一个 Valid BranchPredictionUpdate；`1471–1480` 每次提交一个 block 的 PC、meta、cfi 和 target。`FrontendBundle.scala:743–768` 的更新是 FTB block 结构。`NewFtq.scala:1314–1326` 将 ROB 提交先记入逐指令状态；`Parameters.scala:150–151` 提交宽度为 8。 | 现有 block 更新时点与模拟器逐控制流指令的 commit 时点并不相同，不能直接把它当作 Hint FIFO enqueue。需要独立退休事件批次或可证明等价的适配器：同周期最多 8 条按程序顺序的事件、准确提交周期、实际目标/类型和相应查询身份。若延迟到整个 block 完成再训练，会改变队列溢出与后续预测的 confidence。 |
| T11 | `BPU.scala:896–901` 的更新 ghist 从保存的 histPtr 读回；`NewFtq.scala:1149–1155` 用 FTQ redirect SRAM 恢复现有信息。 | Hint 提交训练应使用 launch 保存的 context 身份，不应重新 hash 当前或目标已有 ghist。Hint squash/trap 要恢复 block-entry snapshot 并重放边界前的有序已退休控制流，现有 histPtr+单 CFI 不能自动证明包含所需信息。 |

## SRAMTemplate 的可复用能力和限制

`SRAMTemplate.scala:138–156` 的底层为 Chisel `SyncReadMem`。`173–174,214–219` 明确只有同步读，singlePort 时写操作禁止同周期读；`249–250` 的 read ready 反映 reset 和写占用，write ready 始终为真。`159–170` 的复位是逐行清零；这不是将 Hint 的有效 metadata/初始 confidence 装入表的协议。

`FoldedSRAMTemplate` 在 `254–292` 将逻辑 set 按 width 折叠，物理行数是 set/width、每行 way 数是 width×way；`335–341` 寄存低位行内索引并选择返回字。折叠改变深宽，不增加独立的每周期访问端口。

双端口选项也不能未经论证直接满足 Hint：`SRAMTemplate.scala:222–240` 对同址读写有独立 bypassWrite 处理；关闭 bypassWrite 时构造的冲突输出来自 LFSR 占位，不能作为固定 read-old/read-new 语义。同文件 `294–328,343–363` 的 FoldedSRAMTemplate avoidSameAddr 可以缓存冲突写，但只有一个冲突暂存位置，且改变实际写入时间。Hint 映射必须自行给出同址冲突、成熟写回、launch 快照和连续冲突的行为证明；仅选择 `singlePort=false` 不够。

这些模板证明目标存在有限同步存储构造方式，不提供 process-specific SRAM 宏、端口时序或物理面积承诺。工艺、时钟和预算仍是未给定输入。

## 新 Hint 模块需要明确的接口契约

以下是待实现的有限接口桥接需求，不是已经存在的 RTL 或已经通过门禁的能力声明。

| 接口 | 最小信息与行为 | 生命周期/验收条件 |
|---|---|---|
| Query launch | 单 block 的有效 token、起止 PC（半开区间）、FTQ/epoch 身份和查询前 Hint 历史；每周期最多一个有序查询。 | 查询是否为空或地址回绕也必须按源契约消耗发射机会。任何流水停顿不能重复 launch；每个 token 保存 launch 时的选择和 confidence 可见性。 |
| S3 override | 与 block token 绑定的候选 PC、tagHit、slot target、launch confidence，以及基线最终 selected-CFI 的类型、taken、PC 和是否有 BTB。 | 按源取消顺序处理 not-taken、return、非 indirect、PC/较早跳转不符、无 BTB；仅允许正确的 indirect 覆盖。RAS 返回预测不能被 Hint 替换。 |
| Hint history event | taken、PC、实际/预测 target、call/return/indirect 类型和严格的事件顺序；独立 GHR、GITHR、LITHR、call/return 状态或经证明的共享表示。 | 同周期多事件的顺序、查询前快照、分支修正和目标高位规范必须与源一致。 |
| FTQ metadata/checkpoint | 查询的 CAM/context/slot 身份、键/标签、launch target/confidence、使用/取消信息；Hint 恢复 checkpoint 或有限 undo 信息。 | 保存到 commit/squash；包括所有 in-flight entries 的容量、并发读写、冲刷与重用。新增 meta 必须重算 MaxMetaLength 或使用单独的有限存储。 |
| Retire-to-train | 最多 CommitWidth 个有序 commit 事件、准确 commit epoch、查询元数据和实际结果。 | FIFO8 先按规定处理成熟写回，再按程序顺序入队；满时丢最新且无反压，至少下一周期才可写，每周期最多一次置信度写回，饱和于 0…3。 |
| Redirect/trap recovery | 恢复的 FTQ token、边界 offset、inclusive/exclusive 规则、block-entry checkpoint、边界前退休控制流序列和本次修正事件。 | 精确重放边界前缀并恰好应用修正一次；不把 trap recovery 误建模成仅恢复 GHR/RAS。需要证明恢复端口和延迟不改变契约。 |
| Initialization/control | 在预测启动前写入 CAM/Context/Slot/初始 confidence 的有限加载接口，以及 measurementActive 等真实行为控制。 | 明确 cold reset、加载完成、启动、统计复位和停止语义；resetStats 不能清掉队列/历史/confidence，结束也不能隐式排空队列。文件解析和日志可以在硬件外完成。 |

## 直接复用与新模块可行性的区分

现有 kmhv2 没有可直接启用的 Hint 组件；接口和状态表示必须增加。已有 ITTAGE 的单端口冲突丢响应、block 才更新、更新旁路及历史宽度若被原样采用，会与 Hint 的可观察行为不同。以上是具体复用方式的冲突，不是 finite replay 不存在 ASIC 实现的证明。

新模块接入在结构上有明确位置：扩展 Composer 的组件及输入、在 s3 施加有限目标选择、新增退休更新通道和 FTQ 恢复状态。尚需周期调度和所有状态/逻辑的完整映射才能裁定 timing/function；还需目标预算才能裁定 area。三阶段数量、有限表大小、已有同名 ITTAGE 都不能替代这些证明。

## 生成 Verilog 证据边界

`provenance.json` 保存了 `build/rtl/ITTage.sv`、`ITTageTable.sv`、`build/VERILOG_BUILD.md` 和 `.git/codex-kmhv2/rtl-manifest.json` 的远端路径、大小和 SHA256；本记录生成时这四份原文尚未在 `evidence/rtl` 内。前序探索称它们属于 2026-09-14 的单核 DefaultConfig/XSTop 生成物，但本记录不把该二手摘要当作行级证据，也不以其证明当前新增 Hint 可 elaboration 或综合。

核心接口和宽度判断以上述已核对的 Scala 原文为证据。若随后补齐生成物，应再核对源码/配置/生成器/子模块身份和生成时间；生成成功本身仍不能证明 Hint 的周期等价、ASIC timing closure 或 area closure。
