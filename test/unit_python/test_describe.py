#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
"""
describe 单元测试: 分级标签模型的用户接口/内部接口拆分

运行: cd test/unit_python && python -m unittest -v test_describe
不依赖 _msmemscope 编译产物(通过 sys.modules mock 注入 _describer)
"""

import importlib.util
import os
import sys
import types
import unittest
from unittest.mock import MagicMock

_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "python", "msmemscope")


def _get_or_create_module(name):
    mod = sys.modules.get(name)
    if mod is None:
        mod = types.ModuleType(name)
        sys.modules[name] = mod
    return mod


def _load_describe():
    """以独立模块加载 describe.py, mock 掉 C++ 扩展(_describer)"""
    msmemscope_pkg = _get_or_create_module("msmemscope")
    msmemscope_pkg.__path__ = []
    mscore = _get_or_create_module("msmemscope._msmemscope")
    if not hasattr(mscore, "_describer"):
        mscore._describer = MagicMock()

    spec = importlib.util.spec_from_file_location(
        "msmemscope.describe", os.path.join(_SRC, "describe.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules["msmemscope.describe"] = module
    spec.loader.exec_module(module)
    return module


_mod = _load_describe()


def _describer_mock():
    return sys.modules["msmemscope._msmemscope"]._describer


class TestLevelConstants(unittest.TestCase):
    """级别常量与 C++ OwnerLevel 枚举对齐"""

    def test_level_values(self):
        self.assertEqual(_mod.FRAMEWORK, 0)
        self.assertEqual(_mod.COMPONENT, 1)
        self.assertEqual(_mod.PROCESS, 2)
        self.assertEqual(_mod.DETAIL_1, 3)
        self.assertEqual(_mod.DETAIL_2, 4)
        self.assertEqual(_mod.USER_DEFINED_1, 5)
        self.assertEqual(_mod.USER_DEFINED_2, 6)
        self.assertEqual(_mod.USER_DEFINED_3, 7)


class TestInternalInterface(unittest.TestCase):
    """内部接口: 带级别, 逐段转发到 _describer"""

    def setUp(self):
        _describer_mock().reset_mock()

    def test_describe_label(self):
        _mod.describe_label("fsdp2", _mod.COMPONENT)
        _describer_mock().describe_label.assert_called_once_with("fsdp2", _mod.COMPONENT)

    def test_undescribe_label(self):
        _mod.undescribe_label("activation", _mod.PROCESS)
        _describer_mock().undescribe_label.assert_called_once_with("activation", _mod.PROCESS)

    def test_describe_addr_multi_level(self):
        labels = [("vllm", _mod.COMPONENT), ("weights", _mod.DETAIL_1), ("lora", _mod.DETAIL_2)]
        _mod.describe_addr(0x1000, labels)
        _describer_mock().describe_addr.assert_called_once_with(0x1000, labels)


class TestUserInterface(unittest.TestCase):
    """用户接口: 无级别, 范围标签走 describe/undescribe, 地址直标默认 USER_DEFINED_1"""

    def setUp(self):
        _describer_mock().reset_mock()

    def test_context_manager(self):
        with _mod.describer(owner="forward_pass"):
            pass
        _describer_mock().describe.assert_called_once_with("forward_pass")
        _describer_mock().undescribe.assert_called_once_with("forward_pass")

    def test_decorator(self):
        @_mod.describer(owner="train_step")
        def step():
            return 1

        self.assertEqual(step(), 1)
        _describer_mock().describe.assert_called_once_with("train_step")
        _describer_mock().undescribe.assert_called_once_with("train_step")

    def test_addr_mark_defaults_user_defined_1(self):
        _mod.describer(0x2000, "my_tensor")
        _describer_mock().describe_addr.assert_called_once_with(0x2000, [("my_tensor", _mod.USER_DEFINED_1)])

    def test_int_addr_direct(self):
        _mod.describer(0x3000, "direct")
        _describer_mock().describe_addr.assert_called_once_with(0x3000, [("direct", _mod.USER_DEFINED_1)])


if __name__ == "__main__":
    unittest.main()
