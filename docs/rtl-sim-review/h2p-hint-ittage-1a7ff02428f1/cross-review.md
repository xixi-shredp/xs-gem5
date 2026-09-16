# Finite Hint replay 的跨门禁复核

本记录由独立 cross_review 角色生成，协调 source-contract-ledger、target-interface-ledger、timing-review、area-review、function-review 和 counterexample-review。它是辅助审查工件；result.yaml 和 review.md 才是最终权威记录及镜像。没有构建、仿真、修改模拟器或修改目标 RTL。

审查范围是向 ICT186 /home/xuejing/rtl/XiangShan-kmhv2 新增 finite replay Hint 模块和必要接口。当前 gem5 HEAD 为 1a7ff02428f1914936ee3e640fe2006da0d1209b，kmhv2 HEAD 为 4b9ddb8a6311bff6f5e29cb3722d1f4236d66292。实际字节身份以同目录 source-manifest.json 为准；最终107文件 manifest hash 为 sha256:2cfd621a9329aece46e072654414fb14260e15beaa1b523d894be50dde009e25，包含96个 gem5/config/test/tool/doc 文件及11份目标 Scala 快照。

## 复核方法

1. 对照独立台账及原始源码，校验观察点、执行顺序、存储生命周期和边界。宿主回调不计作 RTL cycle；源码上界不直接当作 candidate capability。
2. 将重复问题合为一个根因；仅把确实受影响的 gate 放入同一个 finding 的 categories。预想中的坏方案不构成当前未选定映射的 FAIL。
3. 跨查询、成熟写回、FIFO 满、FTQ 释放和恢复构造反例；区分“反驳一种具体映射”和“反驳所有有限实现”。
4. 最终 gate evidence 应是其 contract、metric、finding 和真实 synthetic contributor 的最小 mapping 闭包的精确并集，不能扫描全部 evidence 目录填满三个 gate。

## 已统一的语义事实

| 事实 | 一致的审查含义 |
|---|---|
| 配置锚点 pickle_pure_python_test/18 固定 replay、ideal false、numStages 4、HintS3Stage 2、CAM16、Context4096 总项、2 ways、Slot32、FIFO8、FTQ64、GHR970 | 目标三阶段不等于源三阶段；Hint 修改数组 index 2 及以后。须单独桥接 source 观察点与目标 launch/capture。 |
| CPU 顺序为 hintCycleTick → commit.tick → IEW/rename/decode → fetch.tick，见 cpu.cc:581–590 | C 周期查询看见本周期已成熟写回；C 周期 commit 虽早于 fetch query，但入队更新 readyCycle 为 C+1，所以查询不看见本次入队。不能声称 commit 位于 query 之后。 |
| query 保存 entryId、slot、target、confidence；commit 使用保存身份；writeback 读取当前 entry.conf 饱和加减 | 保存 confidence 决定 launch 覆盖与统计，实际训练 RMW 使用成熟周期的当前值。conf0 的真实 tag hit 仍参与 shadow training。 |
| cancellation 的各拒绝条件都令 eligible 保持 false；源代码选择第一个原因计数 | 必须保持取消集合及输出；若排除诊断计数，单纯交换两个拒绝条件的顺序不自动改变目标或训练。不能把该交换本身写为功能反例。 |
| HintSnapshot 可被 HintQuery 和 FetchTarget 两个 shared_ptr 引用，更新 FIFO 又可延长 query 生命周期 | 同一快照不能按指针数重复计费；反过来，FTQ64 不能证明整个 live-query pool 恰好64。需要包含尚未入 FTQ 的预测及 FTQ 释放后留在更新队列的对象或其必要字段。 |
| Context key 可由 set 与 tag 重建；entryId 是 set/way 地址 | 不把 key 和 entryId 再作为每个 Context 项的额外 payload 重复计数。 |
| 五份镜像实际占用各异，但审查覆盖配置下所有合法镜像 | CAM/Context/Slot 仍按16/4096/32计容量；五份镜像不表示五组硬件，也不允许按本次占用缩小容量。 |
| finite 初始化之后 CAM、Context 的 valid/tag/slot 与 Slot 固定，只有 confidence 更新 | 不需要凭空引入运行时 Context replacement。若候选另行加入热加载/替换，必须明确暂停、排空及身份保护协议，并将其视作新增边界。 |

## 根因合并与固定路由

| 根因 | 当前新模块审查应如何记录 | 最小解除条件 |
|---|---|---|
| 缺少目标 process_node_nm、clock.period_ns、clock.frequency_mhz、selected profile 和 sram/register/cam/logic budgets | timing、area 的 NEEDS_INPUT。gem5 clock 333 是模拟器配置，不能推成 ASIC 时钟，也不影响源功能语义本身。 | 提交一致的 ASIC period/frequency、精确支持的工艺/profile、完整四类预算。 |
| 缺少可重算的 candidate schedule、capability、DAG 与存储端口/需求 | timing、function 的 NEEDS_INPUT；未闭合 state/logic/port 库存和资源需求同时影响 area。 | 冻结所有操作、存储、launch/capture、同周期顺序、服务需求、容量及恢复 schedule，重算 numeric capability。 |
| 970-bit GHR 与 GITHR/LITHR、Call/Return 状态、FTQ query/checkpoint、逐 CFI 退休和恢复桥接未定义 | 一个 integration 根因可覆盖 timing、area、function，避免按 gate 复制三份漂移 finding。 | 定义有限接口、状态编码/共享方式、生命周期、每周期输入/输出、恢复延迟和资源占用。 |
| naive CAM 后移到下一周期才读 mutable confidence | 只对该具体候选是 function/timing FAIL。未选择该候选时，作为对待定义 launch-version 机制的反例，不把它升格为“所有 finite RTL 都失败”。 | 保存 launch 值或有界版本/转发机制，涵盖流水停顿；保证未来队列更新不提前可见，已成熟更新可见。 |
| 原样复用 kmhv2 ITTAGE 的单端口抑制读响应、block update、旧表几何和历史接口 | 直接复用 proposal 的已证功能不一致；若该 proposal 实际被选择，其延迟/服务率冲突还影响 timing。当前 scope 是新模块，宜放入 finding 的反例与 integration 需求，而不制造 scope 外的 open FAIL。 | 新增 Hint 结构，或给出等价扩展的明确契约和证据。不能把旧 ITTAGE bits 当作已授权 Hint 预算。 |
| 同周期 dependent second lookup 或 generic memory 零周期读 | 一旦 mapping 实际声明，就是不依赖 profile 的 timing FAIL；不能只因为缺工艺便将其降成 NEEDS_INPUT，也不能无因地扩大到 area。 | 选择合法跨周期映射或其他经过明确计费的表示，并保持源观察点。 |
| 超过 declared/profile memory access capacity | 按固定条件路由 timing 与 area；undefined/incompatible collision 单独路由 function。 | 明确有效端口、bank 路由、最坏同址 demand、碰撞和串行/并行策略。 |

一个 root finding 可同时列多个 categories，但只应包含真正的因果门禁。severity/blocking 只决定行动优先级，不能豁免聚合。未选定的 direct-reuse 或 naive-retiming 坏方案不应污染当前 finite-new-module 的整体状态。

## 跨门禁反例

**Launch confidence。** 在 C 周期成熟写回之后，entry E 的 confidence 是1。较老错误分支在 C 的 commit 阶段入队，readyCycle 为 C+1。C 的 fetch query Q 保存1；C+1 写回把 E 变为0。若 RTL 在 C 做 CAM、C+1 才无版本地读 E，就会压掉源 Q 的覆盖。解决它需要功能上的值版本保证、时序上的读取/延迟安排，以及面积上的版本或快照成本；三个方面必须描述同一机制。

**FIFO 满与提交突发。** 一个合法周期开始时 FIFO 已满，最多先释放一个成熟项，再收到最多8条有序 commit。只能接受空余容量对应的最老新项，其余丢新；不能反压 commit、丢旧、同周期再成熟或让新项旁路到查询。九次 enqueue 的辅助单元测试支持队列语义，但九条并非一次 commitWidth8 的合法 CPU 提交；部署反例采用“原队列已满 + 合法提交突发”。

**FTQ 释放与延迟训练。** 一条 branch 退休后其 update 仍在 FIFO，FTQ entry 可以被复用。若硬件只在 FIFO 存裸 FTQ index，又覆盖该 entry 的 target/identity，则之后写回读到新 query。可使用自包含更新 payload，或带生命周期保护的有限索引；其位数、容量、端口及 release-before-accept 规则都必须纳入同一 mapping。

**Trap 恢复。** 最长32条有序退休控制流前缀可包含多个 fallthrough-target 控制、call 和 return。只保存最后一次 taken 会丢失 GITHR/LITHR 与 PC-window 历史；逐项用32个 RTL 周期重放又可能增加源模型不存在的延迟。实现需给出在下一允许 query 之前完成的准确机制，不能把有界软件循环作为一周期电路证明。

## 证据去重与修订记录

- 初始 source ledger 中反转的 CPU/query/commit 顺序、错误 decoupled_bpred.cc 路径及 tests/unit 路径已反馈，source reviewer 确认修正。正确退休统计桥接在 src/cpu/pred/btb/decoupled_bpred_stats.cc:888–893；实际测试在 src/cpu/pred/btb/test/hint_indirect_test.cc。
- function reviewer 已将“重排取消谓词改变 target/training”改为仅取消原因计数变化，并明确 saved confidence 和 writeback current confidence 的职责。
- 初始工作曾误把顶层旧 build-1 manifest 当作正式版本；evidence_audit 已找到正式 build-2 的精确 manifest，摘要为5be39be44760477fc47b608df5df76c4dc27669e0edac720b823224cc1a819de，并确认全部96份 gem5 文件均匹配。当前 hint_indirect_test.cc 没有偏离正式版本。旧顶层 manifest 仅该测试文件不同，不得将其误报成当前测试漂移；本次既有测试记录仍仅为模拟器辅助证据，不能证明 RTL 等价。
- 目标 provenance 列出四份生成物 hash，但早期仅有摘要、没有原文。核心结论只引用已核验 Scala/SRAMTemplate；若最后补入生成物，必须再次验证其源码/配置/工具来源，不能凭 hash 字符串使用不存在的行级证据。
- 104752 bits 仅是 CAM848、Context102400、Slot1504 的直接布局小计，不是 total area。history/FTQ/FIFO/流水/恢复/控制和 replica、padding 均须明确；现有 ITTAGE 77824 bits 不计作 Hint 可用资源。
- 不把主审、各 agent 和本记录对同一源事实的复述当成独立的三条 source 证据。优先引用原始 hash 文件与窄行范围；ledger 仅补充推理过程。

## Candidate A 冻结后的复核

已对照 candidate-mapping.md 的冻结版本完成语义复核，其原始 SHA-256 为89966f0fb46f9fdea46bde8de47c553e24afbb9a316b07e32c0a7bc8e820daf3。Candidate A 是明确标注未闭合的有限 inventory：六块存储给出真实字段和端口，但不把 primitive latency 直接当作查询/恢复端到端周期能力。

独立算术核对：CAM 为16×53即848 bits；Context 为2048×50即102400 bits；Slot 为32×47即1504 bits；live history 为128+128+2×(64×47+6+7+28)即6354 bits；FIFO 为8×220即1760 bits，控制10 bits。register 部分为1504+6354+1760+10即9628 bits；部分 inventory 共112876 bits。这些数值与文档一致，只能用于列明部分状态的成本，不是 total Hint area。

Context 的2R1W与FIFO的1R8W并不在 builtin generic memory 支持集内。当前没有 selected profile，且允许完整 calibrated custom profile 选择，因此其支持性是 area NEEDS_INPUT，不能一面假定 builtin 一面给它们 PASS。声明 demand 没有超过声明的物理端口数；以后必须验证实现这些端口所需的 bank/replica/routing。FIFO 的同周期一读八写可能读写同一个将释放的槽，candidate 已要求先释放旧项再按程序序接受新项；最终 collision.description 还应明确该 old-item/read-before-new-write 语义。

Candidate 保留足宽 actual target 和 readyCycle，自包含220-bit FIFO payload 可避免裸 FTQ 指针复用问题。它没有声称已经解决 query launch 版本、checkpoint/replay、GHR970、退役接口或源4-slot到目标3-stage 的时序桥接。保持 numeric contracts/capabilities 空、六项 DAG metric 未决与未分配 FTQ/pipeline/recovery 状态相一致；这是合法的未决审查结果，不是凭空声称实现能力。

我未发现该冻结 inventory 内已证明的功能自相矛盾。按当前范围，timing、area、function 的建议结论均为 NEEDS_INPUT。既有 direct-reuse 和 naive-retiming 反例已明确排除为不接受的方案，不影响该结论。最终 structured YAML 尚需以下闭包检查：

1. 每个数值 candidate contract 来自 mapping.capabilities，不能手填 rtl_value 或从 prose 推断。Candidate A 无完整 bridge，数值 family 空并由 findings/source ledger 记录缺口是与冻结文档一致的选择。
2. 八项 timing observation 恰好齐全。未解析 profile/DAG 的六项 DAG metric 为 null；实际声明 CAM16、单 search、PC47 时独立 CAM 两项为16和752，threshold 保留未解析。
3. state bit ranges 的 ownership 恰好、无重复、无越界；capacity pool 包含实际状态生命周期；完整 operation/port inventory 与 area 记账一致。
4. gate metric IDs、finding IDs 和 evidence IDs 分别恰好等于其拥有或因果路由记录的并集。不要把全部五份镜像、所有 target 源码或所有 reviewer 文档直接塞入每个 gate。
5. evidence_audit 应独立重算最终 manifest、config、profile、evidence hash 与行范围。Default validator 合法不代表 PASS；还必须运行 --require-pass 并保留两份 JSON。

所有状态下 frequency_closure 为 UNPROVEN、area_closure 为 UNPROVEN、signoff_status 为 NOT_PERFORMED。
