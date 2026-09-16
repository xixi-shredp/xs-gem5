# 独立证据身份审计

审计角色：`evidence_audit`。本次仅阅读源码、配置、已有实验工件与 ICT186，并写入本审计记录及原始 manifest 快照；未修改模拟器、RTL、配置、测试，未构建或仿真。审计日期：2026-09-15。

## 源码身份

对 `source-manifest.json` 的全部 107 个文件重新读取原始字节、检查 canonical 相对路径、symlink component、regular file、目录包含关系、文件长度和 SHA-256。按 `rtl-sim-review/source-manifest/v1` 的域分隔、UTF-8 字节排序、U64BE 路径长度和原始 32 字节文件摘要独立重算，得到 `sha256:2cfd621a9329aece46e072654414fb14260e15beaa1b523d894be50dde009e25`，与冻结 manifest 一致。

当前 96 个模拟器、配置、测试、工具与设计文档文件均与正式 v2 build-2 的 manifest 逐文件一致。另 11 个文件为本次取回的目标 RTL 源码快照，不属于 gem5 旧 manifest。

### 已纠正的历史 manifest 歧义

顶层 `results/h2p-hint-ideal-heavy-v2/source-manifest.json` 是旧 build-1 版本，SHA-256 为 `bf725e922875d8b75a25460e3f8820d566d96b12edb4033c867e57e02082d96a`。正式运行 `identity.json` 中记录的 SHA-256 则为 `5be39be44760477fc47b608df5df76c4dc27669e0edac720b823224cc1a819de`。后者的精确副本存在于 `formal/source-manifest.json`、`remote/source-manifest.json` 和 `test-promotion-2/source-manifest.json`。

独立 SSH 读取确认 ICT186 `/home/xuejing/sim/workbench/xs-gem5-hint-ideal-heavy-v2/results/h2p-hint-ideal-heavy-v2/source-manifest.json` 的当前原始文件长度为 2267314 字节，SHA-256 正是 `5be39be44760477fc47b608df5df76c4dc27669e0edac720b823224cc1a819de`。已将本地同字节副本保存到 `evidence/config/formal-source-manifest.json`。

旧、新 manifest 仅 `src/cpu/pred/btb/test/hint_indirect_test.cc` 一项不同：旧文件 54704 字节、摘要 `34642974b040b19cce42e28d224ceee667ccc3bcbdb44c5c48b5516bc15b583a`；正式 build-2 和当前工作树均为 67656 字节、摘要 `46cfaa12db6aafb9cc1a8797613818146b073cb4c22edd55c9d6dad2ecd04f88`。因此当前测试并未偏离正式 build-2；应避免将旧顶层 manifest 的差异误报成当前测试漂移。既有测试记录仍只提供模拟器行为证据，不能证明 RTL 等价。

## 有效配置和五组输入

`pickle_pure_python_test/18/config.ini` 是完整 483 节的有效配置快照，原始摘要为 `53c6d6646dcd5d677ea3c683fc2ac37c05458a19b509dca439bd470e4a6ddc6c`。其余四份配置相较锚点仅 `system.gcpt_file`、`system.cpu.branchPred.hintCandidates` 和 `system.cpu.branchPred.hintMetadata` 的工作负载路径不同。全部配置均为 replay、idealHintIBP false，CAM 16、Context 4096、2 ways、Slot 32、FIFO 8、commitWidth 8。

`evidence/config/provenance.json` 的 20 份 config、identity、metadata、candidates 快照逐一与所列本地原始文件比较，全部原始字节相等，声明的 SHA-256 全部相符。五个 `identity.json` 的 candidate 和 metadata 摘要与快照一致；五个 metadata 的 `source.candidate_sha256` 也与 candidates 一致。正式 run 的有序启动命令保存在相应 `formal/runs/finite/.../status.json` 的 command 数组。

以下占用仅用于输入身份检查，不能用于缩减全容量 RTL 的面积核算：

| 输入 | candidate PCs | 有效 CAM 项 | 有效 Context 项 | Slot 项 |
| --- | ---: | ---: | ---: | ---: |
| deltablue_test/44 | 20 | 1 | 2795 | 32 |
| pickle_pure_python_test/18 | 20 | 1 | 1948 | 32 |
| pyflate_test/44 | 6 | 2 | 774 | 32 |
| regex_v8_test/30 | 10 | 6 | 129 | 32 |
| scimark_fft_test/26 | 6 | 4 | 1469 | 32 |

静态检查确认五份 metadata 的 format/version、hash version、GHR 970、history 编码、mask 次序和配置容量一致；CAM PC 唯一、mask 范围正确、Slot 无重复且不超容量；Context 的 set/way/tag/slot/conf 均在范围内，物理位置和每 set 有效 tag 唯一，所有有效项初始 confidence 为 3 且引用已加载 Slot。此检查用于证据一致性，不是重新运行生产 loader。

metadata 的来源明确标注 `same-slice offline specialization`，其旧采集器与 profile 摘要是离线镜像的来源标签，不是本次 ASIC profile。当前 review 对所有合法有限镜像审查，不能以这五个稀疏实例代替通用实现证明。

## 目标 RTL 来源

独立 SSH 核验 ICT186 `/home/xuejing/rtl/XiangShan-kmhv2` 的 HEAD 为 `4b9ddb8a6311bff6f5e29cb3722d1f4236d66292`，工作树 clean。对下面 11 个当前远端文件重新计算 SHA-256，全部与 `evidence/rtl/provenance.json` 以及本地快照相符；本地文件长度也与 provenance 一致。

| 快照 | 逻辑行数 |
| --- | ---: |
| ITTAGE.scala | 825 |
| BPU.scala | 1217 |
| FrontendBundle.scala | 837 |
| NewFtq.scala | 1813 |
| Parameters.scala | 914 |
| Configs.scala | 523 |
| Composer.scala | 93 |
| WrBypass.scala | 163 |
| Tage.scala | 1058 |
| Bundle.scala | 791 |
| SRAMTemplate.scala | 384 |

`target_identity.txt` 与 provenance 的 HEAD 一致。provenance 还保存四个 generated RTL/build 文件的远端摘要，但这些文件未被复制到本地 `evidence/rtl/`，因此其记录只能作为已有远端检查的 provenance，不能冒充本地可解析的逐行 evidence。

## ASIC 策略身份

`evidence/policy/asic-heuristic-profiles.yaml` 与所用技能 registry 原始字节一致，SHA-256 为 `cd770e7b1dbbdb84af6c85fe4a8ac513a66e422175fd94d3a6749146f00b965c`。用户未给出工艺、时钟、资源预算，尚不能选出 applicable profile；保存 registry 不等于已选择 profile，也不能宣称 timing 或 area closure。

## 最终工件审计

最终 `result.yaml` 与 `review.md` 尚未生成。待主审形成最终工件后核验 evidence 哈希与行范围、manifest/config 身份、Markdown mirror 和 validator diagnostics。本节完成前不把本记录当作最终工件已验证的声明。
