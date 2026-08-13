<h1 align="center">MindStudio MemScope</h1>
<div align="center">
  <p>🚀 <b>昇腾 AI 内存调试调优利器</b></p>
  
  [![License](https://badgen.net/badge/License/MulanPSL-2.0/blue)](./License) [![Ascend](https://img.shields.io/badge/Hardware-Ascend-orange.svg)](https://www.hiascend.com/)
  <br><br>
  [🌐项目主页](https://gitcode.com/Ascend/msmemscope) |
  [📖工具文档](https://msmemscope.readthedocs.io/zh-cn/latest/) |
  [🔥最新进展](#-最新消息) |
  [🤔报告问题](https://gitcode.com/Ascend/msmemscope/issues/new)
  <br><br>
</div>

[English](./README_EN.md) | 简体中文

## 📢 最新消息

- **[2026.2.02]** 🎉 **MindStudio Memscope 26.0.0-alpha.1版本上线！** 支持**Python API采集**方式使用、支持PyTorch框架下**采集内存快照**、支持**识别显存页表属性**并进行落盘、支持获取Driver新增的显存分配接口。

- [2025.12.30]：MindStudio Memscope项目首次上线。

## 📌 简介

MindStudio Memscope（内存分析工具，msMemScope）是基于昇腾硬件开发，用于模型训练与推理过程中的内存问题定位的工具。该工具提供内存泄漏检测、内存对比、内存块监测、内存拆解和低效内存识别等功能，帮助用户完成问题定位与处理。

## 🔍 目录结构

关键目录如下。

```shell
|-- build
   |-- make_run.sh       # 构建软件包脚本
   |-- build.py          # 构建脚本
|-- docs                 # 项目文档介绍 
|-- example              # 项目示例代码
|-- csrc                 # c++源码
   |-- framework         # 命令行解析，完成和event_trace模块的交互，获取内存事件并发送给analysis模块进行处理
   |-- event_trace       # 完成对内存事件的记录并提交框架模块
   |-- analysis          # 内存事件处理和分析
   |-- main.cpp
|-- output
   |-- bin
      |-- msmemscope     # 可执行文件
|-- test                 # UT测试、ST测试
```

## 📝 版本说明

| 发布版本 | 发布时间        | 发布Tag       | 兼容性说明    |
  | ------- | --------------- | ------------- | ------------- |
  | 26.0.0-alpha.1 | 2026/02/04  | tag_MindStudio_26.0.0-alpha.1 | 兼容昇腾CANN 8.5.0及以前版本。请参考《[CANN安装指南](https://www.hiascend.com/cann)》获取CANN安装包。 |

**新增特性**

- 支持PyTorch框架下采集内存快照。
- 支持识别显存页表属性并进行落盘。
- 支持vLLM、Verl、MindSpeed场景下的显存拆解。

<details>
<summary>📖 查看历史版本</summary>

更多历史版本信息请参见《[历史版本](./docs/zh/changelog.md)》。

</details>

## 🛠️ 兼容性信息

msMemScope工具当前支持CANN、Ascend Extension for PyTorch、MindSpore以及Aten算子的内存采集，具体版本支持情况如下表所示。

|产品|说明|
|--------|--------|
|CANN|CANN 8.2.RC1及之后版本的ATB算子（Ascend Transformers Boost）。|
|Ascend Extension for PyTorch|Ascend Extension for PyTorch 7.0.0及之后版本。|
|MindSpore|MindSpore 2.7.0及之后版本。|
|Aten算子|当采集Aten算子下发与访问事件时，需使用PyTorch 2.3.1或更高版本。|

## 🛠️ 环境部署

msMemScope工具支持通过软件包和源码两种方式进行安装，可根据实际需求选择合适的安装方式，具体安装步骤请参见《[msMemScope安装指南](./docs/zh/install_guide.md)》。

## 🚀 快速入门

快速入门旨在帮助用户快速熟悉msMemScope工具的使用方式。具体操作请参见《[msMemScope快速入门](./docs/zh/quick_start.md)》。

## 📖 功能介绍

msMemScope工具提供内存采集、内存分析两大功能。

<table border="1" cellpadding="8" cellspacing="0" style="border-collapse: collapse; width: 100%;">
  <thead>
    <tr>
      <th>功能</th>
      <th>功能说明</th>
      <th>详细功能</th>
      <th>使用场景及说明</th>
    </tr>
  </thead>
  <tbody>
    <!-- 内存采集 -->
    <tr>
      <td rowspan="2">
        <a href="./docs/zh/memory_profile.md">内存采集</a>
      </td>
      <td rowspan="2">
        msMemScope工具提供内存事件的采集能力，允许自定义设置采集内存范围和采集项，为后续分析提供原始数据。
      </td>
      <td>Python接口方式采集</td>
      <td>支持通过Python接口采集信息，提供自定义设置采集内存范围和采集项，采集内存事件和Python Trace事件能力，实现精准采集、高效分析。</td>
    </tr>
    <tr>
      <td>命令行方式采集</td>
      <td>支持通过命令行方式采集信息，提供在非Python场景下采集内存事件与内存分析能力。</td>
    </tr>
    <!-- 内存分析 -->
    <tr>
      <td rowspan="5">
        <a href="./docs/zh/memory_analysis.md">内存分析</a>
      </td>
      <td rowspan="5">
        msMemScope工具基于采集的内存数据，提供泄漏、对比、监测、拆解、低效识别五项分析能力，帮助开发者快速诊断和优化内存问题。
      </td>
      <td>内存泄漏分析</td>
      <td>针对内存长时间未释放和内存泄漏等问题，需要进行内存分析时，msMemScope工具提供内存泄漏分析和kernelLaunch粒度的内存变化分析功能，进行告警定位与分析。</td>
    </tr>
    <tr>
      <td>内存对比分析</td>
      <td>当两个Step内存使用存在差异时，可能会导致内存使用过多，甚至出现OOM（Out of Memory，内存溢出）的问题，则需要使用msMemScope工具的内存对比分析功能来定位并分析问题。</td>
    </tr>
    <tr>
      <td>内存块监测</td>
      <td>在大模型场景中，当遇到内存踩踏定位困难时，msMemScope工具支持通过Python接口和命令行两种方式，在算子执行前后对指定的内存块进行监测。根据内存块数据的变化，快速确定算子间内存踩踏的范围或具体位置。</td>
    </tr>
    <tr>
      <td>内存拆解</td>
      <td>msMemScope工具提供内存拆解功能，支持对CANN层和Ascend Extension for PyTorch框架的内存使用情况进行拆解，输出模型权重、激活值、梯度，以及优化器等组件的内存占用情况。</td>
    </tr>
    <tr>
      <td>低效内存识别</td>
      <td>在训练推理模型过程中，可能会存在部分内存块申请后未立即使用，或使用完毕后未及时释放的低效情况。msMemScope工具可帮助识别这种低效内存的使用现象，从而优化训练推理模型。</td>
    </tr>
  </tbody>
</table>

## 📚 API参考

msMemScope工具提供API接口，便于快速分析内存情况，具体使用方法请参见《[API参考](./docs/zh/api.md)》。

## 📝 相关说明

- 《[贡献指南](./docs/zh/development_guide/contributing_guide.md)》
- 《[免责声明](./docs/zh/legal/disclaimer.md)》
- 《[License声明](./docs/zh/legal/license_notice.md)》

## 💬 建议与交流

欢迎大家为社区做贡献。如果有任何疑问或建议，请提交 [Issues](https://gitcode.com/Ascend/msmemscope/issues)，我们会尽快回复。也可参考《[交流指南](./docs/zh/communication_guide/communication.md)》获取详细的联系方式和支持渠道。
感谢您的支持。

|                                      📱 关注 MindStudio 公众号                                       | 💬 更多交流与支持                                                                                                                                                                                                                                                                                                                                                                                                                     |
|:-----------------------------------------------------------------------------------------------:|:-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| <img src="https://gitcode.com/Ascend/msit/blob/26.0.0/docs/zh/figures/readme/officialAccount.png" width="120"><br><sub>*扫码关注获取最新动态*</sub> | 💡 **加入微信交流群**：<br>关注公众号，回复“交流群”即可获取入群二维码。<br><br>🛠️ **其他渠道**：<br>👉 昇腾助手：[![WeChat](https://img.shields.io/badge/WeChat-07C160?style=flat-square&logo=wechat&logoColor=white)](https://gitcode.com/Ascend/msit/blob/26.0.0/docs/zh/figures/readme/xiaozhushou.png)<br>👉 昇腾论坛：[![Website](https://img.shields.io/badge/Website-%231e37ff?style=flat-square&logo=RSS&logoColor=white)](https://www.hiascend.com/forum/) |

## 🤝 致谢

感谢来自社区的每一个PR，欢迎贡献msMemScope。

## 关于MindStudio团队

华为MindStudio全流程开发工具链团队致力于提供端到端的昇腾AI应用开发解决方案，使能开发者高效完成训练开发、推理开发和算子开发。
