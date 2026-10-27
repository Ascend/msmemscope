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

"""Slim CPU UT for Triton ACCESS (no NPU)."""

import contextlib
import importlib.util
import io
import sys
import types as pytypes
import unittest
from unittest import mock
from pathlib import Path

_SRC = Path(__file__).resolve().parents[2] / "python" / "msmemscope"


def _get_or_create_module(name: str) -> pytypes.ModuleType:
    mod = sys.modules.get(name)
    if mod is None:
        mod = pytypes.ModuleType(name)
        sys.modules[name] = mod
    return mod


def _load_tc():
    pkg = _get_or_create_module("msmemscope")
    pkg.__path__ = [str(_SRC)]
    hijacker_pkg = _get_or_create_module("msmemscope.hijacker")
    hijacker_pkg.__path__ = [str(_SRC / "hijacker")]
    _get_or_create_module("msmemscope._msmemscope")

    util_path = _SRC / "hijacker" / "hijack_utility.py"
    util_spec = importlib.util.spec_from_file_location(
        "msmemscope.hijacker.hijack_utility", util_path
    )
    util_mod = importlib.util.module_from_spec(util_spec)
    sys.modules["msmemscope.hijacker.hijack_utility"] = util_mod
    assert util_spec.loader is not None
    util_spec.loader.exec_module(util_mod)

    tc_path = _SRC / "triton_collection.py"
    tc_spec = importlib.util.spec_from_file_location(
        "msmemscope.triton_collection", tc_path
    )
    module = importlib.util.module_from_spec(tc_spec)
    sys.modules["msmemscope.triton_collection"] = module
    assert tc_spec.loader is not None
    tc_spec.loader.exec_module(module)
    return module


tc = _load_tc()
_DRIVER_MOD = tc._NPU_LAUNCHER_MODULE


def _ensure_pkg(name: str) -> pytypes.ModuleType:
    mod = sys.modules.get(name)
    if mod is None:
        mod = pytypes.ModuleType(name)
        mod.__path__ = []  # type: ignore[attr-defined]
        sys.modules[name] = mod
    return mod


@contextlib.contextmanager
def _fake_ascend_driver(launcher_cls):
    saved = {}
    for name in list(sys.modules):
        if name == "triton" or name.startswith("triton."):
            saved[name] = sys.modules.pop(name)
    try:
        _ensure_pkg("triton")
        _ensure_pkg("triton.backends")
        _ensure_pkg("triton.backends.ascend")
        _ensure_pkg("triton.runtime")
        driver = pytypes.ModuleType(_DRIVER_MOD)
        driver.NPULauncher = launcher_cls
        sys.modules[_DRIVER_MOD] = driver
        runtime_driver = pytypes.ModuleType("triton.runtime.driver")
        runtime_driver.active = pytypes.SimpleNamespace(launcher_cls=launcher_cls)
        sys.modules["triton.runtime.driver"] = runtime_driver
        with mock.patch.object(
            tc.TritonCollector,
            "_active_launcher_targets",
            return_value=[(_DRIVER_MOD, "NPULauncher")],
        ):
            yield driver
    finally:
        for name in list(sys.modules):
            if name == "triton" or name.startswith("triton."):
                sys.modules.pop(name, None)
        for name, old in saved.items():
            sys.modules[name] = old


def _launch_args(tensor, name="unit_k", kinds=None):
    if kinds is None:
        kinds = [1]
    packed = {"tensor_kinds": kinds, "name": name}
    launch = {"name": name}
    return (1, 1, 1, None, None, packed, launch, None, None, tensor)


@contextlib.contextmanager
def _capture_stderr():
    buf = io.StringIO()
    with contextlib.redirect_stderr(buf):
        yield buf


class FakeTensor:
    def __init__(self, ptr, shape, dtype="torch.float32", device="npu:0", itemsize=4):
        self._ptr = ptr
        self.shape = shape
        self.dtype = dtype
        self.device = device
        self.itemsize = itemsize
        self.is_npu = True

    def data_ptr(self):
        return self._ptr

    def numel(self):
        n = 1
        for d in self.shape:
            n *= int(d)
        return n

    def element_size(self):
        return self.itemsize


def _access_marks(emitted):
    return [m for m in emitted if m.startswith("memscope-aten-ac:")]


class TestKindMapping(unittest.TestCase):
    def test_input_output_inout_unknown(self):
        self.assertEqual(tc.kind_to_access_flags(0), (True, False, False))
        self.assertEqual(tc.kind_to_access_flags(1), (False, True, True))
        self.assertEqual(tc.kind_to_access_flags(2), (True, True, True))
        self.assertIsNone(tc.kind_to_access_flags(None))
        self.assertIsNone(tc.kind_to_access_flags(99))

    def test_emit_begin_and_access_for_aligned_kinds(self):
        tensors = [FakeTensor(0x1000 + i * 0x100, (8,)) for i in range(3)]
        kinds = [0, 0, 1]
        with mock.patch.object(
            tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)
        ):
            emitted = tc.emit_triton_access_events(
                "triton.vector_binary",
                tuple(tensors),
                kinds,
                mark_fn=lambda m: None,
            )
        self.assertTrue(emitted[0].startswith("memscope-aten-b:"))
        self.assertEqual(len(_access_marks(emitted)), 3)


class TestCollectorLifecycle(unittest.TestCase):
    def test_enable_disable_wraps_and_emits_be(self):
        class FakeLauncher:
            def __call__(self, *args, **kwargs):
                return "ok"

        collector = tc.TritonCollector()
        with _fake_ascend_driver(FakeLauncher):
            collector.enable()
            t = FakeTensor(0x10, (2,))
            marks = []
            with mock.patch.object(
                tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)
            ):
                with mock.patch.object(
                    tc, "_default_mark", side_effect=lambda m: marks.append(m)
                ):
                    ret = FakeLauncher()(*_launch_args(t))
            self.assertEqual(ret, "ok")
            self.assertTrue(any(m.startswith("memscope-aten-b:") for m in marks))
            self.assertTrue(any(m.startswith("memscope-aten-e:") for m in marks))
            collector.disable()
            marks.clear()
            with mock.patch.object(
                tc, "_default_mark", side_effect=lambda m: marks.append(m)
            ):
                FakeLauncher()(*_launch_args(t))
            self.assertEqual(marks, [])

    def test_pre_emit_raise_still_pairs_end(self):

        class FakeLauncher:
            def __call__(self, *args, **kwargs):
                return "ok"

        collector = tc.TritonCollector()
        t = FakeTensor(0x10, (2,))
        marks = []

        def flaky_emit(*a, **k):
            marks.append(tc.build_begin_mark("triton.unit_k"))
            raise RuntimeError("mid emit")

        with _fake_ascend_driver(FakeLauncher):
            collector.enable()
            with mock.patch.object(tc, "emit_triton_access_events", side_effect=flaky_emit):
                with mock.patch.object(
                    tc, "_default_mark", side_effect=lambda m: marks.append(m)
                ):
                    with _capture_stderr() as err:
                        ret = FakeLauncher()(*_launch_args(t))
            collector.disable()
        self.assertEqual(ret, "ok")
        self.assertTrue(any(m.startswith("memscope-aten-b:") for m in marks))
        self.assertTrue(any(m.startswith("memscope-aten-e:") for m in marks))
        self.assertIn("ACCESS collection failed", err.getvalue())


if __name__ == "__main__":
    unittest.main()
