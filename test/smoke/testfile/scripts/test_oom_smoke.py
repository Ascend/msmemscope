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
OOM 分析 — 冒烟测试脚本（目标 < 1 分钟）。

由 msmemscope 命令行工具拉起，通过 --analysis=oom:K 使能 OOM 分析后快速触发 OOM。

用法:
  msmemscope --analysis=oom:5 --output=./oom_smoke python test_oom_smoke.py
"""

import os
import sys
import logging

import torch
import torch_npu

sys.path.insert(
    0,
    os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "example", "oom")
    ),
)
from oom_utils import trigger_oom, do_training_work

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("test_oom_smoke")


def main():
    device_id = 0
    if "OOM_TEST_DEVICE" in os.environ:
        device_id = int(os.environ["OOM_TEST_DEVICE"])

    logger.info("=== OOM Smoke Start ===")

    # 1 步训练快速产生 alloc/free 记录
    do_training_work(device_id, steps=1)

    # 分配 1GB tensor 直到 OOM
    n_chunks = trigger_oom(device_id)
    logger.info("OOM triggered after %d GB", n_chunks)

    logger.info("=== OOM Smoke Done ===")


if __name__ == "__main__":
    main()
