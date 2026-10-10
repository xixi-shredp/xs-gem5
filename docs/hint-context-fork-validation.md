# Hint Context fork：格式合同与验证计划

本文仅规定本次独立 xs-gem5 worktree 的实现目标与验收方法。预测器与离线格式实现已开始，尚未编译新 binary 或运行 guest；本文不代表已通过 RTL 对比。

## 1. 已确认的历史存储

| Context | 物理存储 | 预测读取 | head / phase | 更新 |
|---|---|---|---|---|
| ITH | 128 个 2b cell，256b | 最新 64 个 cell，128b | 恢复字段保留 8b；物理索引为低 7b | 追加预测 target[2:1] |
| Call | 128 个 2b cell，256b | 最新 64 个 cell，128b | 恢复字段保留 8b；物理索引为低 7b | 移位 2b 后，低 14b XOR call PC 的 fold14 |
| Ret | 128 个 2b cell，256b | 最新 64 个 cell，128b | 恢复字段保留 8b；物理索引为低 7b | 移位 2b 后，低 14b XOR return 指令 PC 的 fold14 |
| LHR | **16 项，每项 16 个 2b cell，即 32b；合计 512b** | 按 PC 选择的一项最新 8 个 cell，16b | **5b phase，模 32；物理 cell 索引为 phase & 15** | 追加预测 target[2:1] |

LHR sel 为 4b，与 CAM 的 16 项对应。不得将“预测读取 16b”解释成“物理存储 16b”。此次不引入 LHR undo 或完整历史快照。每个 PC 的未提交 LHR 更新继续限制为 4 次；保留当前不读取 LHR 的 mask 绕过规则，不扩展其适用范围。

如果恢复 meta 保存全部 16 个 LHR phase，加上正确 target[2:1] 和 sel，其位宽为 `16×5+2+4=86b`。这是字段算术，不代表每项队列必然额外独立实例化全部这些字段；最终报告必须以实际实现为准。

## 2. Hash 编码

表 key 仍为 28b，mask 顺序保持 ITH/LHR/Call/Ret。Context 表仍是 4096 项、2 路，即 2048 sets：set 为 key[10:0]，tag 为 key[27:11]。CAM、Slot、confidence 与队列容量保持当前配置。

编码标识固定为 `xs-gem5-fork-ith128-path128-lhr16-v1`，以独立命名空间区分现有 gem5 v3 与 RTL 长度镜像版本。编码定义：

1. 历史最新 cell 位于低 2b，按年龄依次放入更高位。
2. `F128(x) = XOR((x >> (28*j)) & ((1<<28)-1)), j=0..4`，输入严格为 128b。
3. PC 编码继续为规范 48b 地址右移 1b 后的 47b word；按 28b 分块 XOR 得到 PC fold。
4. 四个分量为 `F128(ITH), zero_extend_28(LHR16), F128(Call), F128(Ret)`。
5. key 为 PC fold 与被 mask 选中分量的 28b rotate-left 的 XOR；四个旋转量分别为 2、3、4、5。
6. Call/Ret 路径更新均为 `(history << 2) XOR fold14(event_pc)`；fold14 对上述 47b PC word 按 14b 分块 XOR，更新截断为物理 256b。Ret 使用当前 return 指令自身 PC，不能未经规格变更而改用上一条 Call PC。

不能使用旧 `fold28(uint64_t)` 截断 128b 输入。实现可以使用两个 uint64_t 或固定宽度容器，但 Python 与 C++ 必须使用同一位序和全部 128b。

## 3. 新格式隔离

metadata、candidate、profile、replay、stats 和 image 必须声明新编码。已冻结 finite 容器 version=5、hash_version=5、上述 context_encoding 和镜像 magic `H2PHCF05`。必须同时满足：

- metadata / profile loader 不接受现有 finite v3 的 history 和 hash 身份。
- 新 profile 的已折叠 contexts 不得作为旧 v3 profile 读取；旧 profile 也不能直接重新标记后构表。
- packed image 使用独立 8 字节 magic 和新 header 身份，不与现有 `H2PHINT3` 或 RTL 长度镜像混用。
- 保持表 body 位宽并不意味着格式兼容；改变历史输入本身已经改变 key。
- 身份审计记录编码标识、合同 SHA、源码 / binary SHA、新 profile SHA、metadata SHA、image SHA 和实际装表证据。
- Python 合同实现放在本 worktree 的 `util/h2p_hint` 包内，并用相对导入。不得继续依赖 `parents[3]/manifest/hint_v3.py`：此 worktree 层级下该表达式指向 `worktrees/manifest`。
- 不修改新项目现有 v3 合同，不读取或写入原项目目录，不将本 fork 的表用于 RTL。

## 4. 恢复与容量检查

全局环有效预测窗口从 32 个 cell 扩大到 64 个 cell，原来的 96 次年轻写入保护上限必须调整为 **64 次**。该上限按每种环分别累计；应阻止第 65 次破坏最旧有效窗口的写入。Call/Ret 的低 12b 恢复保留，因为路径 XOR 会修改新 cell 之前的 6 个 cell。

LHR 物理 16 个 cell、预测读取 8 个 cell，具有 8 个 cell 的保护带。现有最多 4 次未提交写入保持，因此可以依靠恢复 phase 找回原窗口，不需要 undo。恢复时 phase 低 4b 对应物理 head；额外高位记录模 32 的 phase，不能继续使用旧的 `phase & 7`。

## 5. 分阶段验证

### A. 静态合同与离线定向检查

- [ ] C++ / Python hash 固定向量：零历史、低位、bit63/64/127、全 1、四种单分量 mask、全部 16 个 mask、规范高地址 PC。
- [ ] 更新向量：target[2:1] 四值、Call/Ret fold14、高地址、低 14b XOR 跨 cell。
- [ ] 镜像 round-trip、有效位 / conf / 边界行 / 两路 / slot31，固定长度和 SHA。
- [ ] 新 loader 拒绝旧 v3 metadata、profile、candidate 和 image；拒绝仅修改版本但 history / encoding 不匹配的数据。
- [ ] worktree 自包含导入，不依赖外部 manifest 路径。

### B. 生产实现恢复与输入验证

- [ ] 在完成最终 gem5 binary 编译后，使用生产 loader 执行合法 / 非法输入测试。
- [ ] ITH / Call / Ret 的 64 次年轻写入和第 65 次容量边界；分别覆盖物理环回绕、非 H2P redirect 与错误 target 修正。
- [ ] LHR 的 16-cell / 32-phase 回绕，读取全部 16b，连续同 PC 四次未决更新及第五次限制。
- [ ] 不读取 LHR 的 mask 保留现有绕过语义，且恢复不会误用被覆盖的数据。
- [ ] S3 override、Context miss、conf=0 回退、resolve confidence 更新及错误路径撤销、metadata 队列满。
- [ ] collect observer 与 Hint Off 的执行 / 性能统计一致性，避免 profiling 本身改变行为。

### C. 工作负载验证（范围另行确认）

- [ ] 输入 / 参数 / difftest / source / binary 身份审计通过后，重新 Off → profiling → 构表 → 实际装表 On。
- [ ] 所有新表来源于本 fork 的新 profile，旧结果仅作为明确标注的对照。
- [ ] 报告实际指令数、周期、IPC、全局提交非 return 间接分支 target miss 与准确率，以及 Hint 内部纠错 / 伤害 / 反压。
- [ ] 分开表述算法效果、同切片调优效果及模型差异；不宣称已经获得 RTL 验证或面积数据。

本文件不授权扩大 workload 验证范围或改动 RTL。当前阶段仅授权完整 gem5.opt 编译和最终 binary 的定向 helper / loader 验证；未授权执行六片工作负载或改动 RTL。
