# H2P 间接分支最终 target 统计

## 用途与开关

`--h2p-stat` 用于识别最终预测未能提供正确 target 的间接分支。该开关默认关闭，当前只支持单核的 `DecoupledBPUWithBTB` 配置；开启在多核或其他分支预测器配置下会直接报错。

默认关闭时不收集相关数据，也不创建输出文件。开启后，gem5 正常退出时在输出目录创建 `h2p-indirect.txt`；统计窗口内没有符合条件的分支时，该文件为空。

## 统计口径

统计以提交为准，只包含 indirect call 和 indirect jump。return 以及最终被 squash 的动态指令不进入统计。

每个纳入统计的动态间接分支都会记录其实际提交 target。以下任一情况使该动态实例的 miss number 增加一：

- 该动态分支没有最终选用的预测 target；
- 最终选用的预测 target 与提交时的实际 target 不同。

最终 target 可以来自 ITTAGE，也可以来自 BTB fallback；来源不影响判定。
**正确的 BTB fallback 不计 miss**。缺少 ITTAGE 元数据或 ITTAGE 未提供 target
本身不再触发 miss：只要最终预测 target 正确，就属于正确实例。
统计重置会同时清空累计 miss number 和 target 集合，不为统计额外查询预测器。

这是一项“最终 target 预测错误计数”，并不单凭出现一次 miss 就证明该 PC
持续难预测；仍需结合窗口持续性分析。输出没有分支执行次数，不能计算 miss rate。

本次口径修改前生成的 `h2p-detect-interval-1m/` 62 片数据采用旧 ITTAGE-only
口径，包含正确 BTB fallback。旧文件没有保存最终预测，无法从文件精确剔除这些
实例，也不能通过删除单 target PC 来转换；新口径必须重新仿真，不能混用旧结果。

## 输出格式

文件无表头，每行对应一个 miss number 大于零的静态分支，格式如下：

```text
0x80001234, 17, 0x80002000(12), 0x80003000(5)
```

字段依次为十六进制 PC、十进制 miss number，以及该 PC 在当前统计窗口内所有已提交实例的实际 target 和对应 miss 数。target 去重后按数值升序排列，记录行按 PC 数值升序排列。target 集合包含首次 miss 之前已经观察到的已提交 target。

分析汇总文件或单个窗口文件时，可运行：

```bash
python3 util/h2p-indirect-analysis.py <输出目录>/h2p-indirect.txt --top 20
python3 util/h2p-indirect-analysis.py <输出目录>/h2p-indirec/0.txt --top 10
```

省略 `--top` 时默认输出 miss 数最大的 20 行；并列时保留输入顺序。
随后以 `=========== unique targets ===========` 分隔，输出所选分支的全部
去重 target，按地址升序排列。工具兼容旧裸 target 和新 `target(miss)` 格式，
新格式还会核对每行 target miss 之和。

## 分窗口统计和恢复后的测量（新版）

新版所有 target 写成 `target(miss_count)`，例如：

```text
0x80001234, 17, 0x80002000(12), 0x80003000(5), 0x80004000(0)
```

计数归属实际提交 target；每行 target miss 之和等于 PC miss。正确实例的
`target(0)` 也保留。旧版本裸 target 格式仍可由分析工具读取。

`--h2p-stat-interval N` 默认 0，正数必须同时指定 `--h2p-stat`。
按 measure 阶段全部提交的架构指令计数，N=1000 时 `h2p-indirec/0.txt`
记录第 1–1000 条，`1000.txt` 记录第 1001–2000 条。边界指令先记入旧窗口再
关闭窗口；融合退休按原有架构指令单位推进。完整无 miss 窗口产生空文件，
退出保存非空长度尾窗，整除边界不生成多余尾窗。

全程 `h2p-indirect.txt` 与窗口独立累计，周期操作不重置其他 gem5 stats。
额外全局 reset 清空两套 H2P 数据及本进程旧窗口并从 0 编号。
启动时拒绝已有 H2P 输出；批量 runner 将旧目录归档，不覆盖旧实验。

单核 kmhv3 使用提交阶段控制：GCPT 启动时验证内存中的 restorer 寄存器恢复
序列及收尾 mret，必须提交该收尾且恢复到保存 PC 才结束 Restore。其后第一条
提交才开始 Warmup。warm-up 完成只 reset，不 dump；最终 stats 只有 Measure。
`--maxinsts=150000000 --warmup-insts-no-switch=100000000` 排除 restore，
得到 100M warm-up 和 50M measure。恢复无法识别或超出保护上限直接失败。
raw 输入不等待恢复标记。日志 `H2P_PHASE` 给出各边界 tick、总提交数和阶段计数。

指令数沿用当前 CPU 的 `simInsts` 口径：不计 nop 和 instruction prefetch，
微操作只在最后一项计数，融合组计两个架构指令单位。当前支持的融合模式不包含
间接分支，JALR 保持独立动态指令；融合的两个单位只推进窗口计数。未来若增加
AUIPC+JALR 等融合，需要重新核对分支 PC 和第二单位的窗口归属。
若融合组跨 warm-up 边界，`warmup_fusion_split=1`；若末尾融合组实际多退休一条，
`terminal_fusion_excess=1`，该额外单位不进入测量预算。`retired_post_restore`
记录真实退休量，runner 精确核对这些差值，不使用通用容差。融合组不能在不改变
模型的情况下拆开退休，整组微架构计数也不能逐单位拆分；`simInsts` 和 H2P 窗口
严格按预算统计，但这种极端边界下不宣称所有微架构事件都能精确分到两侧。

批量 runner 默认 interval=1M，新口径运行必须指定未使用的新输出目录，保留旧实验
`/home/xuejing/sim/workbench/cpython/build/h2p-detect-interval-1m/`。
审计每片精确阶段计数、50 个窗口、每 target miss 守恒和窗口 miss 累加一致。
由于每窗省略 miss=0 的 PC，全程零 miss target 可能未出现在窗口文件中，
因此只要求窗口 target 是全程集合子集以及所有 miss 计数相等。

批量工具另外支持 `--disable-aslr`（默认关闭），在 Linux 上通过
`setarch <architecture> -R` 启动 gem5，并在任何 workload 启动前检查支持情况。
开关与实际命令前缀纳入身份记录，具有不同执行环境的旧结果不能直接复用。
这是宿主进程运行选项，不是 gem5 配置中的 H2P 参数。

ICT185 验证曾出现相同二进制、相同 off 配置独立运行的时序差异；固定 ASLR
的四组 1M 对照（off/off/total/interval）全部统计和 H2P 汇总一致，但追加的
完整对照在 100M warm-up 边界仍有 tick 差异。因此 `--disable-aslr` 不能视为
该问题的修复。随后定位并修复 LSQ nuke 检查在异步翻译完成、有效地址尚未建立时
读取 store 长度的问题；旧 ITTAGE-only 口径的修复后二进制已通过同片完整三组 100M/50M 对照，全部非宿主
数值统计与 H2P 汇总一致，正式 62 片批次也已通过全部阶段和 3100 窗口审计。原始差异及诊断记录保留在
`results/h2p-interval-validation/nondeterminism/`；后续运行必须继续记录实际环境，
不能用短测一致替代完整对照验收。
