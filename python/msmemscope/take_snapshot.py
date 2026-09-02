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

from functools import wraps
from ._msmemscope import _take_snapshot

import torch_npu


# 此接口不对外，因此不设默认值
def get_device_memory_info(device):
    try:
        memory_info = {
            "device": device,
            "memory_reserved": torch_npu.npu.memory_reserved(device),
            "max_memory_reserved": torch_npu.npu.max_memory_reserved(device),
            "memory_allocated": torch_npu.npu.memory_allocated(device),
            "max_memory_allocated": torch_npu.npu.max_memory_allocated(device),
        }

        # Get total and free memory
        free_mem, total_mem = torch_npu.npu.mem_get_info(device)
        memory_info["total_memory"] = total_mem
        memory_info["free_memory"] = free_mem

        return memory_info
    except ImportError:
        print("[msmemscope]: Import torch_npu failed when get device memory info! Please check it.")
        return {}


def take_snapshot(device_mask=None, name="Memory Snapshot"):
    """
    内存快照的裸接口
    示例:
    import msmemscope
    msmemscope.take_snapshot(0,name="init")
    如果device_mask不填,则会采集当前device的显存信息
    name作为显存采集的事件信息,会随之落盘
    """

    devices_mask = None
    if isinstance(device_mask, int):
        # Single device
        devices_mask = [device_mask]
    elif isinstance(device_mask, (list, tuple)):
        # Multiple devices
        devices_mask = list(device_mask)
    else:
        print("[msmemscope]: Invalid device mask, using current device!")

    # Take snapshot for current device id
    try:
        current_device_id = torch_npu.npu.current_device()
    except (RuntimeError, AttributeError, ImportError):
        print("Warning: Failed to get current device ID")
        return

    if devices_mask is None or current_device_id in devices_mask:
        memory_info = get_device_memory_info(current_device_id)
        if memory_info:
            # Add output and name to memory info
            memory_info["name"] = name
            _take_snapshot(memory_info)
        else:
            print(f"[msmemscope]: Failed to get device: {current_device_id} memory info, snapshot skipped!")


class TakeSnapshot:
    """
    内存快照的下文管理器/装饰器:
    该类既可以通过Python上下文管理器协议,在代码块执行前后自动插入内存事件记录点,
    也可以作为装饰器使用，记录函数执行的内存事件。
    需要注意的是,此功能会在函数开始执行Start,结束时执行End

    示例:
        作为上下文管理器:
        import msmemscope
        # 采集device为0的显存快照,并标记下面的代码块为forward_pass
        with msmemscope.TakeSnapshot(0,"forward_pass"):
            output = model(input_data)

        作为装饰器:
        # 采集device为0的显存快照,并标记这里函数为forward_pass
        @msmemscope.TakeSnapshot(0,"forward_pass")
        def forward_pass(data):
            return model(data)
    """

    def __init__(self, device_mask=None, name="Memory Snapshot"):
        self.device_mask = device_mask
        self.name = name

    def __enter__(self):
        take_snapshot(self.device_mask, self.name + " Start")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        take_snapshot(self.device_mask, self.name + " End")
        return False

    def __call__(self, func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            take_snapshot(self.device_mask, self.name + " Start")

            try:
                result = func(*args, **kwargs)
                return result
            finally:
                take_snapshot(self.device_mask, self.name + " End")

        return wrapper
