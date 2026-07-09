<h1 align="center">MindStudio MemScope</h1>
<div align="center">
  <p>🚀 <b>Ascend AI-powered tool for memory debugging and tuning</b></p>
  
  [![License](https://badgen.net/badge/License/MulanPSL-2.0/blue)](./License) [![Ascend](https://img.shields.io/badge/Hardware-Ascend-orange.svg)](https://www.hiascend.com/)
  <br><br>
  [🌐 Project home page](https://gitcode.com/Ascend/msmemscope) |
  [📖 Documentation](https://msmemscope.readthedocs.io/zh-cn/latest/) |
  [🔥 Latest updates](#- Latest news) |
  [🤔 Issue](https://gitcode.com/Ascend/msmemscope/issues/new)
  <br><br>
</div>

## 📢 What's New

- **[2026.2.02]** 🎉 **MindStudio MemScope 26.0.0-alpha.1 launched!** It supports data collection via Python APIs, memory snapshot collection in the PyTorch framework, identification of memory page table attributes and flushing, and new memory allocation API of the driver.

- [2025.12.30]: The MindStudio MemScope project was debuted.

## 📌 Overview

MindStudio MemScope (msMemScope) is a memory analysis tool developed based on the Ascend hardware to locate memory problems during model training and inference. It provides functions such as memory leak detection, memory comparison, memory block monitoring, memory decomposition, and identification of inefficient memory, helping you locate and handle problems.

## 🔍 Directory Structure

The key directories are as follows:

```shell
|-- build
   |-- make_run.sh      # Package building script
   |-- build.py          # Building script
|-- docs                # Project documentation
|-- example             # Project example code
|-- csrc                 # C++ source code
   |-- framework         # Command line parsing, interacting with event_trace to obtain memory events and send the events to analysis for processing.
   |-- event_trace      # Records memory events and submits them to framework.
   |-- analysis         # Analyzes and processes memory events.
   |-- main.cpp
|-- output
   |-- bin
      |-- msmemscope    # Executable file
|-- test                # UT and ST
```

## 📝 Version Description

| Release Version| Release Date       | Release Tag      | Compatibility   |
  | ------- | --------------- | ------------- | ------------- |
  | 26.0.0-alpha.1 | 2026/02/04  | tag_MindStudio_26.0.0-alpha.1 | Compatible with Ascend CANN 8.5.0 and earlier versions. For details about how to obtain the CANN installation package, see the [CANN Installation Guide](https://www.hiascend.com/cann).|

**What's New**

- Memory snapshots can be collected in the PyTorch framework.
- The memory page table attributes can be identified and flushed to disks.
- Memory decomposition in vLLM, verl, and MindSpeed scenarios is supported.

<details>
<summary>📖 View historical versions.</summary>

For more information about historical versions, see [Historical Versions](./docs/en/changelog.md).

</details>

## 🛠️ Compatibility Information

msMemScope supports memory collection of CANN, Ascend Extension for PyTorch, MindSpore, and Aten operators. The following table lists the supported versions.

|Product|Description|
|--------|--------|
|CANN|Ascend Transformers Boost (ATB) operators of CANN 8.2.RC1 and later versions|
|Ascend Extension for PyTorch|Ascend Extension for PyTorch 7.0.0 and later versions|
|MindSpore|MindSpore 2.7.0 and later versions|
|Aten operators|To collect Aten operator dispatch and access events, use PyTorch 2.3.1 or later.|

## 🛠️ Environment Setup

msMemScope can be installed using either the software package or source code. Select an appropriate installation method based on your requirements. For details, see [msMemScope Installation Guide](./docs/en/install_guide.md).

## 🚀 Quick Start

This section helps you quickly get started with msMemScope. For details, see [msMemScope Quick Start](./docs/en/quick_start.md).

## 📖 Features

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
        <a href="./docs/en/memory_profile.md">Memory Collection</a>
      </td>
      <td rowspan="2">
        msMemScope can collect memory events and allows custom memory collection scopes and items to provide raw data for subsequent analysis.
      </td>
      <td>Collection via Python APIs</td>
      <td>Information is collected through Python APIs, including custom collection scopes and items, memory events, and Python Trace events, for precise collection and efficient analysis.</td>
    </tr>
    <tr>
      <td>Collection via CLIs</td>
      <td>Information is collected through CLIs and memory event collection and memory analysis capabilities in non-Python scenarios are supported.</td>
    </tr>
    <!--Memory analysis-->
    <tr>
      <td rowspan="5">
        <a href="./docs/en/memory_analysis.md">Memory Analysis</a>
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

## 📚 API Reference

msMemScope provides APIs to quickly analyze memory usage. For details, see [API Reference](./docs/en/api.md).

## 📝 References

- [Contribution Guide](./docs/en/development_guide/contributing_guide.md)
- [Disclaimer](./docs/en/legal/disclaimer.md)
- [License Declaration](./docs/en/legal/license_notice.md)

## 💬 Suggestions and Feedback

You are welcome to contribute to the community. If you have any questions or suggestions, please submit an [Issues](https://gitcode.com/Ascend/msit/issues). We will reply as soon as possible. You can also refer to [Communication Guide](./docs/en/communication_guide/communication.md) to contact us.
Thank you for your support.

|                                      📱 Follow the MindStudio WeChat Account                                      | 💬 Communication and Support Channels                                                                                                                                                                                                                                                                                                                                                                                                                    |
|:-----------------------------------------------------------------------------------------------:|:-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| <img src="https://raw.gitcode.com/Ascend/msprobe/raw/master/docs/zh/figures/readme/officialAccount.jpg" width="120"><br><sub>*Scan the QR code to follow us and get the latest updates.*</sub>| 💡 **Join the WeChat group**:<br>Follow the WeChat account and reply "communication group" to obtain the QR code for joining the group.<br><br>🛠️ ️**Other channels**:<br><br>|

## 🤝 Acknowledgments

Thank you to everyone in the community for your PRs. We warmly welcome contributions to msMemScope!

## About the MindStudio Team

The Huawei MindStudio full-pipeline development toolchain team is dedicated to providing an end-to-end solution for building Ascend AI applications, accelerating the processes of training, inference, and operator development.
