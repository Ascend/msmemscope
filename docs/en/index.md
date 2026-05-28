# Welcome to the meMemScope Memory Analysis Tool Tutorial ✨

## 🌟 What's New

- **[2026.2.02]** 🎉 **MindStudio MemScope 26.0.0-alpha.1 launched!** It supports data collection via Python APIs, memory snapshot collection in the PyTorch framework, identification of memory page table attributes and flushing, and new memory allocation API of the driver.

- [2025.12.30]: The MindStudio MemScope project was debuted.

## 🌏 Overview

**MindStudio MemScope** (msMemScope) is a memory analysis tool developed based on the Ascend hardware to locate memory problems during model training and inference. It provides functions such as memory leak detection, memory comparison, memory block monitoring, memory decomposition, and identification of inefficient memory, helping you locate and handle problems.

## 🚀 Core functions

msMemScope provides memory collection and analysis functions.

<table border="1" cellpadding="8" cellspacing="0" style="border-collapse: collapse; width: 100%;">
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
      <th>Sub-function</th>
      <th>Application Scenario</th>
    </tr>
  </thead>
  <tbody>
    <!--Memory collection-->
    <tr>
      <td rowspan="2">
        Memory collection
      </td>
      <td rowspan="2">
        msMemScope can collection memory events and allows custom memory collection scopes and items to provide raw data for subsequent analysis.
      </td>
      <td>Collection via Python APIs</td>
      <td>Information is collected through Python APIs, supporting custom collection scopes and items, memory events, and Python Trace events, for precise collection and efficient analysis.</td>
    </tr>
    <tr>
      <td>Collection via CLIs</td>
      <td>Information is collected through CLIs and memory event collection and memory analysis capabilities in non-Python scenarios are supported.</td>
    </tr>
    <!--Memory analysis-->
    <tr>
      <td rowspan="5">
        Memory analysis
      </td>
      <td rowspan="5">
        msMemScope provides analysis capabilities such as leak detection, comparison, monitoring, decomposition, and identification of inefficient memory based on the collected memory data, helping you quickly diagnose and optimize memory problems.
      </td>
      <td>Memory leak analysis</td>
      <td>If the memory is not deallocated for a long time or a memory leak occurs, msMemScope provides memory leak analysis and change analysis at the kernel launch level to locate and analyze alarms.</td>
    </tr>
    <tr>
      <td>Memory comparison</td>
      <td>If the memory usage differs between two steps, it may lead to excessive memory usage or even out of memory (OOM) errors. In this case, use the memory comparison analysis function of msMemScope to locate and analyze the problem.</td>
    </tr>
    <tr>
      <td>Memory block monitoring</td>
      <td>In foundation model scenarios, if it is difficult to locate memory corruption, msMemScope can monitor the specified memory blocks before and after operator execution through Python APIs and CLIs. Based on changes in the memory block data, it can quickly determine the scope or exact location of memory corruption between operators.</td>
    </tr>
    <tr>
      <td>Memory decomposition</td>
      <td>msMemScope supports memory decomposition to analyze the memory usage of the CANN layer and Ascend Extension for PyTorch framework and outputs model weights, activations, gradients, and optimizer and other component memory usage.</td>
    </tr>
    <tr>
      <td>Identification of inefficient memory</td>
      <td>During model training and inference, some memory blocks may not be used immediately after being allocated or may not be deallocated in a timely manner after being used. msMemScope identifies the inefficient memory usage to optimize model training and inference.</td>
    </tr>
  </tbody>
</table>

## 👉 Recommended Learning Path

To get started with msMemScope, follow this learning path:

* Read [Installation Guide](install_guide.md) to get started with msMemScope and correctly configure an environment.
* Read [Quick Start](quick_start.md) to learn how to configure and run msMemScope to locate memory problems.
* Read [Memory Collection](memory_profile.md), [Memory Analysis](memory_analysis.md), and [Output File Specifications](output_file_spec.md) to better understand memory problem locating.
* Read [API Reference](api.md) to quickly analyze memory status.
* Read [Developer Guide](development_guide/development_guide.md) to learn how to develop msMemScope.

## 📬 Suggestions and Feedback

The Huawei MindStudio full-pipeline development toolchain team is dedicated to providing an end-to-end solution for building Ascend AI applications, accelerating the processes of training, inference, and operator development. You can learn more about the Huawei MindStudio team through the following channels:
<div style="display: flex; align-items: center; gap: 10px;">
    <span>Ascend Forum: </span>
    <a href="https://www.hiascend.com/forum/" rel="nofollow">
        <img src="https://camo.githubusercontent.com/dd0b7ef70793ab93ce46688c049386e0755a18faab780e519df5d7f61153655e/68747470733a2f2f696d672e736869656c64732e696f2f62616467652f576562736974652d2532333165333766663f7374796c653d666f722d7468652d6261646765266c6f676f3d6279746564616e6365266c6f676f436f6c6f723d7768697465" data-canonical-src="https://img.shields.io/badge/Website-%231e37ff?style=for-the-badge&amp;logo=bytedance&amp;logoColor=white" style="max-width: 100%;">
    </a>
    <span style="margin-left: 20px;">Ascend Assistant: </span>
    <a href="https://gitcode.com/Ascend/msmemscope/blob/master/docs/zh/communication_guide/figures/ascend_assistant.jpg">
        <img src="https://camo.githubusercontent.com/22bbaa8aaa1bd0d664b5374d133c565213636ae50831af284ef901724e420f8f/68747470733a2f2f696d672e736869656c64732e696f2f62616467652f5765436861742d3037433136303f7374796c653d666f722d7468652d6261646765266c6f676f3d776563686174266c6f676f436f6c6f723d7768697465" data-canonical-src="./docs/zh/communication_guide/figures/ascend_assistant.jpg" style="max-width: 100%;">
    </a>
</div>

You are welcome to contribute to the community. If you have any questions or suggestions, see [Communication Guide](communication_guide/communication.md) to contact us.

```{toctree}
:maxdepth: 2
:caption: 🚀 Getting Started
:hidden:

install_guide
quick_start
```

```{toctree}
:maxdepth: 1
:caption: 🧭 Function Description
:hidden:

memory_profile
memory_analysis
output_file_spec
```

```{toctree}
:maxdepth: 2
:caption: 🔬 APIs
:hidden:

api
```

```{toctree}
:maxdepth: 2
:caption: 💪 Development Guide
:hidden:

development_guide/development_guide.md
```

```{toctree}
:maxdepth: 2
:caption: 🔍 Communication Guide
:hidden:

communication_guide/communication.md
```
