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
"""
按框架维度组织的地址直标器集合(回调标记)。

decompose 条目的第4字段支持两种标记方式, 由类型分派:
- list: 范围标记, 标记整个函数窗口内全部显存申请, 显式分级标签 [(label, level), ...];
- callable: 回调标记, 由工具框架传入被劫持函数的出参入参 (ret, *args, **kwargs),
  args[0] 为被劫持方法所属的实例(如 NPUModelRunner), 回调自行处理标记逻辑
  (通常为 describe_addr 地址直标, 同样为显式分级标签)。

本模块按框架组织: 每个框架一个 tagger 类, 类内放置该框架的标记函数, 避免后续
框架扩展时钩子类型/注册表膨胀。新增框架时在对应类中补充静态方法, 并在 hijack_map
的 decompose 条目中以函数引用作为第4字段。

地址直标与范围标记是两类互补的打标机制(分级标签模型):
- 范围标记: 在块 carve 时刻按 DescribeTrace 各级标签栈打标, 粒度=缓存池块;
- 地址直标: 事后按张量地址打标, 与 carve 时机解耦, 可纠正块复用/窗口外分配,
  同级别重复时以地址标签为准(OwnerLabelManager 覆盖语义)。

vllm-ascend 场景采用"方案a"规避双标: 被地址直标的目标(load_model /
initialize_kv_cache_tensors)不使用范围标记条目, 其 carve 发生在空栈下,
地址直标后 owner 为干净分级标签(见 RFC 2026-08-03-vllm-ascend-decompose-improve)。

本模块不依赖 torch/torch_npu/vllm, 通过鸭子类型访问张量地址, 避免导入耦合。
"""

from typing import Dict, Optional

from .._msmemscope import _describer
from ..describe import COMPONENT, DETAIL_1, DETAIL_2

# vllm 分级标签常量(显式分级: 组件@细化1@细化2)
_VLLM_WEIGHTS = [("vllm", COMPONENT), ("weights", DETAIL_1)]
_VLLM_WEIGHTS_LORA = [("vllm", COMPONENT), ("weights", DETAIL_1), ("lora", DETAIL_2)]
_VLLM_WEIGHTS_DRAFTER = [("vllm", COMPONENT), ("weights", DETAIL_1), ("drafter", DETAIL_2)]
_VLLM_KV_ATTN = [("vllm", COMPONENT), ("kv_cache", DETAIL_1), ("attn", DETAIL_2)]
_VLLM_KV_MAMBA = [("vllm", COMPONENT), ("kv_cache", DETAIL_1), ("mamba", DETAIL_2)]
_VLLM_KV_HIDDEN_STATE = [("vllm", COMPONENT), ("kv_cache", DETAIL_1), ("hidden_state", DETAIL_2)]

# ---------------------------------------------------------------------------
# 通用辅助(跨框架): 张量地址提取与去重打标
# ---------------------------------------------------------------------------


def _tensor_addr(tensor) -> Optional[int]:
    """
    取张量 NPU 设备地址。分布式张量(DTensor 等)以 _local_tensor 承载实际存储,
    需取本地张量(参考 optimizer_step_hook._to_local_tensor 的适配思路);
    通过鸭子类型识别, 不强制依赖 torch.distributed.tensor。
    """
    local = getattr(tensor, "_local_tensor", None)
    if local is not None:
        tensor = local
    data_ptr = getattr(tensor, "data_ptr", None)
    if data_ptr is None:
        return None
    try:
        addr = data_ptr()
    except Exception:
        return None
    return addr if addr else None


def _describe_tensor_addr(tensor, labels: list, seen: Optional[set] = None) -> None:
    """
    按地址打标(显式分级): labels 为 [(label, level), ...]。
    地址无效或重复(data_ptr 去重)时跳过; 同一回调内必须经 seen 去重,
    避免对同一地址重复打标(覆盖语义下重复打标无意义且浪费)。
    """
    addr = _tensor_addr(tensor)
    if addr is None:
        return
    if seen is not None:
        if addr in seen:
            return
        seen.add(addr)
    _describer.describe_addr(addr, labels)


class VllmAscendTaggers:
    """
    vllm-ascend 框架的地址直标器集合(回调标记)。

    回调约定: `callback(ret, *args, **kwargs)`, ret 为被劫持函数返回值,
    args[0] 为被劫持方法所属的实例。异常由 MemScopeHooklet 吞掉并告警,
    不影响推理主流程。
    """

    # ------------------------------------------------------------------
    # 权重: NPUModelRunner.load_model 回调
    # ------------------------------------------------------------------
    @staticmethod
    def tag_model_weights(ret, *args, **kwargs) -> None:
        """
        load_model 回调: 遍历主模型参数/buffer, 以及条件存在的 drafter
        (投机解码草稿模型)与 LoRA 权重, 按用途打标。与范围标记互斥(方案a):
        load_model 不注册范围标记条目, carve 在空栈下发生, describe_addr
        追加后标签干净。打标顺序: 特定用途(LoRA/drafter)优先, 同地址只标一次。
        """
        runner = args[0] if args else None
        model = getattr(runner, "model", None) if runner is not None else None
        if model is None:
            return
        try:
            seen = set()
            # LoRA: vllm LoRAModel 的 lora_a/lora_b 为 dict[str, nn.Parameter]
            for _, module in model.named_modules():
                for attr in ("lora_a", "lora_b"):
                    lora_params = getattr(module, attr, None)
                    if isinstance(lora_params, dict):
                        for param in lora_params.values():
                            _describe_tensor_addr(param, _VLLM_WEIGHTS_LORA, seen)
            # 投机解码草稿模型权重(条件存在)
            drafter = getattr(runner, "drafter", None)
            drafter_model = getattr(drafter, "model", None) if drafter is not None else None
            if drafter_model is not None:
                VllmAscendTaggers._tag_module_tensors(drafter_model, _VLLM_WEIGHTS_DRAFTER, seen)
            # 主模型权重
            VllmAscendTaggers._tag_module_tensors(model, _VLLM_WEIGHTS, seen)
        except Exception as exc:
            # 打标失败不影响推理主流程
            print(f"[msmemscope] Warning: failed to tag vllm weight addresses, error: {exc}")

    @staticmethod
    def _tag_module_tensors(module, labels: list, seen: set) -> None:
        """遍历模块的参数与 buffer 打标(参数优先, buffer 含量化 scale 等非参数存储)"""
        for _, param in module.named_parameters():
            _describe_tensor_addr(param, labels, seen)
        for _, buf in module.named_buffers():
            _describe_tensor_addr(buf, labels, seen)

    # ------------------------------------------------------------------
    # KV Cache: NPUModelRunner.initialize_kv_cache_tensors 回调
    # ------------------------------------------------------------------
    @staticmethod
    def tag_kv_cache(ret, *args, **kwargs) -> None:
        """
        initialize_kv_cache_tensors 回调: ret 为 {layer_name: tensor | tuple | dict}
        的逐层缓存张量映射。按层判定 kv_cache_spec 类型后归类打标
        (attn/mamba/hidden_state), data_ptr 去重(跨层共享/混合共享只标一次)。
        依赖 vllm-ascend 11.0 内部结构(kv_cache_config.kv_cache_groups 的 spec 类型
        与 _get_layer_kv_cache_specs), 结构变化时静默退化。
        """
        kv_caches = ret if isinstance(ret, dict) else None
        if not kv_caches:
            return
        runner = args[0] if args else None
        try:
            config = getattr(runner, "kv_cache_config", None) if runner is not None else None
            layer_specs = VllmAscendTaggers._get_layer_specs(runner, config) if config is not None else {}
            seen = set()
            for layer_name, tensors in kv_caches.items():
                label = VllmAscendTaggers._kv_cache_label(layer_specs.get(layer_name))
                if label is None:
                    continue
                if isinstance(tensors, dict):
                    tensors = list(tensors.values())
                elif not isinstance(tensors, (list, tuple)):
                    tensors = [tensors]
                for tensor in tensors:
                    _describe_tensor_addr(tensor, label, seen)
        except Exception as exc:
            # 打标失败不影响推理主流程, 与 tag_model_weights 一致打告警
            print(f"[msmemscope] Warning: failed to tag vllm kv_cache addresses, error: {exc}")

    # kv_cache_spec 类型名匹配: 版本间命名漂移时按关键字兜底
    _MAMBA_SPEC_NAMES = ("MambaSpec",)
    _HIDDEN_STATE_SPEC_NAMES = ("HiddenStateCacheSpec", "HiddenStateSpec", "MambaCacheSpec")
    _ATTN_SPEC_NAMES = (
        "AttentionSpec",
        "AscendMLAAttentionSpec",
        "AscendSlidingWindowMLASpec",
        "AscendSFAIndexerCacheSpec",
    )

    @staticmethod
    def _kv_cache_label(spec) -> Optional[list]:
        """
        kv_cache_spec → 分级用途标签; 无法判定返回 None(该层跳过)。
        SFA indexer 缓存并入 attention; MLA 量化 scale 张量与 k/v 共享同一块
        (_adjust_kv_layout 的 overlap 布局), 无法按地址分离, 并入 attn。
        """
        if spec is None:
            return None
        spec_name = type(spec).__name__
        if spec_name in VllmAscendTaggers._MAMBA_SPEC_NAMES or "mamba" in spec_name.lower():
            return _VLLM_KV_MAMBA
        if spec_name in VllmAscendTaggers._HIDDEN_STATE_SPEC_NAMES or "hidden_state" in spec_name.lower():
            return _VLLM_KV_HIDDEN_STATE
        # 其余 kv cache spec(AttentionSpec 系/SFA indexer/未来新增)归为 attention
        return _VLLM_KV_ATTN

    @staticmethod
    def _get_layer_specs(runner, config) -> Dict[str, object]:
        """
        layer_name → kv_cache_spec 映射。
        首选 runner._get_layer_kv_cache_specs(11.0 内部方法, 含 AttentionLayerBase
        的 spec 覆写); 退化路径从 kv_cache_config.kv_cache_groups 构建
        (UniformTypeKVCacheSpecs 按层展开)。
        """
        getter = getattr(runner, "_get_layer_kv_cache_specs", None)
        if getter is not None:
            try:
                specs = getter(config)
            except Exception:
                # 内部接口异常时静默降级, 由下方 kv_cache_groups 兜底
                specs = None
            if isinstance(specs, dict):
                return specs
        specs = {}
        for group in getattr(config, "kv_cache_groups", None) or []:
            layer_names = getattr(group, "layer_names", None) or []
            spec = getattr(group, "kv_cache_spec", None)
            if spec is None:
                continue
            kv_specs = getattr(spec, "kv_cache_specs", None)
            if isinstance(kv_specs, dict):
                for name in layer_names:
                    specs[name] = kv_specs.get(name, spec)
            else:
                for name in layer_names:
                    specs[name] = spec
        return specs
