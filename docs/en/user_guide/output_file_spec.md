# **Output File Specifications**

## **Overview**

msMemScope outputs the following files after it finishes memory analysis.

**Table 1** Output files <a id="output-file-description -1"></a>

|Output File|Description|
|--|--|
|memscope_dump_{_timestamp_}.csv|Memory information result file during memory analysis. It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend/device_{*device_id*}/dump** directory by default. For details, see [memscope_dump_{_timestamp_}.csv](#memscope_dump_timestampcsv).|
|memory_compare_{*timestamp*}.csv|Memory comparison result file during memory analysis, which records baseline memory, memory comparison information, and differences after comparison. It is saved in the **memscopeDumpResults/compare** directory by default. For details, see [memory_compare_{_timestamp_}.csv](#memory_compare_timestampcsv).|
|memscope_dump_{_timestamp_}.db|Memory information file in .db format. It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend/device_{*device_id*}/dump** directory by default. You can use MindStudio Insight to display this file. For details, see [MindStudio Insight Memory Tuning](https://gitcode.com/Ascend/msinsight/blob/master/docs/en/user_guide/memory_tuning.md).|
|python_trace_{_TID_}_{_timestamp_}.csv|Result file of Python Trace collection. It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend/device_{*device_id*}/dump** directory by default. For details, see [python_trace_{_TID_}_{_timestamp}.csv](#python_trace_tid_timestampcsv).|
|config.json|Configuration file of custom collection via Python interfaces. It is saved in the **msmemscope_{*PID*}_{_timestamp_}_ascend** directory by default.|

## memscope_dump_{_timestamp_}.csv

[**Table 2** memscope_dump_{_timestamp_}.csv fields](#memscope_dump_{_timestamp_}.csv-fields) describes the fields in the memory leak detection result file.

**Table 2** memscope_dump_{_timestamp_}.csv fields<a id="memscope_dump_{_timestamp_}.csv-fields"></a>

|Field|Description|
|--|--|
|ID|Event ID.|
|Event|Event types recorded by msMemScope, including:<br> - **SYSTEM**: system-level event<br> - **MALLOC**: memory allocation<br> - **FREE**: memory deallocation<br> - **ACCESS**: memory access<br> - **OP_LAUNCH**: operator execution<br> - **KERNEL_LAUNCH**: kernel execution<br> - **MSTX**: instrumentation<br> - **SNAPSHOT**: memory snapshot data|
|Event Type|Event subtypes.<br> - When **Event** is **SYSTEM**, **Event Type** includes **ACL_INIT** and **ACL_FINI**.<br> - When **Event** is **MALLOC** or **FREE**, **Event Type** includes **HAL**, **PTA**, **MindSpore**, **ATB**, **HOST**, and **PTA_WORKSPACE**.<br> - When **Event** is **ACCESS**, **Event Type** includes **READ**, **WRITE**, and **UNKNOWN**.<br> - When **Event** is **OP_LAUNCH**, **Event Type** includes **ATEN_START**, **ATEN_END**, **ATB_START**, and **ATB_END**.<br> - When **Event** is **KERNEL_LAUNCH**, **Event Type** includes **KERNEL_LAUNCH**, **KERNEL_START**, and **KERNEL_END**.<br> - When **Event** is **MSTX**, **Event Type** includes **Mark**, **Range_start**, and **Range_end**.|
|Name|The value of **Name** depends on the value of **Event**. When the value of **Event** is any of the following, the value of **Name** has different meanings. If the value of **Event** is not one of the following values, the value of **Name** is **N/A**.<br> - **ACCESS**: The value of **Name** is the name or ID of the operator that triggers access.<br>- **OP_LAUNCH**: The value of **Name** is the operator name.<br> - **KERNEL_LAUNCH**: The value of **Name** is the kernel name.<br> - **MSTX**: The value of **Name** is a user-defined instrumentation name.|
|Timestamp(ns)|Time when an event occurs.|
|Process Id|Process ID.|
|Thread ID|Thread ID.|
|Device ID|Device information.|
|Ptr|Memory address, which can be used as the ID of a memory block. The lifecycle of a memory block is from **malloc** of the same **ptr** to the next **free**.|
|Attr|Event-specific attributes. Each event type has its own attribute. The specific display information is as follows:<br> - When **Event** is **MALLOC** or **FREE**, the following parameters are displayed:<br> 1. **allocation_id**: Same **allocation_id** indicates that the operations are performed on the same memory block.<br> 2. **addr**: address<br> 3. **size**: size of the memory allocated or deallocated this time<br> 4. **owner**: memory block owner. The format is "{A}@{B}@{C}..." when multi-level classification is used. This parameter is available only when **Event** is **MALLOC**.<br> 5. **total**: total size of the memory pool. This parameter is available only when **Event Type** is **PTA**, **MindSpore**, or **ATB**.<br> 6. **used**: total size of secondary allocation of the memory pool. This parameter is available only when **Event Type** is **PTA**, **MindSpore**, or **ATB**.<br> 7. **inefficient**: whether the memory is inefficient. The value can be **early_allocation**, **late_deallocation**, or **temporary_idleness**. This parameter is available only when **Event** is **MALLOC** and **Event Type** is **PTA** or **ATB**.<br> - When **Event** is **ACCESS**, the following parameters are displayed:<br> 1. **dtype**: tensor dtype<br> 2. **shape**: tensor shape<br> 3. **size**: tensor size<br> 4. **format**: tensor format<br> 5. **type**: memory pool type like ATB<br> 6. **allocation_id**: The same **allocation_id** indicates that operations are performed in the same memory. This parameter is available only when **Event Type** is **PTA**.<br> - When **Event** is **OP_LAUNCH** and **Event Type** is **ATB_START** or **ATB_END**, the following parameters are displayed:<br> 1. **path**: operator location in the model, for example, **0_1967120/0/0_GraphOperation/0_ElewiseOperation**. The value contains PID, corresponding module name, and operator name.<br> 2. **workspace ptr**: start address of the workspace<br> 3. **workspace size**: workspace size.<br> - When **Event** is **KERNEL_LAUNCH**, the following parameters are displayed:<br> 1. **path**: kernel location in the model, for example, **0_1967120/1/0_GraphOperation/1_ElewiseOperation/0_AddF16Kernel/before**. The path contains PID, corresponding operator name, and kernel name. This parameter is available only when **Event Type** is **KERNEL_START** or **KERNEL_END**.<br> 2. **streamId**: stream ID<br> 3. **taskId**: task ID<br> - When **Event** is **SNAPSHOT**, the following parameters are displayed:<br> 1. **total_mem**: total memory of the device<br> 2. **free_mem**: total free memory of the device<br> 3. **reserved**: total memory reserved for PyTorch<br> 4. **peak_reserved**: peak memory reserved for PyTorch<br> 5. **allocated**: memory used by PyTorch<br> 6. **peak_allocated**: peak memory used by PyTorch<br> 7. **device_utilization**: device memory usage<br> 8. **pt_utilization**: memory usage reserved for PyTorch<br> - When **Event** is set to **MALLOC** and **Event Type** is set to **HAL**, the following parameters are displayed:<br> 1. **page_type**: The value can be **normal**, **huge**, or **giant**.<br> 2. **alloc_type**: The value can be **alloc** or **create**.|
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
|Allocated Memory(byte)|Memory changes before and after the kernel call. If the value is **N/A**, the kernel is not called.|
|Diff Memory(byte)|Relative memory changes of **Base** and **Compare**.<br> - If the value is **0**, the memory changes caused by the kernel call are the same.<br> - If the value is not **0**, the memory changes caused by the kernel call are different.|

## python_trace_{_TID_}_{_timestamp_}.csv

[**Table 4** python_trace_{_TID_}_{_timestamp_}.csv fields](#python_trace_{_TID_}_{_timestamp_}.csv fields) describes the fields in the Python Trace collection result file.

**Table 4** python_trace_{_TID_}_{_timestamp_}.csv fields <a id="python_trace_{_TID_}_{_timestamp_}.csv fields"></a>

|Field|Description|
|--|--|
|FuncInfo|Function name|
|StartTime(ns)|Start timestamp, which is the same as the event timestamp in the **memscope_dump_{_timestamp_}.csv** file.|
|EndTime(ns)|End timestamp|
|Thread Id|Thread ID|
|Process Id|Process ID|
