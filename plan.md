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
