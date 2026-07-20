# **API参考**

## 接口列表

msMemScope工具提供开放接口，帮助用户进行内存分析，识别内存问题。

analyzer类是msMemScope工具新增的离线分析模块，负责所有的离线分析功能。可以从msMemScope导入对应的analyzer分析类，实现内存泄漏分析和自定义低效内存识别。

msMemScope工具提供快速分析接口和基于analyzer类的离线分析两种方式，推荐使用快速分析接口。

- 快速分析接口

    msMemScope工具提供快速分析接口，推荐直接使用快速分析接口进行离线分析，接口列表如[**表 1**  接口列表](#接口列表-1)所示。

    **表 1**  接口列表 <a id="接口列表-1"></a>

    |接口|说明|
    |--|--|
    |list_analyzers|该接口输出msMemScope工具当前支持的所有内存分析类型。|
    |get_analyzer_config|该接口查看运行相应内存分析类型需要输入的参数。|
    |analyze|msMemScope工具提供的快速分析接口。支持内存泄漏分析和自定义低效内存识别。|
    |check_leaks|msMemScope工具提供的内存泄漏快速分析接口。|
    |check_inefficient|msMemScope工具提供的自定义低效内存识别快速分析接口。|
    |enable_npu_sanitizer|msMemScope工具提供的NPU Sanitizer使能接口，联动使能原生Sanitizer并激活自定义算子MSTX打点捕获。|

- analyzer类

    可以直接从msMemScope工具导入analyzer类，进行离线分析，涉及的接口如[**表 2**  analyzer类接口说明](#analyzer类接口说明)所示。但是代码实现较为繁琐，不推荐使用该方式。

    实现示例代码如下：

    ```python
    # 导入内存泄漏的分析类和对应的config
    from msmemscope.analyzer import LeaksAnalyzer, LeaksConfig
    # 声明参数生成config
    leaks_config = LeaksConfig(
        input_path="user/memscope.csv", # input_path以实际路径为准
        mstx_info="test",
        start_index=0
    )
    # 生成分析类实例进行分析
    leaks_analyzer=LeaksAnalyzer()
    leaks_analyzer.analyze(leaks_config)

    # 导入低效内存的分析类和对应的config
    from msmemscope.analyzer import InefficientConfig, InefficientAnalyzer
    # 声明参数生成config
    ineff_config = InefficientConfig(
        input_path="user/ineff.csv", # input_path以实际路径为准
        mem_size=0,
        inefficient_type=["early_allocation","late_deallocation","temporary_idleness"],
        idle_threshold=3000
    )
    # 生成分析类实例进行分析
    ineff_analyzer=InefficientAnalyzer()
    ineff_analyzer.analyze(ineff_config)
    ```

    **表 2**  analyzer类接口说明 <a id="analyzer类接口说明"></a>

    |接口|说明|
    |--|--|
    |LeaksAnalyzer|内存泄漏分析类。|
    |LeaksConfig|内存泄漏分析参数。|
    |InefficientConfig|低效内存分析参数。|
    |InefficientAnalyzer|低效内存分析类。|

## list\_analyzers

**功能说明**

该接口可输出msMemScope工具当前支持的所有内存分析类型，且支持用户打印。当前仅支持内存泄漏分析和低效内存识别。

**函数原型**

```shell
list_analyzers() -> List[str]
```

**参数说明**

无参数。

**返回值说明**

List[str]：字符串列表。

运行后会输出当前msMemScope工具支持的内存分析类型。

**调用示例**

```python
import msmemscope
config_list = msmemscope.list_analyzers()
print(config_list)
```

## get\_analyzer\_config

**功能说明**

该接口可查看运行对应内存分析类型需要输入的参数。

**函数原型**

```shell
get_analyzer_config(analyzer_type: str) -> Dict[str, Any]
```

**参数说明**

|参数名|输入/输出|说明|
|--|--|--|
|str|输入|字符串，代表对应的内存分析类型，可参考list_analyzers的输出结果，例如“leaks”或“inefficient”。|

**返回值说明**

Dict[str, Any]：所有参数的字典，支持直接打印。

运行后会直接输出对应内存分析类型所需的入参信息。

**调用示例**

```python
import msmemscope
leaks_para = msmemscope.get_analyzer_config("leaks")
print(leaks_para)
ineff_para = msmemscope.get_analyzer_config("inefficient")
print(ineff_para)
```

## analyze

**功能说明**

msMemScope工具提供的对外分析接口。支持内存泄漏分析和自定义低效内存识别。

- 内存泄漏分析

  提供对指定范围内的内存泄漏进行离线分析的功能，支持对msMemScope生成的落盘csv文件进行离线分析，并在检测到指定范围内的内存泄漏时触发告警。当前功能仅适用于HAL内存泄漏分析。

  使用该接口前，需要在指定范围内通过mstx的mark进行打点，并使用msMemScope启动用户进程，以获取落盘csv文件。之后，通过该接口输入待分析的csv文件、打点信息以及起始index，即可进行离线泄漏分析。

- 自定义低效内存识别

  支持输入自定义参数，对msMemScope生成的落盘csv文件或db文件进行离线低效内存识别。根据自定义参数规范，灵活设置低效内存识别的内存块阈值、关注的低效内存类型，以及临时闲置的API间隔时间，从而准确识别落盘的csv或db文件中的低效内存。

  > [!NOTE]
  >
  > 如果输入的csv文件或db文件已有低效内存识别的结果，使用自定义低效内存识别功能时，不会清除原有的低效内存识别结果，而是会在此基础上新增识别结果。如果需要多次执行自定义低效内存识别功能，建议备份原始文件。

**函数原型**

```shell
analyze(analyzer_type: str, **kwargs):
```

**参数说明**

- 内存泄漏分析

  参数为leaks时，请参见[check\_leaks](#check_leaks)查看参数说明。

- 自定义低效内存识别

  参数为inefficient时，请参见[check\_inefficient](#check_inefficient)查看参数说明。

**返回值说明**

无返回值。

运行后会输出分析结果。

**调用示例**

```python
import msmemscope
msmemscope.analyze("leaks", input_path="user/memscope.csv", mstx_info="test",start_index=0)

msmemscope.analyze("inefficient",
  input_path="user/ineff.csv",mem_size=0,
  inefficient_type=["early_allocation","late_deallocation","temporary_idleness"],
  idle_threshold=3000
  )
# input_path以实际路径为准
```

## check\_leaks

**功能说明**

msMemScope工具对外提供内存泄漏快速分析接口。

**函数原型**

```shell
check_leaks(input_path: str, mstx_info: str, start_index: int)
```

**参数说明**

所有输入的参数需根据[list\_analyzers](#list_analyzers)和[get\_analyzer\_config](#get_analyzer_config)获取。

|参数名|输入/输出|说明|
|--|--|--|
|input_path|输入|使用msMemScope采集的csv文件所在路径，需使用绝对路径。|
|mstx_info|输入|mark打点使用的mstx文本信息，用于标识泄漏分析的范围。|
|start_index|输入|开始进行泄漏分析的mstx打点索引。|

**返回值说明**

无返回值。

运行后会直接打印显示内存泄漏分析结果。

**调用示例**

```python
import msmemscope
msmemscope.check_leaks(input_path="user/memscope.csv",mstx_info="test",start_index=0)
# input_path以实际路径为准
```

## check\_inefficient

**功能说明**

msMemScope工具对外提供的自定义低效内存识别快速分析接口。

**函数原型**

```shell
check_inefficient(input_path: str, mem_size: int = 0, inefficient_type: List[str] = None, idle_threshold: int = 3000)    # 如果无输入采用默认值
```

**参数说明**

所有输入的参数需根据[list\_analyzers](#list_analyzers)和[get\_analyzer\_config](#get_analyzer_config)获取。

|参数名|输入/输出|说明|
|--|--|--|
|input_path|输入|需要进行离线自定义低效内存识别处理的csv或者db文件路径。|
|mem_size|输入|低效内存阈值，单位：Bytes，低于该阈值的内存块不会输出结果。|
|inefficient_type|输入|低效类型分类，确定判断策略，仅输出用户关注的低效内存类型。当前支持的类型如下：<br> - 过早申请：early_allocation <br> - 过迟释放：late_deallocation <br> - 临时闲置：temporary_idleness|
|idle_threshold|输入|临时闲置阈值，决定临时闲置低效内存的API阈值，可以灵活设置阈值大小。|

**返回值说明**

无返回值。

运行后会打印提示分析过程，并识别结果写入原文件中。

**调用示例**

```python
import msmemscope
msmemscope.check_inefficient(input_path="user/ineff.csv",mem_size=0,
     inefficient_type=["early_allocation","late_deallocation","temporary_idleness"],idle_threshold=3000
     )
# input_path以实际路径为准
```

## enable\_npu\_sanitizer

**功能说明**

msMemScope工具对外提供的NPU Sanitizer使能接口。该接口内部会调用`torch_npu.npu._sanitizer.enable_npu_sanitizer()`使能原生Sanitizer，同时通知C++层激活对`sanitizer-op:`前缀MSTX打点消息的捕获。使能后，开发者在自定义算子调用处通过MSTX接口按规范格式上报的信息将被截获并转换为kernel launch事件，送入原生Sanitizer分析管线进行同步错误检测。

接口详情可参见[NPU Sanitizer自定义算子打点功能介绍](../user_guide/memory_analysis.md#npu-sanitizer自定义算子打点功能介绍)。

**函数原型**

```shell
enable_npu_sanitizer() -> None
```

**参数说明**

无参数。

**返回值说明**

无返回值。

**调用示例**

```python
import msmemscope

# 通过 msMemScope 联动使能 NPU Sanitizer
msmemscope.enable_npu_sanitizer()

# 使能后，在自定义算子调用处通过 mstx.mark() 按规范上报算子信息，
# 第二个参数传入 torch_npu.npu.current_stream().npu_stream 指定执行打点的stream
import mstx
import torch_npu
mstx.mark(
    "sanitizer-op: name=my_ext.my_op;"
    "read=input:0x7f00:1024;"
    "write=output:0x7f08:2048",
    torch_npu.npu.current_stream().npu_stream
)
```
