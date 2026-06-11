# 内存块监测案例

## 概述

本案例介绍了内存分析工具msMemScope的内存块监测功能，目前主要用于内存踩踏的定位。大模型场景下，每张卡的计算任务十分复杂，如果内存踩踏一旦发生，定位非常困难。msMemScope工具支持通过命令行和python接口在算子执行前后监测指定内存块，根据内存块数据的变化，定位算子间内存踩踏的范围或具体位置。

## 前期准备

请参见[安装指南](../../docs/zh/install_guide/install_guide.md)安装msMemScope工具。

在运行本样例中的python场景时，需要配置torch以及torch_npu，具体请参见[Pytorch框架训练环境准备](https://www.hiascend.com/document/detail/zh/Pytorch/720/configandinstg/instg/insg_0001.html)文档。

此外，使用内存块监测功能前，需要注意以下事项：

1. 将环境变量`ASCEND_LAUNCH_BLOCKING`设置为1，以关闭多任务下发，然后再使用msMemScope工具进行数据采集。

    ```bash
    export ASCEND_LAUNCH_BLOCKING=1
    ```

2. 依赖Aten算子访问事件，因此需要torch_npu>=2.3.1。

3. 请查看环境是否满足[Python第三方依赖](../../requirements.txt)，若不满足则执行以下命令安装第三方库。

    ```bash
    # 请先进入项目根目录
    pip install -r ./requirements.txt
    ```

## 执行样例

### 参数说明

以下仅提供本功能样例中使用到的参数解释。其他参数的详细说明请参见[API参考](../../docs/zh/api_reference/api.md)。

| 参数 | 可选/必选 | 说明 |
|------|-----------|------|
| watch | 可选 | start：可选，字符串形式，表示开始监测算子。outid：可选，表示算子的output编号。当Tensor为一个列表时，可以指定需要落盘的Tensor，取值为Tensor在列表中的下标编号。end：可选，字符串形式，表示结束监测算子。full-content：可选，表示全量落盘内存数据，会将每个Tensor对应的二进制文件进行落盘。如果不选择该值，表示轻量化落盘，仅落盘Tensor对应的哈希值。

### 内存块监测

提供两种方式进行内存块监测，命令行方式和python接口方式。

1. 命令行方式可用于指定目标算子，监测其输出是否被踩踏：

    ```bash
    cd ./example/watch
    msmemscope python test_watch.py --watch=torch._ops.aten.mse_loss.default,torch._ops.aten.mse_loss_backward.default,full-content
    ```

2. python接口方式可以直接监测tensor，可以通过config实现和命令行一样的效果，也可以通过添加msmemscope.watcher.watch()和msmemscope.watcher.remove()来控制启停，具体参考test_watch.py。

    ```bash
    cd ./example/watch
    bash test_watch.sh
    ```

## 结果说明

内存块监测的数据将落盘于用户指定路径或者默认路径下。其结构形如：

```bash
memscopeDumpResults
├── msmemscope_xxxxxx_2025xxxxxxxxxx_ascend
    ├── device_0
        ├── dump
            └── memscope_dump_2025xxxxxxxxxx.csv
        └── watch_dump
            ├── dump.bin
            ├── ...
            └── dump.bin
    ├── msmemscope_logs
        └── msmemscope_2025xxxxxxxxxx.log
    └── config.json
├── ...
└── msmemscope_xxxxxx_2025xxxxxxxxxx_ascend
```

在watch_dump目录下将落盘dump文件，全量落盘模式会将每个Tensor对应的二进制文件进行落盘。默认的轻量化模式仅落盘Tensor对应的哈希值，将存放于csv中。
