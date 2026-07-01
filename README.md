<h1 align="center">MindStudio MemScope</h1>

<div align="center">
<p><b><span style="font-size:24px;">昇腾 AI 内存调试调优利器</span></b></p>

 [![快速入门](https://badgen.net/badge/快速入门/QuickStart/blue)](./docs/zh/quick_start/quick_start.md)
 [![AI问答(DeepWiki)](https://badgen.net/badge/AI问答/DeepWiki/blue)](https://deepwiki.com/mindstudio-docs/master)
 [![AI问答(ZRead)](https://badgen.net/badge/AI问答/ZRead/blue)](https://zread.ai/mindstudio-docs/master)
 [![精确搜索](https://badgen.net/badge/精确搜索/ReadTheDocs/blue)](https://mindstudio-docs-master.readthedocs.io)
 [![昇腾社区](https://badgen.net/badge/昇腾社区/Community/blue)](https://www.hiascend.com/cn/developer/software/mindstudio)
 [![报告问题](https://badgen.net/badge/报告问题/Issues/blue)](https://gitcode.com/Ascend/msmemscope/issues/new)

</div>

## ✨ 最新消息

<span style="font-size:14px;">

🔹 **[2026.04.29]**：MindStudio Memscope 26.0.0版本上线！支持OOM场景采集显存快照。

🔹 **[2026.04.08]**：MindStudio Memscope 26.0.beta.1版本上线！显存占用一键拆解功能新增支持vllm、mindspeed场景。

🔹 **[2026.02.01]**：MindStudio Memscope 26.0.0-alpha.1版本上线！ 支持Python API采集方式使用、支持PyTorch框架下采集内存快照、支持识别显存页表属性并进行落盘、支持获取Driver新增的显存分配接口。

🔹 **[2025.12.30]**：MindStudio Memscope 项目首次上线

</span>

## ℹ️ 简介

MindStudio MemScope（内存分析工具，msMemScope）是基于昇腾硬件开发，用于模型训练与推理过程中的内存问题定位的工具。该工具提供内存泄漏检测、内存对比、内存块监测、内存拆解和低效内存识别等功能，帮助用户完成问题定位与处理。

## ⚙️ 功能介绍

msMemScope 工具提供内存采集、内存分析两大功能。

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
        <a href="./docs/zh/user_guide/memory_profile.md">内存采集</a>
      </td>
      <td rowspan="2">
        msMemScope 工具提供内存事件的采集能力，允许自定义设置采集内存范围和采集项，为后续分析提供原始数据。
      </td>
      <td>Python 接口方式采集</td>
      <td>支持通过 Python 接口采集信息，提供自定义设置采集内存范围和采集项，采集内存事件和 Python Trace 事件能力，实现精准采集、高效分析。</td>
    </tr>
    <tr>
      <td>命令行方式采集</td>
      <td>支持通过命令行方式采集信息，提供在非 Python 场景下采集内存事件与内存分析能力。</td>
    </tr>
    <!-- 内存分析 -->
    <tr>
      <td rowspan="5">
        <a href="./docs/zh/user_guide/memory_analysis.md">内存分析</a>
      </td>
      <td rowspan="5">
        msMemScope 工具基于采集的内存数据，提供泄漏、对比、监测、拆解、低效内存识别五项分析能力，帮助开发者快速诊断和优化内存问题。
      </td>
      <td>内存泄漏分析</td>
      <td>针对内存长时间未释放和内存泄漏等问题，需要进行内存分析时，msMemScope 工具提供内存泄漏分析和 kernelLaunch 粒度的内存变化分析功能，进行告警定位与分析。</td>
    </tr>
    <tr>
      <td>内存对比分析</td>
      <td>当两个 Step 内存使用存在差异时，可能会导致内存使用过多，甚至出现 OOM（Out of Memory，内存溢出）的问题，则需要使用 msMemScope 工具的内存对比分析功能来定位并分析问题。</td>
    </tr>
    <tr>
      <td>内存块监测</td>
      <td>在大模型场景中，当遇到内存踩踏定位困难时，msMemScope 工具支持通过 Python 接口和命令行两种方式，在算子执行前后对指定的内存块进行监测。根据内存块数据的变化，快速确定算子间内存踩踏的范围或具体位置。</td>
    </tr>
    <tr>
      <td>内存拆解</td>
      <td>msMemScope 工具提供内存拆解功能，支持对 CANN 层和 Ascend Extension for PyTorch 框架的内存使用情况进行拆解，输出模型权重、激活值、梯度，以及优化器等组件的内存占用情况。</td>
    </tr>
    <tr>
      <td>低效内存识别</td>
      <td>在训练推理模型过程中，可能会存在部分内存块申请后未立即使用，或使用完毕后未及时释放的低效情况。msMemScope 工具可帮助识别这种低效内存的使用现象，从而优化训练推理模型。</td>
    </tr>
  </tbody>
</table>

## 🚀 快速入门

快速入门旨在帮助用户快速熟悉 msMemScope 工具的使用方式，请参见 《[msMemScope 快速入门](./docs/zh/quick_start/quick_start.md)》。

## 📦 安装指南

msMemScope 工具支持通过软件包和源码两种方式进行安装，可根据实际需求选择合适的安装方式，请参见 《[msMemScope 安装指南](./docs/zh/install_guide/install_guide.md)》。

msMemScope 工具当前支持 CANN、Ascend Extension for PyTorch、MindSpore 以及 Aten 算子的内存采集，具体版本支持情况如下表所示。

| 产品 | 说明 |
|------|------|
| CANN | CANN 8.2.RC1 及之后版本的 ATB 算子（Ascend Transformers Boost）。 |
| Ascend Extension for PyTorch | Ascend Extension for PyTorch 7.0.0 及之后版本。 |
| MindSpore | MindSpore 2.7.0 及之后版本。 |
| Aten 算子 | 当采集 Aten 算子下发与访问事件时，需使用 PyTorch 2.3.1 或更高版本。 |

## 📘 使用指南

工具的详细使用方法，请参见 《[msMemScope 使用指南](./docs/zh/user_guide/memory_analysis.md)》。

## 💡 典型案例

通过典型问题场景帮助用户理解并掌握工具使用，请参见 《[msMemScope 典型案例](./docs/zh/best_practices/msmemscope_basic_cases.md)》。

## 📚 API 参考

msMemScope 工具提供 API 接口，便于快速分析内存情况，请参见 《[API 参考](./docs/zh/api_reference/api.md)》。

## 🛠️ 贡献指南

欢迎参与项目贡献，请参见 《[贡献指南](./docs/zh/development_guide/contributing_guide.md)》。

## 🌌 智能检索

为提升文档查阅效率，我们提供多种高效检索方式：

- **[精确搜索（ReadTheDocs）](https://mindstudio-operator-tools-docs.readthedocs.io/zh-cn/latest/)**：全量文档毫秒级结构化检索，精准触达底层配置与 API 细节
- **[AI 智能问答（DeepWiki）](https://deepwiki.com/mindstudio-docs/master)**：基于上下文的 AI 研发助手，自然语言提问，秒级获取答复

## ⚖️ 相关说明

🔹 《[版本说明](./docs/zh/release_notes/release_note.md)》

| 发布版本 | 发布时间 | 发布 Tag | 兼容性说明 |
|---------|----------|----------|------------|
| 26.0.0 | 2026/04/29 | tag_MindStudio_26.0.0.B120_001 | 兼容昇腾 CANN 8.5.0 及以前版本。请参考《[CANN 安装指南](https://www.hiascend.com/cann)》获取 CANN 安装包。 |
| 26.0.beta.1 | 2026/04/08 | tag_MindStudio_26.0.T2.B100_001 | 兼容昇腾 CANN 8.5.0 及以前版本。请参考《[CANN 安装指南](https://www.hiascend.com/cann)》获取 CANN 安装包。 |
| 26.0.0-alpha.1 | 2026/02/04 | tag_MindStudio_26.0.0-alpha.1 | 兼容昇腾 CANN 8.5.0 及以前版本。请参考《[CANN 安装指南](https://www.hiascend.com/cann)》获取 CANN 安装包。 |

**新增特性**

- 支持 PyTorch 框架下采集内存快照。
- 支持识别显存页表属性并进行落盘。
- 支持 vLLM、Verl、MindSpeed 场景下的显存拆解。

<details>
<summary>📖 查看历史版本</summary>

更多历史版本信息请参见 [历史版本](./docs/zh/release_notes/release_note.md)。

</details>

🔹 《[许可证声明](./docs/zh/legal/license_notice.md)》
🔹 《[安全声明](./docs/zh/legal/security_statement.md)》
🔹 《[免责声明](./docs/zh/legal/disclaimer.md)》

## 🤝 建议与交流

欢迎大家为社区做贡献。如果有任何疑问或建议，请提交 [Issues](https://gitcode.com/Ascend/msmemscope/issues)，我们会尽快回复。也可参考《[交流指南](./docs/zh/communication_guide/communication.md)》获取详细的联系方式和支持渠道。感谢您的支持。

|                                      📱 关注 MindStudio 公众号                                       | 💬 更多交流与支持                                                                                                                                                                                                                                                                                                                                                                                                                     |
|:-----------------------------------------------------------------------------------------------:|:-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| <img src="https://raw.gitcode.com/Ascend/msprobe/raw/master/docs/zh/figures/readme/officialAccount.jpg" width="120"><br><sub>*扫码关注获取最新动态*</sub> | 💡 **加入微信交流群**：<br>关注公众号，回复"交流群"即可获取入群二维码。<br><br>🛠️ **其他渠道**：<br>👉 昇腾助手：[![WeChat](https://img.shields.io/badge/WeChat-07C160?style=flat-square&logo=wechat&logoColor=white)](https://gitcode.com/Ascend/msit/blob/master/docs/zh/figures/readme/xiaozhushou.png)<br>👉 昇腾论坛：[![Website](https://img.shields.io/badge/Website-%231e37ff?style=flat-square&logo=RSS&logoColor=white)](https://www.hiascend.com/forum/) |

## 🙏 致谢

本工具由华为 MindStudio 全流程开发工具链团队贡献，致力于提供端到端的昇腾 AI 应用开发解决方案，使能开发者高效完成训练开发、推理开发和算子开发。
感谢来自社区的每一个 PR，欢迎贡献 msMemScope。
