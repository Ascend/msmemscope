#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
"""
cpu_tensor_collection 单元测试: CPU tensor hook 注册/回调/dedup/lifecycle

运行: cd test/unit_python && python -m unittest -v test_cpu_tensor_collection
不依赖 torch/_msmemscope 编译产物(通过 sys.modules mock 注入)
"""

import importlib.util
import os
import sys
import types
import unittest
from unittest.mock import MagicMock, patch

_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "python", "msmemscope")


def _get_or_create_module(name):
    mod = sys.modules.get(name)
    if mod is None:
        mod = types.ModuleType(name)
        sys.modules[name] = mod
    return mod


def _load_module():
    """mock torch, _msmemscope, hijacker, 加载 cpu_tensor_collection.py"""
    # mock torch
    torch_mod = types.ModuleType("torch")
    class FakeTensor:
        pass
    torch_mod.Tensor = FakeTensor
    torch_mod.is_tensor = staticmethod(lambda t: isinstance(t, FakeTensor))
    sys.modules["torch"] = torch_mod

    # mock msmemscope package tree
    _get_or_create_module("msmemscope").__path__ = []
    _get_or_create_module("msmemscope.hijacker").__path__ = []

    # mock _msmemscope._report_cpu_tensor
    mscore = _get_or_create_module("msmemscope._msmemscope")
    mscore._report_cpu_tensor = MagicMock(return_value=True)

    # mock hijack_utility
    hijack_util = types.ModuleType("msmemscope.hijacker.hijack_utility")
    hijack_util.POST_HOOK = 2
    hijack_util.hijacker = MagicMock(return_value="handler_001")
    hijack_util.release = MagicMock()
    sys.modules["msmemscope.hijacker.hijack_utility"] = hijack_util

    spec = importlib.util.spec_from_file_location(
        "msmemscope.cpu_tensor_collection", os.path.join(_SRC, "cpu_tensor_collection.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules["msmemscope.cpu_tensor_collection"] = module
    spec.loader.exec_module(module)
    return module


_mod = _load_module()
_FakeTensor = sys.modules["torch"].Tensor
_hijacker = sys.modules["msmemscope.hijacker.hijack_utility"].hijacker
_release = sys.modules["msmemscope.hijacker.hijack_utility"].release
_report = sys.modules["msmemscope._msmemscope"]._report_cpu_tensor


def _cpu_tensor(ptr=0x1000, nbytes=1024):
    t = _FakeTensor()
    storage = MagicMock()
    storage.data_ptr.return_value = ptr
    storage.nbytes.return_value = nbytes
    t.untyped_storage = MagicMock(return_value=storage)
    t.device = types.SimpleNamespace(type="cpu")
    return t


def _npu_tensor():
    t = _FakeTensor()
    t.device = types.SimpleNamespace(type="npu")
    return t


class TestCpuTensorCollection(unittest.TestCase):
    def setUp(self):
        _mod._cpu_blocks.clear()
        _mod._handlers.clear()
        _report.reset_mock()
        _report.side_effect = None
        _report.return_value = True
        _hijacker.reset_mock()
        _release.reset_mock()

    # --- enable / disable ---

    def test_enable_disable(self):
        _mod.enable_cpu_tensor_collect()
        self.assertEqual(len(_mod._handlers), 3)
        self.assertEqual(_hijacker.call_count, 3)
        # idempotent
        _mod.enable_cpu_tensor_collect()
        self.assertEqual(_hijacker.call_count, 3)

        _mod.disable_cpu_tensor_collect()
        self.assertEqual(len(_mod._handlers), 0)
        self.assertEqual(len(_mod._cpu_blocks), 0)
        self.assertEqual(_release.call_count, 3)

    # --- _is_cpu_tensor ---

    def test_is_cpu_tensor(self):
        self.assertTrue(_mod._is_cpu_tensor(_cpu_tensor()))
        self.assertFalse(_mod._is_cpu_tensor(_npu_tensor()))
        self.assertFalse(_mod._is_cpu_tensor(42))

    # --- _on_cpu_tensor_created ---

    def test_on_cpu_tensor_created(self):
        t = _cpu_tensor(ptr=0x2000, nbytes=4096)
        self.assertIs(_mod._on_cpu_tensor_created(t), t)
        _report.assert_called_once()
        self.assertIn(0x2000, _mod._cpu_blocks)

        # skip non-cpu / zero-ptr
        _mod._on_cpu_tensor_created(_npu_tensor())
        _mod._on_cpu_tensor_created(_cpu_tensor(ptr=0, nbytes=0))
        self.assertEqual(_report.call_count, 1)  # no new calls

        # same-ptr dedup in Python channel
        t2 = _cpu_tensor(ptr=0x2000, nbytes=4096)
        _mod._on_cpu_tensor_created(t2)
        self.assertEqual(_report.call_count, 1)  # still 1, dedup skipped

        # cross-channel dedup: C++ returns False -> no finalize attached
        _report.return_value = False
        t3 = _cpu_tensor(ptr=0x3000, nbytes=1024)
        _mod._on_cpu_tensor_created(t3)
        self.assertNotIn(0x3000, _mod._cpu_blocks)

    # --- _on_storage_freed ---

    def test_on_storage_freed(self):
        t = _cpu_tensor(ptr=0x4000, nbytes=8192)
        _mod._on_cpu_tensor_created(t)
        _report.reset_mock()

        # normal free
        _mod._on_storage_freed(0x4000)
        _report.assert_called_once_with(0x4000, 8192, False, "")
        self.assertNotIn(0x4000, _mod._cpu_blocks)

        # unknown addr noop
        _report.reset_mock()
        _mod._on_storage_freed(0xDEAD)
        _report.assert_not_called()

    # --- exception safety ---

    def test_exception_safe(self):
        _report.side_effect = RuntimeError("boom")
        t = _cpu_tensor(ptr=0x5000, nbytes=1024)
        self.assertIs(_mod._on_cpu_tensor_created(t), t)


if __name__ == "__main__":
    unittest.main()
