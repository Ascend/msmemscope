# **Memory Collection**

## Overview

msMemScope supports memory event collection and allows custom memory collection scope and items to accurately collect key data for subsequent analysis.

- Collection via Python APIs: The collection scope and items are configured through Python APIs to collect memory events and Python Trace events.
- Collection via CLI: Collection parameters are configured through the CLI to collect memory events.
- Collection via mstx instrumentation: Memory events can be collected by enabling mstx instrumentation, together with C and Python scripts.

## Before You Start

For details about how to install msMemScope, see [msMemScope Installation Guide](../install_guide/install_guide.md).

## Collection via Python APIs

### Overview

Python APIs can be used to collect memory events and Python Trace events, while allowing to customize the memory collection scope and parameters for precise collection and efficient analysis.

### Precautions

- Environment variables set in export mode take effect only in the current window. If you do not need to use msMemScope after setting environment variables, you are advised to restore **LD_PRELOAD** and **LD_LIBRARY_PATH** to the previous settings.
- If **events** is set to **traceback**, Python Trace events are collected. After this function is enabled, a .csv file (**python_trace_{*TID*}_{*timestamp*}.csv**) is flushed to the drive. For details, see [Output File Specifications](./output_file_spec.md).
- To disable a collection item, leave the value of the collection item empty. For example, to disable Python Trace collection, set **events** to **""**.
- Python APIs can be used to customize and set multiple collection scopes.

### Usage Example

**Collection via Python APIs**

1. Set environment variables.

    Run the following commands to set **LD_PRELOAD** and **LD_LIBRARY_PATH**.

    ```shell
    export LD_PRELOAD=${memscope_install_path}/lib64/{so_name}:${memscope_install_path}/lib64/{so_name}
    export LD_LIBRARY_PATH=${memscope_install_path}/lib64/:${LD_LIBRARY_PATH}
    ```

    > [!NOTE]
    >
    > Setting the `LD_PRELOAD` environment variable may conflict with other tools in the MindStudio series. Therefore, after you have finished using msMemScope, please promptly clear this environment variable using the command `unset LD_PRELOAD`.

    For details about the parameters, see [**Table 1** Parameter description](#parameter-description).

    **Table 1** Parameter description <a id="Parameter description "></a>

    |Parameter|Description|
    |--|--|
    |memscope_install_path|Installation path of msMemScope|
    |so_name|Name of the SO package to be configured. SO packages are separated by half-width colons (:). The SO packages to be configured include **libascend_kernel_hook.so**, **libascend_mstx_hook.so**, **libatb_abi_0_hook.so**, **libatb_abi_1_hook.so**, and **libleaks_ascend_hal_hook.so**.|
    |LD_LIBRARY_PATH|Environment variable|

2. Collect the memory.

    Run the following sample code to collect memory events. Note that you need to set `msmemscope.config` as required. Set **device**, **level**, **events**, **call_stack**, **analysis**, **watch**, **output**, and **data_format** as required. For details about the parameters, see [Collection via CLI](#collection-via-cli).

    ```python
    import msmemscope

    msmemscope.config(call_stack="c:10,python:5", events="launch,alloc,free", level="0", device="npu", analysis="leaks,decompose", watch="op0,op1,full-content", data_format="db", output="/home/projects/output")
    msmemscope.start()   # Start collection.
    train()              # train() is the user code.
    msmemscope.stop()    # Stop collection.
    ```

    > [!NOTE]
    > 
    > OOM usually occurs in the memory collection scope. Once OOM occurs, the snapshot information before and after OOM is flushed to the drive. For details about the flushed information, see [memscope_dump_{timestamp}.csv](./output_file_spec.md#memory_compare_{_timestamp_}.csv-fields) in *Output File Specifications*. If **Event** is **SNAPSHOT**, check the **Attr** and **Call Stack** fields.

**Python Trace Collection**

- Default collection mode

    msMemScope can collect Trace data of Python code through Python APIs and align the data with memory events on a unified timeline. This helps optimization personnel quickly associate memory events with full-link code and accurately locate problems.

    > [!NOTE]
    > 
    > The Python Trace collection will be removed from MindStudio 26.0.0. You can set **events="traceback"** to collect Python Trace events. For details, see [Collection via Python APIs](#collection-via-python-apis).

    1. Python APIs are added to msMemScope to enable and disable the **Tracer** function. Python code executed between **start** and **stop** will have its Trace data written to the specified path. The code example is as follows:

        ```python
        import msmemscope

        msmemscope.tracer.start()  # Enable Tracer.
        train()                   # train() is the user code.
        msmemscope.tracer.stop()   # Disable Tracer.
        ```

    2. After the execution is complete, a file named **python_trace_{*TID*}_{*timestamp*}.csv** is generated. For details about the file, see [Output File Specifications](./output_file_spec.md).

- Customized collection mode

    msMemScope allows you to customize Trace events through Python APIs. That is, you can call APIs to customize Trace events and focus on core code or code blocks to avoid flushing all Trace events to the drive, improving data collection efficiency. You can use `msmemscope.RecordFunction` to customize Trace events in context (marking a function) or decorator (marking a code block) mode.

    1. Use `msmemscope.RecordFunction` to flush customized Trace event data to the drive. The code example is as follows:

        ```python
        # Code block marking (context mode)
        import msmemscope
        with msmemscope.RecordFunction("forward_pass"):
            output = model(input_data)

        # Function marking (decorator mode)
        import msmemscope
        @msmemscope.RecordFunction("forward_pass")
        def forward_pass(data):
            return model(data)
        ```

    2. Make sure the flush path of the custom Trace data is the same as that of the default Trace data. For details, see [Output File Specifications](./output_file_spec.md).

**Memory Snapshot Collection**

The snapshot information of the memory allocator in the current system can be collected, such as the total free memory and current free memory of the device.

You can enable memory snapshot collection in automatic mode (described in the following part) or one-click mode ([One-Click Analysis](./memory_analysis.md#one-click-analysis)). Memory snapshot collection applies to the following scenarios.

|Scenario|Description|
|----|-----|
|Training|Only the method described in this section can be used to enable memory snapshot collection.|
|Inference|The one-click analysis is supported to enable memory snapshot collection for the vLLM inference framework and record the memory usage of **load_weight**, **profile_run**, **kv_cache**, and **activate** during inference.|
|Reinforcement learning|Reinforcement learning (verl) involves two phases: inference and training. Currently, the one-click analysis function can be used to enable memory snapshot collection only during the inference phase. During training, you can enable memory snapshot collection as instructed in this section.|

Run the following sample code to collect memory snapshots.

You can set parameters for `msmemscope.take_snapshot` as required. For details about the supported parameters, see [Table 2 Snapshot collection parameters](#snapshot-collection-parameters).

```python
import msmemscope

msmemscope.take_snapshot(device_mask=0)   # Collect a memory snapshot.
```

**Table 2** Snapshot collection parameters <a id="snapshot-collection-parameters"></a>

| Parameter| Description|
| ----- | ----- |
|device_mask|Specifies a device. The default value is **NONE**, indicating that the memory usage of all devices is collected. The following formats are supported:<br> - **num**: collects information about a device mask, for example, `msmemscope.take_snapshot(device_mask=0)`.<br> - **list**: collects information about multiple device masks, for example, `msmemscope.take_snapshot(device_mask=[0, 1])`.<br> - **tuple**: collects information about multiple device masks, for example, `msmemscope.take_snapshot(device_mask=(0, 1))`.|
|name|Specifies the name of a collection event, for example, `msmemscope.take_snapshot(name="test_tuple")`. The default value is **Memory Snapshot**.|

After the collection is complete, the result is flushed to the **memscope_dump_{_timestamp_}.csv** file.

> [!NOTE]
> 
> - `msmemscope.take_snapshot` can be called independently to collect data, without depending on `msmemscope.start` and `msmemscope.stop`.
> - `msmemscope.take_snapshot` can be used together with `msmemscope.config`. When they are used together, the path for saving the result file is the value of the first call and does not change.

**Step Collection**

You can add Python APIs to collect step information. This method is recommended in Python scenarios.
The sample code is as follows:

```python
import msmemscope

msmemscope.config()
msmemscope.start()  # Start collection.
for i in range(10):
    train()      # train() is the user code.
    msmemscope.step() # Enter step information.
msmemscope.stop()  # Stop collection.
```

### Output Description

For details about the memory collection result, see [Output File Specifications](./output_file_spec.md).

## Collection via CLI

### Overview

In non-Python scenarios, memory collection and analysis can be performed via CLI.

### Precautions

- The environment variable **TASK_QUEUE_ENABLE** can be configured as required. For details, see [TASK_QUEUE_ENABLE](https://gitcode.com/Ascend/pytorch/blob/v2.7.1-26.0.0/docs/en/environment_variable_reference/TASK_QUEUE_ENABLE.md). When **TASK_QUEUE_ENABLE** is set to **2**, the level-2 optimization of the **task_queue** operator dispatch queue is enabled. At this time, workspace will be collected.
- When you run msMemScope as the user **root**, the system skips file permission verification by printing a message, which poses security risks. You are advised to run msMemScope as a common user.
- When using msMemScope to collect memory data, you are advised to customize collection items. For details, see [Collection via Python APIs](#collection-via-python-apis).
- The CLI-based collection mode does not apply to vLLM-Ascend.

### Syntax

Refer to the following to start msMemScope and collect memory data.

- Method 1 (recommended)

        ```shell
        msmemscope [options] bash user.sh
        ```

- Method 2

        ```shell
        msmemscope [options] -- <prog_name> [prog_options]
        ```

### Parameter Description

**Table 3** Command parameters

|Parameter|Description|
|--|--|
|options|Command options. See Table 4.|
|prog_name|User script name. Ensure the security of the custom script. This parameter is not required when memory comparison is enabled.|
|prog_options|User script parameter. Ensure the security of the custom script parameter. This parameter is not required when memory comparison is enabled.|

**Table 4** Parameters

|Parameter|Optional/Required|Description|
|--|--|--|
|`--help, -h`|Optional|Outputs msMemScope help information.|
|`--version, -v`|Optional|Outputs msMemScope version information.|
|`--steps`|Optional|Selects the step IDs for which to collect memory information. Must be configured as integers within the actual step range. One or more steps can be configured, with a maximum of 5 currently supported. Input step IDs should be separated by commas (either full-width or half-width). If this parameter is not set, memory information for all steps is collected by default. Example: `--steps=1,2,3`.|
|`--device`|Optional|Specifies the device(s) from which to collect information. Options include `npu`, `npu:{id}`, and `cpu`. The default value is `npu`. The value cannot be empty. Multiple devices can be selected simultaneously, separated by commas (either full-width or half-width). Example: `--device=npu`. <br>- If both `npu` and `npu:{id}` are included, the default behavior is to collect information from all NPUs, and `npu:{id}` will not take effect.<br> - `npu`: Collects memory information from all NPUs.<br> - `npu:{*id*}`: Collects memory information from the specified NPU, where `id` is a valid NPU index, in the range of [0, 31]. Multiple NPUs can be specified, separated by commas (either full-width or half-width). Example: `--device=npu:2,npu:7`. <br> - `cpu`: Collects CPU memory information. Currently only pinned memory information is supported.|
|`--level`|Optional|Specifies the level of operator information to collect. Options are `0` and `1`, with a default value of `0`. Example: `--level=0`.<br> - `0`: Can also be written as `op`. Collects operator-level information.<br> - `1`: Can also be written as `kernel`. Collects kernel-level information.<br>In MindStudio 9.0.0, the numeric values `0` and `1` for the `--level` parameter will be deprecated and replaced with `op` and `kernel`.|
|`--events`|Optional|Specifies the events to collect. Options include `alloc`, `free`, `launch`, and `access`. The default value is `alloc,free,launch`. Multiple values should be separated by commas (either full-width or half-width). Example: `--events=alloc,free,launch`. <br> - `alloc`: Collects memory allocation events.<br>- `free`: Collects memory free events.<br> - `launch`:Collects operator/kernel launch events. <br> - `access`: Collects memory access events. Currently only supports ATB and Ascend for PyTorch operator scenarios.<br> - `traceback`: Collects Python Trace events. <br>**Notes**:<br>1. When `--events=alloc` is set, `free` is automatically added, so the actual collection includes both `alloc` and `free`. When `--events=free` is set, `alloc` is automatically added, so the actual collection includes both `alloc` and `free`. When `--events=access` is set, `alloc` and `free` are automatically added, so the actual collection includes `access`, `alloc`, and `free`.<br>2. `--events=traceback` is only supported via the API and cannot be used in the command-line environment.|
|`--call-stack`|Optional|Specifies the call stack to collect. Options include `python` and `c`, and both can be selected simultaneously, separated by commas (either full-width or half-width). The collection depth can be set by appending a number after the option, separated by a colon. The range is [0, 1000], with a default value of `50`. Example: `--call-stack=python, --call-stack=c:20,python:10`.<br> - `python`: Collects Python call stacks.<br> - `c`: Collects C call stacks.|
|`--collect-mode`|Optional|Specifies the memory collection mode. Options are `immediate` and `deferred`, with a default value of `immediate`. Only one value can be selected. Example: `--collect-mode=immediate`.<br> - `immediate`: Immediate collection. Memory information is collected from the start of the user script until it finishes. Can also be used with Python custom collection interfaces to control the collection scope.<br> - `deferred`:Custom collection. Must be used with Python custom collection interfaces. Collection starts only after `msmemscope.start()` is called. If `--collect-mode=deferred` is set without using Python custom collection interfaces, no data (except for a small amount of system data) is collected by default.|
|`--analysis`|Optional|Enables memory analysis features. The default value is `leaks`. If the value is empty, no analysis is enabled. Multiple values can be selected, separated by commas (either full-width or half-width). Example: `--analysis=leaks,decompose`. <br> - `leaks`: Identifies memory leak events.<br> - `inefficient`: Identifies inefficient memory usage. Supports ATB LLM and Ascend for PyTorch single-operator scenarios. Inefficient memory identification can be configured via the API. For details, see [API Reference](../api_reference/api.md).<br> - `decompose`:Enables memory decomposition.<br>Note: When `--analysis=leaks` or `--analysis=decompose` is set, `alloc` and `free` are automatically added to `--events` (i.e., `--events=alloc,free`). When `--analysis=inefficient` is set, `alloc`, `free`, `access`, and `launch` are automatically added (i.e., `--events=alloc,free,access,launch`).|
|`--data-format`|Optional|Specifies the output file format. Options are `db` and `csv`. Choose one format based on requirements. The value cannot be empty. The default value is `csv`. Example: `--data-format=db`.<br> When the output format is `db`, the data can be viewed using the MindStudio Insight tool. See [MindStudio Insight Memory Tuning](https://gitcode.com/Ascend/msinsight/blob/26.1.0/docs/en/user_guide/memory_tuning.md). <br> - `db`: Outputs a `.db` file. <br> - `csv`:Outputs a `.csv` file.|
|`--watch`|Optional|Monitors memory blocks. Options include `start`, `out{id}`, `end`, and `full-content`. Multiple options can be selected, with `end` being required. Values should be separated by commas (either full-width or half-width). Format: `--watch=start:out{id},end,full-content`. Example: `--watch=op0,op1,full-content`.<br> - `start`:Optional, string format representing an operator. The format varies across frameworks. Required when `out{id}` needs to be set.<br> - `out{id}`: Optional, represents the output index of the operator. When a tensor is a list, this specifies which tensor in the list to dump. The value is the index of the tensor in the list.<br> - `end`:Required, string format representing an operator. The format varies across frameworks. <br> - `full-content`: Optional. If selected, the full tensor data is dumped. If not selected, only the hash value of the tensor is dumped.|
|`--output`|Optional|Specifies the output file dump path. The maximum path length is 4096 characters. The default dump directory is `memscopeDumpResults`. Example: `--output=/home/projects/output`.|
|`--log-level`|Optional|Specifies the log level. Options are `info`, `warn`, and `error`. The default value is `warn`.|
|`--compare`|Optional|Enables memory data comparison between steps. This parameter is required only when using the memory comparison feature.|
|`--input`|Optional|pecifies the absolute directory path(s) for comparison files. Both the baseline file path and the comparison file path must be provided, separated by commas (either full-width or half-width). This parameter is only valid when the `compare` feature is enabled. The maximum path length is 4096 characters. Example: `--input=/home/projects/input1,/home/projects/input2`. <br> This parameter is required only when using the memory comparison feature.|

> [!NOTE]
> 
> - When **--events** is set to **launch** and Aten operator dispatch and access events need to be collected, this function can be used only when the PyTorch version under Ascend for PyTorch framework is 2.3.1 or later.
> - If **--analysis** contains **decompose**, the **Attr** parameter in the **memscope\_dump\_\{_timestamp_\}.csv** file contains the memory type and component name.
> - If **--analysis** contains **decompose**, memory decomposition is enabled. Currently, the memory pools of Ascend for PyTorch, MindSpore, and ATB operator frameworks can be classified, and the memory pools of MindSpore framework and ATB operator frameworks do not support fine-grained classification. In the Ascend for PyTorch framework, **aten**, **weight, gradient**, and **optimizer_state** can be classified finely. **weight**, **gradient**, and **optimizer_state** are used only in PyTorch training scenarios (that is, the **optimizer.step\(\)** API call scenario). **aten** is the memory allocated in Aten operators. The PyTorch version must be 2.3.1 or later, and the value of **--level** must contain **0**.
> - When **--level=1** is specified and the tokenizers library of Hugging Face is used, the alarm **"The current process just got forked. Parallelism is disabled."** may be reported. This alarm does not affect functions and can be ignored. To avoid this alarm, run **export TOKENIZERS\_PARALLELISM=false** to disable the parallelism behavior.
> - If **--collect-mode** is set to **deferred** and Python APIs are used to collect data, memory analysis is unavailable. The memory block monitoring, memory decomposition, and identification of inefficient memory functions are available only for the data within the collection scope.
> - MindStudio Insight can display only memory data files in .db format. For details about basic operations, see [MindStudio Insight Basic Operations](https://gitcode.com/Ascend/msinsight/blob/26.1.0/docs/en/user_guide/basic_operations.md).

### Output Description

For details about the memory collection result, see [Output File Specifications](./output_file_spec.md).

## Collection via mstx Instrumentation

### Overview

msMemScope can collect memory data based on the mstx instrumentation capability. It also marks instrumentation locations in visual Trace, allowing you to identify problematic code lines quickly.

### Precautions

- The mstx instrumentation methods vary slightly for C and Python scripts. For details, see [MindStudio Tools Extension Library Interfaces](https://gitcode.com/Ascend/mstx/blob/26.1.0/docs/en/api_reference/README.md).
- You are advised to refer to the C script example for mstx instrumentation.

### Usage Example

The following uses a Python script and a C script as examples to describe how to use msMemScope and mstx to collect memory data.

- Mark the start and end of a step in the training and inference scripts, and use the fixed information **step start** to identify the start of the step. The following is a Python script example:

    ```python
    import mstx
    for epoch in range(15):
        id = mstx.range_start("step start", None) # Mark the start of a step and enable memory analysis.
        ....
        ....
        mstx.range_end(id) # Mark the end of a step.
    ```

- A C script example is as follows:

    ```cpp
    #include <iostream>
    #include "acl/acl.h"
    #include "mstx/ms_tools_ext.h"
    int main(void)
    {
        mstxMarkA("MarkA", nullptr);
        uint64_t id_1 = mstxRangeStartA("step start", nullptr);
        ....
        mstxRangeEnd(id_1);
        return 0;
    }
    ```

> [!NOTE]
> 
> - Only the memory data of a single card can be collected.
> - You can configure **PYTHONMALLOC=malloc** before running the target user program. **PYTHONMALLOC=malloc** is a Python environment variable, which indicates that the default memory allocator of Python is not used. All memory allocations are performed using **malloc**. This configuration has some impact on small memory allocations.

### Output Description

For details about the memory collection result, see [Output File Specifications](./output_file_spec.md).
