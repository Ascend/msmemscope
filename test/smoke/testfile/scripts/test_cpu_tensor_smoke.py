# -------------------------------------------------------------------------
# This file is part of the MindStudio project.
# Copyright (c) 2025 Huawei Technologies Co.,Ltd.
#
# MindStudio is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#
#          http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
# -------------------------------------------------------------------------

"""
CPU tensor memory collection smoke test.

Usage:
  msmemscope --device=npu,cpu --log-level=info python test_cpu_tensor_smoke.py
"""

import logging

import torch
import torch_npu
import mstx

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("test_cpu_tensor_smoke")


def main():
    device_id = 0
    torch.npu.set_device(device_id)

    logger.info("=== CPU Tensor Smoke Start ===")

    # Step 1: create NPU tensor and CPU tensors via all 3 methods
    mstx_id = mstx.range_start("step start", None)

    # NPU tensor (to ensure framework is initialized)
    npu_tensor = torch.randn(16, 16).to("npu:0")

    # Method 1: torch.tensor (creates CPU tensor directly)
    cpu_t1 = torch.tensor([1.0, 2.0, 3.0])

    # Method 2: Tensor.to('cpu')
    cpu_t2 = npu_tensor.to("cpu")

    # Method 3: Tensor.cpu()
    cpu_t3 = npu_tensor.cpu()

    # Allocate a larger CPU tensor for visibility
    cpu_t4 = torch.randn(1024, 1024)

    mstx.range_end(mstx_id)

    # Step 2: more CPU allocations
    mstx_id = mstx.range_start("step start", None)

    npu_tensor2 = torch.randn(32, 32).to("npu:0")
    cpu_t5 = npu_tensor2.cpu()
    cpu_t6 = torch.randn(512, 512)

    mstx.range_end(mstx_id)

    # Prevent GC during collection
    torch.npu.synchronize()
    logger.info("=== CPU Tensor Smoke Done ===")
    return cpu_t1, cpu_t2, cpu_t3, cpu_t4, cpu_t5, cpu_t6


if __name__ == "__main__":
    ret = main()
