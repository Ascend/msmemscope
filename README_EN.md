<!-- md-trans-meta sourceCommit=a2aca8c3ac20e718c319455802fdec0753d15b04 translatedAt=2026-08-10T02:08:22.150Z pushedAt=2026-08-10T02:11:41.914Z -->

<h1 align="center">MindStudio MemScope</h1>

<div align="center">
<p><b><span style="font-size:24px;">Ascend AI Memory Debugging and Tuning Tool</span></b></p>

 [![Quick Start](https://badgen.net/badge/Quick%20Start/QuickStart/blue)](./docs/en/quick_start/quick_start.md)
 [![AI Q&A (DeepWiki)](https://badgen.net/badge/AI%20Q&A/DeepWiki/blue)](https://deepwiki.com/mindstudio-docs/master)
 [![AI Q&A (ZRead)](https://badgen.net/badge/AI%20Q&A/ZRead/blue)](https://zread.ai/mindstudio-docs/master)
 [![Exact Search](https://badgen.net/badge/Exact%20Search/ReadTheDocs/blue)](https://mindstudio-docs-master.readthedocs.io)
 [![Ascend Community](https://badgen.net/badge/Ascend%20Community/Community/blue)](https://www.hiascend.com/cn/developer/software/mindstudio)
 [![Report Issues](https://badgen.net/badge/Report%20Issues/Issues/blue)](https://gitcode.com/Ascend/msmemscope/issues/new)

</div>

English | [简体中文](./README.md)

## ✨ Latest News

<span style="font-size:14px;">

🔹 **[2026.04.29]**: MindStudio MemScope 26.0.0 is released! It supports collecting memory snapshots in OOM scenarios.

🔹 **[2026.04.08]**: MindStudio MemScope 26.0.beta.1 is released! The one-click memory decomposition feature now supports vLLM and MindSpeed scenarios.

🔹 **[2026.02.01]**: MindStudio MemScope 26.0.0-alpha.1 is released! It can collect memory snapshots under the Ascend for PyTorch framework via Python APIs, identifying memory page table attributes and writing them to disk, and obtaining the newly added memory allocation interfaces from the driver.

🔹 **[2025.12.30]**: MindStudio MemScope project is released!

</span>

## ℹ️ Introduction

MindStudio MemScope (msMemScope) is a memory analysis tool, developed based on Ascend hardware for memory issue localization during model training and inference. The tool provides features such as memory leak detection, memory comparison, memory block monitoring, memory decomposition, and identification of inefficient memory, facilitating issue localization and resolution.

## ⚙️ Feature Description

msMemScope provides two major features: memory collection and memory analysis.

<table border="1" cellpadding="8" cellspacing="0" style="border-collapse: collapse; width: 100%;">
  <thead>
    <tr>
      <th>Feature</th>
      <th>Description</th>
      <th>Details</th>
      <th>Usage Scenario</th>
    </tr>
  </thead>
  <tbody>
    <!-- Memory collection -->
    <tr>
      <td rowspan="2">        
        <a href="./docs/en/user_guide/memory_profile.md">Memory collection</a>
      </td>
      <td rowspan="2">        
      msMemScope supports memory event collection capabilities, allowing users to customize the memory collection scope and collection items, thereby supplying raw data for subsequent analysis.
      </td>
      <td>Collection via Python APIs</td>
      <td>This collection mode offers the ability to customize memory collection scope and collection items, as well as to collect memory events and Python trace events, enabling precise collection and efficient analysis.</td>
    </tr>
    <tr>
      <td>Collection via CLI</td>
      <td>This mode supports information collection via command line, facilitating memory event collection and memory analysis in non-Python scenarios.</td>
    </tr>
     <!-- Memory analysis -->   
    <tr>
      <td rowspan="5">        
      <a href="./docs/en/user_guide/memory_analysis.md">Memory analysis</a>
      </td>
      <td rowspan="5">        
        Based on the collected memory data, msMemScope provides five analysis capabilities: leak detection, comparison, monitoring, decomposition, and identification of inefficient memory, helping developers quickly diagnose and optimize memory issues.
      </td>
      <td>Memory leak analysis</td>
      <td>For issues such as long-term unreleased memory and memory leaks, msMemScope provides memory leak analysis and kernelLaunch-granularity memory change analysis to locate and analyze alarms.</td>
    </tr>
    <tr>
      <td>Memory comparison</td>
      <td>Different memory usage between two steps may lead to excessive memory consumption or even Out of Memory (OOM) issues. In such cases, the memory comparison feature of msMemScope is needed to locate and analyze such issues.</td>
    </tr>
    <tr>
      <td>Memory block monitoring</td>
      <td>In large model scenarios, when memory corruption is difficult to locate, msMemScope supports monitoring specified memory blocks before and after operator execution through both Python APIs and command-line. Based on changes in memory block data, the scope or specific location of memory corruption between operators can be quickly identified.</td>
    </tr>
    <tr>
      <td>Memory decomposition</td>
      <td>msMemScope supports memory usage decomposition of the CANN layer and the Ascend for PyTorch framework, and outputs the detailed usage information of model weights, activations, gradients, and optimizers and other components.</td>
    </tr>
    <tr>
      <td>Identification of inefficient memory</td>
      <td>During model training and inference, some memory blocks may not be used immediately after being allocated, or may not be released promptly after being used. msMemScope helps identify such inefficient memory usage patterns, thereby optimizing training and inference models.</td>
    </tr>
  </tbody>
</table>

## 🚀 Getting Started

*[msMemScope Quick Start](./docs/en/quick_start/quick_start.md)* is designed to help users quickly become familiar with how to use msMemScope.

## 📦 Installation Guide

msMemScope can be installed using a package or from source. Choose the appropriate installation method based on your actual needs. For details, see *[msMemScope Installation Guide](./docs/en/install_guide/install_guide.md)*.

msMemScope currently supports memory collection for CANN, Ascend for PyTorch, MindSpore, and Aten operator. The supported versions are listed in the following table.

| Product | Description |
|------|------|
| CANN | ATB (Ascend Transformer Boost) operators of CANN 8.2.RC1 and later versions. |
| Ascend for PyTorch | Ascend for PyTorch 7.0.0 and later versions. |
| MindSpore | MindSpore 2.7.0 and later versions. |
| Aten operator | To collect Aten operator dispatch and access events, PyTorch 2.3.1 or later is required. |

## 📘 Usage Guide

For detailed usage of the tool, see *[msMemScope Usage Guide](./docs/en/user_guide/memory_analysis.md)*.

## 💡 Typical Cases

Understand and master the tool usage through typical cases. For details, see *[msMemScope Typical Cases](./docs/en/best_practices/msmemscope_basic_cases.md)*.

## 📚 API Reference

msMemScope provides APIs for quick memory analysis. For details, see *[API Reference](./docs/en/api_reference/api.md)*.

## 🛠️ Contribution Guide

Welcome to contribute to this project. Please read *[Contribution Guide](./docs/en/development_guide/contributing_guide.md)* before you start.

## 🌌 Intelligent Search

To improve document search efficiency, we provide:

- **[Exact Search (ReadTheDocs)](https://mindstudio-operator-tools-docs.readthedocs.io/zh-cn/latest/)**: Millisecond-level structured search across all the documents, enabling precise access to underlying configurations and API details
- **[AI Q&A (DeepWiki)](https://deepwiki.com/mindstudio-docs/master)**: A context-based AI assistant that delivers responses in seconds through natural language queries

## ⚖️ Related Notes

🔹 *[Release Notes](https://gitcode.com/Ascend/msmemscope/releases)*
🔹 *[License Notice](./docs/en/legal/license_notice.md)*
🔹 *[Security Statement](./docs/en/legal/security_statement.md)*
🔹 *[Disclaimer](./docs/en/legal/disclaimer.md)*

## 🤝 Suggestions and Communication

Contributions to the community are welcome. If you have any questions or suggestions, please submit [issues](https://gitcode.com/Ascend/msmemscope/issues), and we will respond as soon as possible. Thank you for your support.

|                                                                         Instant Messaging (WeChat Group)                                                                          |                                                                               Official Updates (Official Account)                                                                                | More Support (Assistant/Forum)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
|:----------------------------------------------------------------------------------------------------------------------------------------------------------:|:----------------------------------------------------------------------------------------------------------------------------------------------------------------------:|:--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| <img src="https://raw.gitcode.com/Ascend/docs/files/master/common/Writing_Template/figures/qr_code_wechat_work.png" width="120"><br><sub>*Scan to join the technical discussion group*</sub> | <img src="https://raw.gitcode.com/Ascend/docs/files/master/common/Writing_Template/figures/qr_code_wechat_official_account.png" width="120"><br><sub>*Scan to follow the official account*</sub> | Scan the QR code to join the group and follow the official account for the fastest access to the MindStudio user and developer community:<br> **Quick Q&A:** Discuss technical issues with community members in real time<br>**Stay Updated:** Receive version release and feature update notifications as soon as they are available<br> **Knowledge Sharing:** Exchange best practices and hands-on experience with fellow developers  <br> <br> **More Support Channels**: 👉 Ascend Assistant: [![WeChat](https://img.shields.io/badge/WeChat-07C160?style=flat-square&logo=wechat&logoColor=white)](https://gitcode.com/Ascend/msit/blob/master/docs/zh/figures/readme/xiaozhushou.png) 👉 Ascend Forum: [![Website](https://img.shields.io/badge/Website-%231e37ff?style=flat-square&logo=RSS&logoColor=white)](https://www.hiascend.com/forum/) |

## 🙏 Acknowledgments

This tool is contributed by the Huawei MindStudio full-process development toolchain team, dedicated to providing end-to-end Ascend AI application development solutions and enabling developers to efficiently complete training development, inference development, and operator development. Thank you for every PR from the community. Contributions to msMemScope are welcome.
