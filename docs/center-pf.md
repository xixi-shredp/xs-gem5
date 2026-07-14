# 中心化数据预取器

本文介绍中心化数据预取器的使用方式、架构组成、请求流、跨页翻译机制、
关键参数和统计项。该功能目前是一个默认关闭的实验模式，主要面向 KMHV3
aligned L2 cache 层次。

## 设计目标

传统配置在 L1D、L2 和 L3 分别部署预取器，各层独立学习访存模式并产生请求。
中心化模式将模式识别和目标层级选择集中到每个 CPU core 的 L1D 侧：

- 一个 central engine 统一生成候选请求。
- dispatcher 根据预取距离和各级 cache 的实时 prefetch MSHR 余量选择目标层级。
- L2 和 L3 不再运行独立 learner，仅保留被动注入 endpoint。
- 跨页 data prefetch 的翻译结果先保存在专用 PTE buffer 中，避免直接污染
  L1DTLB；只有 demand 命中时才提升到 L1DTLB。

中心化模式通过命令行显式开启，未开启时保持原有多级预取器行为。

## 快速使用

### 构建

```bash
scons build/RISCV/gem5.opt --gold-linker -j64
```

### 运行普通镜像

```bash
./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
  --raw-cpt \
  --generic-rv-cpt=<image-path> \
  --centralized-data-prefetcher
```

### 运行 checkpoint slice

```bash
./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
  --generic-rv-cpt=<slice.zstd> \
  --centralized-data-prefetcher
```

GCPT/SPEC checkpoint slice 不应添加 `--raw-cpt`。

### Baseline A/B

基线模式不传入中心化开关：

```bash
# Baseline
./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
  --generic-rv-cpt=<slice.zstd>

# Centralized
./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
  --generic-rv-cpt=<slice.zstd> \
  --centralized-data-prefetcher
```

A/B 时应保证 binary、配置、checkpoint、DTB、warmup/sample 指令数以及其他命令行
参数完全一致。

## 使用约束

- 中心化模式默认关闭。
- 当前仅支持 KMHV3 aligned L2 路径，不支持 classic L2 配置。
- 必须启用数据预取，不能与 `--no-pf` 同时使用。
- 配置 L3 时 dispatcher 可选择 L1、L2、L3；没有 L3 时最深只分发到 L2。
- 默认只使用 demand read 训练，不使用 store 或 `HardPFReq` 训练。
- two-stage translation 当前绕过 data-prefetch PTE buffer。

## 顶层架构

```text
                    L1D demand access
                            |
                            v
              +---------------------------+
              | CentralizedDataPrefetcher |
              | pattern engines           |
              | candidate queue           |
              | translation + dedup       |
              | dispatcher                |
              +---------------------------+
                   |          |          |
              local L1    L2 endpoint  L3 endpoint
                   |          |          |
                   v          v          v
                  L1D    aligned L2     shared L3

              live MSHR credits are queried at dispatch
              and checked again immediately before issue
```

每个 CPU core 拥有一个 central engine。L2 endpoint 为每核 endpoint；共享 L3
endpoint 使用 core ID 进行 round-robin ingress 仲裁。

### Central engine

`CentralizedDataPrefetcher` 复用 `XSCompositePrefetcher` 的模式识别组件，包括
PHT、SStride、XsStream、BOP 和 CMC。各组件产生的地址不直接进入其原目标 cache，
而是转换为统一候选并进入 central queue。

候选携带以下主要信息：

- 虚拟地址和翻译后的物理地址。
- source、priority 和 prefetch depth。
- 相对触发地址的 cache-line distance。
- preferred level 和最终 actual level。
- context、secure 属性和稳定 sequence。

候选按预取算法给出的 priority 排序，同一 priority 内按进入顺序处理。

### MSHR credit 接口

cache accessor 提供 address-aware prefetch MSHR credit 查询：

- L1 和 L3 直接查询目标 cache 的 MSHR queue。
- aligned L2 根据物理地址选择正确 slice，再查询该 slice 的 MSHR 状态。
- demand 预留和 MSHR target 限制仍由原 cache/MSHR 模型负责。

MSHR 状态会检查两次：

1. dispatcher 选择目标层级时检查。
2. endpoint 将请求交给 cache 前再次检查。

第二次检查失败时，请求可以继续尝试更深层级，防止排队期间 credit 状态变化。

### L2/L3 endpoint

`CentralizedPrefetcherEndpoint` 不学习访存模式，只负责：

- 接收 central engine 的物理地址候选。
- 检查 endpoint 总容量和每核 ingress 容量。
- 检查目标 cache、MSHR、write queue 和重复请求。
- 对共享 endpoint 执行 round-robin 仲裁。
- 将 cache admission 结果显式回传给 central engine。

显式 completion callback 用于释放 dedup 状态和区分 accepted、duplicate、
credit drop 等结果，不依赖固定超时推测请求是否已经进入 cache。

## Dispatcher 策略

### Preferred level

当前依据候选和触发地址之间的绝对 cache-line distance 选择首选层级：

| Distance | Preferred level |
| --- | --- |
| `0..32` lines | L1 |
| `33..128` lines | L2 |
| `>128` lines | L3 |

如果系统没有 L3，L3 preferred request 的最深目标限制为 L2。

### 回退规则

dispatcher 只允许向更深层回退：

```text
L1 preferred: L1 -> L2 -> L3
L2 preferred:       L2 -> L3
L3 preferred:             L3
```

该规则避免远距离预取因为下层资源紧张而污染更小、更敏感的上层 cache。

### 去重

中心化模式包含两级去重：

1. 使用虚拟 block、context 和 secure 属性对未翻译候选去重。
2. 翻译完成后使用物理 block 和 secure 属性跨 source、跨目标层级去重。

dispatcher 和 endpoint 还会查询 L1/L2/L3 的 cache、MSHR、write queue 以及
endpoint 内部队列，避免重复注入已经存在或正在处理的 cache line。

## 跨页预取和 PTE buffer

### 同页请求

候选与触发地址位于同一虚拟页时，central engine 使用触发请求已有的物理页信息
推导目标物理地址，不额外发起 page walk。

### 跨页请求

跨页候选需要通过 DTLB/PTW 翻译：

```text
cross-page data prefetch
          |
          v
       DTLB/PTW
          |
          v
data-prefetch PTE buffer
          |
          | demand hit
          v
        L1DTLB
```

data-prefetch PTE buffer 与 L1DTLB 保持互斥：

- prefetch-only page walk 完成后，PTE 进入专用 buffer，不直接写 L1DTLB。
- demand 命中 buffer 时，PTE 从 buffer 移除并填入 L1DTLB。
- demand refill L1DTLB 时会清除 buffer 中冲突的 PTE。
- TLB flush/invalidation 会同步清除匹配的 buffer entry。
- page walk fault 不会被保存在 buffer 中。

buffer 当前为每个 L1DTLB 16 项 exclusive LRU。

### PTW 资源策略

demand 和 data prefetch 共享 PTW queue 和并发资源，但使用以下保护：

- demand 请求优先选择。
- 为 demand 保留 4 个 PTW queue entry。
- demand 和 prefetch 均受到 PTW level/token 限制。
- prefetch queue 满或超过限制时不得阻塞 demand progress。

## 默认参数

| 参数 | 默认值 | 含义 |
| --- | ---: | --- |
| `central_queue_size` | 64 | ready 与 translating candidate 总容量 |
| `dispatch_width` | 1 | 每周期最多处理的候选数 |
| `l1_distance_threshold` | 32 | L1 最大首选距离，单位为 cache line |
| `l2_distance_threshold` | 128 | L2 最大首选距离，单位为 cache line |
| `l1_min_mshr_credits` | 1 | L1 admission 所需最小 MSHR credit |
| endpoint `queue_size` | 32 | L2/L3 endpoint 总容量 |
| endpoint `per_core_queue_size` | 8 | 每核 ingress 容量 |
| endpoint `arbitration_width` | 1 | 每周期 endpoint 仲裁宽度 |
| endpoint `min_mshr_credits` | 1 | endpoint admission 所需 credit |
| L1 prefetch queue | 32 | L1 本地预取请求队列容量 |
| `train_on_store` | false | 是否使用 store 训练 central engine |
| PTE buffer size | 16 | 每个 L1DTLB 的预取 PTE entry 数 |
| PTW demand reserve | 4 | 为 demand 保留的 PTW queue entry 数 |

当前主要配置入口是：

- `configs/common/Options.py`
- `configs/common/CacheConfig.py`
- `configs/common/PrefetcherConfig.py`
- `src/mem/cache/prefetch/Prefetcher.py`

除 `--centralized-data-prefetcher` 外，大部分参数尚未暴露为命令行选项。

## 关键统计

### Central engine

```text
system.cpu.dcache.prefetcher.totalTrainCount
system.cpu.dcache.prefetcher.generated
system.cpu.dcache.prefetcher.dispatched
system.cpu.dcache.prefetcher.issued
system.cpu.dcache.prefetcher.dropped
system.cpu.dcache.prefetcher.translationRequests
system.cpu.dcache.prefetcher.preferredLevel::1/2/3
system.cpu.dcache.prefetcher.actualLevel::1/2/3
system.cpu.dcache.prefetcher.preferredToActual
system.cpu.dcache.prefetcher.queueOccupancy
```

`actualLevel` 表示 dispatcher 成功放置请求的层级；`issued` 表示请求最终通过
endpoint/cache admission。二者不要求完全相等。

`dropReason` 编码如下：

| 编号 | 原因 |
| ---: | --- |
| 0 | Duplicate |
| 1 | CentralQueueFull |
| 2 | MissingTrigger |
| 3 | TranslationUnavailable |
| 4 | TranslationFailed |
| 5 | PhysicalDuplicate |
| 6 | NoAdmission |
| 7 | EndpointRejected |
| 8 | IssueCreditDrop |

### L2/L3 endpoint

```text
system.l2_wrappers.prefetcher.accepted
system.l2_wrappers.prefetcher.issued
system.l2_wrappers.prefetcher.rejectedFull
system.l2_wrappers.prefetcher.rejectedPerCoreFull
system.l2_wrappers.prefetcher.rejectedCredit
system.l2_wrappers.prefetcher.rejectedDuplicate
system.l2_wrappers.prefetcher.rejectedInsert
system.l2_wrappers.prefetcher.issueCreditDrop

system.l3.prefetcher.accepted
system.l3.prefetcher.issued
system.l3.prefetcher.rejectedFull
system.l3.prefetcher.rejectedCredit
system.l3.prefetcher.rejectedDuplicate
system.l3.prefetcher.issueCreditDrop
```

### PTE buffer 和 PTW

```text
system.cpu.mmu.dtb.dataPrefetchPteBufferLookups
system.cpu.mmu.dtb.dataPrefetchPteBufferDemandHits
system.cpu.mmu.dtb.dataPrefetchPteBufferPrefetchHits
system.cpu.mmu.dtb.dataPrefetchPteBufferInserts
system.cpu.mmu.dtb.dataPrefetchPteBufferPromotions
system.cpu.mmu.dtb.dataPrefetchPteBufferUnusedEvictions
system.cpu.mmu.dtb.dataPrefetchPteBufferL1ConflictRemovals
system.cpu.mmu.dtb.dataPrefetchPteBufferPrefetchOnlyWalks
system.cpu.mmu.dtb.dataPrefetchPteBufferDemandCoalesces
system.cpu.mmu.dtb.dataPrefetchPteBufferTwoStageBypasses

system.cpu.mmu.dtb.walker.ptwDemandQueueEnqueues
system.cpu.mmu.dtb.walker.ptwPrefetchQueueEnqueues
system.cpu.mmu.dtb.walker.ptwPrefetchQueueDrops
system.cpu.mmu.dtb.walker.ptwDemandLevelBlocked
system.cpu.mmu.dtb.walker.ptwPrefetchLevelBlocked
```

## 性能分析建议

分析中心化模式时，不应只比较 `generated` 或 `actualLevel`。建议至少同时观察：

1. L1/L2/L3 demand miss 和 demand MSHR miss。
2. 各层 prefetch issued/useful/unused。
3. cache blocked cycles、load replay 和 MSHR replay。
4. L3 miss、memory demand reads 和 prefetch reads。
5. DTLB miss、TLB replay、PTW memory access。
6. PTE buffer promotion rate 和 unused eviction rate。

当前 L3 endpoint 的 useful/unused attribution 尚不完整，因此需要结合 L3 demand miss
和内存流量间接判断 L3 placement 的收益。

## 当前局限

- central engine 目前主要观察 L1 demand stream，尚未接收完整的 L2 demand-miss、
  refill、PF-hit 和 usefulness feedback。
- target level 主要由固定 distance threshold 决定，尚未使用 source confidence、
  历史 accuracy、reuse distance 或 late feedback。
- L3 useful/unused/source attribution 不完整。
- PTE buffer 容量和跨页触发策略仍是固定配置。
- two-stage translation 尚未进入专用 PTE buffer。
- 当前实现优先保证单核和 aligned L2 路径，多核共享 L3 的公平性需要继续验证。

## 主要源码位置

| 功能 | 文件 |
| --- | --- |
| Central engine、dispatcher、endpoint | `src/mem/cache/prefetch/centralized.hh/.cc` |
| SimObject 参数 | `src/mem/cache/prefetch/Prefetcher.py` |
| 命令行选项 | `configs/common/Options.py` |
| Cache 层次连接 | `configs/common/CacheConfig.py` |
| 默认参数和实例化 | `configs/common/PrefetcherConfig.py` |
| Cache/MSHR credit | `src/mem/cache/base.hh/.cc`、`src/mem/cache/mshr_queue.hh` |
| Aligned L2 slice credit | `src/mem/cache/xs_l2/SlicedCacheAccessor.hh/.cc` |
| Data-prefetch PTE buffer | `src/arch/riscv/tlb.hh/.cc` |
| PTW demand/prefetch 调度 | `src/arch/riscv/pagetable_walker.hh/.cc` |
