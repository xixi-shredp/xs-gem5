# Fork Hint Context 架构合同

本文件描述独立 `fork/hint-context-20261010` worktree 的实现合同，不代表构建或验证已通过。用户已澄清 LHR 为每项 32b、预测读取其中最近 16b；本轮仅实施独立 xs-gem5 探索，不修改 RTL，也不开展六片正式实验。

## 1. 物理存储、读窗口与更新

| Context | 物理存储 | 预测读取 | 推测更新 | 入口恢复信息 |
|---|---|---|---|---|
| ITH | 128 个 2b cell，256b | 最近 64 个 cell，128b | 左移 2b，加入预测 target[2:1] | 8b phase；正确 target[2:1] 由重放事件提供 |
| Call | 128 个 2b cell，256b | 最近 64 个 cell，128b | 左移 2b，低 14b XOR fold14(call 指令 PC) | 8b phase 和入口低 12b |
| Ret | 128 个 2b cell，256b | 最近 64 个 cell，128b | 左移 2b，低 14b XOR fold14(return 指令 PC) | 8b phase 和入口低 12b |
| LHR | 16 项，每项 16 个 2b cell；总计 512b | 选中 PC 最近 8 个 cell，16b | 左移 2b，加入预测 target[2:1] | 全部 16 个 5b phase，正确 target[2:1] 和 4b sel |

所有读窗口以最新事件置于最低 2b，age 越大位位置越高。ITH 与 LHR 仅在 taken、non-return indirect 上更新；Call 与 Ret 按对应分支属性更新。具有 call 和 return 双属性的分支同时更新两个路径环，但不更新 ITH/LHR。

Call/Ret 采用相应事件的指令起始 PC，不额外记录匹配 call 的 PC。令 `P=(pc>>1)&((1<<47)-1)`，`fold14(P)` 为按每 14b 分组的 XOR；逻辑更新为 `H'=(H<<2)`，然后 `H'[13:0] ^= fold14(P)`。环实现先追加 0，再 XOR 最新 7 个 cell。

global phase 是模 256 的 8b 逻辑值，physical index 为 `phase & 127`。LHR phase 是模 32 的 5b 逻辑值，physical index 为 `phase & 15`。初始 phase 分别为 255 和 31，初始数据全 0，下一次写入 index 0。phase 的高位用于编码绕回，不增加存储容量，也不能单独证明在途写入数量。

## 2. Hash 编码

mask 仅 4b，bit 0/1/2/3 分别对应 ITH/LHR/Call/Ret，无 GHR。

定义 `fold28(x)` 将整数第 i 位 XOR 到结果第 `(i mod 28)` 位，`rol28(x,n)` 为 28b 环形左移。对于 128b 读窗口 `(lo64,hi64)`：

```text
fold128 = fold28(lo64) XOR rol28(fold28(hi64), 8)
context = [fold128(ITH), LHR_low16, fold128(Call), fold128(Ret)]
key = fold28((pc >> 1) & ((1 << 47) - 1))
for i in 0..3:
    if mask[i]: key ^= rol28(context[i], i + 2)
key &= (1 << 28) - 1
```

128b 折叠中的旋转 8 来自 `64 mod 28`，不将两个 64b 块未经旋转直接 XOR。LHR_low16 不预折叠或预旋转。PC 是真实指令起始 PC；保持现有 28b key 的表索引/tag 划分和有限表容量。

架构合同使用 `hint-context-fork-v1` 标识；metadata/hash 容器版本为 5，编码标识为 `xs-gem5-fork-ith128-path128-lhr16-v1`，其 hash/读窗口与既有 metadata v3 不兼容。新 profiling、离线构表与实际 load 必须声明新合同并拒绝误用旧 profile/image，不可仅改日志字段后继续接受旧 v3。

## 3. 恢复位与 observer 快照

LHR 恢复字段计数为 `16×5 + 2 + 4 = 86b`：80b 全部 phase，2b 正确 target，4b PC/CAM selector。taken、事件类型、FTQ 身份和一般队列控制字段由外围协议提供，不计入该特定 payload。Call/Ret 每项 20b；ITH 是 8b phase 加 2b 正确 target。正确 target 不应被误作被覆盖值 undo。

预测入口可在 C++ observer 中保存三个 128b 读窗口、16 个 16b 局部读窗口以及写计数，以固定查询时机、输出 profiling、验证恢复。这些 observer/read snapshot 字段不是每 FTQ 项必须实际存储的恢复 payload。collect 的整环副本亦只能用于离线观察，不得宣称 replay 通过整环副本实现有限硬件恢复。

物理恢复先把各环 phase 恢复到入口，再恢复 Call/Ret 入口低 12b；重放严格保留的实际分支前缀，最后应用需要保留的重定向分支和正确 target。任意非 H2P 分支的 redirect 也必须恢复全部局部 phase，不能仅恢复 squash PC 对应项。第二次 redirect 必须仍可恢复到同一入口，已保留前缀不可重复累加。

## 4. 容量保护与准入

global 环 128 cell、读窗口 64 cell，具有 64 次年轻写入保护带。距最老需要恢复的入口已有 64 次写入时，不得再执行第 65 次相应环写入。Call/Ret 的 7-cell XOR 会修改 6 个旧 cell，其入口低 12b 恢复字段负责修复这些修改。原实现 96 次窗口对应 64b 读窗口，不能沿用到本合同。

单 LHR 16 cell、读窗口 8 cell，物理保护带能够容纳最多 8 次年轻写入。但本轮 **继续保留读取 LHR 的 PC 至多 4 次未决更新**，不默认为 8 次，不改变现有准入行为。需要保护的是距最老仍可恢复入口的写入，不是未经 FTQ/commit 协议证明的任意计数。

继续保留现有规则：固定 replay CAM 项的 mask 不读取 LHR（`mask & 2 == 0`）时，绕过该 PC 的四次限制，仍遵守 16 项元数据队列容量以及 global 恢复窗口限制。此规则的安全依据是该局部环从不参与当前装表版本的预测，而不是 pointer 能恢复任意次数的旧数据。

免读条件必须覆盖实际 lookup/hash、shadow/conf=0 查询、profiling 语义和运行期间 mask 不可改变。masked-off 的局部数据即使被 overwrite，不得在未来查询中被读出；在线换表或启用 LHR 前必须清空在途项并重置/重新初始化历史。observer 一致性验证应区分 enabled LHR 与 permanently masked-off LHR，不宣称后者能够任意多次写入后逐 bit 精确恢复。

## 5. 实施与验收关注点

- 同步 C++/离线 Python 的低 128b 折叠和低 16b LHR 编码，固定跨语言向量，包括 bit 64、bit 127、LHR bit 15 和全部 mask。
- 检验 global 64 次安全、第 65 次受保护；Call/Ret 连续 XOR 后恢复低 12b。
- 检验 LHR 4 次读参与更新限制、物理第 8 次边界、phase 31→0 绕回，以及不读 LHR 的 mask 绕过行为。
- 检验非 H2P redirect、同 FTQ 实际前缀、重定向分支重放以及连续 redirect。
- metadata/profile/image 均携带新合同，旧 v3 被拒绝；最终运行必须重新 profiling、构表、实际 load。
- 本轮对最终完整 gem5.opt 运行生产 helper 定向验证和格式验收；不修改 RTL，不开展六片正式实验。
