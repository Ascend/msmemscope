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

    > [!NOTE]NOTE 
    > OOM usually occurs in the memory collection scope. Once OOM occurs, the snapshot information before and after OOM is flushed to the drive. For details about the flushed information, see [memscope_dump_{timestamp}.csv](./output_file_spec.md#memory_compare_{_timestamp_}.csv-fields) in *Output File Specifications*. If **Event** is **SNAPSHOT**, check the **Attr** and **Call Stack** fields.

**Python Trace Collection**

- Default collection mode

    msMemScope can collect Trace data of Python code through Python APIs and align the data with memory events on a unified timeline. This helps optimization personnel quickly associate memory events with full-link code and accurately locate problems.

    > [!NOTE]NOTE 
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

    2. Make sure he flush path of the custom Trace data is the same as that of the default Trace data. For details, [Output File Specifications](./output_file_spec.md).

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

> [!NOTE]NOTE
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

- The environment variable **TASK_QUEUE_ENABLE** can be configured as required. For details, see [TASK_QUEUE_ENABLE](https://gitcode.com/Ascend/pytorch/blob/v2.7.1-7.3.0/docs/en/environment_variable_reference/TASK_QUEUE_ENABLE.md). When **TASK_QUEUE_ENABLE** is set to **2**, the level-2 optimization of the **task_queue** operator dispatch queue is enabled. At this time, workspace will be collected.
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
|options|See Table 2.|
|prog_name|User script name. Ensure the security of the custom script. This parameter is not required when memory comparison is enabled.|
|prog_options|User script parameter. Ensure the security of the custom script parameter. This parameter is not required when memory comparison is enabled.|

**Table 4** Parameters

|Parameter|Description|Required (Yes/No)|
|--|--|--|
|--help, -h|Outputs msMemScope help information.|No|
|--version, -v|Outputs msMemScope version information.|No|
|--steps|Selects the step ID of memory information to be collected. The values must be integers within the actual step range. You can configure one or more step IDs, with a maximum of 5 currently supported. The input step IDs are separated by a full-width or half-width comma (,). If this parameter is not set, the memory information of all steps is collected by default. Example: **--steps=1,2,3**.|No|
|--device|Collects device information. The options are **npu** and **npu:{*id*}**. The default value is **npu**. The value cannot be empty. You can select multiple values at the same time. Use a full-width or half-width comma (,) to separate the values. Example: **--device=npu**.<br>  If the value contains both **npu** and **npu:{*id*}**, the memory information of all NPUs is collected by default, and **npu:{*id*}** does not take effect.<br> - **npu**: collects the memory information of all NPUs.<br> - **npu:{*id*}**: collects the NPU memory information of a specified ID. The value of **id** is the specified ID number. The value range is [0, 31]. The memory information of multiple IDs can be collected. Use a full-width or half-width comma (,) to separate the values. Example: **--device=npu:2,npu:7**.|No|
|--level|Collects operator information. The options are **0** (default) and **1**. Example: **--level=0**.<br> - **0**: The value can also be represented by **op**, which collects information about operators.<br> - **1**: The value can also be represented by **kernel**, which collects information about kernels.<br>In MindStudio 9.0.0, the values **0** and **1** are changed to **op** and **kernel**.|No|
|--events|Collects events. The options are **alloc**, **free**, **launch**, and **access**, with **alloc**, **free**, or **launch** by default. The values are separated by a full-width or half-width comma (,). Example: **--events=alloc,free,launch**.<br> - **alloc**: collects memory allocation events.<br>- **free**: collects memory deallocation events.<br> - **launch**: collects operator/kernel dispatch events.<br> - **access**: collects memory access events. Currently, only memory access events in the operator scenarios of ATB and Ascend Extension for PyTorch can be collected.<br> - **traceback**: collects Python Trace events.<br>Note that when **--events=alloc** is set, **free** is added by default. The actual collection items are **alloc** and **free**. When **--events=free** is set, **alloc** is added by default. The actual collection items are **alloc** and **free**. When **--events=access** is set, **alloc** and **free** are added by default. The actual collection items are **access**, **alloc**, and **free**.|No|
|--call-stack|Collects call stacks. The options are **python** and **c**. You can select both of them and separate them with a full-width or half-width comma (,). You can set the call stack collection depth. Enter a number after the option. The option and the number are separated by a colon (:), indicating the collection depth. The value range is [0, 1000]. The default value is **50**. Example: **--call-stack=python, --call-stack=c:20,python:10**.<br> - **python**: collects the Python call stack.<br> - **c**: collects the C call stack.|No|
|--collect-mode|Specifies a memory collection mode. The options are **immediate** (default) and **deferred**. Only one value can be selected. Example: **--collect-mode=immediate**.<br> - **immediate**: collects memory information immediately when the user script starts to run, and stops collecting when the user script stops running. You can also use the Python custom collection interface to control the collection scope.<br> - **deferred**: collects data after the **msleaks.start()** script is executed. You need to use the Python custom collection interface. If only **--collect-mode** is set to **deferred** and the custom Python API for collection is not used, no data (except for a small amount of system data) is collected by default.|No|
|--analysis|Enables the related memory analysis function. The default value is **leaks**. If the value of **--analysis** is empty, no analysis function is enabled. You can select multiple values and separate them with a full-width or half-width comma (,). Example: **--analysis=leaks,decompose**.<br> - **leaks**: identifies memory leak events.<br> - **inefficient**: identifies inefficient memory. Inefficient memory can be identified in ATB LLM and Ascend Extension for PyTorch single-operator scenarios. This operation can be done through APIs. For details, see [API Reference](../api_reference/api.md).<br> - **decompose**: enables the memory decomposition function.<br>Note that when **--analysis** is set to **leaks** or **decompose**, **alloc** and **free** of **--events** are enabled by default, that is, **--events=alloc,free**. When **--analysis** is set to **inefficient**, **alloc**, **free**, **access**, and **launch** of **--events** are enabled by default, that is, **--events=alloc,free,access,launch**.|No|
|--data-format|Specifies output file formats. The options are **db** and **csv**. Select a format as required. The value cannot be empty, with **csv** by default. Example: **--data-format=db**.<br> If the output file is in .db format, you can use MindStudio Insight to display the file. For details, see [MindStudio Insight Memory Tuning](https://gitcode.com/Ascend/msinsight/blob/master/docs/en/user_guide/memory_tuning.md)<br> - **db**: .db files<br> - **csv**: .csv files|No|
|--watch|Monitors memory blocks. The options are **start**, **out{*id*}**, **end** (mandatory), and **full-content**. You can select multiple values and separate them with a full-width or half-width comma (,). The parameter setting format is **--watch=start:out{*id*},end,full-content**. Example: **--watch=op0,op1,full-content**.<br> - **start**: optional. The value is a string, indicating an operator. The format varies depending on the framework. **start** is mandatory when **out{*id*}** needs to be set.<br> - **out{**id**}**: optional. It indicates the output ID of an operator. When the tensor is a list, you can specify the tensor that needs to be dumped to a given path. The value is the subscript number of the tensor in the list.<br> - **end**: mandatory. The value is a string, indicating an operator. The format varies depending on the framework.<br> - **full-content**: optional. If this value is selected, the complete tensor data is dumped to the specified path. If this value is not selected, the hash value of the tensor is dumped to the specified path.|No|
|--output|Specifies the dump path of the output file. The maximum length of the path is 4,096 characters. The default dump path is **memscopeDumpResults**. Example: **--output=/home/projects/output**.|No|
|--log-level|Specifies the log level. The options are **info**, **warn**, and **error**. The default value is **warn**.|No|
|--compare|Enables memory data comparison between steps. This parameter is mandatory only when memory comparison is enabled.|No|
|--input|Specifies the absolute directory of the comparison files. You need to enter the directories of the baseline file and comparison file and separate them with a full-width or half-width comma (,). This parameter is valid only when the **compare** function is enabled. The maximum length of the path is 4,096 characters. Example: **--input=/home/projects/input1,/home/projects/input2**.<br> This parameter is mandatory only when memory comparison is enabled.|No|

> [!NOTE]NOTE
> 
> - When **--events** is set to **launch** and Aten operator dispatch and access events need to be collected, this function can be used only when the PyTorch version under Ascend Extension for PyTorch framework is 2.3.1 or later.
> - If **--analysis** contains **decompose**, the **Attr** parameter in the **memscope\_dump\_\{_timestamp_\}.csv** file contains the memory type and component name.
> - If **--analysis** contains **decompose**, memory decomposition is enabled. Currently, the memory pools of Ascend Extension for PyTorch, MindSpore, and ATB operator frameworks can be classified, and the memory pools of MindSpore framework and ATB operator frameworks do not support fine-grained classification. In the Ascend Extension for PyTorch framework, **aten**, **weight, gradient**, and **optimizer_state** can be classified finely. **weight**, **gradient**, and **optimizer_state** are used only in PyTorch training scenarios (that is, the **optimizer.step\(\)** API call scenario). **aten** is the memory allocated in Aten operators. The PyTorch version must be 2.3.1 or later, and the value of **--level** must contain **0**.
> - When **--level=1** is specified and the tokenizers library of Hugging Face is used, the alarm **"The current process just got forked. Parallelism is disabled."** may be reported. This alarm does not affect functions and can be ignored. To avoid this alarm, run **export TOKENIZERS\_PARALLELISM=false** to disable the parallelism behavior.
> - If **--collect-mode** is set to **deferred** and Python APIs are used to collect data, memory analysis is unavailable. The memory block monitoring, memory decomposition, and identification of inefficient memory functions are available only for the data within the collection scope.
> - MindStudio Insight can display only memory data files in .db format. For details about basic operations, see [MindStudio Insight Basic Operations](https://gitcode.com/Ascend/msinsight/blob/master/docs/en/user_guide/basic_operations.md).

### Output Description

For details about the memory collection result, see [Output File Specifications](./output_file_spec.md).

## Collection via mstx Instrumentation

### Overview

msMemScope can collect memory data based on the mstx instrumentation capability. It also marks instrumentation locations in visual Trace, allowing you to identify problematic code lines quickly.

### Precautions

- The mstx instrumentation methods vary slightly for C and Python scripts. For details, see [MindStudio Tools Extension Library Interfaces](<>).
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

> [!NOTE]NOTE
> 
> - Only the memory data of a single card can be collected.
> - You can configure **PYTHONMALLOC=malloc** before running the target user program. **PYTHONMALLOC=malloc** is a Python environment variable, which indicates that the default memory allocator of Python is not used. All memory allocations are performed using **malloc**. This configuration has some impact on small memory allocations.

### Output Description

For details about the memory collection result, see [Output File Specifications](./output_file_spec.md).
