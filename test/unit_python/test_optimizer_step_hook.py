#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
"""
optimizer_step_hook 单元测试: DTensor 适配(FSDP2 参数/梯度/优化器状态)

运行: cd test/unit_python && python -m unittest -v test_optimizer_step_hook
不依赖 torch 安装与 _msmemscope 编译产物(通过 sys.modules mock 注入)
"""

import importlib.util
import os
import sys
import types
import unittest
from unittest.mock import MagicMock

_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "python", "msmemscope")


def _get_or_create_module(name):
    """获取或创建 mock 模块(与其他测试文件共存,避免互相覆盖 sys.modules)"""
    mod = sys.modules.get(name)
    if mod is None:
        mod = types.ModuleType(name)
        sys.modules[name] = mod
    return mod


def _load_optimizer_step_hook():
    """mock torch 与 _msmemscope, 以独立模块加载 optimizer_step_hook.py"""
    msmemscope_pkg = _get_or_create_module("msmemscope")
    msmemscope_pkg.__path__ = []
    mscore = _get_or_create_module("msmemscope._msmemscope")
    if not hasattr(mscore, "_describer"):
        mscore._describer = MagicMock()  # describe.py 依赖

    # optimizer_step_hook 依赖 describe(分级标签常量), 先加载并注册到 sys.modules
    spec = importlib.util.spec_from_file_location(
        "msmemscope.describe", os.path.join(_SRC, "describe.py"))
    desc_module = importlib.util.module_from_spec(spec)
    sys.modules["msmemscope.describe"] = desc_module
    spec.loader.exec_module(desc_module)

    torch_mod = types.ModuleType("torch")

    class _Tensor:
        """模拟 torch.Tensor, 仅用于 isinstance 判断"""

    torch_mod.Tensor = _Tensor
    torch_mod.is_tensor = staticmethod(lambda t: isinstance(t, _Tensor))
    optim_mod = types.ModuleType("torch.optim")
    optim_mod.Optimizer = type("Optimizer", (), {})
    optim_impl = types.ModuleType("torch.optim.optimizer")
    optim_impl.register_optimizer_step_post_hook = MagicMock()
    torch_mod.optim = optim_mod
    sys.modules["torch"] = torch_mod
    sys.modules["torch.optim"] = optim_mod
    sys.modules["torch.optim.optimizer"] = optim_impl

    spec = importlib.util.spec_from_file_location(
        "msmemscope.optimizer_step_hook", os.path.join(_SRC, "optimizer_step_hook.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules["msmemscope.optimizer_step_hook"] = module
    spec.loader.exec_module(module)
    return module


_mod = _load_optimizer_step_hook()
_DESC = sys.modules["msmemscope.describe"]
_TorchTensor = sys.modules["torch"].Tensor


class FakeTensor(_TorchTensor):
    """模拟普通 NPU 张量"""

    def __init__(self, device="npu:0", ptr=0x1000):
        self._device = device
        self._ptr = ptr

    @property
    def device(self):
        return self._device

    def data_ptr(self):
        return self._ptr


class FakeDTensor(_TorchTensor):
    """模拟 DTensor: 以 _local_tensor 承载实际存储, grad 与 nn.Parameter 语义一致默认为 None"""

    def __init__(self, local):
        self._local_tensor = local
        self.grad = None


class TestToLocalTensor(unittest.TestCase):
    """分布式张量本地化"""

    def test_dtensor_returns_local(self):
        local = FakeTensor(ptr=0x100)
        self.assertIs(_mod._to_local_tensor(FakeDTensor(local)), local)

    def test_plain_tensor_unchanged(self):
        tensor = FakeTensor(ptr=0x200)
        self.assertIs(_mod._to_local_tensor(tensor), tensor)

    def test_non_tensor_local_falls_back(self):
        weird = FakeTensor(ptr=0x300)
        weird._local_tensor = "not-a-tensor"
        self.assertIs(_mod._to_local_tensor(weird), weird)


class TestAppendTensorInfo(unittest.TestCase):
    """npu 判定与地址上报"""

    def test_npu_tensor_appended(self):
        info = []
        _mod.append_tensor_info(info, FakeTensor(ptr=0x400), [("weight", _DESC.DETAIL_1)])
        self.assertEqual(info, [(0x400, [("weight", _DESC.DETAIL_1)])])

    def test_cpu_tensor_skipped(self):
        info = []
        _mod.append_tensor_info(info, FakeTensor(device="cpu", ptr=0x400), [("weight", _DESC.DETAIL_1)])
        self.assertEqual(info, [])

    def test_dtensor_uses_local_ptr(self):
        info = []
        _mod.append_tensor_info(info, FakeDTensor(FakeTensor(ptr=0x500)), [("gradient", _DESC.DETAIL_1)])
        self.assertEqual(info, [(0x500, [("gradient", _DESC.DETAIL_1)])])


class TestProcessParam(unittest.TestCase):
    """FSDP2 场景: DTensor 参数/梯度/优化器状态"""

    def test_dtensor_param_grad_state(self):
        param = FakeDTensor(FakeTensor(ptr=0x100))
        param.grad = FakeDTensor(FakeTensor(ptr=0x200))
        opt = types.SimpleNamespace(state={
            param: {
                "step": FakeTensor(ptr=0x300),
                "exp_avg": FakeDTensor(FakeTensor(ptr=0x400)),
            }
        })
        info = []
        _mod.process_param(info, param, opt)
        self.assertEqual(info, [
            (0x100, [("weight", _DESC.DETAIL_1)]),
            (0x200, [("gradient", _DESC.DETAIL_1)]),
            (0x300, [("optimizer_state", _DESC.DETAIL_1)]),
            (0x400, [("optimizer_state", _DESC.DETAIL_1)]),
        ])

    def test_no_grad_no_state(self):
        param = FakeDTensor(FakeTensor(ptr=0x100))
        opt = types.SimpleNamespace(state={})
        info = []
        _mod.process_param(info, param, opt)
        self.assertEqual(info, [(0x100, [("weight", _DESC.DETAIL_1)])])


class TestGlobalOptimizerStepHook(unittest.TestCase):
    """step 钩子整体行为: 地址直标走 describe.py 内部接口(与 taggers 一致)"""

    def test_describe_addr_called(self):
        describer = sys.modules["msmemscope._msmemscope"]._describer
        describer.reset_mock()
        param = FakeDTensor(FakeTensor(ptr=0x100))
        opt = types.SimpleNamespace(param_groups=[{"params": [param]}], state={})
        _mod.global_optimizer_step_hook(opt, (), {})
        describer.describe_addr.assert_called_once_with(0x100, [("weight", _DESC.DETAIL_1)])


if __name__ == "__main__":
    unittest.main()
