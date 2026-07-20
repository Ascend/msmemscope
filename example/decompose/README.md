# 内存拆解案例

## 概述

本案例介绍了内存分析工具msMemScope的内存拆解功能，支持用户通过python接口自行对代码段进行描述。

## 前期准备

请参见[安装指南](../../docs/zh/install_guide/install_guide.md)安装msMemScope工具。

此外在运行本样例中的python场景时，需要配置torch以及TorchNPU，具体请参见[Pytorch框架训练环境准备](https://www.hiascend.com/document/detail/zh/Pytorch/720/configandinstg/instg/insg_0001.html)文档。

## 执行样例

### 参数说明

以下仅提供本功能样例中使用到的参数解释。其他参数的详细说明请参见[API参考](../../docs/zh/api_reference/api.md)。

| 参数 | 可选/必选 | 说明 |
|------|-----------|------|
| analysis | 可选 | 启用相关内存分析功能。默认值为leaks，开启内存拆解需添加decompose参数。 |
| owner | 必选 | 内存拆解python接口describe中用于标识内存块信息的字段，由用户自定义。 |

### 内存拆解

1. 配置阶段：使用接口方式需要提前配置环境变量，需要用户根据内存工具的安装目录相应更改LD_PRELOAD和LD_LIBRARY_PATH的路径。请在`test_decompose.sh`脚本中修改以下'msmemscope_path'的值。

    ```python
    TOOL_PATH='msmemscope_path'
    export LD_PRELOAD=${TOOL_PATH}/lib64/libleaks_ascend_hal_hook.so:${TOOL_PATH}/lib64/libascend_mstx_hook.so:${TOOL_PATH}/lib64/libascend_kernel_hook.so
    export LD_LIBRARY_PATH=${TOOL_PATH}/lib64/:${LD_LIBRARY_PATH}
    ```

    在用户脚本中可以添加memscope.config()以及memscope.start()和memscope.stop()来控制采集项和采集范围，用户可通过with语句，使用describe标识代码块，将其中所有事件的owner属性设置为自定义标签。这里仅提供最基础的样例，参考test_decompose.py。其他可配置的采集参数请参见[API参考](../../docs/zh/api_reference/api.md)。

2. 运行阶段：

    ```bash
    cd ./example/decompose
    bash test_decompose.sh
    ```

## 结果说明

内存拆解的信息会附加于dump文件中，如下图所示：

![leaks_analysis](../figures/decompose_analysis.png)
