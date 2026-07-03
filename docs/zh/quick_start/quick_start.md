# **msMemScope快速入门**

## 概述

**简介**

msMemScope工具提供内存事件采集功能，基于采集事件开展内存泄漏检测、内存对比、内存块监测、内存拆解和低效内存识别等分析功能。本文档通过一个简单的PyTorch脚本介绍msMemScope工具的使用方法以及功能。

**环境准备**

请根据[msMemScope安装指南](../install_guide/install_guide.md)完成msMemScope工具安装。<br>
本示例基于PyTorch框架和Ascend Extension for PyTorch插件，请根据[Ascend Extension for PyTorch 下载](https://www.hiascend.com/developer/software/ai-frameworks/pytorch/download)完成安装。

## 操作步骤

1. 设置环境变量。

   1. 使用CANN运行的用户，请执行以下命令配置环境变量。

      ```bash
      source <cann-path>/Ascend/cann/set_env.sh
      ```

      注：其中`cann-path`为CANN的安装目录。

   2. 使用msMemScope工具采集内存数据时，执行以下命令配置环境变量。

      ```bash
      source <path>/msmemscope/set_env.sh
      ```

      注：其中`path`为msMemScope软件包安装位置。

   3. 使用msMemScope的**Python接口方式**需要执行以下命令设置环境变量。推荐将以下命令加入专门的环境变量设置脚本。

      ```bash
      msMemScope_DIR="path"    # "path"需替换为实际的msMemScope路径
      export LD_LIBRARY_PATH=${msMemScope_DIR}/lib64:${LD_LIBRARY_PATH}
      export LD_PRELOAD=${msMemScope_DIR}/lib64/libleaks_ascend_hal_hook.so:${msMemScope_DIR}/lib64/libascend_mstx_hook.so:${msMemScope_DIR}/lib64/libascend_kernel_hook.so:${msMemScope_DIR}/lib64/libatb_abi_0_hook.so:${msMemScope_DIR}/lib64/libatb_abi_1_hook.so
      ```

2. 进入仓库目录后，执行以下命令进入仓库example目录。

   ```bash
   cd ./example
   ```

   example目录下按照使用方式提供了不同代码示例。

   - Python接口使用方式：[example_api](../../../example/example_api.py)
   - 命令行使用方式：[example_cmd](../../../example/example_cmd.py)

3. 选择以下其中一种方式使用msMemScope。**推荐使用Python接口方式。**

   - Python接口使用方式。

     提供config、start、stop、step四种API接口。

     | 接口   | 功能说明                         |
     | ------ | -------------------------------- |
     | config | 设置参数，未指定的参数为默认值。 |
     | start  | 开始采集。                       |
     | stop   | 结束采集。                       |
     | step   | mstx的“step start”固化信息接口。 |

     通过以下命令执行脚本。

     ```bash
     python example_api.py
     ```

   - 命令行使用方式。通过以下命令，使用msMemScope执行脚本。

     ```bash
     msmemscope --events=alloc,free,access,launch --level=kernel,op --call-stack=c,python --analysis=leaks,inefficient,decompose --output=./output --data-format=csv python ./example_cmd.py
     ```

   完整工具参数参考《[内存采集](../user_guide/memory_profile.md)》。

4. 输出结果文件说明。

   输出文件以及内存分析部分详细说明请参考《[输出文件说明](../user_guide/output_file_spec.md)》和《[内存分析](../user_guide/memory_analysis.md)》。
