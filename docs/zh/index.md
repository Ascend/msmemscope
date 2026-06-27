# 欢迎来到 MindStudio MemScope 内存分析工具中文教程 ✨

## 🌟 最新动态

- **[2026.2.02]** 🎉 **MindStudio Memscope 26.0.0-alpha.1版本上线！** 支持**Python API采集**方式使用、支持PyTorch框架下**采集内存快照**、支持**识别显存页表属性**并进行落盘、支持获取Driver新增的显存分配接口。

- [2025.12.30]：MindStudio Memscope项目首次上线。

## 🌏 简介

**MindStudio MemScope**（内存分析工具，msMemScope）是基于昇腾硬件开发，用于模型训练与推理过程中的内存问题定位的工具。该工具提供内存泄漏检测、内存对比、内存块监测、内存拆解和低效内存识别等功能，帮助用户完成问题定位与处理。

## 🚀 核心功能

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
        内存采集
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
        内存分析
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

## 👉 推荐上手路径

为了帮助用户快速上手 msMemScope 内存分析工具，我们推荐按照以下顺序进行学习：

* 对于想要使用 msMemScope 内存分析工具的用户，建议先阅读 [安装指南](install_guide/install_guide.md)，确保环境配置正确。
* 本教程提供的 [快速入门](quick_start/quick_start.md) 将引导你完成基本的内存问题定位配置和运行。
* 详细功能部分将介绍 [内存采集](user_guide/memory_profile.md) 、[内存分析](user_guide/memory_analysis.md) 以及 [输出文件说明](user_guide/output_file_spec.md) 等内容，帮助你更好地理解内存问题定位的场景。
* msMemScope工具提供API接口，便于快速分析内存情况，具体使用方法可以参考 [API使用](api_reference/api.md)。
* 你可以参考 [开发者指南](development_guide/development_guide.md) 部分，了解在 msMemScope 内存工具的开发步骤。

## 📬 建议与交流

华为MindStudio全流程开发工具链团队致力于提供端到端的昇腾AI应用开发解决方案，使能开发者高效完成训练开发、推理开发和算子开发。您可以通过以下渠道更深入了解华为MindStudio团队：
<div style="display: flex; align-items: center; gap: 10px;">
    <span>昇腾论坛：</span>
    <a href="https://www.hiascend.com/forum/" rel="nofollow">
        <img src="https://camo.githubusercontent.com/dd0b7ef70793ab93ce46688c049386e0755a18faab780e519df5d7f61153655e/68747470733a2f2f696d672e736869656c64732e696f2f62616467652f576562736974652d2532333165333766663f7374796c653d666f722d7468652d6261646765266c6f676f3d6279746564616e6365266c6f676f436f6c6f723d7768697465" data-canonical-src="https://img.shields.io/badge/Website-%231e37ff?style=for-the-badge&amp;logo=bytedance&amp;logoColor=white" style="max-width: 100%;">
    </a>
    <span style="margin-left: 20px;">昇腾小助手：</span>
    <a href="https://gitcode.com/Ascend/msmemscope/blob/master/docs/zh/communication_guide/figures/ascend_assistant.jpg">
        <img src="https://camo.githubusercontent.com/22bbaa8aaa1bd0d664b5374d133c565213636ae50831af284ef901724e420f8f/68747470733a2f2f696d672e736869656c64732e696f2f62616467652f5765436861742d3037433136303f7374796c653d666f722d7468652d6261646765266c6f676f3d776563686174266c6f676f436f6c6f723d7768697465" data-canonical-src="./docs/zh/communication_guide/figures/ascend_assistant.jpg" style="max-width: 100%;">
    </a>
</div>

欢迎大家为社区做贡献。如果有任何疑问或建议，请参考[交流指南](communication_guide/communication.md)获取详细的联系方式和支持渠道。

```{toctree}
:maxdepth: 2
:caption: 🚀 开始你的第一步
:hidden:

install_guide
quick_start
```

```{toctree}
:maxdepth: 1
:caption: 🧭 详细功能
:hidden:

memory_profile
memory_analysis
output_file_spec
```

```{toctree}
:maxdepth: 2
:caption: 🔬 API接口
:hidden:

api
```

```{toctree}
:maxdepth: 2
:caption: 💪 开发者指南
:hidden:

development_guide/development_guide.md
```

```{toctree}
:maxdepth: 2
:caption: 🔍 交流指南
:hidden:

communication_guide/communication.md
```
