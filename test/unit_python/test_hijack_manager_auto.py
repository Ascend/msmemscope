#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
"""
hijack_manager 自动使能单元测试: decompose 钩子全量注册 / 去重 / 幂等 / 手动互斥 / 分账释放

覆盖 docs/rfc/2026-07-23-auto-decompose-hijack.md 4.1 节 UT-1~UT-11:
  - UT-1/UT-2/UT-2b: 全量注册与多版本键去重(目标三元组)
  - UT-2c: init_framework_hooks 累积注册(无 clear 覆盖)
  - UT-3: 单条目失败不影响其他
  - UT-4: disable 仅清理自动 handler(手动 snapshot 保留)
  - UT-5: 无 decompose 条目时静默返回
  - UT-6: 目标模块未导入时惰性激活(真实 hijack_utility)
  - UT-7: 手动 decompose 注册抑制自动使能(互斥)
  - UT-8: 重复 enable 幂等
  - UT-9: 手动 snapshot 注册不影响自动使能
  - UT-10: disable 后重新 enable 可全量恢复
  - UT-11: enable_decompose_hooks 不依赖 torch(移除 sys.modules 后仍可调用)

运行: cd test/unit_python && python -m unittest -v test_hijack_manager_auto
不依赖 _msmemscope 编译产物与 torch 安装(通过 sys.modules mock 注入)
"""

import importlib.util
import io
import os
import sys
import types
import unittest
from contextlib import redirect_stdout
from unittest.mock import MagicMock

_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "python", "msmemscope")


def _get_or_create_module(name):
    """获取或创建 mock 模块(与其他测试文件共存,避免互相覆盖 sys.modules)"""
    mod = sys.modules.get(name)
    if mod is None:
        mod = types.ModuleType(name)
        sys.modules[name] = mod
    return mod


def _load_module(rel_path, mod_name):
    """以独立模块加载指定源码文件"""
    spec = importlib.util.spec_from_file_location(mod_name, os.path.join(_SRC, rel_path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[mod_name] = module
    spec.loader.exec_module(module)
    return module


def _load_harness():
    """
    加载 hijack_manager 及其依赖:
      - hijack_utility: 真实模块(纯 Python, 无副作用; hijacker 仅在调用时才注入 PathFinder)
      - hijack_map: mock 掉 _describer / take_snapshot 依赖(与 test_hijack_map.py 同法)
      - hijack_manager: 被测模块(测试中 patch 其命名空间中的 hijacker/release, 不污染真实工具)
    """
    msmemscope_pkg = _get_or_create_module("msmemscope")
    msmemscope_pkg.__path__ = []
    hijacker_pkg = _get_or_create_module("msmemscope.hijacker")
    hijacker_pkg.__path__ = []
    mscore = _get_or_create_module("msmemscope._msmemscope")
    if not hasattr(mscore, "_describer"):
        mscore._describer = MagicMock()
    snapshot_mod = _get_or_create_module("msmemscope.take_snapshot")
    snapshot_mod.take_snapshot = MagicMock()

    _load_module(os.path.join("describe.py"), "msmemscope.describe")
    _load_module(os.path.join("hijacker", "taggers.py"), "msmemscope.hijacker.taggers")
    _load_module(os.path.join("hijacker", "hijack_utility.py"), "msmemscope.hijacker.hijack_utility")
    _load_module(os.path.join("hijacker", "hijack_map.py"), "msmemscope.hijacker.hijack_map")
    return _load_module(os.path.join("hijacker", "hijack_manager.py"), "msmemscope.hijacker.hijack_manager")


_mod = _load_harness()
_UTIL = sys.modules["msmemscope.hijacker.hijack_utility"]
# 级别常量别名(与 C++ OwnerLevel 对齐)
_DESC = sys.modules["msmemscope.describe"]


def _real_mapping():
    """当前 hijack_map 的真实映射(只读引用, 测试用例按需取其子集)"""
    return _mod.memscope_hijack_map.hijack_mapping


def _fake_hijacker(**kwargs):
    """模拟 hijacker: 按关键字返回可哈希的 handler 标识(默认总是成功; 失败注入见 UT-3)"""
    return "handler-{}-{}-{}-{}".format(
        kwargs["module"], kwargs.get("cls", ""), kwargs["function"], kwargs["action"])


class TestAutoEnableBase(unittest.TestCase):
    """公共 setUp: 重置单例状态(每次 MemScopeHijackManager() 调用都会重新执行 __init__), patch hijacker/release"""

    def setUp(self):
        # __new__ 返回单例, __init__ 对既有实例重新执行 → 状态复位
        self.manager = _mod.MemScopeHijackManager()
        self.patch_hijacker = unittest.mock.patch.object(_mod, "hijacker", side_effect=_fake_hijacker)
        self.mock_hijacker = self.patch_hijacker.start()
        self.patch_release = unittest.mock.patch.object(_mod, "release", side_effect=lambda h: None)
        self.mock_release = self.patch_release.start()
        self.addCleanup(self.patch_release.stop)
        self.addCleanup(self.patch_hijacker.stop)

    def _count_calls_by_target(self):
        """统计 hijacker 调用挂载的目标三元组集合(校验去重)"""
        targets = {(c.kwargs["module"], c.kwargs.get("cls", ""), c.kwargs["function"])
                   for c in self.mock_hijacker.call_args_list}
        return targets


class TestEnableFullRegistration(TestAutoEnableBase):
    """UT-1/UT-2/UT-2b: 全量注册与多版本键去重"""

    def test_ut1_vllm_only_full_registration(self):
        """UT-1: 仅 vllm_ascend 映射 → 6 个目标, 每个 pre+post 共 12 个 handler, 累积且无重复"""
        vllm_only = {"vllm_ascend": _real_mapping()["vllm_ascend"]}
        with unittest.mock.patch.object(_mod.memscope_hijack_map, "hijack_mapping", vllm_only), \
                redirect_stdout(io.StringIO()):
            _mod.enable_decompose_hooks()
        self.assertEqual(len(self.manager._enabled_targets), 6)
        self.assertEqual(len(self.manager.auto_registered_handlers), 12)
        self.assertEqual(self.mock_hijacker.call_count, 12)
        # 每个目标挂载 PRE_HOOK + POST_HOOK 各一次
        actions = {(c.kwargs["module"], c.kwargs["cls"], c.kwargs["function"], c.kwargs["action"])
                   for c in self.mock_hijacker.call_args_list}
        self.assertEqual(len(actions), 12)
        targets = self._count_calls_by_target()
        self.assertEqual(len(targets), 6)

    def test_ut2_multi_framework_all_registered(self):
        """UT-2: 全量映射 → vllm 6 + fsdp1 6 + fsdp2 双表去重后 12 = 24 目标, 48 个 handler"""
        with redirect_stdout(io.StringIO()):
            _mod.enable_decompose_hooks()
        self.assertEqual(len(self.manager._enabled_targets), 24)
        self.assertEqual(len(self.manager.auto_registered_handlers), 48)
        self.assertEqual(self.mock_hijacker.call_count, 48)
        targets = self._count_calls_by_target()
        self.assertEqual(len(targets), 24)
        # 分框架抽查
        vllm_targets = {t for t in targets if t[0].startswith("vllm_ascend")}
        torch_targets = {t for t in targets if t[0].startswith("torch.")}
        self.assertEqual(len(vllm_targets), 6)
        self.assertEqual(len(torch_targets), 18)  # fsdp1 6 + fsdp2 并集 12

    def test_ut2b_multi_version_key_dedup(self):
        """UT-2b: fsdp2 双表(2.6-2.9 10 条 + 2.10+ 8 条, 共享 6 条稳定目标) → 并集去重后 12 条目标"""
        pytorch_fsdp2_only = {
            "pytorch": {
                "2.6-2.9": _real_mapping()["pytorch"]["2.6-2.9"],
                "2.10+": _real_mapping()["pytorch"]["2.10+"],
            }
        }
        with unittest.mock.patch.object(_mod.memscope_hijack_map, "hijack_mapping", pytorch_fsdp2_only), \
                redirect_stdout(io.StringIO()):
            _mod.enable_decompose_hooks()
        self.assertEqual(len(self.manager._enabled_targets), 12)
        self.assertEqual(len(self.manager.auto_registered_handlers), 24)
        # 双表共享的 6 条稳定目标只注册一次
        stable_targets = {
            ("torch.distributed.fsdp._fully_shard._fsdp_state", "FSDPState", "_pre_forward"),
            ("torch.distributed.fsdp._fully_shard._fsdp_state", "FSDPState", "_post_forward"),
            ("torch.distributed.fsdp._fully_shard._fsdp_state", "FSDPState", "_pre_backward"),
            ("torch.distributed.fsdp._fully_shard._fsdp_param_group", "FSDPParamGroup", "post_backward"),
            ("torch.distributed.fsdp._fully_shard._fsdp_param", "FSDPParam", "_init_sharded_param"),
            ("torch.distributed.fsdp._fully_shard._fsdp_param", "FSDPParam", "init_all_gather_outputs"),
        }
        registered = self._count_calls_by_target()
        self.assertTrue(stable_targets.issubset(registered))
        for t in stable_targets:
            calls = [c for c in self.mock_hijacker.call_args_list
                     if (c.kwargs["module"], c.kwargs["cls"], c.kwargs["function"]) == t]
            self.assertEqual(len(calls), 2, t)  # pre + post, 无重复

    def test_ut2c_init_framework_hooks_cumulative(self):
        """UT-2c: 多次调用 init_framework_hooks 累积注册(无 clear 覆盖), cleanup 可全部释放"""
        with redirect_stdout(io.StringIO()):
            self.manager.init_framework_hooks("vllm_ascend", "11.0", "worker", "decompose")
            self.manager.init_framework_hooks("pytorch", "2.11.0", "fsdp1", "decompose")
        self.assertEqual(len(self.manager.registered_handlers), 12 + 12)  # 两组全部保留
        with redirect_stdout(io.StringIO()):
            self.manager.cleanup_framework_hooks()
        self.assertEqual(self.mock_release.call_count, 24)
        self.assertEqual(self.manager.registered_handlers, [])


class TestEnableRobustness(TestAutoEnableBase):
    """UT-3/UT-5: 单条目失败与空映射"""

    def test_ut3_single_entry_failure_skipped(self):
        """UT-3: 某条目注册失败仅跳过该条目(load_model), 其余全部注册, 打印 warning"""
        def failing_hijacker(**kwargs):
            if kwargs.get("function") == "load_model":
                raise RuntimeError("mock hijacker failure")
            return _fake_hijacker(**kwargs)

        unittest.mock.patch.object(_mod, "hijacker", side_effect=failing_hijacker).start()
        self.addCleanup(unittest.mock.patch.stopall)
        with redirect_stdout(io.StringIO()) as buf:
            _mod.enable_decompose_hooks()
        self.assertEqual(len(self.manager._enabled_targets), 23)  # 24 目标中 load_model 被跳过
        self.assertEqual(len(self.manager.auto_registered_handlers), 46)
        self.assertIn("load_model", buf.getvalue())
        self.assertIn("Warning", buf.getvalue())
        # 失败目标未入 _enabled_targets, 后续 enable 可重试
        self.assertNotIn(
            ("vllm_ascend.worker.model_runner_v1", "NPUModelRunner", "load_model"),
            self.manager._enabled_targets)

    def test_ut5_no_decompose_entries(self):
        """UT-5: 映射中仅含 snapshot 条目(如仅 mindspeed_llm 场景) → 无任何 hijacker 调用"""
        snapshot_only = {"mindspeed_llm": _real_mapping()["mindspeed_llm"]}
        with unittest.mock.patch.object(_mod.memscope_hijack_map, "hijack_mapping", snapshot_only), \
                redirect_stdout(io.StringIO()):
            _mod.enable_decompose_hooks()
        self.assertEqual(self.mock_hijacker.call_count, 0)
        self.assertEqual(self.manager.auto_registered_handlers, [])
        self.assertEqual(self.manager._enabled_targets, set())


class TestEnableIdempotency(TestAutoEnableBase):
    """UT-8/UT-10: 跨调用幂等与 disable 后恢复"""

    def test_ut8_repeated_enable_idempotent(self):
        """UT-8: 连续两次 enable(config()/aclInit/start() 多次触发) → 第二次不重复挂载"""
        with redirect_stdout(io.StringIO()):
            _mod.enable_decompose_hooks()
            first_count = self.mock_hijacker.call_count
            self.assertEqual(first_count, 48)
            _mod.enable_decompose_hooks()
        self.assertEqual(self.mock_hijacker.call_count, first_count)  # 第二次无新增
        self.assertEqual(len(self.manager.auto_registered_handlers), 48)
        self.assertEqual(len(self.manager._enabled_targets), 24)

    def test_ut10_disable_then_reenable_recovers(self):
        """UT-10: enable → disable → 再次 enable → 全量重新注册, 数量与首次一致"""
        with redirect_stdout(io.StringIO()):
            _mod.enable_decompose_hooks()
            self.assertEqual(self.mock_hijacker.call_count, 48)
            _mod.disable_decompose_hooks()
        self.assertEqual(self.mock_release.call_count, 48)
        self.assertEqual(self.manager.auto_registered_handlers, [])
        self.assertEqual(self.manager._enabled_targets, set())
        with redirect_stdout(io.StringIO()):
            _mod.enable_decompose_hooks()
        self.assertEqual(self.mock_hijacker.call_count, 96)
        self.assertEqual(len(self.manager.auto_registered_handlers), 48)
        self.assertEqual(len(self.manager._enabled_targets), 24)


class TestManualInteraction(TestAutoEnableBase):
    """UT-4/UT-7/UT-9: 手动/自动分账与互斥语义"""

    def test_ut4_disable_only_releases_auto_handlers(self):
        """UT-4: disable 仅释放自动 handler; 手动 snapshot 钩子保留且可正常 release"""
        with redirect_stdout(io.StringIO()):
            _mod.enable_decompose_hooks()
            self.manager.init_framework_hooks("vllm_ascend", "11.0", "worker", "snapshot")
        self.assertEqual(len(self.manager.auto_registered_handlers), 48)
        self.assertEqual(len(self.manager.registered_handlers), 12)  # snapshot 6 目标 × 2
        with redirect_stdout(io.StringIO()):
            _mod.disable_decompose_hooks()
        self.assertEqual(self.mock_release.call_count, 48)  # 仅自动部分被 release
        self.assertEqual(self.manager.auto_registered_handlers, [])
        self.assertEqual(len(self.manager.registered_handlers), 12)  # 手动 snapshot 保留
        with redirect_stdout(io.StringIO()):
            self.manager.cleanup_framework_hooks()
        self.assertEqual(self.mock_release.call_count, 48 + 12)
        self.assertEqual(self.manager.registered_handlers, [])

    def test_ut7_manual_decompose_suppresses_auto(self):
        """UT-7: 手动 init_framework_hooks(decompose) 后自动使能跳过(互斥), 无重复注册"""
        with redirect_stdout(io.StringIO()):
            self.manager.init_framework_hooks("vllm_ascend", "11.0", "worker", "decompose")
        self.assertTrue(self.manager._manual_decompose_init_called)
        calls_before = self.mock_hijacker.call_count
        with redirect_stdout(io.StringIO()) as buf:
            _mod.enable_decompose_hooks()
        self.assertIn("skipping auto-enable", buf.getvalue())
        self.assertEqual(self.mock_hijacker.call_count, calls_before)  # 自动使能未注册任何 handler
        self.assertEqual(self.manager.auto_registered_handlers, [])
        self.assertEqual(len(self.manager.registered_handlers), 12)  # 仅手动钩子生效

    def test_ut9_manual_snapshot_does_not_suppress_auto(self):
        """UT-9: 手动注册 snapshot 组合不置位标记, 自动使能照常注册全部 decompose 目标"""
        with redirect_stdout(io.StringIO()):
            self.manager.init_framework_hooks("vllm_ascend", "11.0", "worker", "snapshot")
        self.assertFalse(self.manager._manual_decompose_init_called)
        with redirect_stdout(io.StringIO()):
            _mod.enable_decompose_hooks()
        self.assertEqual(len(self.manager._enabled_targets), 24)
        self.assertEqual(len(self.manager.auto_registered_handlers), 48)
        self.assertEqual(len(self.manager.registered_handlers), 12)


class TestLazyActivation(unittest.TestCase):
    """UT-6: 目标模块未导入时钩子注册成功但惰性不激活(真实 hijack_utility)"""

    def _real_hijacker(self, function, action):
        return _UTIL.hijacker(
            stub=lambda *args, **kwargs: (args, kwargs),
            module="vllm_ascend.worker.model_runner_v1",
            cls="NPUModelRunner",
            function=function,
            action=action,
        )

    def test_ut6_lazy_activation_no_import(self):
        """注册 vllm_ascend 钩子但不 import 目标模块 → wrapper 已建但 ori_obj 为 None, release 后清理"""
        target = "vllm_ascend.worker.model_runner_v1-NPUModelRunner-load_model"
        pre_handler = self._real_hijacker("load_model", _UTIL.PRE_HOOK)
        post_handler = self._real_hijacker("load_model", _UTIL.POST_HOOK)
        # 清理仅由 finally 承担(覆盖断言成功/失败全部路径), 不再注册 addCleanup, 避免重复 release
        try:
            wrapper = _UTIL.HiJackerManager._hijacker_wrappers.get(target)
            self.assertIsNotNone(wrapper)
            self.assertIsNone(wrapper.ori_obj)  # 目标模块未导入, 惰性未激活
            self.assertIn("vllm_ascend.worker.model_runner_v1",
                          _UTIL.HiJackerPathFinder._modules_of_interest)
            # 未安装框架不会产生任何副作用: 模块未被拦截加载
            self.assertNotIn("vllm_ascend.worker.model_runner_v1", sys.modules)
        finally:
            _UTIL.release(pre_handler)
            _UTIL.release(post_handler)
        # release 后 wrapper 拆除, 模块从拦截集合移除
        self.assertNotIn(target, _UTIL.HiJackerManager._hijacker_wrappers)
        self.assertNotIn("vllm_ascend.worker.model_runner_v1", _UTIL.HiJackerPathFinder._modules_of_interest)


class TestTorchIndependence(unittest.TestCase):
    """UT-11: Python 侧 enable/disable 不依赖 torch(C++ MemScopePythonCall 的 torch 门控见 cpython.cpp)"""

    def setUp(self):
        self.manager = _mod.MemScopeHijackManager()
        self.patch_hijacker = unittest.mock.patch.object(_mod, "hijacker", side_effect=_fake_hijacker)
        self.mock_hijacker = self.patch_hijacker.start()
        self.addCleanup(self.patch_hijacker.stop)

    def test_ut11_enable_without_torch_imported(self):
        """torch 不在 sys.modules 时 enable_decompose_hooks 仍可正常调用(静默注册)"""
        saved = sys.modules.pop("torch", None)
        try:
            with redirect_stdout(io.StringIO()):
                _mod.enable_decompose_hooks()
        finally:
            if saved is not None:
                sys.modules["torch"] = saved
        self.assertEqual(len(self.manager._enabled_targets), 24)
        self.assertEqual(self.mock_hijacker.call_count, 48)


if __name__ == "__main__":
    unittest.main()
