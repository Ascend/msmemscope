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
from pathlib import Path
from unittest import mock

_SRC = Path(__file__).resolve().parents[2] / "python" / "msmemscope"


def _mod(name):
    m = sys.modules.get(name)
    if m is None:
        m = pytypes.ModuleType(name)
        sys.modules[name] = m
    return m


def _load_tc():
    pkg = _mod("msmemscope")
    pkg.__path__ = [str(_SRC)]
    hp = _mod("msmemscope.hijacker")
    hp.__path__ = [str(_SRC / "hijacker")]
    _mod("msmemscope._msmemscope")
    up = _SRC / "hijacker" / "hijack_utility.py"
    us = importlib.util.spec_from_file_location("msmemscope.hijacker.hijack_utility", up)
    um = importlib.util.module_from_spec(us)
    sys.modules[us.name] = um
    us.loader.exec_module(um)
    tp = _SRC / "triton_collection.py"
    ts = importlib.util.spec_from_file_location("msmemscope.triton_collection", tp)
    tm = importlib.util.module_from_spec(ts)
    sys.modules[ts.name] = tm
    ts.loader.exec_module(tm)
    return tm


tc = _load_tc()
_DRV = tc._NPU_LAUNCHER_MODULE


def _pkg(name):
    m = _mod(name)
    if not hasattr(m, "__path__"):
        m.__path__ = []
    return m


@contextlib.contextmanager
def _fake_driver(launcher_cls):
    saved = {k: sys.modules.pop(k) for k in list(sys.modules) if k == "triton" or k.startswith("triton.")}
    try:
        for n in ("triton", "triton.backends", "triton.backends.ascend", "triton.runtime"):
            _pkg(n)
        driver = pytypes.ModuleType(_DRV)
        driver.NPULauncher = launcher_cls
        sys.modules[_DRV] = driver
        rd = pytypes.ModuleType("triton.runtime.driver")
        rd.active = pytypes.SimpleNamespace(launcher_cls=launcher_cls)
        sys.modules["triton.runtime.driver"] = rd
        with mock.patch.object(tc.TritonCollector, "_active_launcher_targets", return_value=[(_DRV, "NPULauncher")]):
            yield
    finally:
        for k in list(sys.modules):
            if k == "triton" or k.startswith("triton."):
                sys.modules.pop(k, None)
        sys.modules.update(saved)


def _args(tensor, name="unit_k", kinds=None):
    kinds = kinds or [1]
    return (1, 1, 1, None, None, {"tensor_kinds": kinds, "name": name}, {"name": name}, None, None, tensor)


class FakeTensor:
    def __init__(self, ptr, shape, itemsize=4):
        self._ptr, self.shape, self.itemsize = ptr, shape, itemsize
        self.dtype, self.device, self.is_npu = "torch.float32", "npu:0", True

    def data_ptr(self):
        return self._ptr

    def numel(self):
        n = 1
        for d in self.shape:
            n *= int(d)
        return n

    def element_size(self):
        return self.itemsize


class TestCore(unittest.TestCase):
    def test_kinds(self):
        self.assertEqual(tc.kind_to_access_flags(0), (True, False, False))
        self.assertEqual(tc.kind_to_access_flags(1), (False, True, True))
        self.assertIsNone(tc.kind_to_access_flags(99))

    def test_kinds_missing_emits_unknown(self):
        t, marks = FakeTensor(0x20, (4,)), []
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            out = tc.emit_triton_access_events("triton.unk", (t,), None, mark_fn=marks.append)
        ac = [m for m in out if m.startswith("memscope-aten-ac:")]
        self.assertEqual(len(ac), 1)
        self.assertIn("is_write=False", ac[0])
        self.assertIn("is_read=False", ac[0])

    def test_enable_emits_be(self):
        class L:
            def __call__(self, *a, **k):
                return "ok"

        c, marks = tc.TritonCollector(), []
        with _fake_driver(L):
            with mock.patch.object(tc, "_ensure_mstx", return_value=True):
                c.enable()
            with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
                with mock.patch.object(tc, "_default_mark", side_effect=marks.append):
                    self.assertEqual(L()(*_args(FakeTensor(0x10, (2,)))), "ok")
            self.assertTrue(any(m.startswith("memscope-aten-b:") for m in marks))
            self.assertTrue(any(m.startswith("memscope-aten-e:") for m in marks))
            c.disable()

    def test_pre_raise_pairs_end(self):
        class L:
            def __call__(self, *a, **k):
                return "ok"

        c, marks = tc.TritonCollector(), []

        def flaky(*a, **k):
            marks.append(tc.build_begin_mark("triton.unit_k"))
            raise RuntimeError("mid")

        buf = io.StringIO()
        with _fake_driver(L):
            with mock.patch.object(tc, "_ensure_mstx", return_value=True):
                c.enable()
            with mock.patch.object(tc, "emit_triton_access_events", side_effect=flaky):
                with mock.patch.object(tc, "_default_mark", side_effect=marks.append):
                    with contextlib.redirect_stderr(buf):
                        self.assertEqual(L()(*_args(FakeTensor(0x10, (2,)))), "ok")
            c.disable()
        self.assertTrue(any(m.startswith("memscope-aten-e:") for m in marks))
        self.assertIn("ACCESS collection failed", buf.getvalue())
        self.assertGreaterEqual(c.stats.get("emit_failed", 0), 1)

if __name__ == "__main__":
    unittest.main()
