# **内存采集**

## 简介

msMemScope工具提供内存事件的采集能力，支持自定义配置采集内存范围和采集项，精准采集关键数据，为后续分析提供可靠的支撑。

- Python接口采集：通过Python接口，配置采集范围和采集项，支持采集内存事件和Python Trace事件。
- 命令行采集：通过命令行方式，配置采集参数，实现内存事件采集。
- mstx打点采集：结合mstx打点能力，可通过C脚本和Python脚本实现内存事件采集。

## 使用前准备

msMemScope工具的安装，请参见[《msMemScope工具安装指南》](../install_guide/install_guide.md)。

## Python接口采集功能介绍

### 功能说明

通过Python接口采集内存事件和Python Trace事件，支持自定义配置采集内存范围和参数项，实现精准采集、高效分析。

### 注意事项

- 使用export方式设置环境变量仅在当前窗口有效。设置环境变量后，如果不再需要使用msMemScope工具的功能，建议将LD_PRELOAD、LD_LIBRARY_PATH恢复成设置前状态。
- 当设置events="traceback"，采集Python Trace事件，开启后，将落盘csv文件，名称为python_trace_{*TID*}_{*timestamp*}.csv，具体信息可参见[输出文件说明](./output_file_spec.md)。
- 如果需要关闭采集项，可设置采集项取值为空。例如，关闭Python Trace采集，则设置events=""即可。
- 使用Python接口方式自定义采集范围，支持设置多段采集范围。

### 使用示例

**Python接口采集**<a id="Python接口采集"></a>

1. 设置环境变量。

    执行以下命令，设置LD_PRELOAD和LD_LIBRARY_PATH环境变量。

    ```shell
    export LD_PRELOAD=${memscope_install_path}/lib64/{so_name}:${memscope_install_path}/lib64/{so_name}
    export LD_LIBRARY_PATH=${memscope_install_path}/lib64/:${LD_LIBRARY_PATH}
    ```

    > [!NOTE]
    >
    > 设置LD_PRELOAD环境变量可能与MindStudio系列其他工具能力存在冲突，因此当使用完毕msMemScope工具后，请及时使用命令`unset LD_PRELOAD`清除该环境变量。

    其中的参数说明如[**表 1**  参数说明](#参数说明)。

    **表 1**  参数说明<a id="参数说明"></a>

    |参数|说明|
    |--|--|
    |memscope_install_path|msMemScope工具的安装路径。|
    |so_name|需要配置的so包名称，每个so包之间以半角冒号分隔。需要配置的so包有libascend_kernel_hook.so、libascend_mstx_hook.so、libatb_abi_0_hook.so、libatb_abi_1_hook.so、libleaks_ascend_hal_hook.so，共5个so包。|
    |LD_LIBRARY_PATH|LD_LIBRARY_PATH环境变量。|

2. 采集内存。

    执行以下示例代码，采集内存事件。需要注意的是，请根据需求自行配置`msmemscope.config`的参数。支持设置device、level、events、call_stack、analysis、watch、output和data_format参数，可根据需求自行设置，具体参数信息可参见[命令行采集功能介绍](#命令行采集功能介绍)。

    ```python
    import msmemscope

    msmemscope.config(call_stack="c:10,python:5", events="launch,alloc,free", level="0", device="npu", analysis="leaks,decompose", watch="op0,op1,full-content", data_format="db", output="/home/projects/output")
    msmemscope.start()   # 开启采集
    train()              # train()为用户代码
    msmemscope.stop()    # 退出采集
    ```

    > [!NOTE]
    >
    > OOM情况一般会出现在内存采集区间，一旦出现，将会落盘OOM前后的快照信息，落盘的具体信息可参考《输出文件说明》中的“[memscope_dump_{timestamp}.csv文件说明](./output_file_spec.md#memscope_dump_timestampcsv文件说明)”，当Event为SNAPSHOT时，查看Attr字段信息和Call Stack字段信息。

**Python Trace采集**

- 默认采集

    msMemScope工具支持通过Python接口采集Python代码的Trace数据，并与内存事件使用统一时间轴，帮助调优人员快速关联内存事件与全链路代码，精准定位问题。

    > [!NOTE]
    >
    > Python Trace采集功能将于MindStudio 26.0.0版本下线，您可通过Python接口采集方式，自定义设置events="traceback"，采集Python Trace事件，可参见[Python接口采集](#Python接口采集)。

    1. 在msMemScope工具中，增加Python接口，用以开启和关闭Tracer功能，在start和stop之间的Python代码，会落盘Trace数据。代码示例如下：

        ```python
        import msmemscope

        msmemscope.tracer.start()  # 开启Tracer功能
        train()                    # train()为用户代码
        msmemscope.tracer.stop()   # 关闭Tracer功能
        ```

    2. 执行完成后，会生成名称为python_trace_{*TID*}_{*timestamp*}.csv的文件，具体文件信息可参见[输出文件说明](./output_file_spec.md)。

- 自定义采集

    msMemScope工具支持通过Python接口自定义Trace事件，可通过调用API接口自定义Trace事件，关注核心代码或代码块，避免落盘全量Trace事件，提高数据采集效率。自定义Trace事件可通过`msmemscope.RecordFunction`接口设置，支持上下文（标记函数）和装饰器两种模式（标记代码块）。

    1. 在msMemScope工具中，使用`msmemscope.RecordFunction`接口，落盘自定义Trace事件数据。代码示例如下：

        ```python
        # 上下文模式，标记代码块
        import msmemscope
        with msmemscope.RecordFunction("forward_pass"):
            output = model(input_data)

        # 装饰器模式，标记函数
        import msmemscope
        @msmemscope.RecordFunction("forward_pass")
        def forward_pass(data):
            return model(data)
        ```

    2. 自定义Trace数据的落盘路径和默认采集的Trace数据落盘路径一致，具体文件信息可参见[输出文件说明](./output_file_spec.md)。

**内存快照采集**

支持采集当前系统内的显存分配器快照信息，例如设备总空闲内存、设备当前空闲内存等信息，便于客户高效获取。

内存快照采集功能可通过两种方式开启，方式一为本节的自动开启，方式二为一键分析功能开启，具体操作可参见[一键分析功能介绍](./memory_analysis.md#一键分析功能介绍)。内存快照采集功能支持以下应用场景。

|场景|说明|
|----|-----|
|训练|仅可使用本节介绍的方法开启内存快照。|
|推理|可使用一键分析功能为vLLM推理框架开启内存快照，可一键记录推理过程中的load_weight、profile_run、kv_cache和activate等环节的内存占用。|
|强化学习|强化学习（verl）涉及推理和训练两个阶段，其中一键分析功能目前仅支持在强化学习的推理过程中开启内存快照；训练过程则可使用本节介绍的方法启用内存快照功能。|

执行以下示例代码，采集内存快照。

`msmemscope.take_snapshot`可自行设置参数，支持的参数请参见[**表 2**  快照采集参数说明](#快照采集参数说明)。

```python
import msmemscope

msmemscope.take_snapshot(device_mask=0)   # 采集一次内存快照
```

**表 2**  快照采集参数说明<a id="快照采集参数说明"></a>

| 参数 | 说明 |
| ----- | ----- |
|device_mask|指定设备，默认值为NONE，表示会采集所有设备的内存使用信息。支持以下三种形式的参数传入：<br> - **num**：指定采集某个device_mask上的信息。示例：`msmemscope.take_snapshot(device_mask=0)` <br> - **list**：指定采集多个device_mask上的信息。示例：`msmemscope.take_snapshot(device_mask=[0, 1])` <br> - **tuple**：指定采集多个device_mask上的信息。示例：`msmemscope.take_snapshot(device_mask=(0, 1))`|
|name|指定采集事件自定义名，默认值为"Memory Snapshot"。示例：`msmemscope.take_snapshot(name="test_tuple")`|

采集完成后，结果会落盘至memscope_dump_{_timestamp_}.csv文件中。

> [!NOTE]
>
> - `msmemscope.take_snapshot`接口支持独立调用以采集数据，不依赖`msmemscope.start`和`msmemscope.stop`的限制。
> - `msmemscope.take_snapshot`接口可和`msmemscope.config`接口配合使用，当同时使用时，结果文件保存路径将使用第一次调用的值，不再改变。

**Step采集**

可通过添加Python接口，采集Step信息。推荐在Python场景下使用该方式采集Step信息。
示例代码如下：

```python
import msmemscope

msmemscope.config()
msmemscope.start()  # 开启采集
for i in range(10):
    train()       # train()为用户代码
    msmemscope.step() # 输入Step信息
msmemscope.stop()  # 退出采集
```

### 输出说明

内存采集的输出结果请参见[输出文件说明](./output_file_spec.md)。

## 命令行采集功能介绍

### 功能说明

在非Python场景下，支持通过命令行方式执行内存采集和分析。

### 注意事项

- 环境变量TASK_QUEUE_ENABLE可自行配置，具体配置可参见[TASK_QUEUE_ENABLE](https://gitcode.com/Ascend/pytorch/blob/v2.7.1-7.3.0/docs/zh/environment_variable_reference/TASK_QUEUE_ENABLE.md)。当TASK_QUEUE_ENABLE配置为2时，开启task_queue算子下发队列Level 2优化，此时会采集workspace内存。
- 使用root用户运行msMemScope工具时，系统会打印提示，跳过文件权限校验，存在安全风险，建议使用普通用户权限安装执行。
- 使用msMemScope工具采集内存时，推荐使用自定义采集方式设置采集项，进行内存采集，具体可参见[Python接口采集功能介绍](#python接口采集功能介绍)。
- 命令行采集方式不支持场景：VLLM-Ascend。

### 命令格式

通过执行以下命令，可以启动msMemScope工具，采集内存数据。

- 方式一（推荐使用此方式）：_user_.sh为用户脚本

        ```shell
        msmemscope [options] bash user.sh
        ```

- 方式二

        ```shell
        msmemscope [options] -- <prog_name> [prog_options]
        ```

### 参数说明

**表 3**  命令行参数说明

|参数|说明|
|--|--|
|options|命令行参数，详细参数参见表4。|
|prog_name|用户脚本名称，请保证自定义脚本的安全性。当开启内存对比功能时不需要输入此参数。|
|prog_options|用户脚本参数，请保证自定义脚本参数的安全性。当开启内存对比功能时不需要输入此参数。|

**表 4**  参数说明

|参数|可选/必选|说明|
|--|--|--|
|--help, -h|可选|输出msMemScope帮助信息。|
|--version, -v|可选|输出msMemScope版本信息。|
|--steps|可选|选择要采集内存信息的Step ID，须配置为实际Step范围内的整数，可配置1个或多个，当前最多支持配置5个。输入的Step ID以逗号（全角半角逗号均可）分割。如果不配置该参数，则默认采集所有Step的内存信息。示例：--steps=1,2,3。|
|--device|可选|采集的设备信息。可选项有npu、npu:{*id*}和cpu，默认值为npu，取值不可为空，可同时选择多个，取值间以逗号（全角半角逗号均可）分隔，示例：--device=npu。<br>  如果取值中同时包含npu和npu:{*id*}，那么默认还是采集所有npu的内存信息，npu:{*id*}不生效。<br> - **npu**：采集所有的npu内存信息。<br> - **npu:{*id*}**：采集指定卡号的npu内存信息，其中id为指定的卡号，需输入有效id，取值范围为[0，31]，可采集多个卡的内存信息，取值间以逗号（全角半角逗号均可）分隔，示例：--device=npu:2,npu:7。<br> - **cpu**：采集cpu内存信息，当前仅支持采集锁页内存信息。|
|--level|可选|采集的算子信息。可选项有0和1，默认值为0，示例：--level=0。<br> - **0**：取值也可写为op，采集op算子的信息。<br> - **1**：取值也可写为kernel，采集kernel算子的信息。<br>在MindStudio 9.0.0版本，--level参数的取值0和1将下线，参数取值将修改为op和kernel。|
|--events|可选|采集事件。可选项有alloc、free、launch和access，默认值为alloc，free，launch，取值间以逗号（全角半角逗号均可）分隔。示例：--events=alloc,free,launch。<br> - **alloc**：采集内存申请事件。<br>- **free**：采集内存释放事件。<br> - **launch**：采集算子/kernel下发事件。<br> - **access**：采集内存访问事件。当前仅支持采集ATB和Ascend for PyTorch算子场景的内存访问事件。<br> - **traceback**：采集Python Trace事件。<br>**需要注意的是**：<br>1. 当设置--events=alloc时，默认会增加free，实际采集的为alloc和free；当设置--events=free时，默认会增加alloc，实际采集的为alloc和free；当设置--events=access时，默认会增加alloc和free，实际采集的为access、alloc和free。<br>2. --events=traceback仅支持通过API接口调用，无法在命令行环境中使用。|
|--call-stack|可选|采集调用栈。可选项有python和c，可同时选择，以逗号（全角半角逗号均可）分隔。可设置调用栈的采集深度，在选项后输入数字，选项与数字以英文冒号间隔，表示采集深度，取值范围为[0，1000]，默认值为50，示例：--call-stack=python，--call-stack=c:20,python:10。<br> - **python**：采集python调用栈。<br> - **c**：采集c调用栈。|
|--collect-mode|可选|内存采集方式。可选项有immediate和deferred，默认值为immediate，取值仅支持选择其一，示例：--collect-mode=immediate。<br> - **immediate**：立即采集，即用户脚本开始运行时，就开始采集内存信息，直到用户脚本运行结束；也可配合Python自定义采集接口控制采集范围。<br> - **deferred**：自定义采集，需配合Python自定义采集接口使用，等待msmemscope.start()脚本执行后，开始采集。如果仅设置--collect-mode=deferred，不使用Python自定义采集接口时，默认不采集任何数据（除少量系统数据）。|
|--analysis|可选|启用相关内存分析功能。默认值为leaks，如果--analysis参数的取值为空，则不启用任何分析功能；可多选，取值间以逗号（全角半角逗号均可）分隔，示例：--analysis=leaks,decompose。<br> - **leaks**：识别内存泄漏事件。<br> - **inefficient**：识别低效内存，支持识别ATB LLM和Ascend for PyTorch单算子场景的低效内存，低效内存识别可通过接口设置，具体操作可参见[API参考](../api_reference/api.md)。<br> - **decompose**：开启内存拆解功能。<br>需要注意的是，当设置--analysis=leaks或--analysis=decompose时，默认会打开--events的alloc和free，即--events=alloc,free；当设置--analysis=inefficient时，默认打开--events的alloc、free、access和launch，即--events=alloc,free,access,launch。|
|--data-format|可选|输出的文件格式。可选项有db和csv，根据需求选择一种格式，取值不可为空，默认值为csv，示例：--data-format=db。<br> 当输出文件为db格式时，可使用MindStudio Insight工具展示，请参见[MindStudio Insight内存调优](https://gitcode.com/Ascend/msinsight/blob/master/docs/zh/user_guide/memory_tuning.md)。<br> - **db**：db格式文件。<br> - **csv**：csv格式文件。|
|--watch|可选|内存块监测。可选项有start，out{*id*}，end和full-content，可多选，其中end为必选，取值间以逗号（全角半角逗号均可）分隔。参数设置格式为：--watch=start:out{*id*},end,full-content，示例：--watch=op0,op1,full-content。<br> - **start**：可选，字符串形式，表示一个算子，不同框架下格式不同。当需要设置out{*id*}时，start为必选。<br> - **out{*id*}**：可选，表示算子的output编号。当Tensor为一个列表时，可以指定需要落盘的Tensor，取值为Tensor在列表中的下标编号。<br> - **end**：必选，字符串形式，表示一个算子，不同框架下格式不同。<br> - **full-content**：可选，如果选择该值，表示落盘完整的Tensor数据，如果不选，则落盘Tensor对应的哈希值。|
|--output|可选|指定输出文件落盘路径，路径最大输入长度为4096。默认的落盘目录为“memscopeDumpResults”。示例：--output=/home/projects/output。|
|--log-level|可选|指定输出日志的级别，可选值有info、warn、error。默认值为warn。|
|--compare|可选|开启Step间内存数据对比功能。仅当使用内存对比功能时，需要设置此参数。|
|--input|可选|对比文件所在的绝对目录，需输入基线文件和对比文件的目录，以逗号（全角半角逗号均可）分隔，仅在compare功能开启时有效，路径最大输入长度为4096。示例：--input=/home/projects/input1,/home/projects/input2。<br> 仅当使用内存对比功能时，需要设置此参数。|

> [!NOTE]
>
> - 当--events=launch，需要采集Aten算子下发与访问事件时，此时需要满足Ascend for PyTorch框架中的PyTorch版本大于或等于2.3.1，才可使用该功能。
> - 当--analysis参数取值包含decompose时，memscope\_dump\_\{_timestamp_\}.csv文件中Attr参数中会包含显存类别和组件名称。
> - 当--analysis参数取值包含decompose时，会开启内存分解功能，当前支持对Ascend for PyTorch框架、MindSpore框架和ATB算子框架的内存池进行分类，但是暂不支持对MindSpore框架和ATB算子框架的内存池进行细分类。在Ascend for PyTorch框架下，可支持对aten、weight、gradient、optimizer\_state进行细分类，其中weight、gradient、optimizer\_state仅限于PyTorch的训练场景（即调用optimizer.step\(\)接口的场景），aten是aten算子中申请的内存，需要同时满足PyTorch版本大于或等于2.3.1，--level参数取值中包含0。
> - 当参数--level=1，且使用Hugging Face的tokenizers库时，可能会遇到“当前发生进程fork，并行被禁用”的告警提示。这个告警信息本身不会影响功能，可以选择忽略。如果希望避免这类告警，可执行export TOKENIZERS\_PARALLELISM=false来关闭并行的行为。
> - 当--collect-mode=deferred，且使用Python接口进行采集数据时，内存分析功能不可用，内存块监测、拆解和低效内存识别功能只对采集范围内的数据可用。
> - MindStudio Insight工具仅支持展示db格式的内存数据文件，基本操作请参见[MindStudio Insight基础操作](https://gitcode.com/Ascend/msinsight/blob/master/docs/zh/user_guide/basic_operations.md)。

### 输出说明

内存采集的输出结果请参见[输出文件说明](./output_file_spec.md)。

## mstx打点采集功能介绍

### 功能说明

msMemScope工具可以结合mstx打点能力进行内存采集，同时msMemScope工具能够在可视化Trace中标记打点位置，便于用户定位问题代码行。

### 注意事项

- 对于C脚本和Python脚本，mstx打点方式略有不同，具体信息可参考[MindStudio Tools Extension Library接口](https://gitcode.com/Ascend/mstx/blob/master/docs/zh/api_reference/README.md)。
- 推荐使用C脚本示例进行mstx打点采集。

### 使用示例

下方分别以Python脚本和C脚本为例，展示msMemScope工具结合mstx执行的内存采集操作。

- 请在训练推理脚本内的Step开始和结束处进行标记，并使用固化信息“step start”标识Step开始，Python脚本示例如下：

    ```python
    import mstx
    for epoch in range(15):
        id = mstx.range_start("step start", None) # 标识Step开始并开启内存分析功能
        ....
        ....
        mstx.range_end(id)  # 标识Step结束
    ```

- C脚本示例如下：

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
> - 仅支持采集单卡局部的内存数据。
> - 在目标用户程序前可配置PYTHONMALLOC=malloc。PYTHONMALLOC=malloc是Python的环境变量，表示不采用Python的默认内存分配器，所有的内存分配均使用malloc，该配置对小内存申请有一定影响。

### 输出说明

内存采集的输出结果请参见[输出文件说明](./output_file_spec.md)。
