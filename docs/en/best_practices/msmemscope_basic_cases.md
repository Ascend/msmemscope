# Basic Cases

<!-- md-trans-meta sourceCommit=00da2ad13c99c17f86963b267badaba231ff7b0a translatedAt=2026-08-10T03:17:42.534Z pushedAt=2026-08-10T03:19:04.535Z -->

<br>

## 1. Memory Leak

### 1.1 Procedure

1. Refer to *[msMemScope Installation Guide](../install_guide/install_guide.md)* to complete the configuration of relevant environment variables.
2. Enter the repository directory and prepare a simple PyTorch training script. The following uses `example_api.py` as an example.

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

    # User code: simple model training.
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

3. Construct a memory leak scenario in the script: within the training loop, allocate a tensor at each iteration without releasing it to simulate continuous memory growth.

    ```python
    # Add the following code inside the "for step in range(10):" loop to construct a leak.
    leak_tensors.append(torch.randn(1024, 1024).to(device))  # Apply at each iteration without releasing the tensor.
    ```

4. Execute the script.

    ```shell
    python example_api.py
    ```

5. After execution, the tool outputs the memory leak analysis results. Refer to the "Memory Leak Analysis" in [*msMemScope User Guide*](../user_guide/memory_analysis.md) to analyze abnormal behavior.

### 1.2 Example Description

- In the [Procedure](#11-procedure), a typical leak scenario is constructed: 4 MB (1024 × 1024 × 4 bytes) of device memory is allocated in each step but not released.
- The tool detects continuous memory growth and reports the leak address, size, and associated step information in the output, corresponding to the constructed anomaly scenario.

## 2. Memory Comparison

### 2.1 Procedure

1. Prepare a PyTorch training script and use the `msmemscope` command to collect memory data from two different steps.

2. Collect data from the first step (using `Step 2` as an example).

    ```shell
    msmemscope --events=alloc,free --level=kernel --steps=2 --output=./output/step2 python train.py
    ```

3. Collect data from the second step (using `Step 5` as an example).

    ```shell
    msmemscope --events=alloc,free --level=kernel --steps=5 --output=./output/step5 python train.py
    ```

4. Execute the `comparison` command to compare the memory usage differences between the two steps.

    ```shell
    msmemscope --compare --input=./output/step2,./output/step5 --level=kernel
    ```

5. The comparison results are output to the `memscopeDumpResults/compare/` directory, with `memory_compare_{timestamp}.csv` generated. Refer to the "Memory Comparison" in [*msMemScope User Guide*](../user_guide/memory_analysis.md) to analyze the differences.

### 2.2 Example Description

- In the preceding operations, if the training logic of `Step 2` and `Step 5` differs (for example, `Step 5` allocates an additional temporary buffer), the comparison report will show the newly added or reduced memory allocation records between the two steps.
- The comparison file allows you to quickly identify which specific operator or module caused the memory difference between the two steps, providing a basis for OOM issue troubleshooting.

## 3. Memory Block Monitoring

### 3.1 Procedure

1. Disable multi-task dispatch to ensure operators execute in order.

    ```shell
    export ASCEND_LAUNCH_BLOCKING=1
    ```

2. Import the `watcher` module in the user script and specify the tensor to be monitored.

    ```python
    import torch
    import torch_npu
    import msmemscope

    torch.npu.synchronize()
    test_tensor = torch.randn(256, 256).to('npu:0')
    # Monitor the tensor. dump_nums=3 indicates that data is dumped to disk at most three times.
    msmemscope.watcher.watch(test_tensor, name="test_tensor", dump_nums=3)

    # Execute some operator operations, where memory corruption may occur.
    result = test_tensor + 1
    result = result * 2

    torch.npu.synchronize()
    msmemscope.watcher.remove(test_tensor)
    ```

3. Enable the memory block monitoring feature and launch the script.

    ```shell
    msmemscope --watch=start:op0,end:op1,full-content python user_script.py
    ```

4. After the command execution is complete, the results are output to the `memscopeDumpResults/watch_dump/` directory. Refer to the "Memory Block Monitoring " in *[msMemScope User Guide](../user_guide/memory_analysis.md)* to analyze the results.

### 3.2 Example Description

- In the preceding procedure, `msmemscope.watcher.watch` monitors the memory block corresponding to `test_tensor`.
- Construct a memory corruption scenario: between `watch` and `remove`, an operator writes out-of-bounds to a memory region adjacent to `test_tensor`.
- By comparing the bin files or hash values before and after operator execution, you can precisely identify which operator caused the change in memory data.

## 4. Memory Decomposition

### 4.1 Procedure

1. Import `msmemscope` in the PyTorch training script and use `describe` to mark code segments.

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

    # Method 1: Use a decorator to mark a function
    @describe.describer(owner="model_init")
    def init_model():
        model = torch.nn.Linear(4096, 4096).to(device)
        return model

    # Method 2: Use the with statement to mark a code block
    with describe.describer(owner="forward_pass"):
        model = init_model()
        data = torch.randn(64, 4096).to(device)
        output = model(data)

    msmemscope.stop()
    ```

2. Execute the script.

    ```shell
    python train_decompose.py
    ```

3. After execution completes, the flushed `memscope_dump_{timestamp}.csv` file contains the `owner` field, which marks the component to which each memory allocation belongs. Refer to "Memory Decomposition" in [*msMemScope User Guide*](../user_guide/memory_analysis.md) to analyze the results.

### 4.2 Example Description

- In the preceding procedure, the model initialization function is marked with the `@describe.describer(owner="model_init")` decorator, and the forward propagation code block is marked with `with describe.describer(owner="forward_pass")`.
- The tool categorizes memory allocation events based on these markers, and the `owner` field in the output displays `model_init` and `forward_pass` respectively, helping you understand the memory usage of each component.

## 5. Identification of Inefficient Memory

### 5.1 Procedure

1. Prepare a PyTorch single-operator script, and enable identification of inefficient memory.

    ```shell
    msmemscope --events=alloc,free,access,launch --analysis=inefficient --level=op python inefficient_demo.py
    ```

2. Construct a script `inefficient_demo.py` containing inefficient memory usage.

    ```python
    import torch
    import torch_npu

    device = torch.device("npu:0")

    # Early allocation: allocated in advance but used much later.
    early_tensor = torch.randn(1024, 1024).to(device)

    # Other operations interleaved in between.
    temp1 = torch.randn(512, 512).to(device)
    temp2 = torch.randn(512, 512).to(device)
    del temp1
    del temp2

    # early_tensor is only used at this point.
    result = early_tensor + 1

    # Delayed release: not released promptly after use.
    used_tensor = torch.randn(2048, 2048).to(device)
    _ = used_tensor * 2  # Last usage.

    # Other operations interspersed in between.
    temp3 = torch.randn(1024, 1024).to(device)
    del temp3

    # used_tensor is only released at this point.
    del used_tensor
    ```

3. After execution, the tool marks inefficient memory events in the output file. Refer to"Identification of Inefficient Memory" in *[msMemScope User Guide](../user_guide/memory_analysis.md)* to analyze the results.

### 5.2 Example Description

- The above script constructs two types of inefficient memory scenarios:
    - **Early allocation**: After `early_tensor` is allocated, `temp1` and `temp2` are allocated and freed in between before `early_tensor` is used for the first time. The tool identifies this as "early allocation."
    - **Delayed release**: After `used_tensor` is last used, `temp3` is allocated and freed in between before `used_tensor` is released. The tool identifies this as "delayed release."
- The tool report indicates the type, address, and size of inefficient memory, helping you optimize their memory usage strategy.

## 6. Memory Snapshot Collection in OOM Scenario

### 6.1 Procedure

1. Use the Python API to configure collection parameters and enable snapshot collection.

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
        # Simulate a large memory allocation that may trigger OOM.
        tensors = []
        for i in range(100):
            tensors.append(torch.randn(1024, 1024, 128).to(device))
    except RuntimeError as e:
        # When OOM occurs, the tool automatically dumps snapshot information to disk.
        print(f"OOM detected: {e}")

    msmemscope.stop()
    ```

2. Execute the script.

    ```shell
    python oom_demo.py
    ```

3. When OOM occurs, the tool automatically dumps snapshot information to the `memscope_dump_{timestamp}.csv` file. Refer to the "Collection via Python APIs" section in *[msMemScope User Guide](../user_guide/memory_profile.md)* and *[Output File Specification](../user_guide/output_file_spec.md)* to analyze the snapshot data.

### 6.2 Example Description

- The preceding script continuously allocates large blocks of device memory in a loop until OOM is triggered.
- When OOM occurs, msMemScope automatically captures the current device memory snapshot and records the allocation information of each memory block.
- By analyzing the snapshot file, you can quickly identify which memory allocations caused OOM, thereby locating the root cause of the issue.
