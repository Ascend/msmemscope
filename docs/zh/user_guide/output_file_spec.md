# **输出文件说明**

## 简介

msMemScope工具进行内存分析后，输出的文件如[**表 1**  输出文件说明](#输出文件说明-1)。

**表 1**  输出文件说明<a id="输出文件说明-1"></a>

|输出文件名称|说明|
|--|--|
|memscope_dump_{_timestamp_}.csv|使用内存分析功能时，输出内存信息结果文件，并默认保存在msmemscope_{*PID*}_{_timestamp_}_ascend/device_{*device_id*}/dump目录下，具体详情信息可参见[memscope_dump_{_timestamp_}.csv文件说明](#memscope_dump_timestampcsv文件说明)。|
|memory_compare_{*timestamp*}.csv|使用内存对比功能时，输出内存对比信息结果文件，记录的是基线内存信息、对比内存信息和对比后的内存差异信息，输出文件默认保存在memscopeDumpResults/compare目录下，具体详情信息可参见[memory_compare_{_timestamp_}.csv文件说明](#memory_compare_timestampcsv文件说明)。|
|memscope_dump_{_timestamp_}.db|db格式的内存信息结果文件，默认保存在msmemscope_{*PID*}_{_timestamp_}_ascend/device_{*device_id*}/dump目录下，可使用MindStudio Insight工具展示，展示结果及具体操作请参见[MindStudio Insight内存调优](https://gitcode.com/Ascend/msinsight/blob/master/docs/zh/user_guide/memory_tuning.md)。|
|python_trace_{_TID_}_{_timestamp_}.csv|Python Trace采集的结果文件，默认保存在msmemscope_{*PID*}_{_timestamp_}_ascend/device_{*device_id*}/dump目录下，具体详情信息可参见[python_trace_{_TID_}_{_timestamp_}.csv文件说明](#python_trace_tid_timestampcsv文件说明)。|
|config.json|Python接口自定义采集的配置信息文件，默认保存在msmemscope_{*PID*}_{_timestamp_}_ascend目录下。|
|leak_overview_{*stage*}.txt|Host堆内存泄漏检测的泄漏概览报告，默认保存在msmemscope_{*PID*}_{_timestamp_}_ascend/host_leak目录下，具体详情信息可参见[leak_overview_{*stage*}.txt文件说明](#leak_overview_stagetxt文件说明)。|
|block_detail_{*stage*}.csv|Host堆内存泄漏检测的泄漏代码块详情文件（仅event模式且窗口内存在未释放块时生成），默认保存在msmemscope_{*PID*}_{_timestamp_}_ascend/host_leak目录下，具体详情信息可参见[block_detail_{*stage*}.csv文件说明](#block_detail_stagecsv文件说明)。|

## memscope_dump_{_timestamp_}.csv文件说明

内存泄漏检测的结果文件字段解释如[**表 2**  memscope_dump_{_timestamp_}.csv文件字段及含义](#memscope_dump_{_timestamp_}.csv文件字段及含义)所示。

**表 2**  memscope_dump_{_timestamp_}.csv文件字段及含义 <a id="memscope_dump_{_timestamp_}.csv文件字段及含义"></a>

|字段|说明|
|--|--|
|ID|事件ID。|
|Event|msMemScope记录的事件类型，包括以下几种类型：<br><ul>**SYSTEM**：系统级事件。</ul><br><ul>**MALLOC**：内存申请。</ul><br><ul>**FREE**：内存释放。</ul><br><ul>**ACCESS**：内存访问。</ul><br><ul>**OP_LAUNCH**：算子执行。</ul><br><ul>**KERNEL_LAUNCH**：kernel执行。</ul><br><ul>**MSTX**：打点。</ul><br><ul>**SNAPSHOT**：内存快照数据。</ul><br><ul>**OOM_DETAIL**：OOM详细分析数据。</ul><br><ul>**OOM_Snapshot**：OOM时的自动记录的内存快照数据。</ul>|
|Event Type|事件子类型。<br><ul>当Event为SYSTEM时，Event Type包含ACL_INIT和ACL_FINI。</ul><br><ul>当Event为MALLOC或FREE时，Event Type包含HAL、PTA、MindSpore、ATB、HOST和PTA_WORKSPACE。</ul><br><ul>当Event为ACCESS时，Event Type包含READ、WRITE和UNKNOWN。</ul><br><ul>当Event为OP_LAUNCH时，Event Type包含ATEN_START、ATEN_END、ATB_START和ATB_END。</ul><br><ul>当Event为KERNEL_LAUNCH时，Event Type包含KERNEL_LAUNCH、KERNEL_START和KERNEL_END。</ul><br><ul>当Event为MSTX时，Event Type包含Mark、Range_start和Range_end。</ul><br><ul>当Event为SNAPSHOT时，Event Type包含SNAPSHOT。</ul><br><ul>当Event为OOM_DETAIL时，Event Type包含以下类型：<br>1. OOM_TRIGGER：触发OOM的操作信息。<br> 2. OOM_RECENT_ALLOC：按时间排序的最近已申请但未释放的内存记录。<br> 3. OOM_TOP_ALLOC：按大小排序的最大已申请但未释放的内存记录。</ul>|
|Name|与Event值有关，当Event值为以下值时，Name代表不同的含义。当Event值为其余值时，Name的值为N/A。<br><ul>**ACCESS**：Name为引发访问的算子名/ID。</ul><br><ul>**OP_LAUNCH**：Name为算子名称。</ul><br><ul>**KERNEL_LAUNCH**：Name为kernel名称。</ul><br><ul>**MSTX**：Name为自定义打点名称。</ul><br><ul>**OOM_DETAIL**：Name根据 Event Type 分别为“OOM_Trigger”、“OOM_RecentAlloc”或“OOM_TopAlloc”。</ul>|
|Timestamp(ns)|事件发生的时间。|
|Process Id|进程号。|
|Thread ID|线程号。|
|Device ID|设备信息。ID为数值时，代表当前设备信息为对应的NPU卡序号，当ID为“cpu”时，代表当前设备信息为cpu。|
|Ptr|内存地址，可以作为标识内存块的id值，一个内存块的生命周期是同一个ptr的malloc到下一次free。|
|Attr|事件特有属性，每个事件类型有各自的属性项。具体展示信息如下所示：<br><ul> **当Event为MALLOC或FREE时，会展示以下参数信息**：<br> 1. allocation_id：相同的allocation_id属于对同一块内存的操作。<br> 2. addr：地址。<br> 3. size：本次申请或者释放的内存大小。<br> 4. owner：内存块所有者，多级分类时格式为{A}@{B}@{C}.....，仅当Event为MALLOC时，存在此参数。<br> 5. used：内存使用量，随Event Type不同含义不同：<br>   - Event Type为HAL时，表示本进程HAL维度显存使用量（工具采集到的HAL申请/释放累计值）。<br>   - Event Type为PTA、MindSpore、ATB或PTA_WORKSPACE时，表示内存池内已分配内存大小。<br>   - Event Type为HOST且Attr中标记pinned:true时，表示采集到的锁页内存块使用量。<br> 6. total：内存池总大小，当Event Type为PTA、MindSpore、ATB或PTA_WORKSPACE时，存在此参数；当Event Type为HOST且Attr中无pinned:true标记时（CPU tensor数据），表示活跃CPU tensor数据内存累计值。<br> 7. process_used：本进程显存占用，随Event Type不同含义不同：<br>   - Event Type为HAL时，表示本进程在该设备上的显存占用，与npu-smi info的Process memory(MB)一致。<br>   - Event Type为PTA、MindSpore、ATB或PTA_WORKSPACE时，含义与Event Type为HAL时相同。<br>   - Event Type为HOST且Attr中标记pinned:true时，表示本进程物理内存使用量（VmRSS）。<br> 8. device_used：整卡显存占用，随Event Type不同含义不同：<br>   - Event Type为HAL时，表示整卡显存占用，与npu-smi info的HBM-Usage(MB)一致。<br>   - Event Type为PTA、MindSpore、ATB或PTA_WORKSPACE时，含义与Event Type为HAL时相同。<br>   - Event Type为HOST时，不存在此参数。<br> 9. inefficient：表示是否为低效内存，值表示低效类别，其中early_allocation表示过早申请，late_deallocation表示过迟释放，temporary_idleness表示临时闲置。仅当Event为MALLOC，Event Type为PTA或ATB时，存在此参数。<br>**说明**：process_used和device_used在程序初始化阶段，由于设备上下文还未完成初始化，可能采集不到（字段省略），属正常现象，初始化完成后可正常采集。</ul><br><ul>**当Event为ACCESS时，会展示以下参数信息**：<br> 1. dtype：Tensor的dtype。<br> 2. shape：Tensor的shape。<br> 3. size：Tensor的size。<br> 4. format：Tensor的format。<br> 5. type：访问内存池类型，例如ATB。<br> 6. allocation_id：相同的allocation_id属于对同一块内存的操作，仅当Event Type为PTA时，存在此参数。</ul><br><ul>**当Event为OP_LAUNCH，Event Type为ATB_START或ATB_END时，会展示以下参数信息**：<br> 1. path：算子在模型中的位置，例如“0_1967120/0/0_GraphOperation/0_ElewiseOperation”，其中包含pid、所属模块名和算子名。<br> 2. workspace ptr：workspace内存起始地址。<br> 3. workspace size：workspace内存大小。</ul><br><ul>**当Event为KERNEL_LAUNCH时，会展示以下参数信息**：<br> 1. path：kernel在模型中的位置，例如“0_1967120/1/0_GraphOperation/1_ElewiseOperation/0_AddF16Kernel/before”，其中包含pid、所属算子和kernel名，仅当Event Type为KERNEL_START或KERNEL_END时，存在此参数。<br> 2. streamId：stream编号。<br> 3. taskId：任务编号。</ul><br><ul>**当Event为SNAPSHOT时，会展示以下参数信息**：<br> 1. total_mem：设备总内存。<br> 2. free_mem：设备总空闲内存。<br> 3. reserved：torch框架预留总内存。<br> 4. peak_reserved：torch框架预留总内存峰值。<br> 5. allocated：torch框架使用内存。<br> 6. peak_allocated：torch框架使用内存峰值。<br> 7. device_utilization：设备内存使用率。<br> 8. pt_utilization：torch预留内存使用率。</ul><br><ul>**当Event为MALLOC，Event Type为HAL时，会展示以下参数信息**：<br> 1. page_type：有三种类型，为normal、huge和giant。<br> 2. alloc_type：有两种类型，为alloc和create。</ul><br><ul>**当Event为MALLOC或FREE，且Event Type为HOST时，会展示以下参数信息**：<br> 1. addr：地址。<br> 2. size：本次申请或者释放的内存大小。<br> HOST类型统一承载offload锁页内存与CPU tensor数据内存，二者通过Attr字段区分：<br> - offload锁页内存（Attr中标记pinned:true）：<br>   3. pinned：标记为true，表示为锁页内存。<br>   4. used：采集到的锁页内存块使用量。<br>   5. process_used：本进程物理内存使用量（VmRSS）。<br> - CPU tensor数据内存（Attr中包含total字段，无pinned:true标记）：<br>   3. total：活跃CPU tensor数据内存累计值。</ul><br><ul>**当Event为OOM_DETAIL时，会展示以下参数信息**：<br> 1. 当Event Type为OOM_TRIGGER时：func表示触发OOM的函数名，req_size表示申请内存大小，flag表示申请标志位，ret表示驱动返回码。<br> 2. 当Event Type为OOM_RECENT_ALLOC或OOM_TOP_ALLOC时：pool表示内存池类型，ptr表示内存地址，size表示分配大小，timestamp表示分配时间戳，step表示所属Step编号，kernel表示所属kernel编号，client表示客户端进程ID。这些记录同时包含Call Stack(Python)和Call Stack(C)调用栈信息。</ul>|
|Call Stack(Python)|Python调用栈信息（可选）。|
|Call Stack(C)|C调用栈信息（可选）。|

## memory_compare_{_timestamp_}.csv文件说明

内存对比的结果文件字段解释如[**表 3**  memory_compare_{_timestamp_}.csv文件字段说明](#memory_compare_{_timestamp_}.csv文件字段说明)所示。

**表 3**  memory_compare_{_timestamp_}.csv文件字段说明 <a id="memory_compare_{_timestamp_}.csv文件字段说明"></a>

|字段|说明|
|--|--|
|Event|msMemScope记录的对比事件类型，包括OP_LAUNCH和KERNEL_LAUNCH两种类型。|
|Name|kernel的名称。|
|Device ID|设备类型、卡号。|
|Base|input输入的第一个文件路径中的数据。|
|Compare|input输入的第二个文件路径中的数据。|
|Allocated Memory(Byte)|kernel调用前后的内存变化。如果为N/A，表示不存在该kernel的调用。|
|Diff Memory(Byte)|Base和Compare的内存相对变化。<br><ul>当数值为0时，表示该kernel调用所引起的内存变化没有差异。</ul><br><ul>当数值不为0时，表示该kernel调用所引起的内存变化存在差异。</ul>|

## python_trace_{_TID_}_{_timestamp_}.csv文件说明

Python Trace采集结果文件的字段解释如[**表 4**  python_trace_{_TID_}_{_timestamp_}.csv文件字段说明](#python_trace_{_TID_}_{_timestamp_}.csv文件字段说明)所示。

**表 4**  python_trace_{_TID_}_{_timestamp_}.csv文件字段说明 <a id="python_trace_{_TID_}_{_timestamp_}.csv文件字段说明"></a>

|字段|说明|
|--|--|
|FuncInfo|函数名。|
|StartTime(ns)|开始时间戳，和memscope_dump_{_timestamp_}.csv中的事件时间戳是一致的。|
|EndTime(ns)|结束时间戳。|
|Thread Id|线程ID。|
|Process Id|进程ID。|

## leak_overview_{_stage_}.txt文件说明

Host堆内存泄漏检测的泄漏概览报告文件，检测窗口关闭后生成（summary与event模式均输出）。文件保存在`msmemscope_{*PID*}_{_timestamp_}_ascend/host_leak`目录下，`{stage}`为窗口序号。

报告按章节组织，如[**表 5**  leak_overview_{_stage_}.txt报告章节说明](#leak_overview_stagetxt报告章节说明)所示。

**表 5**  leak_overview_{_stage_}.txt报告章节说明<a id="leak_overview_stagetxt报告章节说明"></a>

|章节|内容|
|--|--|
|Data Health Analysis|数据健康度分析：如实标注本窗口的追踪决策。包括检测窗口起止时间与时长、上报模式、总申请/释放计数、去重调用栈数（含记账键深K与类归因语义）、未归因块数及占比、死栈淘汰统计（仅发生时输出）、截断标注（bit0=块表满转溢出通道、bit1=栈表满且死栈回收无供给时新栈转未知桶、bit2=溢出账本满记账停止，仅bit2构成数据不完整）、溢出通道分流与逆向修正统计、开窗前free统计、采样率（非1时标注采样视图）、块大小阈值与未追踪统计、符号化覆盖率（未解析栈数）。统计不可得时标注`Snapshot: unavailable`。|
|Total Unfreed|总泄漏量：本窗口内申请且未释放的字节数/块数合计（含未知桶与溢出通道存活块）及平均值、最大值。|
|Unfreed Block Size Distribution|泄漏块大小排布：未释放块按大小分桶的块数、字节、占总泄漏量百分比。默认7桶：0~256B、256B~1kB、1kB~4kB、4kB~32kB、32kB~256kB、256kB~1MB、1MB以上。|
|Pre-Window Free Size Distribution|开窗前free大小排布：开窗前申请、窗口期间释放的内存按大小分桶的释放次数与字节数。|
|TOP N Leak Sites|TOP N泄漏点：按泄漏怀疑指数（LSI，Leak Suspicion Index，0~100）降序的泄漏点列表，默认N=10，上限1024。每行包含状态分类（suspected_leak/growth_watch，周转形态追加/turnover）、LSI值、Growth/Release/Lifetime/Pattern/Scale因子、未释放/申请/释放统计、序列增长斜率与拍数、降级标注及完整符号化调用栈文本。章节首行输出节拍序列元信息（top-k=256、max-stacks=1024、拍数与时长）。未知桶行为`(unknown bucket: unattributed blocks)`，栈文本缺失标注`(unresolved stack <stack id>)`。|
|Resident Baselines|常驻基线子块：按未释放量降序的常驻栈列表，不占TOP N名额，判据来源标注resident/early或resident/turnover；闭窗报告不截断。开窗前free占窗口内总申请过半时，报告额外输出缓存周转NOTE提示。|

### 内容示例

leak_overview_{_stage_}.txt为纯文本报告（非表格文件），以下为event模式、单个检测窗口的报告内容示例（数值为示意，非真实采集数据）：

```text
====== Host Leak Overview: stage=1, pid=1234 ======
LSI: Leak Suspicion Index (0-100); higher LSI = more likely a genuine leak. See TOP N Leak Sites section for details.

--- Data Health Analysis ---
Window: 100000000000 -> 200000000000 (duration: 100s)
Mode: event
Tracked: 102 allocations / 1149576B allocated; 0 freed / 0B
Distinct stacks: 2 (key depth K=20, category semantics)
Symbolized: 2/2 stacks (unresolved: 0)

--- Total Unfreed ---
Total unfreed: 1149576 bytes in 102 blocks (avg 11270B, max 1048576B)

--- Unfreed Block Size Distribution ---
range           blocks       bytes  % of total
[0, 256)              0           0         0%
[256, 1K)           101      101000         8%
[1K, 4K)              0           0         0%
[4K, 32K)             0           0         0%
[32K, 256K)           0           0         0%
[256K, 1M)            0           0         0%
[1M, +inf)            1     1048576        91%

--- Pre-Window Free Size Distribution ---
range           frees       bytes
(no pre-window frees)

--- TOP 10 Leak Sites (by LSI desc) ---
Series: top-k=256, max-stacks=1024, beats=101(100s, 1s/beat)
  1. [suspected_leak] LSI 82  Growth 1.00  Release 1.00  Lifetime 0.50  Pattern 0.00  Scale 1.00  unfreed/alloc 1.00
     unfreed 101000B(101 blocks) | alloc 101x1000B, freed 0x0B
     growth 1000B/beat  pts 96  degraded -  stack 0x5
     main
     foo() [0x1]

Resident baselines (1 stacks, 1MiB; G≈0 & early-allocated, or turnover (low unfreed/alloc & flat tail); excluded from suspect list, no TOP N slot):
  1. [resident/early] LSI 31  Growth 0.00  Release 1.00  Lifetime 0.50  Scale 0.15  unfreed/alloc 1.00
     unfreed 1048576B(1 blocks) | alloc 1x1MiB, freed 0x0B
     growth 0B/beat  pts 96  degraded -  stack 0x6
     pool
```

示例要点说明：

- **头部与Data Health Analysis**：首行`====== Host Leak Overview: stage=1, pid=1234 ======`标识窗口序号与进程号；`Window`行为窗口起止时刻与时长；`Mode`为上报模式（event/summary）；`Tracked`行是窗口内经记账的申请/释放总量；`Distinct stacks`为去重栈数（键深K=20，类归因语义）；`Symbolized`为符号化覆盖率。出现截断、采样、死栈淘汰、溢出通道等情况时，本小节会追加对应标注行（仅发生时输出），例如：
  - `Truncated: block table full (allocations -> overflow channel)`——块表满，申请转溢出通道（仅归因粒度退化）。
  - `Truncated: stack table (dead-stack recycling active, no reclaimable stack at full)`——栈表满且死栈回收无供给，新栈归入未知桶（仅归因粒度退化）。
  - `Truncated: overflow channel full (recording stopped at N live blocks) [window data incomplete: not a leak conclusion]`——溢出通道也满，记账停止，窗口数据不完整。
  - `Evicted: N stacks recycled (M allocs / B bytes folded to unknown bucket)`——栈表满时死栈回收，折叠量并入未知桶。
  - `Overflow channel: N allocations / B bytes diverted; M freed / C B (reverse-corrected)`——块表满时申请转溢出通道、释放做逆向修正的统计。
  - `Pre-window frees: N / B bytes (allocated before window, not in ledger)`——开窗前申请、窗口期间释放的独立通道统计。
  - `Sampling: 1/4 (sampled view)`——采样视图（采样率倒数>1时）。
  - `Size threshold: 1024B (untracked: N allocations / M B)`——块大小阈值过滤（阈值>0时）。
  - `Snapshot: unavailable (host hook not bound / query failed)`——统计不可得（极端早期退出等）。
  三个截断位同时置位时，对应标注以` | `连接为一行输出（bit2置位时行尾追加`[window data incomplete: not a leak conclusion]`）。
- **Total Unfreed**：一行给出窗口内未释放总量（块数/字节/均值/最大值），精确值不缩略。
- **Unfreed Block Size Distribution**：默认7桶，表头`range/blocks/bytes/% of total`，末桶上限渲染为`+inf`；占比为整数截断（多桶占比之和可能不足100%）；无未释放块时输出`(no unfreed blocks)`。
- **Pre-Window Free Size Distribution**：开窗前free独立通道，表头`range/frees/bytes`；无记录时输出`(no pre-window frees)`。
- **TOP N Leak Sites**：`Series`行为节拍序列元信息（top-k=256、max-stacks=1024、拍数与窗口时长）；每个条目4行结构——首行`序号. [状态] LSI 值 + 五个研判因子 + unfreed/alloc占比`，第二行未释放/申请/释放统计，第三行增长斜率（B/拍）、有效拍数、降级原因（无降级显示`-`）与栈ID，其后为完整符号化栈文本（逐帧缩进）。序列降级（no_series/series_evicted/insufficient_series/no_warmup_thread）时Growth/Pattern因子显示`-`（Release/Lifetime/Scale仍为数值）；栈文本缺失标注`(unresolved stack <栈ID>)`。
- **Resident Baselines**：头行标注常驻栈数与未释放量合计、判据来源（G≈0且早期分配，或周转形态）；条目结构同TOP N（差异：省略Pattern因子、不占TOP N名额）；闭窗报告不截断。
- **NOTE行**：开窗前free字节超过窗口内总申请一半时，报告末尾输出缓存周转NOTE（如`NOTE: 64KiB pre-window allocations were freed within this window (cache turnover pattern); ...`），提示结合unfreed/alloc占比解读常驻分类。

> [!NOTE]
>
> 窗口开启期间的中间概览（进程外控制通道`display host_leak summary`查询）章节结构与上述一致，差异（interim标注、冻结标注、常驻截断、不落盘）详见[窗口内中间概览（巡检）](./memory_analysis.md#窗口内中间概览巡检)。

## block_detail_{_stage_}.csv文件说明

Host堆内存泄漏检测的泄漏代码块详情文件，仅在`--host-leak-mode=event`且窗口内存在未释放块时生成。文件保存在`msmemscope_{*PID*}_{_timestamp_}_ascend/host_leak`目录下，与同窗号概览报告对应。

文件为CSV格式，表头为首行`addr,size,alloc_ts,Call Stack(C),Call Stack(Python)`，逐块一行、块大小降序（相同大小按地址升序），字段如[**表 6**  block_detail_{_stage_}.csv文件字段及含义](#block_detail_stagecsv文件字段及含义)所示。

**表 6**  block_detail_{_stage_}.csv文件字段及含义<a id="block_detail_stagecsv文件字段及含义"></a>

|字段|含义|格式|
|--|--|--|
|addr|未释放块起始地址|`0x`前缀 + 16位小写十六进制零填充。|
|size|块大小（字节）|十进制。|
|alloc_ts|窗口内分配时间戳（纳秒）|十进制。|
|Call Stack(C)|C调用栈文本（自顶帧到root帧，帧间以换行分隔）；混合栈（C+Python）中为该栈marker之前的纯C栈部分|双引号包裹字符串（RFC 4180引号字段，内嵌换行保留，内部`"`转义为`""`）。栈文本缺失时写`(unresolved stack)`；未知桶（栈表超限块）写`(unknown bucket: unattributed blocks)`。|
|Call Stack(Python)|Python帧文本（混合栈中marker之后的py帧部分）|格式同C列。未采集Python帧的栈（纯C栈/占位行）该列为空`""`。|

> [!NOTE]
>
> - 明细文件仅覆盖块表口径，溢出通道块（块表满降级转入，无栈归因）不进入明细；概览报告的Total Unfreed含溢出通道存活块，两者差值即溢出通道块。
> - 调用栈文本按块内联自含（C/Python两列），不依赖同窗概览报告即可独立解析。
