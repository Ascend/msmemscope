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

from ._msmemscope import _watcher
from ._msmemscope import _tracer
from ._msmemscope import start, stop, step, config  # noqa: F401
from ._msmemscope import _enable_npu_sanitizer
from .analyzer import analyze, list_analyzers, get_analyzer_config, check_leaks, check_inefficient  # noqa: F401
from .hijacker.hijack_manager import memscope_hijack_manager
from .record_function import RecordFunction  # noqa: F401
from .take_snapshot import TakeSnapshot  # noqa: F401
from .utils import import_with_optional_deps

tracer = _tracer
watcher = _watcher

# 指定需要的依赖包
take_snapshot = import_with_optional_deps("take_snapshot", "take_snapshot", ["torch", "torch_npu"])


def init_framework_hooks(framework: str, version: str, component: str, hook_type: str):
    """
    init_framework_hooks:注册对应framework的所有默认hook函数钩子

    framework:支持框架(当前支持vllm_ascend,后续将支持verl/fsdp)
    version:框架对应版本
    component:指定要hook的组件或模块 vllm_ascend对应的worker,verl对应的actor_rollout\ref\critic\reward等
    hook_type:对应的hook函数(decompose:显存拆解,sanpshot:显存快照)
    """
    return memscope_hijack_manager.init_framework_hooks(framework, version, component, hook_type)


def cleanup_framework_hooks():
    """
    cleanup_framework_hooks:清除对应framework的所有默认hook函数钩子
    """
    return memscope_hijack_manager.cleanup_framework_hooks()


def enable_npu_sanitizer():
    """
    通过 msmemscope 联动使能 NPU Sanitizer。

    该接口内部会：
    1. 调用 torch_npu 原生的 _sanitizer.enable_npu_sanitizer() 使能原生 sanitizer
    2. 通知 C++ 层 SanitizerOpHandler 进入激活状态，
       开始识别并处理 sanitizer-op: 前缀的 MSTX 打点消息

    使用方式：
        import msmemscope
        msmemscope.enable_npu_sanitizer()

    使能后，开发者在自定义算子调用处通过 mstx.mark("sanitizer-op: ...") 上报的
    信息将被截获并转换为 kernel launch 事件，送入原生 sanitizer 分析管线。
    """
    from torch_npu.npu import _sanitizer

    _sanitizer.enable_npu_sanitizer()
    _enable_npu_sanitizer()
