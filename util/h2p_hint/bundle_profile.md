# 独立 Bundle profiling 与真实 Trace 导出

`configs/example/bundle_profile.py` 使用普通 BackendV2 机器和真实 guest CSR 启动。它以 guest 配置的退休预算为终点，并等待 Started/Busy 清零、最后一笔 Trace 写响应完成后，从已完成的 NC 物理内存导出 `bundle-trace.bin`。不将性能窗口的 2M 指令停止当作 profiling 完成；CPU 的 Finished 窗口不能继续退休，因此本入口从初始化起采用独立驱动，不改变原性能入口或添加硬件 phase gate。

沿用真实 checkpoint 配置参数，将入口替换为 `configs/example/bundle_profile.py`，额外传入 `--bundle-profile-kind=discovery` 或 `detail`。需要 `--h2p-hint-bundle`、对应保留区的 `--h2p-hint-nc-range`、新内嵌 restorer/容器及 NEMU difftest。普通入口要求的 `--maxinsts` 仍用于配置参数检查，但本入口明确取消该性能停止条件，在 `bundle-profile.json` 记录原参数和 `performance_instruction_limit_applied=false`。guest restorer 的默认预算为独立3M，包含启动后的恢复指令，不承诺严格覆盖目标2M区域。

`--bundle-profile-tail-ticks` 是整个独立驱动的模拟时间上限，默认100,000,000,000 ticks；`--bundle-profile-quantum-ticks` 是状态采样间隔，默认100,000,000 ticks。绝对 tick/maxtime 限制仍生效。正常 guest 提前退出且尚未完成 profiling、超时、无完成记录、无效状态或致命 P_Error/L_Error 都使实验失败；不把未写完 Trace 导出成成功结果。

导出时 CPU 只读接口拒绝 Started/Busy 或 DMA 在途状态，仅允许读取实际完成的22B记录范围，每块最多1MiB。它读取现有物理内存，不做缓存写回、不伪造总线响应、不装载 Hint 表。导出的真实字节随后全部经过严格 Trace 解码；必须有有效 final，或满足完整容量终止的 P_Error=010 条件。输出记录文件/hash、完成记录数、错误码和采样时间，不报告性能 IPC。

验证证据：`candidate/profile-export-independent-c9ce64bf7f89/validation.json`。ps1 新候选真实guest启动1M Discovery，最终3个写beat完成，读取的22B内存与 final 编码逐字节相同，NEMU difftest启用。该定点只有final，没有普通/Boundary样本；真实3M workload、新NC checkpoint和64MiB容量仍需后续实验。正式性能比较继续使用原入口，严格1M warmup+1M measure。
