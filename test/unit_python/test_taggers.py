#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
"""
taggers 单元测试: 框架维度组织的地址直标器(VllmAscendTaggers)

运行: cd test/unit_python && python -m unittest -v test_taggers
不依赖 _msmemscope 编译产物与 torch_npu(通过 sys.modules mock 注入 _describer)
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


class FakeTensor:
    """模拟 torch.Tensor 的最小对象: 仅提供 data_ptr"""

    def __init__(self, addr):
        self._addr = addr

    def data_ptr(self):
        return self._addr


class FakeModule:
    """模拟 nn.Module: 参数/buffer/lora 结构"""

    def __init__(self, params=None, buffers=None, modules=None):
        self._params = params or []
        self._buffers = buffers or []
        self._modules = modules or []
        self.lora_a = None
        self.lora_b = None

    def named_parameters(self):
        for name, param in self._params:
            yield name, param

    def named_buffers(self):
        for name, buf in self._buffers:
            yield name, buf

    def named_modules(self):
        yield "", self
        for name, module in self._modules:
            yield name, module


def _get_or_create_module(name):
    """获取或创建 mock 模块(与其他测试文件共存,避免互相覆盖 sys.modules)"""
    mod = sys.modules.get(name)
    if mod is None:
        mod = types.ModuleType(name)
        sys.modules[name] = mod
    return mod


def _load_taggers():
    """以独立模块加载 taggers.py, mock 掉 C++ 扩展(_describer)"""
    msmemscope_pkg = _get_or_create_module("msmemscope")
    msmemscope_pkg.__path__ = []
    hijacker_pkg = _get_or_create_module("msmemscope.hijacker")
    hijacker_pkg.__path__ = []
    mscore = _get_or_create_module("msmemscope._msmemscope")
    if not hasattr(mscore, "_describer"):
        mscore._describer = MagicMock()

    # taggers 依赖 describe(分级标签常量), 先加载并注册到 sys.modules
    spec = importlib.util.spec_from_file_location(
        "msmemscope.describe", os.path.join(_SRC, "describe.py"))
    desc_module = importlib.util.module_from_spec(spec)
    sys.modules["msmemscope.describe"] = desc_module
    spec.loader.exec_module(desc_module)

    spec = importlib.util.spec_from_file_location(
        "msmemscope.hijacker.taggers", os.path.join(_SRC, "hijacker", "taggers.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules["msmemscope.hijacker.taggers"] = module
    spec.loader.exec_module(module)
    return module


_mod = _load_taggers()
_T = _mod.VllmAscendTaggers


class TestTensorAddr(unittest.TestCase):
    """地址提取: 无效地址/DTensor 本地张量/无 data_ptr"""

    def test_plain_addr(self):
        self.assertEqual(_mod._tensor_addr(FakeTensor(0x1000)), 0x1000)

    def test_zero_addr_skipped(self):
        self.assertIsNone(_mod._tensor_addr(FakeTensor(0)))

    def test_dtensor_local_tensor(self):
        # 分布式张量以 _local_tensor 承载实际存储
        class Dt:
            def __init__(self):
                self._local_tensor = FakeTensor(0x2000)
        self.assertEqual(_mod._tensor_addr(Dt()), 0x2000)

    def test_no_data_ptr(self):
        self.assertIsNone(_mod._tensor_addr(object()))


class TestTagModelWeights(unittest.TestCase):
    """权重地址直标: 主模型/LoRA/drafter/去重/异常防护"""

    def setUp(self):
        _mod._describer.reset_mock()

    def _runner(self):
        runner = MagicMock()
        runner.model = FakeModule(
            params=[("weight", FakeTensor(0x1000)), ("lm_head.weight", FakeTensor(0x2000))],
            buffers=[("cos_sin_cache", FakeTensor(0x3000))],
        )
        return runner

    def test_main_model_weights(self):
        _T.tag_model_weights(None, self._runner())
        calls = _mod._describer.describe_addr.call_args_list
        self.assertEqual(len(calls), 3)
        for addr, owner in [(0x1000, "vllm@weights"), (0x2000, "vllm@weights"), (0x3000, "vllm@weights")]:
            self.assertIn(((addr, _mod._VLLM_WEIGHTS),), calls)

    def test_lora_specific_label_wins(self):
        runner = self._runner()
        lora_module = FakeModule()
        lora_module.lora_a = {"q_proj": FakeTensor(0x1000)}  # 与主权重同地址(共享块视图)
        runner.model = FakeModule(
            params=[("weight", FakeTensor(0x1000))],
            modules=[("lora_layer", lora_module)],
        )
        _T.tag_model_weights(None, runner)
        calls = _mod._describer.describe_addr.call_args_list
        # 特定标签优先且仅标一次: 同地址只出现 @lora, 不出现主权重标签
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0], (0x1000, _mod._VLLM_WEIGHTS_LORA))

    def test_drafter_weights(self):
        runner = self._runner()
        runner.drafter = MagicMock()
        runner.drafter.model = FakeModule(params=[("weight", FakeTensor(0x4000))])
        _T.tag_model_weights(None, runner)
        calls = _mod._describer.describe_addr.call_args_list
        self.assertIn(((0x4000, _mod._VLLM_WEIGHTS_DRAFTER),), calls)

    def test_no_model_noop(self):
        runner = MagicMock()
        runner.model = None
        _T.tag_model_weights(None, runner)
        _mod._describer.describe_addr.assert_not_called()

    def test_exception_safe(self):
        runner = MagicMock()

        class BoomModel:
            def named_parameters(self):
                raise RuntimeError("boom")
            def named_buffers(self):
                return []
            def named_modules(self):
                yield "", self

        runner.model = BoomModel()
        with redirect_stdout(io.StringIO()):
            _T.tag_model_weights(None, runner)  # 不抛异常
        self.assertFalse(_mod._describer.describe_addr.called)


def _spec(name):
    """按类名构造 kv_cache_spec 桩(type() 返回真实类型, 类名即 spec 名)"""
    return type(name, (), {})()


def _kv_config(groups):
    config = MagicMock()
    config.kv_cache_groups = groups
    return config


def _attn_group(layers, spec):
    group = MagicMock()
    group.layer_names = layers
    group.kv_cache_spec = spec
    return group


class TestKvCacheLabel(unittest.TestCase):
    """spec 类型 → 标签归类"""

    def test_attn(self):
        self.assertEqual(_T._kv_cache_label(_spec("AttentionSpec")), _mod._VLLM_KV_ATTN)
        self.assertEqual(_T._kv_cache_label(_spec("AscendMLAAttentionSpec")), _mod._VLLM_KV_ATTN)
        self.assertEqual(_T._kv_cache_label(_spec("AscendSFAIndexerCacheSpec")), _mod._VLLM_KV_ATTN)

    def test_mamba(self):
        self.assertEqual(_T._kv_cache_label(_spec("MambaSpec")), _mod._VLLM_KV_MAMBA)

    def test_hidden_state(self):
        self.assertEqual(_T._kv_cache_label(_spec("HiddenStateCacheSpec")), _mod._VLLM_KV_HIDDEN_STATE)

    def test_unknown_spec_defaults_attn(self):
        self.assertEqual(_T._kv_cache_label(_spec("FutureSpecX")), _mod._VLLM_KV_ATTN)

    def test_none(self):
        self.assertIsNone(_T._kv_cache_label(None))


class TestTagKvCache(unittest.TestCase):
    """KV Cache 地址直标: 类型归类/去重/退化路径/异常防护"""

    def setUp(self):
        _mod._describer.reset_mock()

    def _runner(self, layer_specs, groups=None):
        runner = MagicMock()
        runner._get_layer_kv_cache_specs.return_value = layer_specs
        runner.kv_cache_config = _kv_config(groups)
        return runner

    def test_classify_by_layer(self):
        ret = {
            "model.layers.0.self_attn": FakeTensor(0xA000),
            "model.layers.0.mamba": FakeTensor(0xB000),
            "model.layers.0.cache_only": FakeTensor(0xC000),
        }
        layer_specs = {
            "model.layers.0.self_attn": _spec("AttentionSpec"),
            "model.layers.0.mamba": _spec("MambaSpec"),
            "model.layers.0.cache_only": _spec("HiddenStateCacheSpec"),
        }
        _T.tag_kv_cache(ret, self._runner(layer_specs))
        calls = _mod._describer.describe_addr.call_args_list
        self.assertEqual(len(calls), 3)
        self.assertIn(((0xA000, _mod._VLLM_KV_ATTN),), calls)
        self.assertIn(((0xB000, _mod._VLLM_KV_MAMBA),), calls)
        self.assertIn(((0xC000, _mod._VLLM_KV_HIDDEN_STATE),), calls)

    def test_cross_layer_shared_dedupe(self):
        shared = FakeTensor(0xA000)
        ret = {"layer.0": shared, "layer.1": shared}
        layer_specs = {"layer.0": _spec("AttentionSpec"), "layer.1": _spec("AttentionSpec")}
        _T.tag_kv_cache(ret, self._runner(layer_specs))
        _mod._describer.describe_addr.assert_called_once_with(0xA000, _mod._VLLM_KV_ATTN)

    def test_tuple_and_dict_values(self):
        ret = {
            "layer.0": (FakeTensor(0xA000), FakeTensor(0xA100)),
            "layer.1": {"k": FakeTensor(0xB000), "v": FakeTensor(0xB100)},
        }
        layer_specs = {"layer.0": _spec("AttentionSpec"), "layer.1": _spec("MambaSpec")}
        _T.tag_kv_cache(ret, self._runner(layer_specs))
        calls = _mod._describer.describe_addr.call_args_list
        self.assertEqual(len(calls), 4)

    def test_no_spec_layer_skipped(self):
        ret = {"layer.0": FakeTensor(0xA000)}
        _T.tag_kv_cache(ret, self._runner({}))
        _mod._describer.describe_addr.assert_not_called()

    def test_empty_ret_noop(self):
        _T.tag_kv_cache(None, self._runner({}))
        _T.tag_kv_cache({}, self._runner({}))
        _mod._describer.describe_addr.assert_not_called()

    def test_fallback_to_groups(self):
        """runner 无 _get_layer_kv_cache_specs 时退化为按 kv_cache_groups 归类"""
        runner = MagicMock()
        runner.configure_mock(_get_layer_kv_cache_specs=None)
        runner.kv_cache_config = _kv_config([
            _attn_group(["layer.0"], _spec("MambaSpec")),
            _attn_group(["layer.1"], _spec("AttentionSpec")),
        ])
        ret = {"layer.0": FakeTensor(0xA000), "layer.1": FakeTensor(0xB000)}
        _T.tag_kv_cache(ret, runner)
        calls = _mod._describer.describe_addr.call_args_list
        self.assertIn(((0xA000, _mod._VLLM_KV_MAMBA),), calls)
        self.assertIn(((0xB000, _mod._VLLM_KV_ATTN),), calls)

    def test_uniform_type_specs_expanded(self):
        uniform = MagicMock()
        uniform.kv_cache_specs = {"layer.0": _spec("MambaSpec"), "layer.1": _spec("AttentionSpec")}
        runner = MagicMock()
        runner.configure_mock(_get_layer_kv_cache_specs=None)
        runner.kv_cache_config = _kv_config([_attn_group(["layer.0", "layer.1"], uniform)])
        ret = {"layer.0": FakeTensor(0xA000), "layer.1": FakeTensor(0xB000)}
        _T.tag_kv_cache(ret, runner)
        calls = _mod._describer.describe_addr.call_args_list
        self.assertIn(((0xA000, _mod._VLLM_KV_MAMBA),), calls)
        self.assertIn(((0xB000, _mod._VLLM_KV_ATTN),), calls)

    def test_exception_safe(self):
        runner = self._runner({})

        def boom_ret(*args, **kwargs):
            raise RuntimeError("boom")
        runner._get_layer_kv_cache_specs.side_effect = boom_ret
        with redirect_stdout(io.StringIO()):
            _T.tag_kv_cache({"layer.0": FakeTensor(0xA000)}, runner)
        self.assertFalse(_mod._describer.describe_addr.called)


class TestFrameworkClassOrganization(unittest.TestCase):
    """框架维度组织: 类暴露该框架的标记函数, 回调约定 (ret, *args, **kwargs)"""

    def setUp(self):
        _mod._describer.reset_mock()  # 共享 mock, 避免跨文件调用记录污染

    def test_required_taggers_exposed(self):
        self.assertTrue(callable(_T.tag_model_weights))
        self.assertTrue(callable(_T.tag_kv_cache))

    def test_callback_receives_ret_and_instance(self):
        """回调约定: 以 (ret, *args) 位置调用, ret 传回参, args[0] 为实例"""
        runner = MagicMock()
        runner.model = FakeModule(params=[("weight", FakeTensor(0x1000))])
        _T.tag_model_weights("some_ret", runner)  # ret 被忽略, args[0]=runner 生效
        _mod._describer.describe_addr.assert_called_once_with(0x1000, _mod._VLLM_WEIGHTS)


if __name__ == "__main__":
    unittest.main()
