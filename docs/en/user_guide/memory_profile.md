# **Memory Collection**

## Overview

msMemScope supports memory event collection and allows custom memory collection scope and items to accurately collect key data for subsequent analysis.

- Collection via Python APIs: The collection scope and items are configured through Python APIs to collect memory events and Python Trace events.
- Collection via CLI: Collection parameters are configured through the CLI to collect memory events.
- Collection via mstx instrumentation: Memory events can be collected by enabling mstx instrumentation, together with C and Python scripts.

## Before You Start

For details about how to install msMemScope, see the [msMemScope Installation Guide](../install_guide/install_guide.md).

## Collection via Python APIs

### Overview

Python APIs can be used to collect memory events and Python Trace events, while allowing to customize the memory collection scope and parameters for precise collection and efficient analysis.

### Precautions

- Environment variables set in export mode take effect only in the current window. If you do not need to use msMemScope after setting environment variables, you are advised to restore `LD_PRELOAD` and `LD_LIBRARY_PATH` to the previous settings.
- If `events` is set to `traceback`, Python Trace events are collected. After this function is enabled, a .csv file (`python_trace_{*TID*}_{*timestamp*}.csv`) is flushed to the drive. For details, see [Output File Specifications](./output_file_spec.md).
- To disable a collection item, you can use the `none` keyword. For example, to disable event collection, set `events="none"`; to disable the analysis function, set `analysis="none"`.
- Python APIs can be used to customize and set multiple collection scopes.
- After `msmemscope.start()` and before `msmemscope.stop()`, calls to `msmemscope.config()` do not take effect and a warning is printed. To adjust the collection configuration, run `stop()` first, adjust the configuration, and then call `start()` again.
- **Historical memory awareness**: In Python API mode, when `msmemscope.start()` is called, the memory blocks that were already allocated and not yet freed before the call (existing memory) are automatically recorded in the output file as well. This mechanism ensures that the total memory statistics within the `start` to `stop` interval are accurate. Historical memory blocks are only recorded (written to drive) and do not participate in analysis flows such as leak analysis, memory decomposition, or inefficient memory identification. This mechanism is transparent to users and requires no additional configuration.

### Usage Example

**Collection via Python APIs**

1. Set environment variables.

    Run the following commands to set the environment variables required for using Python APIs.

    ```shell
    source msmemscope --load-api-env
    ```

    > [!WARNING]
    >
    > After you have finished using msMemScope, promptly clear this environment variable using the command `source msmemscope --unload-api-env` to avoid conflicts with subsequent operations. If the environment variable is not cleared in time, unpredictable errors may occur when you subsequently perform `set_env-type` operations or manually set `LD-type` environment variables such as `LD_PRELOAD` and `LD_LIBRARY_PATH`.

    **Table 1** Parameter description <a id="Parameter description "></a>

    |Parameter|Description|
    |--|--|
    |`--load-api-env[=npu\|host]`|Sets the environment variables required for using APIs. It must be executed using `source`. Values: <br> - **npu**: Assembles the NPU hook chain, used for NPU memory collection and analysis. If no value is specified, **npu** is used as the default value.<br> - **host**: Assembles only the host heap memory leak detection hook. Before setting `analysis="host-leaks"` via the Python API, it must be loaded in this mode. For details, see [Host Heap Memory Leak Detection](./memory_analysis.md#host-heap-memory-leak-detection). <br> **npu** and **host** are mutually exclusive. The mode loaded later automatically removes the `LD_PRELOAD` entry set by the other mode.|
    |`--unload-api-env`|Clears the msMemScope-related environment variable entries while keeping the values of other tools. It must be executed using `source`.|

2. Collect the memory.

    Run the following sample code to collect memory events. Note that you need to set `msmemscope.config` as required. Set `device`, `level`, `events`, `call_stack`, `analysis`, `watch`, `output_path`, and `format` as required. For details about the parameters, see [Collection via CLI](#collection-via-cli).

    ```python
    import msmemscope

    msmemscope.config(call_stack="c:10,python:5", events="launch,alloc,free", level="0", device="npu", analysis="leaks,decompose", watch="op0,op1,full-content", format="db", output_path="/home/projects/output")
    msmemscope.start()   # Start collection.
    train()              # train() is the user code.
    msmemscope.stop()    # Stop collection.
    ```

    > [!NOTE]
    >
    > OOM usually occurs in the memory collection scope. Once OOM occurs, the snapshot information before and after OOM is flushed to the drive. For details about the flushed information, see [memscope_dump_{timestamp}.csv](./output_file_spec.md#memory_compare_{_timestamp_}.csv-fields) in *Output File Specifications*. If `Event` is `SNAPSHOT`, check the `Attr` and `Call Stack` fields.

**Python Trace Collection**

- Default collection mode

    msMemScope can collect Trace data of Python code through Python APIs and align the data with memory events on a unified timeline. This helps optimization personnel quickly associate memory events with full-link code and accurately locate problems.

    > [!NOTE]
    >
    > The Python Trace collection will be removed from MindStudio 26.0.0. You can set `events="traceback"` to collect Python Trace events. For details, see [Collection via Python APIs](#collection-via-python-apis).

    1. Python APIs are added to msMemScope to enable and disable the `Tracer` function. Python code executed between `start` and `stop` will have its Trace data written to the specified path. The code example is as follows:

        ```python
        import msmemscope

        msmemscope.tracer.start()  # Enable Tracer.
        train()                   # train() is the user code.
        msmemscope.tracer.stop()   # Disable Tracer.
        ```

    2. After the execution is complete, a file named `python_trace_{TID}_{timestamp}.csv` is generated. For details about the file, see the [Output File Specifications](./output_file_spec.md).

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

    2. Make sure the flush path of the custom Trace data is the same as that of the default Trace data. For details, see the [Output File Specifications](./output_file_spec.md).

**Memory Snapshot Collection**

The snapshot information of the memory allocator in the current system can be collected, such as the total free memory and current free memory of the device.

You can enable memory snapshot collection in automatic mode (described in the following part) or one-click mode ([One-Click Analysis](./memory_analysis.md#one-click-analysis)). Memory snapshot collection applies to the following scenarios.

|Scenario|Description|
|----|-----|
|Training|Only the method described in this section can be used to enable memory snapshot collection.|
|Inference|The one-click analysis is supported to enable memory snapshot collection for the vLLM inference framework and record the memory usage of `load_weight`, `profile_run`, `kv_cache`, and `activate` during inference.|
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

After the collection is complete, the result is flushed to the `memscope_dump_{_timestamp_}.csv` file.

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

For details about the memory collection result, see the [Output File Specifications](./output_file_spec.md).

## Collection via CLI

### Overview

In non-Python scenarios, memory collection and analysis can be performed via CLI.

### Precautions

- The environment variable `TASK_QUEUE_ENABLE` can be configured as required. For details, see [TASK_QUEUE_ENABLE](https://gitcode.com/Ascend/pytorch/blob/v2.7.1-26.0.0/docs/en/environment_variable_reference/TASK_QUEUE_ENABLE.md). When `TASK_QUEUE_ENABLE` is set to **2**, the level-2 optimization of the `task_queue` operator dispatch queue is enabled. At this time, workspace will be collected.
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
|`options`|Command options. See Table 4.|
|`prog_name`|User script name. Ensure the security of the custom script. This parameter is not required when memory comparison is enabled.|
|`prog_options`|User script parameter. Ensure the security of the custom script parameter. This parameter is not required when memory comparison is enabled.|

**Table 4** Parameters

|Parameter|Optional/Required|Description|
|--|--|--|
|`--help, -h`|Optional|Outputs msMemScope help information.|
|`--version, -V`|Optional|Outputs msMemScope version information.|
|`--verbose, -v`|Optional|Sets the log level to `debug`, which is equivalent to `--log-level=debug`.|
|`--quiet, -q`|Optional|Sets the log level to `error`, which is equivalent to `--log-level=error`.|
|`--debug`|Optional|Sets the log level to `debug`, which is equivalent to `--log-level=debug`.|
|`--steps`|Optional|Selects the step IDs for which to collect memory information. Must be configured as integers within the actual step range. One or more steps can be configured, with a maximum of 5 currently supported. Input step IDs should be separated by commas (either full-width or half-width). If this parameter is not set, memory information for all steps is collected by default. Example: `--steps=1,2,3`.|
|`--device`|Optional|Specifies the device(s) from which to collect information. Options include `npu`, `npu:{id}`, and `cpu`. The default value is `npu`. The value cannot be empty. Multiple devices can be selected simultaneously, separated by commas (either full-width or half-width). Example: `--device=npu`.<br>  If both `npu` and `npu:{id}` are included, the default behavior is to collect information from all NPUs, and `npu:{id}` does not take effect.<br> - `npu`: Collects memory information from all NPUs.<br> - `npu:{id}`: Collects memory information from the specified NPU, where id is the specified NPU index. A valid id in the range of [0, 31] must be entered. Multiple NPUs can be specified, separated by commas (either full-width or half-width). Example: `--device=npu:2,npu:7`.<br> - `cpu`: Collects CPU memory information, including offload pinned memory and CPU tensor data memory. CPU memory events are uniformly of the HOST type and are output to the `device_cpu/` directory. Pinned memory is marked with `pinned:true` in `Attr` (without the `total` field), and CPU tensor data memory contains the `total` field in `Attr` (without the `pinned:true` mark). CPU memory events are only written to drive and do not participate in leak analysis or any other analysis item.|
|`--level`|Optional|Specifies the level of operator information to collect. Options are `op` and `kernel`, with a default value of `op`. Example: `--level=op`.<br> - `op`: Collects op operator information.<br> - `kernel`: Collects kernel operator information.<br> The old values `0` and `1` remain compatible (`0` is equivalent to `op` and `1` is equivalent to `kernel`), but they are deprecated and a deprecation warning is displayed when they are used.|
|`--events`|Optional|Specifies the events to collect. Options include `alloc`, `free`, `launch`, `access`, `traceback`, and `none`. The default value is `alloc,free,launch`. Multiple values should be separated by commas (either full-width or half-width). Example: `--events=alloc,free,launch`.<br> - `alloc`: Collects memory allocation events.<br>- `free`: Collects memory free events.<br> - `launch`: Collects operator/kernel launch events.<br> - `access`: Collects memory access events. Currently only supports the memory access events of ATB and Ascend for PyTorch operator scenarios.<br> - `traceback`: Collects Python Trace events.<br> - `none`: Does not collect any event type. When `none` appears together with other event type values, `none` prevails (all event types are cleared) and a warning is output at the same time.<br>**Notes**:<br>1. If the `alloc` and `free` events are not configured in pairs, allocation-free pairing information may be missing, which affects the visualization of the data in MindStudio Insight (for example, an incomplete memory timeline or inaccurate leak analysis results).<br>2. `--events=traceback` is only supported via API calls and cannot be used in the command-line environment.|
|`--call-stack`|Optional|Specifies the call stack to collect. Options include `python` and `c`, and both can be selected simultaneously, separated by commas (either full-width or half-width). The collection depth can be set by appending a number after the option, separated by a half-width colon. The range is [0, 1000], with a default value of `50`. Example: `--call-stack=python`, `--call-stack=c:20,python:10`.<br> - `python`* Collects Python call stacks.<br> - `c`: Collects C call stacks.|
|`--collect-mode`|Optional|Specifies the memory collection mode. Options are `immediate` and `deferred`, with a default value of `immediate`. Only one value can be selected. Example: `--collect-mode=immediate`.<br> - `immediate`: Immediate collection. Memory information is collected from the start of the user script until it finishes. Can also be used with Python custom collection interfaces to control the collection scope.<br> - `deferred`:Custom collection. Must be used with Python custom collection interfaces. Collection starts only after `msmemscope.start()` is called. If `--collect-mode=deferred` is set without using Python custom collection interfaces, no data (except for a small amount of system data) is collected by default.|
|`--analysis`|Optional|Enables memory analysis features. The default value is `leaks`. Use the `none` keyword to disable all analysis features. Multiple values can be selected, separated by commas (either full-width or half-width). Example: `--analysis=leaks,decompose`.<br> - `leaks`: Identifies memory leak events.<br> - `inefficient`: Identifies inefficient memory usage. Supports ATB LLM and Ascend for PyTorch single-operator scenarios. Inefficient memory identification can be configured via the API. For details, see [API Reference](../api_reference/api.md).<br> - `decompose`: Enables memory decomposition.<br> - `oom[:K]`: Enables OOM detailed analysis. `K` is optional and indicates the number of most recently unfreed memory records to collect. The default value is 10, and the value range is [1, 1000]. An error is reported when the value is out of range or invalid. When an OOM occurs, information about the operation that triggers the OOM (`func`, `req_size`, `ret`), the latest `K` unfreed records sorted by time, and the largest `K` unfreed records sorted by size are automatically collected. Enabling this feature automatically enables `alloc` and `free` event collection, with no additional --events configuration required. For details about how to use it, see [OOM Analysis](./memory_analysis.md#oom-analysis).<br> - `host-leaks`: Enables host heap memory leak detection. Host heap memory blocks that are allocated and not freed within the detection interval are aggregated by call stack to output a leak overview report. It is mutually exclusive with leaks, decompose, inefficient, and oom and cannot be enabled together in the same configuration. For details about parameters and usage, see [Host Heap Memory Leak Detection](./memory_analysis.md#host-heap-memory-leak-detection).<br> - `none`: Disables all analysis features. When `none` appears together with other analysis type values, `none` prevails (all analysis types are cleared) and a warning is output at the same time.|
|`--format`|Optional|Specifies the output file format. Options are `db` and `csv`. Choose one format based on requirements. The value cannot be empty. The default value is `csv`. Example: `--format=db`.<br> When the output format is `db`, the data can be viewed using the MindStudio Insight tool. See [MindStudio Insight Memory Tuning](https://gitcode.com/Ascend/msinsight/blob/master/docs/en/user_guide/memory_tuning.md).<br> - `db`: Outputs a db file.<br> - `csv`: Outputs a csv file.<br> The old parameter name `--data-format` remains compatible, but it is deprecated and a deprecation warning is displayed when it is used.|
|`--watch`|Optional|Monitors memory blocks. Options include `start`, `out{id}`, `end`, and `full-content`. Multiple options can be selected, with `end` being required. Values should be separated by commas (either full-width or half-width). Format: `--watch=start:out{id},end,full-content`. Example: `--watch=op0:out0,end,full-content`.<br> - `start`: Optional, string format representing an operator. The format varies across frameworks. Required when `out{id}` needs to be set.<br> - `out{id}`: Optional, represents the output index of the operator. When a tensor is a list, this specifies which tensor in the list to dump. The value is the index of the tensor in the list.<br> - `end`: Required, string format representing an operator. The format varies across frameworks.<br> - `full-content`: Optional. If selected, the full tensor data is dumped. If not selected, only the hash value of the tensor is dumped.|
|`--output-path`|Optional|Specifies the output file dump path. The maximum path length is 4096 characters. The default dump directory is memscopeDumpResults. Example: `--output-path=/home/projects/output`.<br> The old parameter name `--output` remains compatible, but it is deprecated and a deprecation warning is displayed when it is used.|
|`--log-level`|Optional|Specifies the log level. Options are `debug`, `info`, `warning`, and `error`. The default value is `info`. Example: `--log-level=warning`.<br> - `debug`: Outputs debug-level logs.<br> - `info`: Outputs info-level logs.<br> - `warning`: Outputs warning-level logs.<br> - `error`: Outputs error-level logs.<br> The old value warn remains compatible (it is equivalent to warning), but it is deprecated and a deprecation warning is displayed when it is used.|
|`--compare`|Optional|Enables memory data comparison between steps. This parameter is required only when using the memory comparison feature.|
|`--input-path`|Optional|Specifies the absolute directory path(s) for comparison files. Both the baseline file directory and the comparison file directory must be provided, separated by commas (either full-width or half-width). This parameter is only valid when the `compare` feature is enabled. The maximum path length is 4096 characters. Example: `--input-path=/home/projects/input1,/home/projects/input2`.<br> This parameter is required only when using the memory comparison feature.<br> The old parameter name `--input` remains compatible, but it is deprecated and a deprecation warning is displayed when it is used.|
|`--pid, -p`|Optional|Out-of-process control channel attach mode: attaches to an already running target process (PID) and enters an interactive control session (with tab completion), in which control words for start/stop, inspection, and configuration view/modify can be issued. It is mutually exclusive with `--compare` and direct startup commands and cannot be used at the same time. For details, see [Out-of-Process Control Channel (Dynamic Attach)](#out-of-process-control-channel-dynamic-attach).|
|`--command, -c`|Optional|Used together with `--pid` to issue a single control word and then exit. It is suitable for scripted scenarios. Example: `msmemscope --pid 12345 --command "display memory summary"`.|

> [!NOTE]
>
> - When `--events` is set to `launch` and Aten operator dispatch and access events need to be collected, this function can be used only when the PyTorch version under Ascend for PyTorch framework is 2.3.1 or later.
> - If `--analysis` contains `decompose`, the `Attr` parameter in the `memscope_dump_{_timestamp_}.csv` file contains the memory type and component name.
> - If `--analysis` contains `decompose`, memory decomposition is enabled. Currently, the memory pools of Ascend for PyTorch, MindSpore, and ATB operator frameworks can be classified, and the memory pools of MindSpore framework and ATB operator framework do not support fine-grained classification. In the Ascend for PyTorch framework, `aten`, `weight, gradient`, and `optimizer_state` can be classified finely. `weight`, `gradient`, and `optimizer_state` are used only in PyTorch training scenarios (that is, the `optimizer.step()` API call scenario). `aten` is the memory allocated in Aten operators. The PyTorch version must be 2.3.1 or later, and the value of `--level` must contain `op`.|
> - When `--analysis` contains `oom`, alloc and free event collection is automatically enabled (this affects only the collection behavior and not the dump configuration of the `--events` parameter). OOM data collects only the unfreed records around the time when the OOM occurs and does not affect the performance of the normal malloc/free hot path.|
> - When `--device` contains `cpu`, CPU memory events (with `Event Type` set to `HOST`) are written to the device_cpu/ directory. CPU memory events are only written to disk and do not participate in leak analysis, inefficient memory identification, memory decomposition, OOM analysis, or any other analysis item. Offload pinned memory and CPU tensor data memory are uniformly of the HOST type and are distinguished by the pinned:true or total field in Attr. CPU tensor collection depends on Python hooks and takes effect only after start() (in the CLI mode, it takes effect after NPU initialization). CPU tensors that existed before start() are not covered. CPU tensor data memory currently supports only tensors created through common entry points such as `torch.tensor`, `Tensor.to("cpu")`, and `Tensor.cpu()`.|
> - When `--level=kernel` is specified and the tokenizers library of Hugging Face is used, the alarm **"The current process just got forked. Parallelism is disabled."** may be reported. This alarm does not affect functions and can be ignored. To avoid this alarm, run `export TOKENIZERS_PARALLELISM=false` to disable the parallelism behavior.|
> - If `--collect-mode` is set to `deferred` and Python APIs are used to collect data, memory analysis is unavailable. The memory block monitoring, memory decomposition, and identification of inefficient memory functions are available only for the data within the collection scope.
> - MindStudio Insight can display only memory data files in .db format. For details about basic operations, see [MindStudio Insight Basic Operations](https://gitcode.com/Ascend/msinsight/blob/master/docs/en/user_guide/basic_operations.md).

### Output Description

For details about the memory collection result, see the [Output File Specifications](./output_file_spec.md).

## Collection via mstx Instrumentation

### Overview

msMemScope can collect memory data based on the mstx instrumentation capability. It also marks instrumentation locations in visual Trace, allowing you to identify problematic code lines quickly.

### Precautions

- The mstx instrumentation methods vary slightly for C and Python scripts. For details, see [MindStudio Tools Extension Library Interfaces](https://gitcode.com/Ascend/mstx/blob/master/docs/en/api_reference/README.md).
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
> - You can configure `PYTHONMALLOC=malloc` before running the target user program. `PYTHONMALLOC=malloc` is a Python environment variable, which indicates that the default memory allocator of Python is not used. All memory allocations are performed using malloc. This configuration has some impact on small memory allocations.

### Output Description

For details about the memory collection result, see the [Output File Specifications](./output_file_spec.md).

## Out-of-Process Control Channel (Dynamic Attach)

### Function Description

The out-of-process control channel allows dynamic attaching and inspection of an **already running** target process, so that control words can be issued without restarting the process:

- The target process must have `libascend_leaks.so` loaded (that is, a process started through msMemScope, or a process started after running `source msmemscope --load-api-env[=npu|host]`; both hook chains bring in this SO), and a control listening thread (thread name `msmemscope_ctrl`) resides in the process.
- After a successful attaching, an interactive control session is entered (prompt `msmemscope>`, with tab completion), in which control words for start/stop, inspection, and configuration view/modification can be issued.
- Control words are dispatched after being validated against a trustlist inside the target process, and the controller does not directly access the memory of the target process. Control words are exchanged with the target process over a local Unix domain socket. The socket file (`/tmp/msmemscope_socket_<PID>`) exists only during the handshake and is deleted once the target process is registered successfully.

### Usage Example

1. Attach to the target process (`PID` is the process ID) and enter the interactive session.

    ```shell
    msmemscope --pid 12345
    ```

    Before attaching, the controller verifies that the target process exists, that the uid is the same (the uid restriction does not apply when the attach command is run as the root user), that `libascend_leaks.so` has been loaded, and that the control listening thread is ready. If any of these conditions is not met, an error is reported and no signal is sent.

2. Issue control words in the session, for example to query the memory overview, query the interim overview of host heap leaks, or stop detection.

    ```text
    msmemscope> display memory summary
    msmemscope> display host_leak summary
    msmemscope> stop
    ```

3. In scripted scenarios, use `--command` to issue a single control word and exit immediately after execution.

    ```shell
    msmemscope --pid 12345 --command "display memory summary"
    msmemscope --pid 12345 --command "display memory block --pool hal --TOPN 20"
    ```

### Control Words

The control words supported by the interactive session are listed in the following table. An error message is returned for an invalid control word.

|Control Word|Description|
|--|--|
|`start`|Starts collection/detection (equivalent to `msmemscope.start()`).|
|`stop`|Stops collection/detection (equivalent to `msmemscope.stop()`; the host heap leak detection window is then closed and a report is output).|
|`step`|Marks a step (equivalent to `msmemscope.step()`).|
|`display hook`|Displays the hook types loaded in the target process (Host/NPU/None).|
|`display analyzer`|Lists the analyzers registered in the target process.|
|`display config`|Displays the configuration currently in effect in the target process.|
|`display memory summary`|Memory overview: outputs device_used/process_used/hal_current/hal_peak per card, as well as the current values and peaks of pinned memory (Host pinned), CPU tensor (Host tensor), and each memory pool (pta/pta_workspace/mindspore/atb) (not displayed when there is no data).|
|`display memory block [--pool <pool>] [--TOPN <N>]`|Outputs the top N unfreed memory blocks in descending order of size (`addr`/`size`/`alloc_ts`/`allocation_id`). `--pool` is optional and can be set to `host`/`hal`/`pta`/`pta_workspace`/`atb`/`mindspore`. If it is not specified, all pools are included. The value range of `--TOPN` is 1 to 100, and the default value is 10.|
|`display host_leak summary`|Queries the interim overview of the host heap leak detection window (valid while the window is open; it does not close the window and does not affect accounting). When the window is closed, `no active window` is returned. For details about the content and marks, see [Interim Overview Within the Window (Inspection)](./memory_analysis.md#interim-overview-within-the-window-inspection).|
|`set config PARAM`|Updates the configuration of the target process (CLI-style parameters, for example `--analysis=host-leaks --host-leak-mode=summary`). It is available only in the stopped state (not collecting); requests issued during collection are rejected.|
|`exit`|Exits the session and detaches.|
|`help`|Displays help for all control words.|

### Precautions

- The attaching mode is mutually exclusive with `--compare` and direct startup commands and cannot be used at the same time.
- When attaching repeatedly to an already attached process, the controller reports "already attached". If a socket is left over but the target process no longer listens (for example, the target process has exited), the controller automatically cleans it up and rebuilds it.
- `set config` can be issued only in the stopped state (not collecting); requests issued during collection are rejected with a prompt to run stop first.
- `display host_leak summary` returns the interim overview only when the host heap leak detection window is open; when no window is open, `no active window` is returned.
- The interactive session supports tab completion: a unique candidate is completed directly, multiple candidates are listed while the input is preserved, and a common prefix is expanded first. Dynamic analyzer names are fetched automatically when they are used for the first time.
- When the session exits (`exit`/`Ctrl+C`/`Ctrl+D`), socket leftovers are automatically cleaned up and no residual files remain. After the target process exits, the session displays a prompt that the target process has exited.

## Interpreting Output Data

The output file of memory collection records memory events and the memory statistics at the event time, which can be used to plot memory usage curves. For details about the output file fields, see the [Output File Specifications](./output_file_spec.md).

### Statistics Fields

In the `Attr` field of memory events (`MALLOC`/`FREE`), in addition to basic attributes such as `allocation_id`, `addr`, and `size`, the following memory statistics fields are included:

|Field|Description|
|--|--|
|`used`|Memory usage. When Event Type is HAL, it is the HAL-dimension memory usage of the current process (the cumulative HAL allocation/deallocation value collected by the tool); when Event Type is PTA, MindSpore, ATB, or PTA_WORKSPACE, it is the allocated size in the memory pool; when Event Type is HOST and `pinned:true` is marked in `Attr`, it is the collected pinned memory block usage.|
|`total`|Total size of the memory pool. It exists for memory pool events (Event Type is PTA, MindSpore, ATB, or PTA_WORKSPACE); when Event Type is HOST and no `pinned:true` is marked in `Attr` (CPU tensor data), it indicates the cumulative active CPU tensor data memory.|
|`process_used`|Memory usage of the current process. When Event Type is HAL, it is the memory usage of the current process on this device, which is the same as `Process memory(MB)` of `npu-smi info`; the meaning is the same for memory pool events; when Event Type is HOST and `pinned:true` is marked in `Attr`, it is the physical memory usage of the current process (VmRSS).|
|`device_used`|Full-NPU memory usage. When Event Type is HAL, it is the full-NPU memory usage, which is the same as `HBM-Usage(MB)` of `npu-smi info`; the meaning is the same for memory pool events; when Event Type is HOST, it is not output.|

> [!NOTE]
>
> - During program initialization, the device context has not been initialized yet, so `process_used` and `device_used` may fail to be collected (the fields are omitted). This is normal, and they can be collected normally after initialization is complete.
> - If collection fails (no card, no driver environment, insufficient permissions, and so on), the `process_used` and `device_used` fields are omitted and not output.
> - When systemd is not installed, the tool skips DCMI initialization to avoid hanging, and `process_used` and `device_used` may fail to be collected (the fields are omitted).

### Full-NPU Memory Curve

Plot the `device_used` field of HAL events (or memory pool events) against Timestamp(ns) to obtain the full-NPU memory usage curve.

- `device_used` is the full-NPU memory usage at the event time, which is the same as `HBM-Usage(MB)` returned by `npu-smi info` and can be used to cross-check the collected data.
- The curve is event-driven, so there is no sampling point in idle time periods without memory events.

### Process Memory Curve

Plot the `process_used` field of HAL events against Timestamp(ns) to obtain the memory usage curve of the current process on the device.

- `process_used` is the memory usage of the current process on this device, which is the same as `Process memory(MB)` returned by `npu-smi info` and can be used to cross-check the collected data.
- The curve reflects the actual usage change of the current process after allocating or freeing memory. Compared with the used curve (the HAL-dimension cumulative value collected by the tool), it can distinguish the parts that the tool fails to collect completely.
- Order-of-magnitude relationship between the curves: used of HAL events (the cumulative value collected by the tool) ≤ `process_used` (driver statistics, including allocator fragmentation and cache) ≤ `device_used` (full card, including the usage of other processes and the on-chip part).
