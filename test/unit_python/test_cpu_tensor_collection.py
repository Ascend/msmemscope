#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
"""
cpu_tensor_collection 单元测试: CPU tensor hook 注册/回调/dedup/lifecycle

运行: cd test/unit_python && python -m unittest -v test_cpu_tensor_collection
不依赖 torch 安装与 _msmemscope 编译产物(通过 sys.modules mock 注入)
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


def _load_cpu_tensor_collection():
    """mock torch, _msmemscope, hijacker 并以独立模块加载 cpu_tensor_collection.py"""
    # --- mock torch ---
    torch_mod = types.ModuleType("torch")

    class FakeTensor:
        pass

    torch_mod.Tensor = FakeTensor
    torch_mod.is_tensor = staticmethod(lambda t: isinstance(t, FakeTensor))
    sys.modules["torch"] = torch_mod

    # --- mock msmemscope package tree ---
    msmemscope_pkg = _get_or_create_module("msmemscope")
    msmemscope_pkg.__path__ = []
    hijacker_pkg = _get_or_create_module("msmemscope.hijacker")
    hijacker_pkg.__path__ = []

    # --- mock _msmemscope._report_cpu_tensor (returns True = accepted by default) ---
    mscore = _get_or_create_module("msmemscope._msmemscope")
    mscore._report_cpu_tensor = MagicMock(return_value=True)

    # --- mock hijack_utility (POST_HOOK = 2, hijacker/return handler_id str, release = noop) ---
    hijack_util = types.ModuleType("msmemscope.hijacker.hijack_utility")
    hijack_util.POST_HOOK = 2
    hijack_util.hijacker = MagicMock(return_value="test_handler_001")
    hijack_util.release = MagicMock()
    sys.modules["msmemscope.hijacker.hijack_utility"] = hijack_util

    # --- load real source ---
    spec = importlib.util.spec_from_file_location(
        "msmemscope.cpu_tensor_collection", os.path.join(_SRC, "cpu_tensor_collection.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules["msmemscope.cpu_tensor_collection"] = module
    spec.loader.exec_module(module)
    return module


_mod = _load_cpu_tensor_collection()
_FakeTensor = sys.modules["torch"].Tensor
_hijacker = sys.modules["msmemscope.hijacker.hijack_utility"].hijacker
_release = sys.modules["msmemscope.hijacker.hijack_utility"].release
_report = sys.modules["msmemscope._msmemscope"]._report_cpu_tensor


def _make_cpu_tensor(ptr=0x1000, nbytes=1024):
    """构造一个模拟 CPU tensor: device.type == 'cpu', 有 untyped_storage()"""
    t = _FakeTensor()
    storage = MagicMock()
    storage.data_ptr.return_value = ptr
    storage.nbytes.return_value = nbytes
    t.untyped_storage = MagicMock(return_value=storage)
    t.device = types.SimpleNamespace(type="cpu")
    return t


def _make_npu_tensor():
    """构造一个 NPU tensor"""
    t = _FakeTensor()
    t.device = types.SimpleNamespace(type="npu")
    return t


class TestIsCpuTensor(unittest.TestCase):
    def test_cpu_tensor_returns_true(self):
        t = _make_cpu_tensor()
        self.assertTrue(_mod._is_cpu_tensor(t))

    def test_npu_tensor_returns_false(self):
        t = _make_npu_tensor()
        self.assertFalse(_mod._is_cpu_tensor(t))

    def test_non_tensor_returns_false(self):
        self.assertFalse(_mod._is_cpu_tensor(42))
        self.assertFalse(_mod._is_cpu_tensor("string"))
        self.assertFalse(_mod._is_cpu_tensor(None))


class TestEnableDisable(unittest.TestCase):
    def setUp(self):
        _mod._handlers.clear()
        _mod._cpu_blocks.clear()
        _hijacker.reset_mock()
        _release.reset_mock()

    def test_enable_registers_three_hooks(self):
        _mod.enable_cpu_tensor_collect()
        self.assertEqual(len(_mod._handlers), 3)
        self.assertEqual(_hijacker.call_count, 3)

    def test_enable_idempotent(self):
        _mod.enable_cpu_tensor_collect()
        first_count = _hijacker.call_count
        _mod.enable_cpu_tensor_collect()
        self.assertEqual(_hijacker.call_count, first_count)  # no new registrations

    def test_disable_clears_state(self):
        _mod.enable_cpu_tensor_collect()
        self.assertEqual(len(_mod._handlers), 3)
        _mod.disable_cpu_tensor_collect()
        self.assertEqual(len(_mod._handlers), 0)
        self.assertEqual(len(_mod._cpu_blocks), 0)
        self.assertEqual(_release.call_count, 3)

    def test_disable_idempotent(self):
        _mod.disable_cpu_tensor_collect()
        _mod.disable_cpu_tensor_collect()
        self.assertEqual(len(_mod._handlers), 0)
        self.assertEqual(len(_mod._cpu_blocks), 0)


class TestOnCpuTensorCreated(unittest.TestCase):
    def setUp(self):
        _mod._cpu_blocks.clear()
        _report.reset_mock()
        _report.side_effect = None
        _report.return_value = True

    def test_cpu_tensor_reports_malloc(self):
        t = _make_cpu_tensor(ptr=0x2000, nbytes=4096)
        result = _mod._on_cpu_tensor_created(t)
        self.assertIs(result, t)
        _report.assert_called_once()
        self.assertIn(0x2000, _mod._cpu_blocks)
        size, finalizer = _mod._cpu_blocks[0x2000]
        self.assertEqual(size, 4096)
        self.assertIsNotNone(finalizer)

    def test_non_cpu_tensor_skipped(self):
        t = _make_npu_tensor()
        _mod._on_cpu_tensor_created(t)
        _report.assert_not_called()
        self.assertEqual(len(_mod._cpu_blocks), 0)

    def test_non_tensor_skipped(self):
        _mod._on_cpu_tensor_created("not_a_tensor")
        _report.assert_not_called()
        self.assertEqual(len(_mod._cpu_blocks), 0)

    def test_zero_ptr_skipped(self):
        t = _make_cpu_tensor(ptr=0, nbytes=0)
        result = _mod._on_cpu_tensor_created(t)
        self.assertIs(result, t)
        _report.assert_not_called()
        self.assertEqual(len(_mod._cpu_blocks), 0)

    def test_same_ptr_dedup(self):
        t1 = _make_cpu_tensor(ptr=0x3000, nbytes=1024)
        _mod._on_cpu_tensor_created(t1)
        self.assertEqual(_report.call_count, 1)
        self.assertIn(0x3000, _mod._cpu_blocks)

        # Second creation with same ptr (e.g., shared storage) -> no MALLOC report
        t2 = _make_cpu_tensor(ptr=0x3000, nbytes=1024)
        _mod._on_cpu_tensor_created(t2)
        # _report_cpu_tensor should NOT be called again (same-channel dedup)
        self.assertEqual(_report.call_count, 1)

    def test_cross_channel_dedup_no_finalize(self):
        # _cpp._report_cpu_tensor returns False → Python should NOT attach finalize
        _report.return_value = False
        t = _make_cpu_tensor(ptr=0x4000, nbytes=2048)
        result = _mod._on_cpu_tensor_created(t)
        self.assertIs(result, t)
        _report.assert_called_once()
        self.assertNotIn(0x4000, _mod._cpu_blocks)

    def test_exception_safe(self):
        _report.side_effect = RuntimeError("C++ extension not available")
        t = _make_cpu_tensor(ptr=0x5000, nbytes=1024)
        # Should not raise; returns original tensor
        result = _mod._on_cpu_tensor_created(t)
        self.assertIs(result, t)

    def test_error_caught_in_creation(self):
        """Exception in _on_cpu_tensor_created prints warning but returns ret"""
        t = _make_cpu_tensor(ptr=0x5001, nbytes=1024)
        # Force exception by removing _report_cpu_tensor from mock
        with patch.object(sys.modules["msmemscope._msmemscope"], "_report_cpu_tensor", None):
            result = _mod._on_cpu_tensor_created(t)
            self.assertIs(result, t)  # still returns ret


class TestOnStorageFreed(unittest.TestCase):
    def setUp(self):
        _mod._cpu_blocks.clear()
        _report.reset_mock()
        _report.side_effect = None
        _report.return_value = True

    def test_freed_reports_free(self):
        t = _make_cpu_tensor(ptr=0x6000, nbytes=8192)
        _mod._on_cpu_tensor_created(t)
        _report.reset_mock()

        _mod._on_storage_freed(0x6000)
        _report.assert_called_once_with(0x6000, 8192, False, "")
        self.assertNotIn(0x6000, _mod._cpu_blocks)

    def test_unknown_ptr_noop(self):
        _mod._on_storage_freed(0xDEAD)
        _report.assert_not_called()
        self.assertEqual(len(_mod._cpu_blocks), 0)

    def test_double_free_noop(self):
        t = _make_cpu_tensor(ptr=0x7000, nbytes=1024)
        _mod._on_cpu_tensor_created(t)
        _report.reset_mock()

        _mod._on_storage_freed(0x7000)
        self.assertEqual(_report.call_count, 1)
        self.assertNotIn(0x7000, _mod._cpu_blocks)

        # Second free of same ptr is noop (already removed from _cpu_blocks)
        _report.reset_mock()
        _mod._on_storage_freed(0x7000)
        _report.assert_not_called()


class TestModuleStateIsolation(unittest.TestCase):
    """验证 enable/disable 之间状态完全隔离"""

    def setUp(self):
        _mod.disable_cpu_tensor_collect()
        _report.reset_mock()
        _report.side_effect = None
        _report.return_value = True
        _hijacker.reset_mock()
        _release.reset_mock()

    def test_full_lifecycle(self):
        _mod.enable_cpu_tensor_collect()
        self.assertEqual(len(_mod._handlers), 3)

        # Create two tensors
        t1 = _make_cpu_tensor(ptr=0xA000, nbytes=100)
        t2 = _make_cpu_tensor(ptr=0xB000, nbytes=200)
        _mod._on_cpu_tensor_created(t1)
        _mod._on_cpu_tensor_created(t2)
        self.assertEqual(len(_mod._cpu_blocks), 2)

        # Free one
        _mod._on_storage_freed(0xA000)
        self.assertEqual(len(_mod._cpu_blocks), 1)
        self.assertNotIn(0xA000, _mod._cpu_blocks)
        self.assertIn(0xB000, _mod._cpu_blocks)

        # Disable clears all
        _mod.disable_cpu_tensor_collect()
        self.assertEqual(len(_mod._cpu_blocks), 0)
        self.assertEqual(len(_mod._handlers), 0)


if __name__ == "__main__":
    unittest.main()
