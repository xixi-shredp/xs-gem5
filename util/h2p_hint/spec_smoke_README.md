# SPEC smoke 编排

`spec_smoke.py` 串接独立文件接口：Discovery 原始 trace → 冻结 CAM →
Detail 原始 trace → 离线镜像 → Hint On。离线工具只读取 SPEC 文件，
不读取模拟器 observer、JSON history 或进程内状态。

本轮只接受 manifest 中 7 个 benchmark、10 个切片，1M warmup + 1M measure，
必须开启 NEMU difftest。脚本拒绝 full 的 100M + 200M 配置，不修改 manifest。

**所有编译、测试和实验只能在 ps1/ps2 远程服务器运行，包括 Python 检查。**
runner 检查实际 hostname，接受 `ps1`、`ps2` 或团队通过 SSH 核实的
`iv-yexl3v2u4gwh2ysmqmn1`（ps1）、`iv-yexl3v2u4gwh2ysms14a`（ps2），
以及相应 FQDN；不提供跳过主机检查的开关。
审计同时记录 SSH destination 和实际 hostname。本地仅编辑和阅读文件；
`prepare`、离线构表等全部 CLI 阶段也会在非授权主机拒绝。
每次子进程启动前再次核验主机。

本地不运行 scons 或其他 gem5 编译命令。本 runner 没有 build 子命令；
如以后增加，必须在编译进程启动前复用同一 `require_experiment_host()` 检查。
构建由部署端通过 `ssh ps1` 或 `ssh ps2` 发起，在远端项目的 `xs-gem5` 目录执行 scons，
并留存 SSH 命令、实际 hostname、构建日志及最终 ELF SHA256。

将相同版本代码和 manifest 部署到 ps1/ps2，在 SSH 会话内从远端项目根目录
执行下方命令；所有参数均应指向实际执行主机存在的文件和输出路径。
远端项目根目录由脚本自身位置确定，输出仍限定为该项目的
`results/model/smoke-test/` 子目录。已核实远端项目根目录为
`/home/xuejing/workbench/kmhv3-hint-norvv-nol3`，checkpoint 根目录为
`/home/xuejing/workloads/cpython/cpts/no-rvv-uart16550-100m200m/`。
部署后 `--input-root` 应使用后者，不要求 checkpoint 先回传本地。

每阶段 `command.json` 保存 argv、cwd、hostname/FQDN、SSH_CONNECTION 与
SSH_ORIGINAL_COMMAND（若会话提供）；阶段指纹保留执行主机身份。
SSH 启动与结果回传由部署端留存实际命令和日志。将远端 results 回传本地时，
保留原始相对目录、文件内容及远端身份，不改写原始证据中的远端绝对路径。
本地仅审阅回传结果；继续实验应回到原执行主机的运行目录 resume。

SSH 登录授权远端后，从项目根目录执行准备检查（不会启动仿真）：

```bash
PYTHONDONTWRITEBYTECODE=1 python3 xs-gem5/util/h2p_hint/spec_smoke.py \
  --manifest manifest/smoke-test.json \
  --workload-manifest workload/cpython-norvv-uart16550-100m200m-heavy.json \
  --input-root /home/xuejing/workloads/cpython/cpts/no-rvv-uart16550-100m200m/ \
  --ref-so deps/NEMU-gem5/build/riscv64-nemu-interpreter-so \
  --gem5 xs-gem5/build/RISCV/gem5.opt \
  --output-root results/model/smoke-test/spec-smoke-001 \
  --stage prepare --jobs auto
```

在远端编译完全结束且镜像/trace 接入可用后，将 `prepare` 替换为 `all`。
也可依次执行 `baseline`、`discovery`、`select-cam`、`detail`、`build`、
`replay`、`select`。单阶段执行要求其直接和递归前置阶段已通过校验。
输出必须放在 `results/model/smoke-test/` 下的专用子目录，所有日志、
命令、指纹和阶段报告均在该目录内。

`--jobs auto` 每阶段采集 CPU affinity/cgroup quota、系统一分钟负载、
MemAvailable/cgroup 可用内存，以剩余资源计算子进程并发上限；
默认每进程预留 12GiB，并留 2 个 CPU 和 8GiB 余量。
可按实测内存使用 `--memory-per-job-gib`，显式 `--jobs N` 超过当前预算会拒绝。
其他团队 RTL/构建的负载算作已占用，不能直接按机器总核数分配。
每次子进程启动前再次采集资源，预算不足时等待；阶段计划与启动快照写入
`resources/` 和 `resource-admission.json`，不会停止其他团队任务。
阶段按 10 个切片并行；replay 可同时执行同片的最多 3 个去重候选，
所有候选与切片共享同一资源上限，总任务数至多 30。

可重复传 `--slice scimark_fft/51` 等参数分组运行。manifest 始终验证完整
10 片清单，分组只执行选定切片并输出 `complete:false` 的 partial 报告；
不得把分组成功称为全量 smoke PASS。全部切片完成后，不带 `--slice`
执行 `--stage select`，递归核验精确 10 片证据再生成完整 `summary.json`。
单个 runner 当前只调度所在授权主机，不自动跨主机分发；另一主机的分组
须独立记录主机、二进制和输入身份，不能移动原始结果后伪装同主机 resume。

如需导入独立运行的基线，只有其 ELF SHA 与当前 revised 模型 ELF 完全一致时才允许添加：

```text
--import-baseline-root results/model/smoke-test/baseline
```

这只读取原目录，不复制、重写或调整原日志和 stats。要求基线完整通过，
根目录具有 `identity.json`（`hint-baseline-identity/v1`，记录 binary、config、
reference 各自 path/sha256 与 warmup_insts/measure_insts）、
`reference-audit.json`、`run-summary.json`；每片位于 `<bench>_<ID>/`，
提供 `command.json`、`run.log`、`config.ini`、`stats.txt`。
导入验证冻结 ELF、参考库 ABI 审计及实际 difftest 配置、checkpoint SHA、
原始基本命令、相同配置源码 SHA 和严格退休窗口。
本轮已经修订 CFI 识别恢复与 PHR 坐标，最终 A/B 必须使用同一新 ELF
分别重跑 Hint Off/On。此前不同 SHA 的冻结 baseline 仅保留为准备证据，
不能导入作为最终主 A/B；runner 会拒绝这种导入。默认 `baseline` 阶段
直接使用 `--gem5` 指定的新 ELF 运行 Hint Off。
原文件路径与 SHA 写入阶段审计，之后 resume 仍会验证这些原始证据。

模拟器接口为：

```text
baseline: --h2p-hint-mode=off
discovery: --h2p-hint-mode=discover
detail: --h2p-hint-mode=detail --h2p-hint-candidates=<cam.bin>
replay: --h2p-hint-mode=replay --h2p-hint-metadata=<load.bin>
```

各阶段都附加 `--h2p-stat --enable-difftest --difftest-ref-so=...`，
保留 manifest 的基础命令及固定指令窗口。程序直接执行参数数组，不经过 shell。
仿真输出 Discovery=`hint-discovery.bin`、Detail=`hint-detail.bin`；
兼容同目录 `discovery.bin`、`detail.bin`。

runner 为紧凑 raw trace 添加 `trace.json` 描述文件，包含 schema/version/hash、
record_bytes、termination、完整记录容量和 data SHA256；Detail 额外绑定
144B CAM 镜像 SHA256。当前 runner 只接受正常结束且有合法 final 的采集；
不会把缺失 final 的文件自动当成容量终止，不能处理失败采集作为有效输入。
格式解析和所有字段合法性检查交由独立 `spec_trace.py validate`。

构表执行 `rank-masks`，分别输出 `static-best`、`uniform9`、`uniform15`
整镜像；按完整镜像 SHA256 去重，每个保留镜像运行一次 Hint On。
`select` 根据实际测得的 `measure_ipc` 选最佳 Hint 候选，同时报告与 baseline
的比值及是否加速。若所有 Hint 候选均变慢，报告明确显示，不把它们标成收益。
离线静态分数仅用于生成候选，不替代 IPC 测量。

每次仿真验证正常退出、difftest 配置与指定参考库一致，并使用现有
`retirement_window` 交叉检查恢复、warmup、measure 的精确退休计数、
fusion 边界、核心周期和 stats。stats.txt 若含 warmup 与 measure 多块，
只读使用最后一个完整区块，记录原始块数，不重写原始 stats。
IPC 来自严格 measure 指令数/周期。
错误、缺失 phase、指令窗口不符或损坏 trace 均不写成功阶段标记。

预测统计全部来自 measure 区间的公开提交计数：

- `all_indirect`：`returnNum + otherNum` 为总数、`returnMiss + otherMiss`
  为 miss，属于 BPU committed CFI miss 口径；保留 return/nonreturn 分项。
- `nonreturn_ittage_plus_hint`：公开 `h2p-target-summary.json` 的 executions
  与 target_misses，后者必须等于 absent_target + valid_wrong_target。
  缺失目标即使实际 target 等于 fallthrough 也计 miss。此项是 final-target
  口径，不与前项的 CFI miss 定义混为一谈。
- `hint_override`：仅 `hint-stats.json` 中 `global.overrides` 与
  `global.override_misses`，表示实际使用 Hint 覆盖的已提交分支准确率。
- `hint_tag_hit`：`global.tag_hits` 与 `global.tag_hit_misses`，包括 conf=0
  的 shadow target，独立于真正使用 Hint 的统计。

分母为零时准确率为 JSON null。runner 核对非 return summary 的逐 PC 合计，
并与 Hint global executions/final_misses 核对，同时检查 corrected/harmed 恒等式。
不使用含 wrong-path 的 queryStats.overrides，不读取私有 replay JSONL 计算准确率。
公开计数文件纳入阶段 artifact SHA256。
另保留公开 queue 的写入、丢弃、pending、rollback、置信度状态转移与资源回退计数。
`recoveries` 是实际 SRAM oldConf=0 到 newConf>0 的 applied write（可能是 stale 0→3），
不等于 query-conf0 且 baseline miss 的请求数；提交 conf0 hit 可由
`global.tag_hits - global.conf_enabled` 派生。busy/stalls 是 Hint 回退尝试数，
与前端阻塞周期及已提交准确率分母分别报告。

`stage.json` 记录命令产物、checkpoint、二进制、参考库、配置、runner、
离线工具和 manifest 的 SHA256。resume 验证完整产物及递归前置阶段指纹，
不会把失败目录当成成功结果。失败阶段目录需先审阅，使用新输出根目录重跑；
脚本不会自动清理或覆盖证据。已成功阶段可直接 resume。最终
`summary.json` 是每片实际 IPC 选择报告，由主任务汇总到最终结果文件。

本工具不编译模拟器，不搬运远端 workload，不启动 full，也不修改 RTL。
