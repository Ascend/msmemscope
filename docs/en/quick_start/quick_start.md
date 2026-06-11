# Getting Started with msMemScope

## Overview

### Introduction

msMemScope collects memory events and conducts memory leak detection, memory comparison, memory block monitoring, memory usage breakdown, and inefficient memory identification based on the collected events. This document describes how to use msMemScope and its functions through a simple PyTorch script.

### Environment Setup

For details, [msMemScope Installation Guide](../install_guide/install_guide.md).

## Procedure

1. Set environment variables.

   1. Run the following command as the CANN running user to configure environment variables.

      ```bash
      source <cann-path>/Ascend/cann/set_env.sh
      ```

      Note: `cann-path` is the CANN installation directory.

   2. Run the following command to configure environment variables when using msMemScope to collect memory data.

      ```bash
      source <path>/msmemscope/set_env.sh
      ```

      Note: `path` is the msMemScope installation directory.

   3. Run the following commands to set environment variables to use msMemScope's Python APIs. You are advised to add the following commands to the environment variable setting script.

      ```bash
      msMemScope_DIR="path"    # Replace path with the actual msMemScope path.
      export LD_LIBRARY_PATH=${msMemScope_DIR}/lib64:${LD_LIBRARY_PATH}
      export LD_PRELOAD=${msMemScope_DIR}/lib64/libleaks_ascend_hal_hook.so:${msMemScope_DIR}/lib64/libascend_mstx_hook.so:${msMemScope_DIR}/lib64/libascend_kernel_hook.so:${msMemScope_DIR}/lib64/libatb_abi_0_hook.so:${msMemScope_DIR}/lib64/libatb_abi_1_hook.so
      ```

2. Access the repository and then run the following command to go to the `example` directory in the repository.

   ```bash
   cd ./example
   ```

   In the `example` directory, the following code examples are provided:

   - Python: [example_api](../../../example/example_api.py)
   - CLI: [example_cmd](../../../example/example_cmd.py)

3. Use msMemScope in either of the following ways. **Python APIs are recommended.**

   - Python APIs

     The `config`, `start`, `stop`, and `step` APIs are provided.

     | API  | Description                        |
     | ------ | -------------------------------- |
     | config | Sets parameters. For parameters not specified, use their default values.|
     | start  | Starts data collection.                      |
     | stop   | Stops data collection                      |
     | step   | Fixed information API of `step start` for mstx.|

     Run the following command to execute the script:

     ```bash
     python example_api.py
     ```

   - CLI: Run the following command to execute the script using msMemScope:

     ```bash
     msmemscope --events=alloc,free,access,launch --level=kernel,op --call-stack=c,python --analysis=leaks,inefficient,decompose --output=./output --data-format=csv python ./example_cmd.py
     ```

   For more information about tool parameters, see [Memory Collection](../user_guide/memory_profile.md).

4. Check the output result file.

   For details about the output file and memory analysis, see [Output File Description](../user_guide/output_file_spec.md) and [Memory Analysis](../user_guide/memory_analysis.md).
