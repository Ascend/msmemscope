# 基础案例

<br>

## 1. 内存泄漏检测

### 1.1 操作步骤

1. 参考《[msMemScope 安装指南](../install_guide/install_guide.md)》完成相关环境变量的配置。
2. 进入仓库目录，准备一个简单的 PyTorch 训练脚本。以 `example_api.py` 为例，脚本内容如下：

    ```python
    import torch
    import torch_npu
    import msmemscope

    msmemscope.config(
        events="alloc,free,launch",
        level="op",
        analysis="leaks",
        output="./output"
    )

    msmemscope.start()

    # 用户代码：简单的模型训练
    device = torch.device("npu:0")
    model = torch.nn.Linear(1024, 1024).to(device)
    optimizer = torch.optim.SGD(model.parameters(), lr=0.01)
    leak_tensors = []

    for step in range(10):
        data = torch.randn(64, 1024).to(device)
        output = model(data)
        loss = output.sum()
        loss.backward()
        optimizer.step()
        msmemscope.step()

    msmemscope.stop()
    ```

3. 在上述脚本中构造一个内存泄漏场景：在训练循环中，每次迭代申请一个 Tensor 但不释放，模拟内存持续增长。

    ```python
    # 在 for step in range(10): 循环内部添加以下代码，构造泄漏
    leak_tensors.append(torch.randn(1024, 1024).to(device))  # 每次迭代申请，不释放
    ```

4. 执行脚本：

    ```shell
    python example_api.py
    ```

5. 执行完成后，工具会输出内存泄漏分析结果。请参考《[msMemScope 使用指南](../user_guide/memory_analysis.md)》中的"内存泄漏分析功能介绍"分析异常行为。

### 1.2 内存泄漏示例说明

- 在[操作步骤](#11-操作步骤)中，构造了一个典型的泄漏场景：每个 Step 内申请了 4MB（1024×1024×4Bytes）的显存但未释放。
- 工具会检测到内存持续增长，并在输出中报告泄漏的地址、泄漏大小以及关联的 Step 信息，与构造的异常场景对应。

## 2. 内存对比分析

### 2.1 操作步骤

1. 准备一个 PyTorch 训练脚本，使用 msmemscope 命令行方式采集两个不同 Step 的内存数据。

2. 采集第一个 Step 的数据（以 Step 2 为例）：

    ```shell
    msmemscope --events=alloc,free --level=kernel --steps=2 --output=./output/step2 python train.py
    ```

3. 采集第二个 Step 的数据（以 Step 5 为例）：

    ```shell
    msmemscope --events=alloc,free --level=kernel --steps=5 --output=./output/step5 python train.py
    ```

4. 执行对比命令，比较两个 Step 的内存使用差异：

    ```shell
    msmemscope --compare --input=./output/step2,./output/step5 --level=kernel
    ```

5. 对比结果会输出到 `memscopeDumpResults/compare/` 目录下，生成 `memory_compare_{timestamp}.csv` 文件。请参考《[msMemScope 使用指南](../user_guide/memory_analysis.md)》中的"内存对比分析功能介绍"分析差异。

### 2.2 内存对比示例说明

- 在上述操作中，如果 Step 2 和 Step 5 的训练逻辑存在差异（例如 Step 5 多申请了一块临时缓冲区），对比报告中会显示两个 Step 之间新增或减少的内存分配记录。
- 通过对比文件可快速定位到具体哪个算子或模块在两个 Step 间产生了内存差异，为 OOM 问题排查提供依据。

## 3. 内存块监测

### 3.1 操作步骤

1. 关闭多任务下发，保证算子按序执行：

    ```shell
    export ASCEND_LAUNCH_BLOCKING=1
    ```

2. 在用户脚本中引入 watcher 模块，指定需要监测的 Tensor：

    ```python
    import torch
    import torch_npu
    import msmemscope

    torch.npu.synchronize()
    test_tensor = torch.randn(256, 256).to('npu:0')
    # 监测该 Tensor，dump_nums=3 表示最多落盘 3 次
    msmemscope.watcher.watch(test_tensor, name="test_tensor", dump_nums=3)

    # 执行一些算子操作，可能发生内存踩踏
    result = test_tensor + 1
    result = result * 2

    torch.npu.synchronize()
    msmemscope.watcher.remove(test_tensor)
    ```

3. 使用命令行开启内存块监测功能，拉起用户脚本：

    ```shell
    msmemscope --watch=start:op0,end:op1,full-content python user_script.py
    ```

4. 命令执行完成后，结果会输出到 `memscopeDumpResults/watch_dump/` 目录。请参考《[msMemScope 使用指南](../user_guide/memory_analysis.md)》中的"内存块监测功能介绍"分析监测结果。

### 3.2 内存块监测示例说明

- 在上述操作中，`msmemscope.watcher.watch` 监测了 `test_tensor` 对应的内存块。
- 构造一个内存踩踏场景：在 `watch` 和 `remove` 之间，某个算子越界写入了 `test_tensor` 相邻的内存区域。
- 通过对比算子执行前后的 bin 文件或哈希值，可以精确定位是哪个算子导致了内存数据的变化。

## 4. 内存拆解

### 4.1 操作步骤

1. 在 PyTorch 训练脚本中导入 msmemscope，使用 describe 接口对代码段进行标记：

    ```python
    import torch
    import torch_npu
    import msmemscope
    import msmemscope.describe as describe

    msmemscope.config(
        events="alloc,free",
        analysis="decompose",
        data_format="csv",
        output="./output"
    )

    msmemscope.start()

    device = torch.device("npu:0")

    # 方式一：使用装饰器标记函数
    @describe.describer(owner="model_init")
    def init_model():
        model = torch.nn.Linear(4096, 4096).to(device)
        return model

    # 方式二：使用 with 语句标记代码块
    with describe.describer(owner="forward_pass"):
        model = init_model()
        data = torch.randn(64, 4096).to(device)
        output = model(data)

    msmemscope.stop()
    ```

2. 执行脚本：

    ```shell
    python train_decompose.py
    ```

3. 执行完成后，落盘的 `memscope_dump_{timestamp}.csv` 文件中会包含 `owner` 字段，标记各内存分配所属的组件。请参考《[msMemScope 使用指南](../user_guide/memory_analysis.md)》中的"内存拆解功能介绍"分析拆解结果。

### 4.2 内存拆解示例说明

- 在上述操作中，通过 `@describe.describer(owner="model_init")` 装饰器标记了模型初始化函数，通过 `with describe.describer(owner="forward_pass")` 标记了前向传播代码块。
- 工具会根据标记将内存分配事件归类，输出中 `owner` 字段分别显示 `model_init` 和 `forward_pass`，帮助用户了解各部分的内存占用情况。

## 5. 低效内存识别

### 5.1 操作步骤

1. 准备一个 PyTorch 单算子调用脚本，使用命令行方式开启低效内存识别：

    ```shell
    msmemscope --events=alloc,free,access,launch --analysis=inefficient --level=op python inefficient_demo.py
    ```

2. 构造一个存在低效内存使用的脚本 `inefficient_demo.py`，示例如下：

    ```python
    import torch
    import torch_npu

    device = torch.device("npu:0")

    # 过早申请：提前申请但很久之后才使用
    early_tensor = torch.randn(1024, 1024).to(device)

    # 中间穿插其他操作
    temp1 = torch.randn(512, 512).to(device)
    temp2 = torch.randn(512, 512).to(device)
    del temp1
    del temp2

    # 此时才使用 early_tensor
    result = early_tensor + 1

    # 过迟释放：使用完毕后未及时释放
    used_tensor = torch.randn(2048, 2048).to(device)
    _ = used_tensor * 2  # 最后一次使用

    # 中间穿插其他操作
    temp3 = torch.randn(1024, 1024).to(device)
    del temp3

    # 此时才释放 used_tensor
    del used_tensor
    ```

3. 执行完成后，工具会在输出文件中标记低效内存事件。请参考《[msMemScope 使用指南](../user_guide/memory_analysis.md)》中的"低效内存识别功能介绍"分析结果。

### 5.2 低效内存识别示例说明

- 上述脚本构造了两类低效内存场景：
    - **过早申请**：`early_tensor` 申请后，中间穿插了 `temp1`、`temp2` 的申请和释放，之后才首次使用 `early_tensor`，工具会将其识别为"过早申请"。
    - **过迟释放**：`used_tensor` 最后一次使用后，中间穿插了 `temp3` 的申请和释放，之后才释放 `used_tensor`，工具会将其识别为"过迟释放"。
- 工具报告会指明低效内存的类型、地址和大小，帮助用户优化内存使用策略。

## 6. OOM 场景内存快照采集

### 6.1 操作步骤

1. 使用 Python 接口配置采集参数，并启用快照采集：

    ```python
    import torch
    import torch_npu
    import msmemscope

    msmemscope.config(
        events="alloc,free",
        data_format="csv",
        output="./output"
    )

    msmemscope.start()

    device = torch.device("npu:0")
    try:
        # 模拟大内存申请，可能触发 OOM
        tensors = []
        for i in range(100):
            tensors.append(torch.randn(1024, 1024, 128).to(device))
    except RuntimeError as e:
        # OOM 发生时，工具会自动落盘快照信息
        print(f"OOM detected: {e}")

    msmemscope.stop()
    ```

2. 执行脚本：

    ```shell
    python oom_demo.py
    ```

3. OOM 发生时，工具会自动落盘快照信息到 `memscope_dump_{timestamp}.csv` 文件中。请参考《[msMemScope 使用指南](../user_guide/memory_profile.md)》中的"Python接口采集功能介绍"和《[输出文件说明](../user_guide/output_file_spec.md)》分析快照数据。

### 6.2 OOM 快照示例说明

- 上述脚本在循环中不断申请大块显存，直到触发 OOM。
- msMemScope 工具在 OOM 发生时自动采集当前显存快照，记录各内存块的分配信息。
- 通过分析快照文件，可以快速识别出哪些内存分配导致了 OOM，从而定位问题根因。
