# Output File Specifications

## Overview

msMemScope outputs the following files after it finishes memory analysis.

**Table 1** Output files <a id="output-file-description -1"></a>

|Output File|Description|
|--|--|
|memscope_dump_{_timestamp_}.csv|Memory information result file during memory analysis. It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend/device_{*device_id*}/dump** directory by default. For details, see [memscope_dump_{_timestamp_}.csv](#memscope_dump_timestampcsv).|
|memory_compare_{*timestamp*}.csv|Memory comparison result file during memory analysis, which records baseline memory, memory comparison information, and differences after comparison. It is saved in the **memscopeDumpResults/compare** directory by default. For details, see [memory_compare_{_timestamp_}.csv](#memory_compare_timestampcsv).|
|memscope_dump_{_timestamp_}.db|Memory information file in .db format. It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend/device_{*device_id*}/dump** directory by default. You can use MindStudio Insight to display this file. For details, see [MindStudio Insight Memory Tuning](https://gitcode.com/Ascend/msinsight/blob/master/docs/en/user_guide/memory_tuning.md).|
|python_trace_{_TID_}_{_timestamp_}.csv|Result file of Python Trace collection. It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend/device_{*device_id*}/dump** directory by default. For details, see [python_trace_{_TID_}_{_timestamp}.csv](#python_trace_tid_timestampcsv).|
|config.json|Configuration file of custom collection via Python interfaces. It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend** directory by default.|
|leak_overview_{*stage*}.txt|Leak overview report of host heap memory leak detection. It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend/host_leak** directory by default. For details, see [leak_overview_{*stage*}.txt file description](#leak_overview_stagetxt-file-description).|
|block_detail_{*stage*}.csv|Leak code block detail file of host heap memory leak detection (generated only in event mode and when unfreed blocks exist in the window). It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend/host_leak** directory by default. For details, see [block_detail_{*stage*}.csv file description](#block_detail_stagecsv-file-description).|

## memscope_dump_{_timestamp_}.csv

[**Table 2** memscope_dump_{_timestamp_}.csv fields](#memscope_dump_{_timestamp_}.csv-fields) describes the fields in the memory leak detection result file.

**Table 2** memscope_dump_{_timestamp_}.csv fields<a id="memscope_dump_{_timestamp_}.csv-fields"></a>

|Field|Description|
|--|--|
|ID|Event ID.|
|Event|Event types recorded by msMemScope, including the following types:<br><ul>**SYSTEM**: system-level event.</ul><br><ul>**MALLOC**: memory allocation.</ul><br><ul>**FREE**: memory deallocation.</ul><br><ul>**ACCESS**: memory access.</ul><br><ul>**OP_LAUNCH**: operator execution.</ul><br><ul>**KERNEL_LAUNCH**: kernel execution.</ul><br><ul>**MSTX**: instrumentation.</ul><br><ul>**SNAPSHOT**: memory snapshot data.</ul><br><ul>**OOM_DETAIL**: detailed OOM analysis data.</ul><br><ul>**OOM_Snapshot**: memory snapshot data automatically recorded when an OOM occurs.</ul>|
|Event Type|Event subtypes.<br><ul>When **Event** is **SYSTEM**, **Event Type** includes **ACL_INIT** and **ACL_FINI**.</ul><br><ul>When **Event** is **MALLOC** or **FREE**, **Event Type** includes **HAL**, **PTA**, **MindSpore**, **ATB**, **HOST**, and **PTA_WORKSPACE**.</ul><br><ul>When **Event** is **ACCESS**, **Event Type** includes **READ**, **WRITE**, and **UNKNOWN**.</ul><br><ul>When **Event** is **OP_LAUNCH**, **Event Type** includes **ATEN_START**, **ATEN_END**, **ATB_START**, and **ATB_END**.</ul><br><ul>When **Event** is **KERNEL_LAUNCH**, **Event Type** includes **KERNEL_LAUNCH**, **KERNEL_START**, and **KERNEL_END**.</ul><br><ul>When **Event** is **MSTX**, **Event Type** includes **Mark**, **Range_start**, and **Range_end**.</ul><br><ul>When **Event** is **SNAPSHOT**, **Event Type** includes **SNAPSHOT**.</ul><br><ul>When **Event** is **OOM_DETAIL**, **Event Type** includes the following types:<br>1. **OOM_TRIGGER**: information about the operation that triggers the OOM.<br> 2. **OOM_RECENT_ALLOC**: the most recently allocated but not yet freed memory records sorted by time.<br> 3. **OOM_TOP_ALLOC**: the largest allocated but not yet freed memory records sorted by size.</ul>|
|Name|The value of **Name** depends on the value of **Event**. When the value of **Event** is any of the following, the value of **Name** has different meanings. If the value of **Event** is not one of the following values, the value of **Name** is **N/A**.<br><ul>**ACCESS**: The value of **Name** is the name or ID of the operator that triggers access.</ul><br><ul>**OP_LAUNCH**: The value of **Name** is the operator name.</ul><br><ul>**KERNEL_LAUNCH**: The value of **Name** is the kernel name.</ul><br><ul>**MSTX**: The value of **Name** is a user-defined instrumentation name.</ul><br><ul>**OOM_DETAIL**: The value of **Name** is **OOM_Trigger**, **OOM_RecentAlloc**, or **OOM_TopAlloc** depending on **Event Type**.</ul>|
|Timestamp(ns)|Time when an event occurs.|
|Process Id|Process ID.|
|Thread ID|Thread ID.|
|Device ID|Device information. If the ID is a number, it represents the NPU index. If the ID is set to **cpu**, it means CPU is used.|
|Ptr|Memory address, which can be used as the ID of a memory block. The lifecycle of a memory block is from **malloc** of the same **ptr** to the next **free**.|
|Attr|Event-specific attributes. Each event type has its own attribute. The specific display information is as follows:<br><ul> **When **Event** is **MALLOC** or **FREE**, the following parameters are displayed**:<br> 1. **allocation_id**: Same **allocation_id** indicates that the operations are performed on the same memory block.<br> 2. **addr**: address.<br> 3. **size**: size of the memory allocated or deallocated this time.<br> 4. **owner**: memory block owner. The format is "{A}@{B}@{C}..." when multi-level classification is used. This parameter is available only when **Event** is **MALLOC**.<br> 5. **used**: memory usage. Its meaning varies with **Event Type**:<br>   - When **Event Type** is **HAL**, it indicates the HAL-dimension memory usage of the current process (the cumulative HAL allocation/deallocation value collected by the tool).<br>   - When **Event Type** is **PTA**, **MindSpore**, **ATB**, or **PTA_WORKSPACE**, it indicates the allocated memory size in the memory pool.<br>   - When **Event Type** is **HOST** and **pinned:true** is marked in **Attr**, it indicates the collected pinned memory block usage.<br> 6. **total**: total size of the memory pool. This parameter is available when **Event Type** is **PTA**, **MindSpore**, **ATB**, or **PTA_WORKSPACE**; when **Event Type** is **HOST** and no **pinned:true** is marked in **Attr** (CPU tensor data), it indicates the cumulative active CPU tensor data memory.<br> 7. **process_used**: memory usage of the current process. Its meaning varies with **Event Type**:<br>   - When **Event Type** is **HAL**, it indicates the memory usage of the current process on this device, which is the same as Process memory(MB) of npu-smi info.<br>   - When **Event Type** is **PTA**, **MindSpore**, **ATB**, or **PTA_WORKSPACE**, the meaning is the same as that when **Event Type** is **HAL**.<br>   - When **Event Type** is **HOST** and **pinned:true** is marked in **Attr**, it indicates the physical memory usage of the current process (VmRSS).<br> 8. **device_used**: memory usage of the whole card. Its meaning varies with **Event Type**:<br>   - When **Event Type** is **HAL**, it indicates the memory usage of the whole card, which is the same as HBM-Usage(MB) of npu-smi info.<br>   - When **Event Type** is **PTA**, **MindSpore**, **ATB**, or **PTA_WORKSPACE**, the meaning is the same as that when **Event Type** is **HAL**.<br>   - When **Event Type** is **HOST**, this parameter does not exist.<br> 9. **inefficient**: whether the memory is inefficient. The value can be **early_allocation**, **late_deallocation**, or **temporary_idleness**. This parameter is available only when **Event** is **MALLOC** and **Event Type** is **PTA** or **ATB**.<br>**Note**: During program initialization, **process_used** and **device_used** may fail to be collected (the fields are omitted) because the device context has not been initialized yet. This is normal, and they can be collected normally after initialization is complete.</ul><br><ul>**When **Event** is **ACCESS**, the following parameters are displayed**:<br> 1. **dtype**: tensor dtype.<br> 2. **shape**: tensor shape.<br> 3. **size**: tensor size.<br> 4. **format**: tensor format.<br> 5. **type**: memory pool type like ATB.<br> 6. **allocation_id**: The same **allocation_id** indicates that operations are performed in the same memory. This parameter is available only when **Event Type** is **PTA**.</ul><br><ul>**When **Event** is **OP_LAUNCH** and **Event Type** is **ATB_START** or **ATB_END**, the following parameters are displayed**:<br> 1. **path**: operator location in the model, for example, **0_1967120/0/0_GraphOperation/0_ElewiseOperation**. The value contains PID, corresponding module name, and operator name.<br> 2. **workspace ptr**: start address of the workspace.<br> 3. **workspace size**: workspace size.</ul><br><ul>**When **Event** is **KERNEL_LAUNCH**, the following parameters are displayed**:<br> 1. **path**: kernel location in the model, for example, **0_1967120/1/0_GraphOperation/1_ElewiseOperation/0_AddF16Kernel/before**. The path contains PID, corresponding operator name, and kernel name. This parameter is available only when **Event Type** is **KERNEL_START** or **KERNEL_END**.<br> 2. **streamId**: stream ID.<br> 3. **taskId**: task ID.</ul><br><ul>**When **Event** is **SNAPSHOT**, the following parameters are displayed**:<br> 1. **total_mem**: total memory of the device.<br> 2. **free_mem**: total free memory of the device.<br> 3. **reserved**: total memory reserved for PyTorch.<br> 4. **peak_reserved**: peak memory reserved for PyTorch.<br> 5. **allocated**: memory used by PyTorch.<br> 6. **peak_allocated**: peak memory used by PyTorch.<br> 7. **device_utilization**: device memory usage.<br> 8. **pt_utilization**: memory usage reserved for PyTorch.</ul><br><ul>**When **Event** is **MALLOC** and **Event Type** is **HAL**, the following parameters are displayed**:<br> 1. **page_type**: The value can be **normal**, **huge**, or **giant**.<br> 2. **alloc_type**: The value can be **alloc** or **create**.</ul><br><ul>**When **Event** is **MALLOC** or **FREE** and **Event Type** is **HOST**, the following parameters are displayed**:<br> 1. **addr**: address.<br> 2. **size**: size of the memory allocated or deallocated this time.<br> The **HOST** type covers both offload pinned memory and CPU tensor data memory, which are distinguished by the **Attr** field:<br> - Offload pinned memory (marked with **pinned:true** in **Attr**):<br>   3. **pinned**: marked as **true**, indicating pinned memory.<br>   4. **used**: collected pinned memory block usage.<br>   5. **process_used**: physical memory usage of the current process (VmRSS).<br> - CPU tensor data memory (the **total** field is contained in **Attr** and **pinned:true** is not marked):<br>   3. **total**: cumulative active CPU tensor data memory.</ul><br><ul>**When **Event** is **OOM_DETAIL**, the following parameters are displayed**:<br> 1. When **Event Type** is **OOM_TRIGGER**: **func** indicates the name of the function that triggers the OOM, **req_size** indicates the requested memory size, **flag** indicates the request flag, and **ret** indicates the driver return code.<br> 2. When **Event Type** is **OOM_RECENT_ALLOC** or **OOM_TOP_ALLOC**: **pool** indicates the memory pool type, **ptr** indicates the memory address, **size** indicates the allocation size, **timestamp** indicates the allocation timestamp, **step** indicates the ID of the step to which the record belongs, **kernel** indicates the ID of the kernel to which the record belongs, and **client** indicates the client process ID. These records also contain Call Stack(Python) and Call Stack(C) information.</ul>|
|Call Stack(Python)|(Optional) Python call stack information.|
|Call Stack(C)|(Optional) C call stack information.|

## memory_compare_{_timestamp_}.csv

[**Table 3** memory_compare_{_timestamp_}.csv fields](#memory_compare_{_timestamp_}.csv-fields) describes the fields in the memory comparison result file.

**Table 3** memory_compare_{_timestamp_}.csv fields <a id="memory_compare_{_timestamp_}.csv-fields"></a>

|Field|Description|
|--|--|
|Event|Comparison event type recorded by msMemScope. The value can be **OP_LAUNCH** or **KERNEL_LAUNCH**.|
|Name|Kernel name|
|Device ID|Device type and ID|
|Base|Data in the first input file path|
|Compare|Data in the second input file path|
|Allocated Memory(Byte)|Memory changes before and after the kernel call. If the value is **N/A**, the kernel is not called.|
|Diff Memory(Byte)|Relative memory changes of **Base** and **Compare**.<br><ul>If the value is **0**, the memory changes caused by the kernel call are the same.</ul><br><ul>If the value is not **0**, the memory changes caused by the kernel call are different.</ul>|

## python_trace_{_TID_}_{_timestamp_}.csv

[**Table 4** python_trace_{_TID_}_{_timestamp_}.csv fields](#python_trace_{_TID_}_{_timestamp_}.csv_fields) describes the fields in the Python Trace collection result file.

**Table 4** python_trace_{_TID_}_{_timestamp_}.csv fields <a id="python_trace_{_TID_}_{_timestamp_}.csv_fields"></a>

|Field|Description|
|--|--|
|FuncInfo|Function name|
|StartTime(ns)|Start timestamp, which is the same as the event timestamp in the **memscope_dump_{_timestamp_}.csv** file.|
|EndTime(ns)|End timestamp|
|Thread Id|Thread ID|
|Process Id|Process ID|

## leak_overview_{_stage_}.txt File Description

Leak overview report file of host heap memory leak detection, generated after the detection window is closed (output in both summary and event modes). The file is saved in the `msmemscope_{*PID*}_{_timestamp_}_ascend/host_leak` directory, where `{stage}` is the window sequence number.

The report is organized by sections, as shown in [**Table 5** leak_overview_{_stage_}.txt report section description](#leak_overview_stagetxt-report-section-description).

**Table 5** leak_overview_{_stage_}.txt report section description<a id="leak_overview_stagetxt-report-section-description"></a>

|Section|Content|
|--|--|
|Data Health Analysis|Truthfully marks the tracing decisions of this window. It includes the start/end time and duration of the detection window, the reporting mode, the total allocation/deallocation counts, the number of distinct call stacks (including the ledger key depth K and the class attribution semantics), the number of unattributed blocks and their proportion, the dead stack eviction statistics (output only when it occurs), the truncation marks (bit0 = full block table, migrated to the overflow channel; bit1 = full stack table full, new stacks migrated to the unknown bucket when dead stack reclamation has nothing to supply; bit2 = full overflow ledger, accounting stopped; only bit2 means incomplete data), the overflow channel diversion and reverse correction statistics, the pre-window free statistics, the sampling rate (a sampled view is marked when the rate is not 1), the block size threshold and untracked statistics, and the symbolization coverage (the number of unresolved stacks). When statistics are unavailable, `Snapshot: unavailable` is marked.|
|Total Unfreed|Total number of bytes and blocks allocated but not freed within this window (including surviving blocks in the unknown bucket and the overflow channel), together with the average and maximum values.|
|Unfreed Block Size Distribution|Number of blocks, bytes, and proportion of unfreed blocks bucketed by size. 7 buckets by default: 0–256 B, 256 B–1 KB, 1 KB–4 KB, 4 KB–32 KB, 32 KB–256 KB, 256 KB–1 MB, and above 1 MB.|
|Pre-Window Free Size Distribution|Number of frees and the number of bytes, bucketed by size, of memory that was allocated before the window was opened and freed during the window.|
|TOP N Leak Sites|A list of leak sites sorted in descending order of the leak suspicion index (LSI, 0–100), with N=10 by default and a maximum of 1024. Each row contains the status category (suspected_leak/growth_watch, with /turnover appended for the turnover pattern), the LSI value, the Growth/Release/Lifetime/Pattern/Scale factors, the unfreed/allocation/deallocation statistics, the series growth slope and the number of beats, the degradation mark, and the complete symbolized call stack text. The first line of the section outputs the beat series metadata (top-k=256, max-stacks=1024, the number of beats, and the duration). The unknown bucket row is `(unknown bucket: unattributed blocks)`, and a missing stack text is marked as `(unresolved stack <stack id>)`.|
|Resident Baselines|A list of resident stacks sorted in descending order of the unfreed amount. It does not occupy a TOP N slot, and the criterion source is marked as resident/early or resident/turnover. The closed-window report is not truncated. When pre-window frees account for more than half of the total allocations within the window, the report additionally outputs a cache turnover NOTE.|

### Content Example

**leak_overview_{_stage_}.txt** is a plain text report (not a table file). The following is a report content example for the event mode and a single detection window (the values are for reference, not real collected data):

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

Notes:

- **Header and Data Health Analysis**: The first line `====== Host Leak Overview: stage=1, pid=1234 ======` identifies the window sequence number and the process ID; the `Window` line gives the start and end times of the window and its duration; `Mode` is the reporting mode (event/summary); the `Tracked` line is the total allocations and deallocations accounted for within the window; `Distinct stacks` is the number of distinct stacks (key depth: K=20; class attribution semantics); and `Symbolized` is the symbolization coverage. When truncation, sampling, dead stack eviction, or overflow channel situations occur, this subsection appends the corresponding mark lines (output only when they occur), for example:
  - `Truncated: block table full (allocations -> overflow channel)`: the block table is full and allocations are diverted to the overflow channel (only the attribution granularity degrades).
  - `Truncated: stack table (dead-stack recycling active, no reclaimable stack at full)`: the stack table is full and dead stack recycling has no supply, so new stacks are put into the unknown bucket (only the attribution granularity degrades).
  - `Truncated: overflow channel full (recording stopped at N live blocks) [window data incomplete: not a leak conclusion]`: the overflow channel is also full and accounting stops; the window data is incomplete.
  - `Evicted: N stacks recycled (M allocs / B bytes folded to unknown bucket)`: dead stacks are recycled when the stack table is full, and the folded amount is merged into the unknown bucket.
  - `Overflow channel: N allocations / B bytes diverted; M freed / C B (reverse-corrected)`: statistics of allocations diverted to the overflow channel and of the reverse correction of deallocations when the block table is full.
  - `Pre-window frees: N / B bytes (allocated before window, not in ledger)`: statistics of the independent channel for memory that was allocated before the window was opened and freed during the window.
  - `Sampling: 1/4 (sampled view)`: sampled view (when the reciprocal of the sampling rate is greater than 1).
  - `Size threshold: 1024B (untracked: N allocations / M B)`: block size threshold filtering (when the threshold is greater than 0).
  - `Snapshot: unavailable (host hook not bound / query failed)`: statistics unavailable (for example, an extremely early exit).
  When the three truncation bits are set at the same time, the corresponding marks are joined into one line with ` | ` (when bit2 is set, `[window data incomplete: not a leak conclusion]` is appended to the end of the line).
- **Total Unfreed**: One line gives the total unfreed amount within the window (blocks/bytes/average/maximum); exact values are not abbreviated.
- **Unfreed Block Size Distribution**: 7 buckets by default, with the header `range/blocks/bytes/% of total`; the upper limit of the last bucket is rendered as `+inf`; percentages are truncated to integers (the sum of the percentages of multiple buckets may be less than 100%); when there are no unfreed blocks, `(no unfreed blocks)` is output.
- **Pre-Window Free Size Distribution**: An independent channel for pre-window frees, with the header `range/frees/bytes`; when there are no records, `(no pre-window frees)` is output.
- **TOP N Leak Sites**: The `Series` line is the beat series metadata (top-k=256, max-stacks=1024, the number of beats, and the window duration); each entry has a 4-line structure — the first line is `sequence number. [status] LSI value + five assessment factors + unfreed/alloc ratio`, the second line is the unfreed/allocation/deallocation statistics, the third line is the growth slope (B/beat), the number of valid beats, the degradation reason (`-` when there is no degradation), and the stack ID, followed by the complete symbolized stack text (indented frame by frame). When the series is degraded (`no_series`/`series_evicted`/`insufficient_series`/`no_warmup_thread`), the `Growth`/`Pattern` factors are displayed as `-` (`Release`/`Lifetime`/`Scale` remain numeric); a missing stack text is marked with an unresolved-stack placeholder.
- **Resident Baselines**: The header line marks the number of resident stacks, the total unfreed amount, and the criterion source (G≈0 and early-allocated, or the turnover pattern); the entry structure is the same as that of TOP N (differences: the `Pattern` factor is omitted and no TOP N slot is occupied); the closed-window report is not truncated.
- **NOTE line**: When the pre-window free bytes exceed half of the total allocations within the window, the report outputs a cache turnover NOTE at the end (for example, `NOTE: 64KiB pre-window allocations were freed within this window (cache turnover pattern); ...`), to remind you to interpret the resident classification together with the unfreed/alloc ratio.

> [!NOTE]
>
> The section structure of the interim overview during the window (queried through the out-of-process control channel `display host_leak summary`) is the same as above. For the differences (interim marks, frozen marks, resident truncation, and no persistence to disk), see [Interim Overview Within the Window (Inspection)](./memory_analysis.md#interim-overview-within-the-window-inspection).

## block_detail_{_stage_}.csv File Description

Leak code block detail file of host heap memory leak detection, generated only when `--host-leak-mode=event` and unfreed blocks exist in the window. The file is saved in the `msmemscope_{*PID*}_{_timestamp_}_ascend/host_leak` directory and corresponds to the overview report with the same window number.

The file is in CSV format and its first line is the header `addr,size,alloc_ts,Call Stack(C),Call Stack(Python)`. Each block occupies one row and blocks are sorted in descending order of size (blocks of the same size are sorted in ascending order of address). The fields are shown in [**Table 6** block_detail_{_stage_}.csv file fields and meanings](#block_detail_stagecsv-file-fields-and-meanings).

**Table 6** block_detail_{_stage_}.csv fields<a id="block_detail_stagecsv-file-fields-and-meanings"></a>

|Field|Meaning|Format|
|--|--|--|
|`addr`|Start address of the unfreed block|`0x` prefix plus 16-digit lowercase hexadecimal zero padding.|
|`size`|Block size (bytes)|Decimal.|
|`alloc_ts`|Allocation timestamp within the window (nanoseconds)|Decimal.|
|`Call Stack(C)`|C call stack text (from the top frame to the root frame, with frames separated by line breaks); for a mixed stack (C+Python), this is the pure C stack part before the stack marker|Double-quoted string (RFC 4180 quoted field; embedded line breaks are preserved and an inner quote is escaped by doubling it). When the stack text is missing, `(unresolved stack)` is written; for the unknown bucket (blocks beyond the stack table limit), `(unknown bucket: unattributed blocks)` is written.|
|`Call Stack(Python)`|Python frame text (the Python frame part after the marker in a mixed stack)|Same format as the C column. For stacks for which no Python frame is collected (pure C stacks or placeholder lines), this column is empty.|

> [!NOTE]
>
> - The detail file covers only the block table scope. Overflow channel blocks (diverted when the block table is full and without stack attribution) do not enter the detail file. The `Total Unfreed` in the overview report includes the surviving blocks in the overflow channel, and the difference between the two is the number of overflow channel blocks.
> - The call stack text is inline and self-contained per block (the C and Python columns) and can be parsed independently without relying on the overview report of the same window.
