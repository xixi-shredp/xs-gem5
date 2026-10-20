# Fork Session：xs-gem5 Hint Context 128b / LHR 16b

仅修改此独立 worktree；保留父项目现有模拟器、RTL 及性能结果。分支 `fork/hint-context-20261010`，当前配置快照提交 `1da69c10ee`。

## 已确认规格

- ITH、Call、Ret：各 128×2b 物理环，预测读取最近 128b；恢复 head 8b，Call/Ret 另保存低 12b。
- 用户澄清 LHR：16 项，各 **32b** 物理环，预测读取最近 **16b**；每项 head 5b，sel 4b。总物理 LHR 512b；16 项 head 加正确 target/sel 的恢复字段为 **86b**。
- LHR 物理单元索引低 4b，5b head 为模 32 写入 phase。保留现有读取 LHR 的 PC 四次未决限制及不读取 LHR 的 mask 条件绕过。
- hash 编码由实现方确定：128b 按 bit 位置 mod 28 XOR fold，LHR 16b 零扩展；ITH/LHR/Call/Ret 分别 rotate28 2/3/4/5 后与 PC fold XOR。
- Call/Ret 使用各自事件指令 PC；先移位 2b，再将 PC[47:1] 的 14b XOR fold 异或到低 14b。
- 全局环扩展读取窗口后，年轻写入保护上限由 96 改为 64；不能只改 hash 而保留旧恢复界限。

## 阶段一：隔离与合同

- [x] 建立 worktree，将当前未提交 BackendV2/No-RVV/No-LLC 修改快照到独立分支；现有工作区状态不变。
- [x] 并行 A：只读审查环形历史、恢复、准入与 hash 合同。
- [x] 并行 B：只读审查 profiling、构表、装表镜像及验证入口。
- [x] A 按用户最新 LHR 32b 澄清完成架构文档与 arch.json。
- [x] B 按最新澄清完成格式与验证文档。
- [x] 用户继续授权；本次按独立 xs-gem5 探索范围实施，不改 RTL，不宣称已完成对应 RTL 对比。
- [x] 主代理复核架构/格式合同后进入实现。新 finite metadata/profile/hash version 5，镜像 magic `H2PHCF05`，编码标识 `xs-gem5-fork-ith128-path128-lhr16-v1`；拒绝旧 v3，物理表 body 布局保持。

## 阶段二：实现

- [x] 并行 A：128b 全局读取、32b LHR 物理环/16b 读取、head 位宽、恢复及边界。
- [x] 并行 B：新 Context 格式、Python/C++ hash、profiling/离线构表/loader；严格拒绝旧编码，工具不依赖父项目旧合同。16 项离线测试通过，1200 条独立逐 bit oracle hash 向量已生成。
- [x] 主代理集成复核；父项目源码及结果不变。21 项来源文件身份、测试日志终态及父 xs-gem5 tracked diff 未变检查通过；独立静态审查放行最终构建，不替代生产测试。

## 阶段三：构建与验证

- [x] 完整编译最终 gem5.opt；进程退出后验证可执行权限，不运行编译中间产物。首轮完成后发现配置默认 DRAM ini 路径层级问题，复制同 SHA ini 到本 worktree 并修复；第二轮完整 SCons 和 fresh link 返回 0。
- [x] 固定跨语言 hash 向量、物理环 wrap、LHR 连续写入/恢复、Call/Ret 路径恢复、64/65 写入边界、错误路径与非 H2P redirect。生产 helper 与 1200 条向量通过。
- [x] 通过最终 binary 验证新格式装表和旧表拒绝：52/52 场景通过（3 合法、49 非法）。本轮不运行 checkpoint；未把 Host SimObject 测试称为 NEMU difftest。
- [x] 独立有限结构/恢复协议代码复核和终态身份审核通过：10600 项源码/依赖身份、122 项审查；未做对应 RTL、综合、STA 或面积对比。
- [x] 主代理签核，报告变化与验证限制。报告及审计位于父项目 results/hint-context-fork/20261010/；六片性能未重跑，不宣称新 IPC 收益。

## 阶段四：用户授权六片性能复测

采用已有会话合同：FFT/73、pickle/39、pickle/23、pyflate/44、deltablue/44、regex/49；1M warmup + 1M measure，现有独立 NEMU difftest；全局最多 16 个物理核单核 guest（CPU 0–15）。每片最多 12 张不同候选表，按实际 IPC 选优，明确为同切片专项调优；同时保留本轮 fresh 默认纠错减伤害构表作为对照。

新结果：父项目 `results/hint-context-fork/20261010-performance/`。不复用旧 v3 的 profile/image，不修改预测器源码或 RTL。

### 4.1 输入、工具与预检

- [x] 并行 A：冻结新 binary/config/source/DDR/NEMU ABI、六片输入与权重，核对原始完整构建和 post-commit 记录。
- [x] 并行 B：独立复制并适配 16 核候选运行工具到 v5，保持候选预算/去重/组合/Fraction IPC 规则；真实绑定新 raw build/source 身份。
- [x] 并行 C：独立审查适配入口、离线测试、候选预算、阶段/装表/源身份，拒绝旧编码数据。
- [x] 主代理复核并签核启动入口；不运行尚未通过预检的 guest。

### 4.2 六片真实全流程

- [x] 六片逐片 Off → fresh profiling → 离线构表 → 实际 load/Hint On，按完成事件推进并行任务。
- [x] 全部候选实际实测；每片不超过 12 张不同表；失效/失败结果不参与最优选择。
- [x] 全程检查 difftest、restore/warmup/reset stats/measure、实际指令数与源码/镜像身份。
- [x] 主代理复核终态运行及预算，保存默认表和 IPC 最优表。

### 4.3 统计、对比与报告

- [x] A 独立原始身份/正确性审计；B 独立实际 I/C、间接分支准确率/miss、Hint 内部计数与候选排名复算。
- [x] Benchmark 内按 SimPoint 权重聚合 I/C 和 branch/miss，pickle 两点合并，五 benchmark 等权几何平均收益。
- [x] 比较本轮 Off/默认 On/最优 On 与上轮旧 Context 默认/最优结果；核对 Off 一致性，分开 Context 改变与选表影响。
- [x] 输出中文 rpt.md、CSV/JSON、实际最优表/镜像及审计。按可用 stats 比较前后端、Bad Speculation 与访存指标；缺失项明确列出，不捏造 Top-Down 数据。
- [x] 主代理签核，列出性能回退和同切片专项调优限制。


# 2026-10-19：按只读 spec.md 对齐，并重跑 UART16550 十片 1M/1M

本轮仅独立 worktree 模拟器；CSR/硬件 profiling 不实施，继续仿真 collect/load。保持既有内存模型；不改 RTL、父模拟器或 spec.md。新结果位于 `results/model/smoke-test/20261019-spec-aligned-uart16550-1m1m/`。旧基线/二进制已冻结。用户确认 snapshot conf 饱和 ±1，仅实际 Hint 使用时训练、无 FIFO 前递；Hint II=3。

## 阶段一：规格与旧基线

- [x] 冻结旧 binary、已通过的真实 baseline 证据、Spec SHA 和新实验合同。
- [x] 三个并行只读设计：历史/hash、预测/恢复时序、离线工具与 startPC 统计。
- [x] 用户明确训练/II边界和仅模拟器范围；主代理冻结接口。

## 阶段二：并行实现与集成

- [x] A：global7b head；16×16b LHR/read8b/4b phase；新 mask/hash；恢复及生产 helper。
- [x] B：startPC CAM/hash、Hint busy/取消、资源满非阻塞且跳过 local 写、单口与快照 conf/撤销、Discovery。
- [x] C：v6 元数据/镜像、新 Python hash/top16startPC 构表、审计、跨语言向量与边界测试。
- [x] 独立复核发现并修正：运行恢复不再依赖64份FTQ局部head快照，使用16项Hint journal逆序回退；完整快照只作观察断言。
- [x] 独立复核发现并修正：错误路径置信度撤销与正常更新共用单端口仲裁，保留有限journal直到撤销完成；仅Hint回退。
- [x] 主代理集成复核：有限结构、更新/恢复协议、旧格式拒绝与接口一致。

## 阶段三：完整构建与功能门槛

- [x] 完整 SCons 最终 gem5.opt 构建并等待退出；冻结 source/config/binary/DRAM/ref 身份。
- [x] 最终 binary 生产 helper：hash/镜像、满队列与四项fallback、busy/S3、非H2P redirect、置信度与错误路径恢复。
- [x] 主代理复核正确性与身份门槛，失败返工。

## 阶段四：十片真实实验

- [x] 新工具与预检，10 UART checkpoint/weights/1M+1M/CBO NEMU/16cores 冻结；融合边界严格审计。
- [x] 每片 Off→fresh profiling→新默认表→实际 load/On，至多12唯一候选并行实测 IPC 选优。
- [x] 原始 difftest、窗口、实际装表身份与目标预测统计独立复核；失败排除并修复重跑。

## 阶段五：统计与报告

- [x] 独立正确性/身份审计与 I/C/miss/准确率/候选排名复算。
- [x] 分片、benchmark SimPoint加权、跨benchmark等权几何平均；明确专项调优、回退和剩余模型抽象。
- [x] 中文 rpt.md、CSV/JSON、最优镜像与审计，主代理签核。


# 2026-10-19：LHR 32b / 最近8目标 / 同PC八项在途复测

仅当前独立worktree，Spec只读。16个LHR各32b，读取16b，phase5b；最多8次受保护在途写，第9次Hint回退并跳过LHR写；所有mask统一限制。沿用精确startPC、无旋转hash、II3、16meta、32单口conf FIFO、snapshot±1、串行撤销及有限journal恢复。不混入no-LHR bypass或DRAM修复。Spec算术标注70b/85b按字段实际分别86b/101b记录，不改文档。新结果：results/model/smoke-test/20261019-spec-lhr32-inflight8-uart16550-1m1m/。

## 阶段一：合同与旧基线

- [x] 核对新Spec、保持仅模拟器范围、继承十片UART1M/1M+独立CBO NEMU+16核/最多12表IPC选优合同。
- [x] 保存v6实际binary、修改源码副本/补丁与已通过的98运行基线证据；冻结新合同。

## 阶段二：并行实现与复核

- [x] A：历史16cell/32b、读取16b、5bphase、八次保护及恢复边界。
- [x] B：所有mask八项准入、strictv7 load、production helper第8/9次及单口撤销/恢复回归。
- [x] C：v7 Python/history/profile/image审计、旧版本拒绝及跨语言全位向量。
- [x] 主代理独立集成复核与交叉接口审查，确认仅本轮规格改变。

## 阶段三：完整构建与功能验证

- [x] 完整scons -j$(nproc) build/RISCV/gem5.opt退出后验证ELF/执行权限并冻结source/binary。
- [x] 最终binary helper、hash/镜像/恢复/8与9项/fullmeta/rollback功能验证。
- [x] 新运行工具与全部输入/参考库身份预检，主代理放行。

## 阶段四：十片正式实验

- [x] 全新Off→Discovery/Detail→构表→实际load/On，每片至多12唯一表；全局16核按完成事件推进。
- [x] 独立核对全部difftest/1M1M融合边界/装表身份/原始目标miss及实际IPC排名。

## 阶段五：复算与报告

- [x] 原decimal SimPoint加权I/C/accuracy和6benchmark等权几何平均，分片Off/default/best与miss变化。
- [x] 对比v6基线与收益、LHR/busy/meta/conf回退，区分读窗口与在途上限同时改变的因果限制。
- [x] 中文rpt.md/CSV/JSON、选中表/镜像和root签核，完成plan。
