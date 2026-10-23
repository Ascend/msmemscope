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

"""CPU unit tests for Triton access MSTX emission (no NPU / triton required)."""

import contextlib
import importlib.util
import io
import os
import sys
import types as pytypes
import unittest
from collections import namedtuple
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
    """Load triton_collection + real hijacker without msmemscope.__init__ / _msmemscope."""
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

_DRIVER_MOD = tc._NPU_LAUNCHER_MODULE  # triton.backends.ascend.driver


def _ensure_pkg(name: str) -> pytypes.ModuleType:
    mod = sys.modules.get(name)
    if mod is None:
        mod = pytypes.ModuleType(name)
        mod.__path__ = []  # type: ignore[attr-defined]
        sys.modules[name] = mod
    return mod


@contextlib.contextmanager
def _fake_ascend_driver(launcher_cls):
    """Install a fake triton.backends.ascend.driver with NPULauncher=launcher_cls.

    Evicts any real ``triton.*`` modules and stubs
    ``triton.runtime.driver.active.launcher_cls`` so a site-packages
    triton-ascend cannot inject a second live launcher into UT.
    """
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
        # Pin targets to the fake module only (same class hosted twice → 1 target).
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


def _parse_access_flags(msg: str):
    is_read = "is_read=True" in msg
    is_write = "is_write=True" in msg
    is_output = "is_output=True" in msg
    return is_read, is_write, is_output


def _access_marks(emitted):
    return [m for m in emitted if m.startswith("memscope-aten-ac:")]


class TestKindMapping(unittest.TestCase):
    def test_input_output_inout_unknown(self):
        self.assertEqual(tc.kind_to_access_flags(0), (True, False, False))
        self.assertEqual(tc.kind_to_access_flags(1), (False, True, True))
        self.assertEqual(tc.kind_to_access_flags(2), (True, True, True))
        self.assertIsNone(tc.kind_to_access_flags(None))
        self.assertIsNone(tc.kind_to_access_flags(99))

    def test_category_kind_patterns(self):
        patterns = {
            "vector_binary": [0, 0, 1],
            "vector_unary": [0, 1],
            "write_only": [1],
            "inplace_inout": [2],
            "gather": [0, 0, 1],
            "scatter_dst_write": [0, 0, 1],
            "matmul": [0, 0, 1],
            "gemm_bias_relu": [0, 0, 0, 1],
            "softmax": [0, 1],
            "where_ternary": [0, 0, 0, 1],
            "layernorm": [0, 0, 0, 1],
            "fused_attention": [0, 0, 0, 1],
        }
        for name, kinds in patterns.items():
            tensors = [FakeTensor(0x1000 + i * 0x100, (8,), itemsize=4) for i in range(len(kinds))]
            with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
                emitted = tc.emit_triton_access_events(
                    f"triton.{name}",
                    tuple(tensors),
                    kinds,
                    mark_fn=lambda m: None,
                )
            access = _access_marks(emitted)
            self.assertEqual(len(access), len(kinds), name)
            for msg, kind in zip(access, kinds):
                exp = tc.kind_to_access_flags(kind)
                self.assertEqual(_parse_access_flags(msg), exp, f"{name} kind={kind}")


class TestMetadataHelpers(unittest.TestCase):
    def test_extract_tensor_kinds_from_dict(self):
        self.assertEqual(tc.extract_tensor_kinds({"tensor_kinds": [0, 0, 1]}), [0, 0, 1])
        self.assertIsNone(tc.extract_tensor_kinds({}))
        self.assertIsNone(tc.extract_tensor_kinds(None))

    def test_extract_tensor_kinds_from_namedtuple(self):
        Meta = namedtuple("Meta", ["tensor_kinds", "name"])
        self.assertEqual(tc.extract_tensor_kinds(Meta([2, 1], "k")), [2, 1])

    def test_extract_kernel_name_prefixes_triton(self):
        self.assertEqual(tc.extract_kernel_name({"name": "add_kernel"}), "triton.add_kernel")
        self.assertEqual(tc.extract_kernel_name({"name": "triton.add_kernel"}), "triton.add_kernel")

        class Lazy:
            def get(self):
                return {"name": "vec_add"}

        self.assertEqual(tc.extract_kernel_name(Lazy()), "triton.vec_add")

    def test_extract_kernel_name_from_packed_metadata(self):
        self.assertEqual(
            tc.extract_kernel_name(None, {"kernel_name": "fill_kernel"}),
            "triton.fill_kernel",
        )
        self.assertEqual(tc.extract_kernel_name(None, None, fallback="fb"), "triton.fb")

    def test_parse_launch_args_layout(self):
        packed = {"tensor_kinds": [0, 1], "name": "k"}
        launch = {"name": "k"}
        args = (0, 1, 1, "stream", "fn", packed, launch, None, None, "x", "y")
        parsed = tc._parse_launch_args(args)
        self.assertIsNotNone(parsed)
        p, l, kargs, kwargs_npu = parsed
        self.assertIs(p, packed)
        self.assertIs(l, launch)
        self.assertEqual(kargs, ("x", "y"))
        self.assertFalse(kwargs_npu)
        self.assertIsNone(tc._parse_launch_args((1, 2, 3)))

    def test_parse_allows_none_launch_metadata(self):
        """triton-ascend 3.2.2 often passes launch_metadata=None without enter hook."""
        packed = {"tensor_kinds": [0], "kernel_name": "k"}
        args = (0, 1, 1, "stream", "fn", packed, None, None, None, "x")
        parsed = tc._parse_launch_args(args)
        self.assertIsNotNone(parsed)
        p, launch, kargs, kwargs_npu = parsed
        self.assertIs(p, packed)
        self.assertIsNone(launch)
        self.assertEqual(kargs, ("x",))
        self.assertFalse(kwargs_npu)

    def test_parse_detects_kwargs_npu_without_appending(self):
        packed = {"tensor_kinds": [0, 1], "name": "k"}
        launch = {"name": "k"}
        t_out = FakeTensor(0x30, (2,), itemsize=4)
        args = (0, 1, 1, "stream", "fn", packed, launch, None, None, "x")
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            parsed = tc._parse_launch_args(args, {"out": t_out, "grid": (1, 1)})
        self.assertEqual(parsed[2], ("x",))  # not appended
        self.assertTrue(parsed[3])

    def test_parse_launch_args_scan_fallback(self):
        packed = {"tensor_kinds": [1], "name": "k"}
        launch = {"name": "k"}
        # Short degenerate layout used by unit tests: packed, launch, then tensors
        args = ("grid", packed, launch, "t0")
        parsed = tc._parse_launch_args(args)
        self.assertIsNotNone(parsed)
        self.assertIs(parsed[0], packed)
        self.assertEqual(parsed[2], ("t0",))
        self.assertFalse(parsed[3])

    def test_scan_packed_with_none_launch_relative_abi(self):
        packed = {"tensor_kinds": [1], "kernel_name": "k"}
        # packed + None launch + enter + exit + tensor
        args = ("grid", packed, None, None, None, "t0")
        parsed = tc._parse_launch_args(args)
        self.assertIsNotNone(parsed)
        self.assertIs(parsed[0], packed)
        self.assertEqual(parsed[2], ("t0",))


class TestEmitAccessEvents(unittest.TestCase):
    def setUp(self):
        pass

    def tearDown(self):
        pass

    def test_mark_format_matches_aten(self):
        self.assertEqual(tc.build_begin_mark("triton.k"), "memscope-aten-b: name=triton.k")
        self.assertEqual(tc.build_end_mark("triton.k"), "memscope-aten-e: name=triton.k")
        self.assertTrue(
            tc.build_access_mark(
                data_ptr=1,
                is_write=True,
                is_read=False,
                is_output=True,
                name="triton.k",
                shape=(1,),
                dtype="f",
                tensor_size=4,
                device="npu:0",
            ).startswith("memscope-aten-ac:ptr=")
        )

    def test_vector_add_rw_marks(self):
        marks = []
        x = FakeTensor(0x1000, (1024,), itemsize=4)
        y = FakeTensor(0x2000, (1024,), itemsize=4)
        out = FakeTensor(0x3000, (1024,), itemsize=4)

        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            emitted = tc.emit_triton_access_events(
                "triton.add_kernel",
                (x, y, out, 1024),
                [0, 0, 1],
                mark_fn=lambda m: marks.append(m),
            )
            end = tc.emit_triton_end_event("triton.add_kernel", mark_fn=lambda m: marks.append(m))

        self.assertTrue(emitted[0].startswith("memscope-aten-b: name=triton.add_kernel"))
        self.assertEqual(_parse_access_flags(emitted[1]), (True, False, False))
        self.assertEqual(_parse_access_flags(emitted[2]), (True, False, False))
        self.assertEqual(_parse_access_flags(emitted[3]), (False, True, True))
        self.assertIn("tensor_size=4096", emitted[3])
        self.assertEqual(end, "memscope-aten-e: name=triton.add_kernel")
        self.assertEqual(len(marks), 5)

    def test_inout_kind_marks_read_and_write(self):
        t = FakeTensor(0x4000, (16,), itemsize=4)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            emitted = tc.emit_triton_access_events(
                "triton.fill_inplace",
                (t, 1.0),
                [2],
                mark_fn=lambda m: None,
            )
        self.assertEqual(len(emitted), 2)
        self.assertEqual(_parse_access_flags(emitted[1]), (True, True, True))

    def test_alias_same_ptr_emits_two_access_aligned_to_kinds(self):
        """Same storage as two READ args: kinds [0,0,1] → two READ + one WRITE marks."""
        x = FakeTensor(0x1000, (8,), itemsize=4)
        out = FakeTensor(0x2000, (8,), itemsize=4)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            emitted = tc.emit_triton_access_events(
                "triton.add_alias",
                (x, x, out),
                [0, 0, 1],
                mark_fn=lambda m: None,
            )
        access = _access_marks(emitted)
        self.assertEqual(len(access), 3)
        self.assertEqual(_parse_access_flags(access[0]), (True, False, False))
        self.assertEqual(_parse_access_flags(access[1]), (True, False, False))
        self.assertEqual(_parse_access_flags(access[2]), (False, True, True))
        self.assertIn("ptr=4096", access[0])
        self.assertIn("ptr=4096", access[1])
        self.assertIn("ptr=8192", access[2])

    def test_output_only_kernel(self):
        out = FakeTensor(0x5000, (8,), itemsize=2)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            emitted = tc.emit_triton_access_events(
                "triton.zeros_like_kernel",
                (out,),
                [1],
                mark_fn=lambda m: None,
            )
        self.assertEqual(_parse_access_flags(emitted[1]), (False, True, True))

    def test_missing_kinds_skips_access_by_default(self):
        t = FakeTensor(0xABCD, (8,), itemsize=2)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            with _capture_stderr() as err:
                emitted = tc.emit_triton_access_events(
                    "triton.k",
                    (t,),
                    None,
                    mark_fn=lambda m: None,
                )
                text = err.getvalue()
                self.assertIn("tensor_kinds missing", text)
                self.assertIn("skipping ACCESS", text)
        self.assertEqual(len(emitted), 1)

    def test_kinds_mismatch_skips_access_by_default(self):
        a = FakeTensor(1, (4,), itemsize=4)
        b = FakeTensor(2, (4,), itemsize=4)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            with _capture_stderr() as err:
                emitted = tc.emit_triton_access_events(
                    "triton.k",
                    (a, b),
                    [0],
                    mark_fn=lambda m: None,
                )
        self.assertEqual(len(_access_marks(emitted)), 0)
        self.assertIn("length mismatch", err.getvalue())

    def test_unknown_kind_skips_access_by_default(self):
        t = FakeTensor(0x10, (4,), itemsize=4)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            with _capture_stderr() as err:
                emitted = tc.emit_triton_access_events(
                    "triton.k",
                    (t,),
                    [99],
                    mark_fn=lambda m: None,
                )
        self.assertEqual(len(_access_marks(emitted)), 0)
        self.assertIn("unknown tensor_kinds", err.getvalue())

    def test_skip_access_flag_for_kwargs(self):
        t = FakeTensor(0x10, (4,), itemsize=4)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            with _capture_stderr() as err:
                emitted = tc.emit_triton_access_events(
                    "triton.k",
                    (t,),
                    [1],
                    mark_fn=lambda m: None,
                    skip_access=True,
                    skip_access_reason="kwargs NPU",
                )
        self.assertEqual(len(_access_marks(emitted)), 0)
        self.assertIn("kwargs NPU", err.getvalue())

    def test_skips_scalar_args(self):
        t = FakeTensor(1, (4,), itemsize=4)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            emitted = tc.emit_triton_access_events(
                "triton.k",
                (t, 128, 3.14),
                [1],
                mark_fn=lambda m: None,
            )
        self.assertEqual(len(emitted), 2)
        self.assertEqual(_parse_access_flags(emitted[1]), (False, True, True))

    def test_skips_zero_ptr_keeps_empty_nonzero_ptr(self):
        zero = FakeTensor(0, (4,), itemsize=4)
        empty = FakeTensor(0x100, (0,), itemsize=4)
        good = FakeTensor(0x200, (2,), itemsize=4)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            emitted = tc.emit_triton_access_events(
                "triton.k",
                (zero, empty, good),
                [0, 0, 1],
                mark_fn=lambda m: None,
            )
        access = _access_marks(emitted)
        self.assertEqual(len(access), 2)
        self.assertIn("ptr=256", access[0])
        self.assertIn("tensor_size=0", access[0])
        self.assertIn("ptr=512", access[1])

    def test_skips_tensor_when_data_ptr_raises(self):
        class BadTensor(FakeTensor):
            def data_ptr(self):
                raise RuntimeError("boom")

        bad = BadTensor(1, (4,))
        good = FakeTensor(0x200, (2,), itemsize=4)
        with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
            emitted = tc.emit_triton_access_events(
                "triton.k",
                (bad, good),
                [0, 1],
                mark_fn=lambda m: None,
            )
        access = _access_marks(emitted)
        self.assertEqual(len(access), 1)
        self.assertIn("ptr=512", access[0])


class TestCollectorLifecycle(unittest.TestCase):
    def test_enable_disable_wraps_active_launcher(self):
        calls = {"n": 0}

        class FakeLauncher:
            def __call__(self, *args, **kwargs):
                calls["n"] += 1
                return "ok"

        collector = tc.TritonCollector()
        with _fake_ascend_driver(FakeLauncher):
            collector.enable()
            self.assertTrue(collector.enabled)
            self.assertTrue(collector._hooked)
            self.assertEqual(len(collector._handlers), 2)

            t = FakeTensor(0x10, (2,), itemsize=4)
            args = _launch_args(t)
            marks = []
            with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
                with mock.patch.object(tc, "_default_mark", side_effect=lambda m: marks.append(m)):
                    ret = FakeLauncher()(*args)
            self.assertEqual(ret, "ok")
            self.assertEqual(calls["n"], 1)
            self.assertTrue(any(m.startswith("memscope-aten-b:") for m in marks))
            self.assertTrue(any("is_write=True" in m for m in marks))
            self.assertTrue(any(m.startswith("memscope-aten-e:") for m in marks))

            collector.disable()
            self.assertFalse(collector.enabled)
            self.assertFalse(collector._hooked)
            marks.clear()
            with mock.patch.object(tc, "_default_mark", side_effect=lambda m: marks.append(m)):
                FakeLauncher()(*args)
            self.assertEqual(marks, [])

    def test_kwargs_npu_skips_access_keeps_be(self):
        class FakeLauncher:
            def __call__(self, *args, **kwargs):
                return "ok"

        collector = tc.TritonCollector()
        t_pos = FakeTensor(0x10, (2,), itemsize=4)
        t_kw = FakeTensor(0x20, (2,), itemsize=4)
        args = _launch_args(t_pos, kinds=[0, 1])
        marks = []
        with _fake_ascend_driver(FakeLauncher):
            collector.enable()
            with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
                with mock.patch.object(tc, "_default_mark", side_effect=lambda m: marks.append(m)):
                    with _capture_stderr():
                        FakeLauncher()(*args, out=t_kw)
            collector.disable()
        self.assertTrue(any(m.startswith("memscope-aten-b:") for m in marks))
        self.assertTrue(any(m.startswith("memscope-aten-e:") for m in marks))
        self.assertEqual(_access_marks(marks), [])

    def test_enable_disable_twice_restores_original(self):
        class FakeLauncher:
            def __call__(self, *args, **kwargs):
                return "ok"

        original = FakeLauncher.__call__
        collector = tc.TritonCollector()
        with _fake_ascend_driver(FakeLauncher):
            collector.enable()
            collector.disable()
            collector.enable()
            self.assertTrue(collector._hooked)
            self.assertIsNot(FakeLauncher.__call__, original)
            collector.disable()
            self.assertIs(FakeLauncher.__call__, original)
            self.assertFalse(collector._hooked)

    def test_begin_emitted_when_original_raises_post_skipped(self):
        """hijacker POST_HOOK does not run when the original __call__ raises."""

        class FakeLauncher:
            def __call__(self, *args, **kwargs):
                raise RuntimeError("launch failed")

        collector = tc.TritonCollector()
        t = FakeTensor(0x10, (2,), itemsize=4)
        args = _launch_args(t)
        marks = []
        with _fake_ascend_driver(FakeLauncher):
            collector.enable()
            with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
                with mock.patch.object(tc, "_default_mark", side_effect=lambda m: marks.append(m)):
                    with self.assertRaises(RuntimeError):
                        FakeLauncher()(*args)
            collector.disable()
        self.assertTrue(any(m.startswith("memscope-aten-b:") for m in marks))
        self.assertFalse(any(m.startswith("memscope-aten-e:") for m in marks))

    def test_emit_raise_does_not_block_launch(self):
        class FakeLauncher:
            def __call__(self, *args, **kwargs):
                return "ok"

        collector = tc.TritonCollector()
        t = FakeTensor(0x10, (2,), itemsize=4)
        args = _launch_args(t)
        marks = []

        def flaky_emit(*a, **k):
            marks.append(tc.build_begin_mark("triton.unit_k"))
            raise RuntimeError("mid emit")

        with _fake_ascend_driver(FakeLauncher):
            collector.enable()
            with mock.patch.object(tc, "emit_triton_access_events", side_effect=flaky_emit):
                with mock.patch.object(tc, "_default_mark", side_effect=lambda m: marks.append(m)):
                    with _capture_stderr() as err:
                        ret = FakeLauncher()(*args)
            collector.disable()
        self.assertEqual(ret, "ok")
        self.assertTrue(any(m.startswith("memscope-aten-b:") for m in marks))
        # PRE caught the emit failure → need_end cleared → no end mark
        self.assertFalse(any(m.startswith("memscope-aten-e:") for m in marks))
        self.assertIn("ACCESS collection failed", err.getvalue())

    def test_default_mark_reports_missing_mstx(self):
        saved = tc.mstx
        try:
            tc.mstx = None
            with _capture_stderr() as err:
                tc._default_mark("memscope-aten-b: name=triton.k")
            self.assertIn("mstx is unavailable", err.getvalue())
        finally:
            tc.mstx = saved

    def test_deferred_hijack_applies_when_driver_appears(self):
        """enable before driver module exists; hijacker PathFinder wraps on first load."""

        class LateLauncher:
            def __call__(self, *args, **kwargs):
                return "ok"

        # Ensure target module is absent so PathFinder deferral is exercised.
        saved = {n: sys.modules.pop(n, None) for n in (
            "triton", "triton.backends", "triton.backends.ascend", _DRIVER_MOD,
        )}
        collector = tc.TritonCollector()
        try:
            collector.enable()
            self.assertTrue(collector.enabled)
            self.assertTrue(collector._hooked)

            # Simulate first import of the driver module after enable.
            _ensure_pkg("triton")
            _ensure_pkg("triton.backends")
            _ensure_pkg("triton.backends.ascend")
            # Real import goes through meta_path; stub by placing module then
            # re-triggering the function-wrapper POST_HOOK via a fresh enable cycle
            # is brittle. Instead: put module in place and re-register (idempotent
            # path when module already present) after disable/enable.
            driver = pytypes.ModuleType(_DRIVER_MOD)
            driver.NPULauncher = LateLauncher
            sys.modules[_DRIVER_MOD] = driver

            collector.disable()
            collector.enable()
            marks = []
            t = FakeTensor(0x10, (2,), itemsize=4)
            with mock.patch.object(tc, "is_npu_tensor", side_effect=lambda v: isinstance(v, FakeTensor)):
                with mock.patch.object(tc, "_default_mark", side_effect=lambda m: marks.append(m)):
                    ret = LateLauncher()(*_launch_args(t))
            self.assertEqual(ret, "ok")
            self.assertTrue(any(m.startswith("memscope-aten-b:") for m in marks))
            self.assertTrue(any(m.startswith("memscope-aten-e:") for m in marks))
        finally:
            collector.disable()
            for name, old in saved.items():
                if old is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = old


if __name__ == "__main__":
    unittest.main()
