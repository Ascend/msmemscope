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

## memscope_dump_{_timestamp_}.csv文件说明

内存泄漏检测的结果文件字段解释如[**表 2**  memscope_dump_{_timestamp_}.csv文件字段及含义](#memscope_dump_{_timestamp_}.csv文件字段及含义)所示。

**表 2**  memscope_dump_{_timestamp_}.csv文件字段及含义 <a id="memscope_dump_{_timestamp_}.csv文件字段及含义"></a>

|字段|说明|
|--|--|
|ID|事件ID。|
|Event|msMemScope记录的事件类型，包括以下几种类型：<br><ul>**SYSTEM**：系统级事件。</ul><br><ul>**MALLOC**：内存申请。</ul><br><ul>**FREE**：内存释放。</ul><br><ul>**ACCESS**：内存访问。</ul><br><ul>**OP_LAUNCH**：算子执行。</ul><br><ul>**KERNEL_LAUNCH**：kernel执行。</ul><br><ul>**MSTX**：打点。</ul><br><ul>**SNAPSHOT**：内存快照数据。</ul><br><ul>**OOM_DETAIL**：OOM详细分析数据。</ul><br><ul>**OOM_Snapshot**：OOM时的自动记录的内存快照数据。</ul>|
|Event Type|事件子类型。<br><ul>当Event为SYSTEM时，Event Type包含ACL_INIT和ACL_FINI。</ul><br><ul>当Event为MALLOC或FREE时，Event Type包含HAL、PTA、MindSpore、ATB、HOST、HOST_PINNED和PTA_WORKSPACE。</ul><br><ul>当Event为ACCESS时，Event Type包含READ、WRITE和UNKNOWN。</ul><br><ul>当Event为OP_LAUNCH时，Event Type包含ATEN_START、ATEN_END、ATB_START和ATB_END。</ul><br><ul>当Event为KERNEL_LAUNCH时，Event Type包含KERNEL_LAUNCH、KERNEL_START和KERNEL_END。</ul><br><ul>当Event为MSTX时，Event Type包含Mark、Range_start和Range_end。</ul><br><ul>当Event为SNAPSHOT时，Event Type包含SNAPSHOT。</ul><br><ul>当Event为OOM_DETAIL时，Event Type包含以下类型：<br>1. OOM_TRIGGER：触发OOM的操作信息。<br> 2. OOM_RECENT_ALLOC：按时间排序的最近已申请但未释放的内存记录。<br> 3. OOM_TOP_ALLOC：按大小排序的最大已申请但未释放的内存记录。</ul>|
|Name|与Event值有关，当Event值为以下值时，Name代表不同的含义。当Event值为其余值时，Name的值为N/A。<br><ul>**ACCESS**：Name为引发访问的算子名/ID。</ul><br><ul>**OP_LAUNCH**：Name为算子名称。</ul><br><ul>**KERNEL_LAUNCH**：Name为kernel名称。</ul><br><ul>**MSTX**：Name为自定义打点名称。</ul><br><ul>**OOM_DETAIL**：Name根据 Event Type 分别为“OOM_Trigger”、“OOM_RecentAlloc”或“OOM_TopAlloc”。</ul>|
|Timestamp(ns)|事件发生的时间。|
|Process Id|进程号。|
|Thread ID|线程号。|
|Device ID|设备信息。ID为数值时，代表当前设备信息为对应的NPU卡序号，当ID为“cpu”时，代表当前设备信息为cpu。|
|Ptr|内存地址，可以作为标识内存块的id值，一个内存块的生命周期是同一个ptr的malloc到下一次free。|
|Attr|事件特有属性，每个事件类型有各自的属性项。具体展示信息如下所示：<br><ul> **当Event为MALLOC或FREE时，会展示以下参数信息**：<br> 1. allocation_id：相同的allocation_id属于对同一块内存的操作。<br> 2. addr：地址。<br> 3. size：本次申请或者释放的内存大小。<br> 4. owner：内存块所有者，多级分类时格式为{A}@{B}@{C}.....，仅当Event为MALLOC时，存在此参数。<br> 5. used：内存使用量，随Event Type不同含义不同：<br>   - Event Type为HAL时，表示本进程HAL维度显存使用量（工具采集到的HAL申请/释放累计值）。<br>   - Event Type为PTA、MindSpore、ATB或PTA_WORKSPACE时，表示内存池内已分配内存大小。<br>   - Event Type为HOST_PINNED时，表示采集到的HOST内存块使用量。<br> 6. total：内存池总大小，仅当Event Type为PTA、MindSpore、ATB或PTA_WORKSPACE时，存在此参数。<br> 7. process_used：本进程显存占用，随Event Type不同含义不同：<br>   - Event Type为HAL时，表示本进程在该设备上的显存占用，与npu-smi info的Process memory(MB)一致。<br>   - Event Type为PTA、MindSpore、ATB或PTA_WORKSPACE时，含义与Event Type为HAL时相同。<br>   - Event Type为HOST_PINNED时，表示本进程物理内存使用量（VmRSS）。<br> 8. device_used：整卡显存占用，随Event Type不同含义不同：<br>   - Event Type为HAL时，表示整卡显存占用，与npu-smi info的HBM-Usage(MB)一致。<br>   - Event Type为PTA、MindSpore、ATB或PTA_WORKSPACE时，含义与Event Type为HAL时相同。<br>   - Event Type为HOST_PINNED时，不存在此参数。<br> 9. inefficient：表示是否为低效内存，值表示低效类别，其中early_allocation表示过早申请，late_deallocation表示过迟释放，temporary_idleness表示临时闲置。仅当Event为MALLOC，Event Type为PTA或ATB时，存在此参数。<br>**说明**：process_used和device_used在程序初始化阶段，由于设备上下文还未完成初始化，可能采集不到（字段省略），属正常现象，初始化完成后可正常采集。</ul><br><ul>**当Event为ACCESS时，会展示以下参数信息**：<br> 1. dtype：Tensor的dtype。<br> 2. shape：Tensor的shape。<br> 3. size：Tensor的size。<br> 4. format：Tensor的format。<br> 5. type：访问内存池类型，例如ATB。<br> 6. allocation_id：相同的allocation_id属于对同一块内存的操作，仅当Event Type为PTA时，存在此参数。</ul><br><ul>**当Event为OP_LAUNCH，Event Type为ATB_START或ATB_END时，会展示以下参数信息**：<br> 1. path：算子在模型中的位置，例如“0_1967120/0/0_GraphOperation/0_ElewiseOperation”，其中包含pid、所属模块名和算子名。<br> 2. workspace ptr：workspace内存起始地址。<br> 3. workspace size：workspace内存大小。</ul><br><ul>**当Event为KERNEL_LAUNCH时，会展示以下参数信息**：<br> 1. path：kernel在模型中的位置，例如“0_1967120/1/0_GraphOperation/1_ElewiseOperation/0_AddF16Kernel/before”，其中包含pid、所属算子和kernel名，仅当Event Type为KERNEL_START或KERNEL_END时，存在此参数。<br> 2. streamId：stream编号。<br> 3. taskId：任务编号。</ul><br><ul>**当Event为SNAPSHOT时，会展示以下参数信息**：<br> 1. total_mem：设备总内存。<br> 2. free_mem：设备总空闲内存。<br> 3. reserved：torch框架预留总内存。<br> 4. peak_reserved：torch框架预留总内存峰值。<br> 5. allocated：torch框架使用内存。<br> 6. peak_allocated：torch框架使用内存峰值。<br> 7. device_utilization：设备内存使用率。<br> 8. pt_utilization：torch预留内存使用率。</ul><br><ul>**当Event为MALLOC，Event Type为HAL时，会展示以下参数信息**：<br> 1. page_type：有三种类型，为normal、huge和giant。<br> 2. alloc_type：有两种类型，为alloc和create。</ul><br><ul>**当Event为MALLOC或FREE，且Event Type为HOST_PINNED时，会展示以下参数信息**：<br> 1. addr：地址。<br> 2. size：本次申请或者释放的内存大小。<br> 3. pinned：是否为锁页内存。<br> 4. used：采集到的HOST内存块使用量。<br> 5. process_used：本进程物理内存使用量（VmRSS）。</ul><br><ul>**当Event为MALLOC或FREE，且Event Type为HOST时（CPU tensor数据），会展示以下参数信息**：<br> 1. addr：地址。<br> 2. size：本次申请或者释放的内存大小。</ul><br><ul>**当Event为OOM_DETAIL时，会展示以下参数信息**：<br> 1. 当Event Type为OOM_TRIGGER时：func表示触发OOM的函数名，req_size表示申请内存大小，flag表示申请标志位，ret表示驱动返回码。<br> 2. 当Event Type为OOM_RECENT_ALLOC或OOM_TOP_ALLOC时：pool表示内存池类型，ptr表示内存地址，size表示分配大小，timestamp表示分配时间戳，step表示所属Step编号，kernel表示所属kernel编号，client表示客户端进程ID。这些记录同时包含Call Stack(Python)和Call Stack(C)调用栈信息。</ul>|
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
