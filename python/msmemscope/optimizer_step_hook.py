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

import sys
from typing import List, Tuple, Dict, Any
import torch
from torch.optim import Optimizer
from torch.optim.optimizer import register_optimizer_step_post_hook
from .describe import DETAIL_1, describe_addr


def _to_local_tensor(tensor: torch.Tensor) -> torch.Tensor:
    """
    分布式张量(DTensor等)以 _local_tensor 承载实际NPU存储,需要取其本地张量才能拿到
    真实的 device/data_ptr;普通 Tensor 原样返回。通过 duck-typing 识别,不强制依赖
    torch.distributed.tensor(避免引入版本耦合)
    """
    local = getattr(tensor, "_local_tensor", None)
    return local if isinstance(local, torch.Tensor) else tensor


def append_tensor_info(
    tensor_info_list: List[Tuple[int, list]],
    tensor: torch.Tensor,
    labels: list,
) -> None:
    # FSDP2(fully_shard)的参数/梯度/优化器状态均为DTensor,统一取其本地张量上报地址;
    # labels 为显式分级标签 [(label, level), ...], 按决策省略 COMPONENT=model(PTA亦不上报),
    # 仅细化级别(DETAIL_1), 避免覆盖 hijack 范围标记的组件/流程级别
    tensor = _to_local_tensor(tensor)
    if 'npu' in str(tensor.device).lower():
        tensor_info_list.append(tuple((tensor.data_ptr(), labels)))


def process_param(tensor_info_list: List[Tuple[int, list]], param: torch.Tensor, opt: Optimizer):
    append_tensor_info(tensor_info_list, param, [("weight", DETAIL_1)])
    if param.grad is not None:
        append_tensor_info(tensor_info_list, param.grad, [("gradient", DETAIL_1)])

    if param in opt.state:
        for _, state in opt.state[param].items():
            if torch.is_tensor(state):
                append_tensor_info(tensor_info_list, state, [("optimizer_state", DETAIL_1)])


def global_optimizer_step_hook(opt: Optimizer, args: Tuple[Any], kwargs: Dict[Any, Any]):
    tensor_info_list: List[Tuple[int, list]] = []

    for param_group in opt.param_groups:
        for param in param_group['params']:
            process_param(tensor_info_list, param, opt)

    # 地址直标走 describe.py 内部接口(与 taggers 一致); 原 report_tensor 通道已移除
    for addr, labels in tensor_info_list:
        describe_addr(addr, labels)


class OptimizerStepHook:
    def __init__(self):
        self.global_handle = None
        self.enabled = False

    def __del__(self):
        if (sys is not None) and (not sys.is_finalizing()) and self.enabled:
            self.disable()

    def enable(self):
        self.global_handle = register_optimizer_step_post_hook(global_optimizer_step_hook)
        self.enabled = True

    def disable(self):
        if self.global_handle is not None:
            self.global_handle.remove()
        self.enabled = False


def enable_optimizer_step_hook():
    optimizer_step_hook.enable()


def disable_optimizer_step_hook():
    optimizer_step_hook.disable()


print("[msmemscope]: Enable optimizer step hook.")
optimizer_step_hook = OptimizerStepHook()
