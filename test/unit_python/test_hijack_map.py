#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
"""
hijack_map 单元测试: 版本区间键解析 / owner 别名 / 同标签重入保护 / 条目注册回归

运行: cd test/unit_python && python -m unittest -v test_hijack_map
不依赖 _msmemscope 编译产物与 torch_npu(通过 sys.modules mock 注入)
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


def _load_hijack_map():
    """以独立模块加载 hijack_map.py, mock 掉 C++ 扩展(_describer)与 take_snapshot 依赖"""
    msmemscope_pkg = _get_or_create_module("msmemscope")
    msmemscope_pkg.__path__ = []
    hijacker_pkg = _get_or_create_module("msmemscope.hijacker")
    hijacker_pkg.__path__ = []
    mscore = _get_or_create_module("msmemscope._msmemscope")
    if not hasattr(mscore, "_describer"):
        mscore._describer = MagicMock()
    snapshot_mod = _get_or_create_module("msmemscope.take_snapshot")
    snapshot_mod.take_snapshot = MagicMock()

    # hijack_map 依赖 describe(分级标签内部接口)与 taggers(相对导入 .._msmemscope 命中上面的 mock),
    # 须先加载并注册到 sys.modules, 使 from .describe / from .taggers import 命中真实模块
    _load_module(os.path.join("describe.py"), "msmemscope.describe")
    _load_module(os.path.join("hijacker", "taggers.py"), "msmemscope.hijacker.taggers")
    return _load_module(os.path.join("hijacker", "hijack_map.py"), "msmemscope.hijacker.hijack_map")


_mod = _load_hijack_map()
_DESC = sys.modules["msmemscope.describe"]
# 级别常量别名(与 C++ OwnerLevel 对齐)
C = _DESC.COMPONENT
P = _DESC.PROCESS
D1 = _DESC.DETAIL_1
D2 = _DESC.DETAIL_2


def _describer_mock():
    """describe.py 内部绑定的 _describer mock(与 taggers/hijack_map 共享同一对象)"""
    return sys.modules["msmemscope._msmemscope"]._describer


class TestParseVersion(unittest.TestCase):
    """版本字符串解析"""

    def test_pre_release_suffix_stripped(self):
        self.assertEqual(_mod._parse_version("2.14.0a0"), (2, 14, 0))
        self.assertEqual(_mod._parse_version("2.6.0rc1"), (2, 6, 0))

    def test_partial_version_padded(self):
        self.assertEqual(_mod._parse_version("2.6"), (2, 6, 0))
        self.assertEqual(_mod._parse_version("11.0"), (11, 0, 0))

    def test_plain_version(self):
        self.assertEqual(_mod._parse_version("2.10.1"), (2, 10, 1))


class TestMatchVersion(unittest.TestCase):
    """版本键匹配: 精确 / 最低版本 / 区间 / 通配"""

    def test_exact(self):
        self.assertTrue(_mod._match_version("2.14.0", "2.14.0"))
        self.assertFalse(_mod._match_version("2.13.0", "2.14.0"))
        self.assertTrue(_mod._match_version("11.0", "11.0"))

    def test_min_plus(self):
        self.assertTrue(_mod._match_version("2.10.0", "2.10+"))
        self.assertTrue(_mod._match_version("2.14.0a0", "2.10+"))
        self.assertFalse(_mod._match_version("2.9.9", "2.10+"))

    def test_range_covers_branch_patches(self):
        # 2.6-2.9 覆盖 2.6~2.9 各分支的补丁版本(含 2.9.x)
        self.assertTrue(_mod._match_version("2.6.0", "2.6-2.9"))
        self.assertTrue(_mod._match_version("2.9.9", "2.6-2.9"))
        self.assertFalse(_mod._match_version("2.10.0", "2.6-2.9"))
        self.assertFalse(_mod._match_version("2.5.9", "2.6-2.9"))

    def test_wildcard(self):
        self.assertTrue(_mod._match_version("0.0.1", "*"))


class TestGetHookEntries(unittest.TestCase):
    """劫持条目查询与版本解析顺序"""

    def test_fsdp2_range_key(self):
        entries = _mod.memscope_hijack_map.get_hook_entries("pytorch", "2.11.0", "fsdp2", "decompose")
        self.assertEqual(len(entries), 8)
        self.assertTrue(all(len(entry) == 4 for entry in entries))
        self.assertEqual(entries[0][3], [("fsdp2", C), ("activation", P)])

    def test_fsdp2_pre_release_version(self):
        entries = _mod.memscope_hijack_map.get_hook_entries("pytorch", "2.14.0a0", "fsdp2", "decompose")
        self.assertEqual(len(entries), 8)

    def test_fsdp2_old_layout_range(self):
        # 2.6~2.9 命中旧布局表(10条: 6条稳定目标 + 4条双命名空间foreach条目)
        for version in ("2.6.0", "2.7.1", "2.9.0"):
            entries = _mod.memscope_hijack_map.get_hook_entries("pytorch", version, "fsdp2", "decompose")
            self.assertEqual(len(entries), 10, version)
        # 双命名空间条目齐全
        targets = {(entry[0], entry[2]) for entry in entries}
        self.assertIn(("torch.distributed.fsdp._fully_shard._fsdp_collectives", "foreach_all_gather"), targets)
        self.assertIn(("torch.distributed.fsdp._fully_shard._fsdp_param_group", "foreach_all_gather"), targets)
        self.assertIn(("torch.distributed.fsdp._fully_shard._fsdp_collectives", "foreach_reduce"), targets)
        self.assertIn(("torch.distributed.fsdp._fully_shard._fsdp_param_group", "foreach_reduce"), targets)

    def test_fsdp2_below_range(self):
        # 2.5 及更早不在支持范围(_composable.fsdp 更早布局)
        with redirect_stdout(io.StringIO()):
            entries = _mod.memscope_hijack_map.get_hook_entries("pytorch", "2.5.9", "fsdp2", "decompose")
        self.assertEqual(entries, [])

    def test_fsdp1_entries(self):
        # fsdp1 单表覆盖 2.6~2.14(forward 覆写 + _runtime_utils 窗口 + FlatParamHandle 申请点)
        entries = _mod.memscope_hijack_map.get_hook_entries("pytorch", "2.11.0", "fsdp1", "decompose")
        self.assertEqual(len(entries), 6)
        owners = {tuple(entry[3]) for entry in entries}
        self.assertEqual(owners, {
            (("fsdp1", C), ("activation", P)),
            (("fsdp1", C), ("backward", P)),
            (("fsdp1", C), ("sharded_weight", D1)),
            (("fsdp1", C), ("all_gather", D1)),
            (("fsdp1", C), ("gradient", D1)),
        })

    def test_fsdp1_all_versions(self):
        for version in ("2.6.0", "2.9.0", "2.14.0a0"):
            entries = _mod.memscope_hijack_map.get_hook_entries("pytorch", version, "fsdp1", "decompose")
            self.assertEqual(len(entries), 6, version)

    def test_fsdp1_union_with_fsdp2(self):
        # 并集解析: 同一版本下 fsdp1(2.6+) 与 fsdp2(2.6-2.9/2.10+) 组件互不干扰
        self.assertEqual(len(_mod.memscope_hijack_map.get_hook_entries("pytorch", "2.7.0", "fsdp1", "decompose")), 6)
        self.assertEqual(len(_mod.memscope_hijack_map.get_hook_entries("pytorch", "2.7.0", "fsdp2", "decompose")), 10)
        self.assertEqual(len(_mod.memscope_hijack_map.get_hook_entries("pytorch", "2.11.0", "fsdp1", "decompose")), 6)
        self.assertEqual(len(_mod.memscope_hijack_map.get_hook_entries("pytorch", "2.11.0", "fsdp2", "decompose")), 8)

    def test_version_union_exact_precedence(self):
        # 精确键存在时只命中精确键(不并集)
        version_map = {"2.14.0": {"a": {}}, "2.6+": {"b": {}}}
        self.assertEqual(_mod.memscope_hijack_map._resolve_version_keys(version_map, "2.14.0"), ["2.14.0"])
        self.assertEqual(_mod.memscope_hijack_map._resolve_version_keys(version_map, "2.11.0"), ["2.6+"])

    def test_cross_key_dedup(self):
        # 同一组件表出现在多个匹配键时按目标三元组去重
        fake_map = {
            "pytorch": {
                "2.6-2.9": {"demo": {"decompose": [["m", "C", "f", "tag"]]}},
                "2.6+": {"demo": {"decompose": [["m", "C", "f", "tag"]]}},
            }
        }
        with unittest.mock.patch.object(_mod.memscope_hijack_map, "hijack_mapping", fake_map):
            entries = _mod.memscope_hijack_map.get_hook_entries("pytorch", "2.7.0", "demo", "decompose")
        self.assertEqual(entries, [["m", "C", "f", "tag"]])

    def test_fsdp1_below_range(self):
        with redirect_stdout(io.StringIO()):
            entries = _mod.memscope_hijack_map.get_hook_entries("pytorch", "2.5.9", "fsdp1", "decompose")
        self.assertEqual(entries, [])

    def test_unknown_framework(self):
        with redirect_stdout(io.StringIO()):
            entries = _mod.memscope_hijack_map.get_hook_entries("unknown_fw", "1.0", "c", "decompose")
        self.assertEqual(entries, [])

    def test_vllm_decompose_regression(self):
        entries = _mod.memscope_hijack_map.get_hook_entries("vllm_ascend", "11.0", "worker", "decompose")
        self.assertEqual(len(entries), 6)
        self.assertTrue(all(len(entry) == 4 for entry in entries))
        # 前4条为范围标记(显式分级标签): 瞬时分配窗口 profile/serve/warmup/capture
        self.assertEqual([e[3] for e in entries[:4]],
                         [[("vllm", C), ("profile", P)], [("vllm", C), ("serve", P)],
                          [("vllm", C), ("warmup", P)], [("graph_pool", D1)]])
        # 后2条为回调标记(callable): 权重/KV Cache 地址直标
        self.assertTrue(all(callable(e[3]) for e in entries[4:]))
        targets = {(e[1], e[2]) for e in entries[4:]}
        self.assertIn(("NPUModelRunner", "load_model"), targets)
        self.assertIn(("NPUModelRunner", "initialize_kv_cache_tensors"), targets)

    def test_vllm_no_double_tag(self):
        """双标规避自检(方案a): 回调标记目标不得同时出现在范围标记条目中"""
        entries = _mod.memscope_hijack_map.get_hook_entries("vllm_ascend", "11.0", "worker", "decompose")
        range_targets = {(e[1], e[2]) for e in entries if isinstance(e[3], list)}
        callback_targets = {(e[1], e[2]) for e in entries if callable(e[3])}
        self.assertTrue(callback_targets)
        self.assertTrue(range_targets.isdisjoint(callback_targets))

    def test_vllm_snapshot_regression(self):
        entries = _mod.memscope_hijack_map.get_hook_entries("vllm_ascend", "11.0", "worker", "snapshot")
        self.assertEqual(len(entries), 6)

    def test_verl_snapshot_regression(self):
        entries = _mod.memscope_hijack_map.get_hook_entries("verl", "0.7.0", "TaskRunner", "snapshot")
        self.assertEqual(len(entries), 5)


class TestGetHookletList(unittest.TestCase):
    """hooklet 生成: owner 别名与默认 identifier"""

    def setUp(self):
        _describer_mock().reset_mock()

    def test_fsdp2_hooklet_owner_alias(self):
        hooklets = _mod.memscope_hijack_map.get_hooklet_list("pytorch", "2.12.0", "fsdp2", "decompose")
        self.assertEqual(len(hooklets), 8)
        owners = {tuple(hooklet.owner) for hooklet in hooklets}
        self.assertIn((("fsdp2", C), ("activation", P)), owners)
        self.assertIn((("fsdp2", C), ("backward", P)), owners)
        self.assertIn((("fsdp2", C), ("sharded_weight", D1)), owners)
        self.assertIn((("fsdp2", C), ("all_gather_output", D1)), owners)
        self.assertIn((("fsdp2", C), ("gradient", D1)), owners)
        self.assertEqual(len(owners), 5)

    def test_fsdp2_old_layout_hooklet(self):
        hooklets = _mod.memscope_hijack_map.get_hooklet_list("pytorch", "2.8.0", "fsdp2", "decompose")
        self.assertEqual(len(hooklets), 10)
        owners = {tuple(hooklet.owner) for hooklet in hooklets}
        self.assertEqual(owners, {
            (("fsdp2", C), ("activation", P)), (("fsdp2", C), ("backward", P)),
            (("fsdp2", C), ("sharded_weight", D1)), (("fsdp2", C), ("all_gather_output", D1)),
            (("fsdp2", C), ("gradient", D1)),
        })
        # 双命名空间条目均使用显式分级标签
        self.assertTrue(all(hooklet.owner == [("fsdp2", C), ("all_gather_output", D1)]
                            for hooklet in hooklets if hooklet.method_name == "foreach_all_gather"))

    def test_fsdp1_hooklet(self):
        hooklets = _mod.memscope_hijack_map.get_hooklet_list("pytorch", "2.12.0", "fsdp1", "decompose")
        self.assertEqual(len(hooklets), 6)
        owners = {tuple(hooklet.owner) for hooklet in hooklets}
        self.assertEqual(owners, {
            (("fsdp1", C), ("activation", P)), (("fsdp1", C), ("backward", P)),
            (("fsdp1", C), ("sharded_weight", D1)), (("fsdp1", C), ("all_gather", D1)),
            (("fsdp1", C), ("gradient", D1)),
        })
        # 窗口成对条目(backward)使用同一分级标签
        backward = [h for h in hooklets if h.owner == [("fsdp1", C), ("backward", P)]]
        self.assertEqual(len(backward), 2)
        self.assertEqual({h.method_name for h in backward}, {"_pre_backward_hook", "_post_backward_hook"})

    def test_vllm_hooklet_semantic_owner(self):
        hooklets = _mod.memscope_hijack_map.get_hooklet_list("vllm_ascend", "11.0", "worker", "decompose")
        self.assertEqual(len(hooklets), 6)
        # 范围标记 hooklet: owner 为显式分级标签, identifier 不变(事件溯源仍可定位到函数)
        range_hooklets = [h for h in hooklets if h.callback is None]
        self.assertEqual([h.owner for h in range_hooklets],
                         [[("vllm", C), ("profile", P)], [("vllm", C), ("serve", P)],
                          [("vllm", C), ("warmup", P)], [("graph_pool", D1)]])
        self.assertEqual(range_hooklets[0].identifier,
                         "vllm_ascend.worker.model_runner_v1@NPUModelRunner@profile_run")

    def test_vllm_callback_hooklet_resolution(self):
        hooklets = _mod.memscope_hijack_map.get_hooklet_list("vllm_ascend", "11.0", "worker", "decompose")
        callback_hooklets = [h for h in hooklets if h.callback is not None]
        self.assertEqual(len(callback_hooklets), 2)
        by_method = {h.method_name: h for h in callback_hooklets}
        self.assertEqual(by_method["load_model"].callback.__name__, "tag_model_weights")
        self.assertEqual(by_method["initialize_kv_cache_tensors"].callback.__name__, "tag_kv_cache")

    def test_callback_hooklet_behavior(self):
        """回调标记: 前置不describe, 后置调用回调且不影响返回值, 异常被吞"""
        calls = []
        # 契约: callback(ret, *args, **kwargs), args[0] 为被劫持实例(见 taggers.py 模块注释)
        hooklet = _mod.MemScopeHooklet(
            "decompose", "a.b", "C", "f", callback=lambda ret, *args, **kw: calls.append((ret, args[0])))
        args, kwargs = hooklet.prehook_func(1, 2)
        self.assertEqual((args, kwargs), ((1, 2), {}))
        self.assertEqual(_describer_mock().describe_label.call_count, 0)  # 回调标记不describe
        ret = hooklet.posthook_func("ok", 1, 2)
        self.assertEqual(ret, "ok")
        self.assertEqual(calls, [("ok", 1)])

    def test_callback_hooklet_exception_safe(self):
        def bad_callback(ret, inst):
            raise RuntimeError("boom")

        hooklet = _mod.MemScopeHooklet("decompose", "a.b", "C", "f", callback=bad_callback)
        with redirect_stdout(io.StringIO()):
            ret = hooklet.posthook_func("ok", "inst")
        self.assertEqual(ret, "ok")

    def test_invalid_mark_skipped(self):
        with redirect_stdout(io.StringIO()) as buf:
            # 第4元素既非 list 也非 callable: 条目被跳过
            entries = [["a.b", "C", "f", 42]]
            with unittest.mock.patch.object(
                _mod.memscope_hijack_map, "get_hook_entries", return_value=entries):
                hooklets = _mod.memscope_hijack_map.get_hooklet_list("vllm_ascend", "11.0", "worker", "decompose")
        self.assertEqual(hooklets, [])
        self.assertIn("int", buf.getvalue())


class TestOwnerValidation(unittest.TestCase):
    """分级用途标签约束校验: [(label, level), ...] 列表, 每段无前导@/非空/≤128字符, level 0~7"""

    def test_valid_labels(self):
        self.assertTrue(_mod._validate_owner([("vllm", C), ("weights", D1)]))
        self.assertTrue(_mod._validate_owner([("vllm", C), ("kv_cache", D1), ("attn", D2)]))
        self.assertTrue(_mod._validate_owner([("graph_pool", D1)]))
        self.assertTrue(_mod._validate_owner([]))
        self.assertTrue(_mod._validate_owner(None))

    def test_leading_at_rejected(self):
        self.assertFalse(_mod._validate_owner([("@vllm", C)]))

    def test_invalid_level_rejected(self):
        self.assertFalse(_mod._validate_owner([("vllm", 8)]))
        self.assertFalse(_mod._validate_owner([("vllm", -1)]))
        self.assertFalse(_mod._validate_owner([("vllm", "COMPONENT")]))

    def test_empty_label_rejected(self):
        self.assertFalse(_mod._validate_owner([("", C)]))
        self.assertFalse(_mod._validate_owner([("vllm", C), ("", D1)]))

    def test_too_long_rejected(self):
        self.assertFalse(_mod._validate_owner([("x" * 200, C)]))

    def test_non_list_rejected(self):
        self.assertFalse(_mod._validate_owner("vllm@weights"))

    def test_invalid_owner_warns_on_construction(self):
        with redirect_stdout(io.StringIO()) as buf:
            _mod.MemScopeHooklet("decompose", "a.b", "C", "f", owner=[("@bad", C)])
        self.assertIn("Warning", buf.getvalue())


class TestHookletDescribeForward(unittest.TestCase):
    """分级标签转发: hooklet 逐段上报 describe_label/undescribe_label, 同级别嵌套计数由 C++ 侧管理"""

    def setUp(self):
        self._describer_mock = _describer_mock()  # 共享 mock,不依赖 sys.modules 状态
        self._describer_mock.reset_mock()

    def test_prehook_forwards_each_segment(self):
        hooklet = _mod.MemScopeHooklet("decompose", "m", "C", "f",
                                       owner=[("fsdp2", C), ("activation", P)])
        hooklet.prehook_func(1, 2)
        self._describer_mock.describe_label.assert_has_calls(
            [unittest.mock.call("fsdp2", C), unittest.mock.call("activation", P)])
        self._describer_mock.describe_label.call_count = 0
        hooklet.posthook_func(None, 1, 2)
        self._describer_mock.undescribe_label.assert_has_calls(
            [unittest.mock.call("fsdp2", C), unittest.mock.call("activation", P)])

    def test_nested_same_owner_forwards_each(self):
        # 根模块与子模块嵌套前向: 两个 hooklet 均逐段转发, 计数去重由 C++ DescribeTrace 处理
        h1 = _mod.MemScopeHooklet("decompose", "m", "C", "f",
                                  owner=[("fsdp2", C), ("activation", P)])
        h2 = _mod.MemScopeHooklet("decompose", "m2", "C2", "f2",
                                  owner=[("fsdp2", C), ("activation", P)])
        h1.prehook_func(1, 2)
        h2.prehook_func(3, 4)
        self.assertEqual(self._describer_mock.describe_label.call_count, 4)
        h2.posthook_func(None, 3, 4)
        h1.posthook_func(None, 1, 2)
        self.assertEqual(self._describer_mock.undescribe_label.call_count, 4)

    def test_posthook_without_prehook_forwards(self):
        hooklet = _mod.MemScopeHooklet("decompose", "m", "C", "f", owner=[("tag", D1)])
        hooklet.posthook_func(None)
        self._describer_mock.undescribe_label.assert_called_once_with("tag", D1)

    def test_empty_owner_noop(self):
        hooklet = _mod.MemScopeHooklet("decompose", "m", "C", "f")
        hooklet.prehook_func(1, 2)
        hooklet.posthook_func(None, 1, 2)
        self._describer_mock.describe_label.assert_not_called()
        self._describer_mock.undescribe_label.assert_not_called()

    def test_snapshot_hooklet_behavior(self):
        snapshot_mock = _mod.take_snapshot
        snapshot_mock.reset_mock()
        hooklet = _mod.MemScopeHooklet("snapshot", "a.b", "", "train", owner=[("s1", D1)])
        hooklet.prehook_func()
        hooklet.posthook_func(None)
        self.assertEqual(snapshot_mock.call_count, 2)
        self._describer_mock.describe_label.assert_not_called()


if __name__ == "__main__":
    unittest.main()
