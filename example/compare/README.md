# 内存对比案例

## 概述

本案例介绍了内存分析工具msMemScope的内存对比功能，目前主要用于处理Step间发生的显存差异对比。如果训练推理参数一致，但是CANN或框架的版本不配套，那么任务的两个不同Step的内存使用可能存在差异。msMemScope提供了用户对比分析、定位问题的能力。

## 前期准备

请参见[安装指南](../../docs/zh/install_guide/install_guide.md)安装msMemScope工具。

此外，使用对比功能前，需要将环境变量关闭task_queue算子下发队列优化后，再使用msMemScope工具采集两个不同step的数据。

```bash
export TASK_QUEUE_ENABLE=0
```

## 执行样例

### 参数说明

以下仅提供本功能样例中使用到的参数解释。其他参数的详细说明请参见[API参考](../../docs/zh/api_reference/api.md)。

| 参数 | 可选/必选 | 说明 |
|------|-----------|------|
| input | 必选 | 指定需要对比的两份文件目录。 |
| level | 可选 | 指定对比的级别，可填写op或者kernel，默认为op级别。 |

### 内存对比

样例提供了正常运行和存在泄漏内存的两份落盘文件。运行只需要在命令行执行：

```bash
cd ./example/compare
msmemscope --compare --input=baseline.csv,leaks.csv
```

## 结果说明

内存对比的数据将落盘于用户指定路径或者默认路径下。其结构形如：

```bash
memscopeDumpResults
└── msmemscope_xxxxxx_2025xxxxxxxxxx_ascend
    ├── compare
        └── memory_compare_2025xxxxxxxxxx.csv
    ├── msmemscope_logs
        └── msmemscope_2025xxxxxxxxxx.log
    └── config.json
```

在compare目录下，对比文件将落盘，其中展示算子之间的内存差异。
