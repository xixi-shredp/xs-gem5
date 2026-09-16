# Finite Hint replay 的独立面积与存储审查

本文件由独立 area reviewer 生成。范围是有限 Hint 新增状态及其在 kmhv2 上的接口负担；不把原有整个 BPU 的面积算作 Hint 增量。已完成对主审冻结的 Candidate A 的独立面积审查，area结论为 **NEEDS_INPUT**。以下可精确计算的是指定布局下的逻辑位数；它们既不是物理面积，也不是任意实现的数学最小值。

源码身份取自同目录 `source-manifest.json`，最终107个文件的manifest hash 为 `sha256:2cfd621a9329aece46e072654414fb14260e15beaa1b523d894be50dde009e25`。本reviewer从现场完整文件重新计算了全部107个逐文件哈希和规定算法的manifest hash，均一致。被审candidate为同目录 `candidate-mapping.md`，SHA-256为 `89966f0fb46f9fdea46bde8de47c553e24afbb9a316b07e32c0a7bc8e820daf3`。目标是取回的 kmhv2 `4b9ddb8a6311bff6f5e29cb3722d1f4236d66292` 源码。没有修改功能代码、构建或仿真。

## 冻结 Candidate A 的最终独立复核

已逐项重新计算六个storage row：CAM16×53为848位；Context将两个way打包为每set50位，2048×50为102400位；Shared Slot32×47为1504位；Live HintHistory为6354位；8项训练payload为8×220、即1760位；head/tail/occupancy控制为10位。后三类寄存器连同Shared Slot共9628位，六行已声明库存小计为112876位。没有算术错误，没有把4096总Context项误作4096 sets，也没有把key或entryId重复算进Context项。

全部六行取一个副本、没有已计的fixed overhead。这只足以说明这六行的已知逻辑位库存；candidate明确列出尚未分配的pipeline/query metadata、confidence版本保存、FTQ历史checkpoint、970位GHR接口、退休前缀/恢复、控制状态和宏相关额外成本。因此9628/102400/848三个分类小计和112876总库存小计均不能作为完整Hint资源总量。任何把这些数改称“全部面积”或以其直接比较总预算而得出PASS的canonical呈现都会与该候选范围冲突。

Context声明dedicated read2/write1，最大2次read与1次write；FIFO声明read1/write8，最大1次read与8次write。二者在声明的物理端口计数上没有超过容量，而且分别形成schema允许命名的2R1W和1R8W拓扑。但它们都不属于内置generic的1RW/1R1W允许集合。当前未选择profile，也没有提供完整custom profile和宏/库calibration，所以此条件目前贡献area NEEDS_INPUT，不能假装已获内置支持，也不能把“未选profile”提前写成已证明不在某个已选集合的FAIL。若后续直接选择内置profile而保持这两个布局，就会产生area FAIL。

FIFO最大读1写8的同周期场景在逻辑容量上成立：周期开始有1个成熟旧项，先读取并释放它，再按程序顺序接收8个新项，最后occupancy为8。head/tail各3位、occupancy4位能编码该环形队列的0至8占用。这个容量事实不证明其带一周期read的返回、confidence写回、8路enqueue与FTQ数据供应能按source schedule完成；相关capability仍必须保持未决。Context与history采用正一周期primitive latency，不出现把零周期寄存器读取伪装成合法memory policy的情况。

该候选为两个PC窗口保留47位word和必要head/count/fold，为有限replay的16个候选保留LITHR，并明确保留FIFO稳定副本，满足本清单直接编码的数学假设。它没有从目标已有ITTAGE容量中扣除任何“免费资源”。尚未证明的state所有权、容量及service桥接已被candidate明示；因此当前area结论是缺少完整布局、profile和预算导致的NEEDS_INPUT，未发现支持area FAIL的已声明超容量或全局非法端口拓扑。

复核范围是上述SHA绑定的candidate文本与源码清单；生成canonical YAML后仍需执行schema、bit-range ownership、resource observation和gate causal evidence校验。本文不替代该校验，也不决定overall。

## 容量与直接表布局

配置锚点 `evidence/config/pickle_pure_python_test/18/config.ini:279–304` 固定 FSQ/FTQ 各64项、Hint CAM16项、Context4096总项、2 ways、Slot32项、FIFO8项、GHR970位、4个预测阶段和64字节预测块。`hint_indirect.cc:424–429` 推导 Context 为2048 sets，索引11位，28位 key 去掉索引后 tag 为17位。

| 逻辑存储 | 直接布局与全部字段 | 位数 | Ownership 与依据 |
|---|---|---:|---|
| 候选 CAM | 16 ×（PC47 + mask5 + valid1） | 848 | PC、mask、有效成员信息；`hint_indirect.hh:154–158`，`.cc:554–570` |
| Context | 2048 sets × 2 ways ×（valid1 + tag17 + slot5 + conf2） | 102400 | 4096个独立物理项；`.cc:615–644`，`796–821` |
| Shared Slot | 32 × target47 | 1504 | 目标常量；`.cc:571–579`、`819–821` |
| 上述直接布局小计 | 848 + 102400 + 1504 | 104752 | 只覆盖这三类表，不含历史、FIFO、流水、FTQ、恢复、初始化或任何复制 |

地址装载器要求两字节对齐、bit47符号扩展至bit63，因此存储PC/target[47:1]的47位可以无损恢复合法镜像中的完整地址（`hint_indirect.cc:187–205`）。输入查询范围与实际提交目标不能未经额外证明也截成47位：这些参数是64位 `Addr`，没有经过该装载检查。

该 CAM 布局每项设置一个有效位，便于表示全部合法镜像；源码本身仅在有效项中保留排序后的成员。若选择前缀长度编码，可改变有效信息成本，但必须另行声明加载、顺序和查询逻辑，不能悄悄少算。Slot 未加载位置永不被有效 Context 引用，固定加载协议可以不为每个 Slot 再存一个 valid；该结论要求镜像在运行期间不热更新。

Context 的 `key` 不应重复计入表容量。它由 set 与 tag 唯一确定（`.cc:640–644`）；`entryId` 是 set×2+way 的12位地址，也不是每个表项的 payload。若拆分只读与可训练存储，valid/tag/slot 部分为4096×23、即94208位，confidence 部分为4096×2、即8192位。拆分本身尚不证明查询和更新端口足够，也不解决 prediction-time confidence 快照问题。

五份现有镜像使用的有效 CAM/Context/Slot 数分别为 deltablue 1/2795/32、pickle 1/1948/32、pyflate 2/774/32、regex 6/129/32、scimark_fft 4/1469/32。它们只能证明当前镜像占用；本次要求覆盖全部合法镜像，所以不能以这些占用数替换16/4096/32容量。

## 历史、快照和恢复状态

| 状态 | 直接有界编码 | 位数 | 依据 |
|---|---|---:|---|
| GITHR | 64个target[2:1]，两个64位字 | 128 | `hint_types.hh:19`；`hint_history.cc:168–170` |
| 有效 finite LITHR | 至多16个CAM PC，每个4×2位 | 128 | replay `historyPcs` 来自CAM；`hint_indirect.cc:443–449`；`hint_history.cc:171–180` |
| 一个 Call/Return PC window | 64×47位word + 6位head + 7位count + 28位fold | 3049 | `hint_history.hh:23–34`；`.cc:47–67` |
| 两个 PC windows | 2×3049 | 6098 | Call和Return可由同一控制指令同时更新，不能合成一个窗口 |
| 一个直接编码的有效 HintHistory | 128 + 128 + 6098 | 6354 | 不含共享的PC配置、GHR、query metadata和指针 |

源码 finite 数组实际有20个 `uint8_t` local项；replay仅给CAM PC建立索引，最多16个，因此另外4项从零初值不可达非零。若忠实保留20项，`HintHistory` 的上述逻辑展开是6386位。`HintSnapshot` 还复制970位原始GHR并缓存28位折叠值，得到7384位的直接逻辑展开。该数不包含宿主padding、指针、分配器或引用计数，也不等于所需RTL快照位数。

原始GHR只用于构造fold和诊断输出；恢复Hint历史读的是 `snapshot->history`（`hint_history.cc:187–206`）。gem5 FTQ另有 `history` 保存同一block-entry GHR（`common.hh:402`；`decoupled_bpred.cc:1500–1513`）。因此不能把970位原始GHR和全局PC配置在每个对象中重复计算后称为必需面积。相反，若选择共享、重算或撤销日志，必须证明恢复观察点、保存数、索引生命周期和带宽，不能只写“复用FTQ”。kmhv2原有GHR256位也不能自动提供Hint所需970位历史。

纯功能范围还有进一步压缩空间：每个PC window word之后只参与 `hintFold28` 的旧事件删除；完整47位word的逐项输出只在诊断 `historyJson` 中使用。显式改为保存28位fold的候选窗口，每窗可用64×28+6+7+28、即1833位，总HintHistory为3922位。这里仅说明一个可以提交验证的编码选择；当前源码并没有该RTL实现，且其运行调度仍须计入operation和端口，不能据此直接承诺面积门禁通过。

FTQ可保留64个不同历史版本。若明确选择每项完整复制上述6354位HintHistory，仅64份payload就是406656位；还需一个live history、预测流水中尚未入FTQ的版本、有效位及索引。这个数字是“完整复制”方案的条件成本，不是普适下限。`HintQuery::snapshot` 与 `FetchTarget::hintSnapshot` 在源码指向同一对象（`hint_indirect.cc:727`；`common.hh:383–384`），同一版本不可因两个指针被计两次。更新FIFO还可能延长query及snapshot寿命；若硬件保留指针式索引，就必须在FTQ释放后保护所指状态。

恢复还依赖有序 `retiredControls`（`common.hh:386–399,467–477`），每个预测块最多64/2、即32条控制记录。对Hint恢复而言，记录至少要提供用于边界比较和Call/Return的PC、target[2:1]、indirect/call/return/taken；GHR恢复另需isCond。不能将“32条记录”写为“最后一次taken”一条。完整FTQ每项独立保存与仅保留正在退休的前缀是不同候选设计；后者必须证明顺序退休、不同时持有多个有效前缀、trap与squash的消费时点。当前没有选定该布局，因此没有把32×64或单份32项任意写成最终容量。

## Confidence FIFO 与 query metadata

FIFO深度8已由配置和满检测固定；满时丢弃新请求，旧请求不能被覆盖（`hint_indirect.cc:974–994`）。源码FIFO payload包含 `shared_ptr<const HintQuery>`，实际目标和时间/序号（`hint_indirect.hh:167–174`），并在写回访问saved entryId、slot、key、hintTarget（`.cc:1027–1041`）。只计8个宿主指针既没有定义有限硬件ID，也漏掉其所保持的状态生命周期。

一种保守且自包含的有限payload编码是：entryId12位、key28位、slot5位、hintTarget47位、actual64位、readyCycle64位，合计220位/项，8项共1760位。若采用普通环形FIFO，head3位、tail3位和occupancy4位再增加10位。该编码仅作为可审计候选说明；没有声称已选宏、已完成端口映射或适用的周期能力。sequence、commitCycle和queryCycle只用于本范围排除的诊断输出，可不作为功能FIFO状态；`readyCycle`参与写回资格，不能因名字类似诊断字段而直接删除。

也可以在enqueue时计算完整64位target相等性，之后只保留方向bit；因valid/tag/slot在整个运行期不变，身份检查还可以通过加载后不改表的硬件不变量保证。若逐周期控制证明“先drain、后enqueue、新项下周期才可见”，绝对readyCycle还可能改为隐含一周期资格。这些压缩都要求明确的接口、operation和生命周期证明；本次清单不把它们当已完成设计。

提交宽度为8，而confidence最多每周期写一项。任何FIFO方案都必须承受合法同周期多enqueue、同周期先pop后push，以及剩余空间不足时按提交顺序接收前缀、丢弃后缀。不能把8项深度当作只有一个写端口也足够的证明，不能将declared banks或replicas再乘一次来虚增端口能力。

query必须跨预测阶段与FTQ保存对应PC、tagHit、预测时confidence/目标、eligible和稳定的entry身份，直到最终覆盖和提交训练完成。`blockStart/end`、baseline target等字段有诊断用途，不是全部都要照抄为RTL寄存器；`completed`、`eligible`则参与训练资格。最终候选须按每个阶段实际存活字段给出storage ownership，不能只写一个没有位宽、深度、生命周期的“metadata”。

## kmhv2 存储复用及门禁路由

目标 `ITTAGE.scala:244–274` 的entry为valid/tag/counter/target-offset/useful，使用 `FoldedSRAMTemplate(singlePort=true)`；`SRAMTemplate.scala:156` 是 `SyncReadMem`。该结构并不含Hint的CAM、Context/Slot分离、Hint历史或训练FIFO。复用模块参数化的SRAM包装不等于已有容量可免费占用。

目标在read/update并发时令tagged prediction无效（`ITTAGE.scala:284–287`）。Hint源模型没有这一丢查询规则；不能为了复用单端口直接移植该行为。目标ITTAGE counter还使用WrBypass（`:341–349`），其功能与Hint“query保存当时confidence、更新FIFO不旁路到该query”不同。若使用banking或复制解除读写冲突，所有副本、更新广播和冲突处理状态必须在area中计入。

按本技能固定policy，area不因“代码是C++”或“需要新接口”而自动FAIL：

- 工艺、`clock.period_ns`、`clock.frequency_mhz`、profile和sram/register/cam/logic预算未给出，所依赖结论为NEEDS_INPUT。
- 未闭合的全部历史/流水/FTQ/FIFO/恢复布局，或未解析的副本、宏padding和控制位，使完整资源总量为NEEDS_INPUT。104752位不得冒充总量。
- generic memory端口模型不在已选profile集合中，路由area FAIL；合法需求超过声明物理端口或profile容量，路由timing与area FAIL。内置只接受1RW或1R1W。
- generic `register` 数组和SRAM同受读延迟与端口policy约束。改名为register不能把零周期读取变成PASS；零周期generic read和同周期dependent second lookup路由timing FAIL，不能无证据扩大为area FAIL。
- 已确定不兼容的collision路由function FAIL；没有budget或尚未选择mapping与已经证明超容量是两类不同问题。

完整area判定所需最小补齐内容是：选定state编码与共享/复制策略；为每一块声明kind、depth、banks、replicas、完整字段、控制overhead、端口、访问需求和collision；为FTQ/流水/FIFO给出统一生命周期与容量账本；提交工艺/时钟/profile及四类资源预算。本文不决定overall。

固定边界：`frequency_closure: UNPROVEN`、`area_closure: UNPROVEN`、`signoff_status: NOT_PERFORMED`。
