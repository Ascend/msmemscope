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
from typing import Callable, Dict, List, Optional, Tuple, Union

from ..describe import COMPONENT, DETAIL_1, PROCESS, describe_label, undescribe_label
from ..take_snapshot import take_snapshot
from .taggers import VllmAscendTaggers

DEFAULT_PRIORITY = 100

# 用途标签约束: 单段标签长度上限(与 C++ MAX_DESCRIBE_OWNER_LENGTH 对齐)
MAX_OWNER_LENGTH = 128
# 合法级别范围: 与 C++ OwnerLevel 枚举对齐(见 csrc/analysis/event.h)
MIN_OWNER_LEVEL = 0
MAX_OWNER_LEVEL = 7


def _parse_version(ver: str) -> Tuple[int, ...]:
    """
    版本字符串解析: '2.14.0a0' -> (2, 14, 0),补齐到三段
    非数字后缀(alpha/rc/dev等预发布标记)截断
    """
    parts = []
    for seg in ver.split("."):
        digits = ""
        for ch in seg:
            if not ch.isdigit():
                break
            digits += ch
        parts.append(int(digits) if digits else 0)
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts)


def _next_branch(ver: Tuple[int, ...]) -> Tuple[int, ...]:
    """分支上界: (2, 9, 0) -> (2, 10, 0),即 hi 所在分支的下一个分支版本"""
    parts = list(ver)
    if len(parts) >= 2:
        parts[-2] += 1
        parts[-1] = 0
    else:
        parts[-1] += 1
    return tuple(parts)


def _match_version(ver: str, key: str) -> bool:
    """
    版本键匹配,支持四种形式:
    (1) 精确版本: "2.14.0"   -- 仅匹配该版本
    (2) 最低版本: "2.10+"    -- 版本 >= 2.10(含)
    (3) 版本区间: "2.6-2.9"  -- 2.6 <= 版本 < 2.10,即 2.6~2.9 各分支的全部版本(含补丁)
    (4) 通配兜底: "*"        -- 任意版本
    """
    if key == "*":
        return True
    ver_tuple = _parse_version(ver)
    if key.endswith("+"):
        return ver_tuple >= _parse_version(key[:-1])
    if "-" in key:
        lo, hi = key.split("-", 1)
        # 区间上界取 hi 分支的下一个版本,使 "2.6-2.9" 覆盖 2.9.x 全部补丁版本
        return _parse_version(lo) <= ver_tuple < _next_branch(_parse_version(hi))
    return ver_tuple == _parse_version(key)


def _validate_owner(owner: list) -> bool:
    """
    用途标签合法性校验(分级模型): 条目为 [(label, level), ...] 列表,
    每段标签无前导@、非空、≤128字符, level 为 0~7 的合法级别。
    返回 False 时由调用方告警(不阻断注册)。
    """
    if not owner:
        return True
    if not isinstance(owner, list):
        return False
    for label, level in owner:
        if not isinstance(label, str) or not label:
            return False
        if label.startswith("@"):
            return False
        if len(label) > MAX_OWNER_LENGTH:
            return False
        if not isinstance(level, int) or level < MIN_OWNER_LEVEL or level > MAX_OWNER_LEVEL:
            return False
    return True


class MemScopeHooklet:
    """
    MemScope场景下最小的Hook单元,封装了单个劫持目标的前置和后置钩子逻辑,支持显存拆解和显存快照两种劫持方法
    """

    def __init__(
        self,
        hook_type: str,
        module: str,
        class_name: str,
        method_name: str,
        priority: int = DEFAULT_PRIORITY,
        owner: Optional[list] = None,
        callback: Optional[Callable] = None,
    ):
        """
        初始化Hook单元
        :param hook_type: 钩子功能类型(如 decompose/snapshot)
        :param module: 目标模块名(如 vllm_ascend.worker.model_runner_v1)
        :param class_name: 目标类名(如 NPUModelRunner)
        :param method_name: 目标方法名(如 load_model)
        :param priority: 钩子优先级(数值越小优先级越高)
        :param owner: 范围标记分级用途标签 [(label, level), ...](如 [("fsdp2", COMPONENT), ("activation", PROCESS)]),
                      逐段上报至 DescribeTrace 对应级别, 同级别嵌套计数由 C++ 侧管理
        :param callback: 回调标记(decompose 条目第4字段为 callable 时),约定签名 callback(ret, *args, **kwargs),args[0]为被劫持实例
        """
        self.hook_type = hook_type
        self.module = module
        self.class_name = class_name
        self.method_name = method_name
        self.priority = priority
        self.identifier = f"{self.module}@{self.class_name}@{self.method_name}"
        self.owner = owner if owner else []
        self.callback = callback
        if owner and not _validate_owner(owner):
            print(
                f"[msmemscope] Warning: 用途标签 '{owner}' 不符合约束(每段无前导@/非空/≤{MAX_OWNER_LENGTH}字符, level 0~7),请检查 {self.identifier}"
            )

    def prehook_func(self, *args, **kwargs):
        # 回调标记: 前置为空操作, 标记逻辑由回调在函数返回后自行处理(如地址直标)
        if self.callback is not None:
            return args, kwargs
        if self.hook_type == "decompose":
            # 分级标签逐段上报(内部接口带级别); 同级别嵌套计数由 DescribeTrace 管理
            for label, level in self.owner:
                describe_label(label, level)
        elif self.hook_type == "snapshot":
            take_snapshot(None, f"{self.identifier}@Start")
        return args, kwargs

    def posthook_func(self, ret, *args, **kwargs):
        # 回调标记: 执行回调
        if self.callback is not None:
            self._run_callback(ret, args, kwargs)
            return ret
        if self.hook_type == "decompose":
            for label, level in self.owner:
                undescribe_label(label, level)
        elif self.hook_type == "snapshot":
            take_snapshot(None, f"{self.identifier}@End")
        return ret

    def _run_callback(self, ret, args, kwargs):
        """执行回调标记: 异常只告警, 不影响被劫持函数的正常返回"""
        if self.callback is None:
            return
        try:
            self.callback(ret, *args, **kwargs)
        except Exception as exc:
            print(f"[msmemscope] Warning: 回调标记执行失败({self.identifier}), 错误: {exc}")


class MemScopeHijackMap:
    """
    单例类:维护framework/version/component/hook_type四级劫持映射,提供劫持单元列表生成能力
    """

    _instance = None

    def __new__(cls, *args, **kwargs):
        """单例模式：确保全局只有一个实例"""
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __init__(self):
        """初始化核心映射表:framework → version → component → hook_type → 劫持目标列表"""
        self.hijack_mapping: Dict[str, Dict[str, Dict[str, Dict[str, List[List[Union[str, Callable]]]]]]] = {
            # decompose 条目第4字段按类型分派标记方式(分级标签模型):
            # - list: 范围标记(窗口内全部显存申请打标), 显式分级标签 [(label, level), ...],
            #   逐段上报至 DescribeTrace 对应级别(COMPONENT/PROCESS/DETAIL_1等), 同级别嵌套计数由 C++ 管理
            # - callable: 回调标记(框架传入被劫持函数出参入参 (ret, *args, **kwargs)),
            #   回调自行处理标记逻辑(如 describe_addr 地址直标, 同样显式分级)
            "vllm_ascend": {
                "11.0": {
                    "worker": {
                        "decompose": [
                            [
                                "vllm_ascend.worker.model_runner_v1",
                                "NPUModelRunner",
                                "profile_run",
                                [("vllm", COMPONENT), ("profile", PROCESS)],
                            ],
                            [
                                "vllm_ascend.worker.model_runner_v1",
                                "NPUModelRunner",
                                "execute_model",
                                [("vllm", COMPONENT), ("serve", PROCESS)],
                            ],
                            [
                                "vllm_ascend.worker.worker",
                                "NPUWorker",
                                "compile_or_warm_up_model",
                                [("vllm", COMPONENT), ("warmup", PROCESS)],
                            ],
                            [
                                "vllm_ascend.worker.model_runner_v1",
                                "NPUModelRunner",
                                "capture_model",
                                [("graph_pool", DETAIL_1)],
                            ],
                            [
                                "vllm_ascend.worker.model_runner_v1",
                                "NPUModelRunner",
                                "load_model",
                                VllmAscendTaggers.tag_model_weights,
                            ],
                            [
                                "vllm_ascend.worker.model_runner_v1",
                                "NPUModelRunner",
                                "initialize_kv_cache_tensors",
                                VllmAscendTaggers.tag_kv_cache,
                            ],
                        ],
                        "snapshot": [
                            ["vllm_ascend.worker.model_runner_v1", "NPUModelRunner", "load_model"],
                            ["vllm_ascend.worker.model_runner_v1", "NPUModelRunner", "profile_run"],
                            ["vllm_ascend.worker.model_runner_v1", "NPUModelRunner", "initialize_kv_cache"],
                            ["vllm_ascend.worker.model_runner_v1", "NPUModelRunner", "execute_model"],
                            ["vllm_ascend.worker.worker", "NPUWorker", "compile_or_warm_up_model"],
                            ["vllm_ascend.worker.model_runner_v1", "NPUModelRunner", "capture_model"],
                        ],
                    }
                }
            },
            "verl": {
                "0.7.0": {
                    "TaskRunner": {
                        "snapshot": [
                            ["verl.trainer.main_ppo", "TaskRunner", "add_actor_rollout_worker"],
                            ["verl.trainer.main_ppo", "TaskRunner", "add_critic_worker"],
                            ["verl.trainer.main_ppo", "TaskRunner", "add_reward_model_worker"],
                            ["verl.trainer.main_ppo", "TaskRunner", "add_ref_policy_worker"],
                            ["verl.trainer.main_ppo", "TaskRunner", "run"],
                        ]
                    }
                }
            },
            "mindspeed_llm": {
                "0.12.1": {
                    "training": {
                        "snapshot": [
                            # 训练入口
                            ["mindspeed_llm.training.training", "", "train"],
                            ["mindspeed_llm.training.training", "", "train_step"],
                            # 模型和优化器初始化
                            [
                                "megatron.training.training",
                                "",
                                "setup_model_and_optimizer",
                            ],  # 包含权重、梯度、优化器的显存申请
                            ["megatron.training.training", "", "get_model"],  # 申请权重的显存
                            ["megatron.core.optimizer", "", "get_megatron_optimizer"],  # 申请初始优化器的显存
                            # 前向传播(常用类方法的forward)
                            ["mindspeed_llm.core.models.gpt.gpt_model", "GPTModel", "forward"],
                            [
                                "mindspeed_llm.core.transformer.transformer_block",
                                "TransformerBlock",
                                "forward",
                            ],  # Block由多个layer组成
                            ["mindspeed_llm.core.transformer.transformer_layer", "TransformerLayer", "forward"],
                            ["mindspeed_llm.core.transformer.attention", "SelfAttention", "forward"],
                            ["mindspeed_llm.core.transformer.mlp", "MLP", "forward"],
                            ["mindspeed_llm.core.transformer.moe.moe_layer", "MoELayer", "forward"],
                            ["mindspeed_llm.core.transformer.moe.router", "TopKRouter", "forward"],
                            # 反向传播
                            ["megatron.core.pipeline_parallel.schedules", "", "backward_step"],  # 通用流水线并行
                            [
                                "mindspeed_llm.core.pipeline_parallel.dualpipe.gpt_model",
                                "",
                                "gpt_model_backward",
                            ],  # DualPipe 高性能训练
                            # 损失函数
                            ["mindspeed_llm.core.models.gpt.gpt_model", "GPTModel", "compute_language_model_loss"],
                            ["megatron.core.tensor_parallel.cross_entropy", "", "vocab_parallel_cross_entropy"],
                            # 优化器(optimizer.py)
                            ["megatron.core.optimizer.optimizer", "MegatronOptimizer", "step"],
                            ["megatron.core.optimizer.optimizer", "MixedPrecisionOptimizer", "step"],
                            ["megatron.core.optimizer.optimizer", "Float16OptimizerWithFloat16Params", "step"],
                            ["megatron.core.optimizer.optimizer", "FP32Optimizer", "step"],
                            ["megatron.core.optimizer.optimizer", "ChainedOptimizer", "step"],
                            # 优化器(distrib_optimizer.py, 继承MixedPrecisionOptimizer)
                            [
                                "megatron.core.optimizer.distrib_optimizer",
                                "DistributedOptimizer",
                                "step_with_ready_grads",
                            ],
                        ]
                    }
                }
            },
            "pytorch": {
                # 版本键支持精确版本/区间键(如 2.10+)/通配键(*),多版本共享同一张劫持函数表;
                # 多键并集解析: 查询版本时返回所有匹配键的组件条目并集(跨键按目标三元组去重),
                # 支持 fsdp1(2.6+ 单表覆盖全版本)与 fsdp2(按布局分双表)共存
                # FSDP1(FullyShardedDataParallel)目标 2.6~2.14 全程稳定(forward 覆写结构 +
                # _runtime_utils 窗口钩子 + FlatParamHandle 申请点, 2.6/2.14 双时点验证)
                "2.6+": {
                    "fsdp1": {
                        "decompose": [
                            # 前向激活值: FSDP 实例 forward 窗口(含 unshard/前向体/reshard, 嵌套由重入保护处理)
                            [
                                "torch.distributed.fsdp.fully_sharded_data_parallel",
                                "FullyShardedDataParallel",
                                "forward",
                                [("fsdp1", COMPONENT), ("activation", PROCESS)],
                            ],
                            # 反向激活值: 模块反向段窗口 (pre_backward_hook 描述 / post_backward_hook 撤销)
                            [
                                "torch.distributed.fsdp._runtime_utils",
                                "",
                                "_pre_backward_hook",
                                [("fsdp1", COMPONENT), ("backward", PROCESS)],
                            ],
                            [
                                "torch.distributed.fsdp._runtime_utils",
                                "",
                                "_post_backward_hook",
                                [("fsdp1", COMPONENT), ("backward", PROCESS)],
                            ],
                            # 分片权重: FSDP 包装时申请
                            [
                                "torch.distributed.fsdp._flat_param",
                                "FlatParamHandle",
                                "_init_flat_param_and_metadata",
                                [("fsdp1", COMPONENT), ("sharded_weight", DETAIL_1)],
                            ],
                            # all-gather 输出缓冲: 每次 unshard (_full_param_padded/_full_prec_full_param_padded 统一入口)
                            [
                                "torch.distributed.fsdp._flat_param",
                                "FlatParamHandle",
                                "_alloc_padded_unsharded_flat_param",
                                [("fsdp1", COMPONENT), ("all_gather", DETAIL_1)],
                            ],
                            # 分片梯度: 反向 reduce-scatter 归约 (含 RS 输入缓冲与降精度转换)
                            [
                                "torch.distributed.fsdp._runtime_utils",
                                "",
                                "_reduce_grad",
                                [("fsdp1", COMPONENT), ("gradient", DETAIL_1)],
                            ],
                        ]
                    }
                },
                # FSDP2(fully_shard)按 _fully_shard 布局分两张表:
                #  - "2.6-2.9" 旧布局: all-gather 输出缓冲在自定义算子 all_gather_copy_in 内部申请,
                #    reduce-scatter 缓冲在 foreach_reduce 内联申请, 无 DefaultAllGather/DefaultReduceScatter
                #    allocate 类(2.9 已实测为旧布局, 新锚点自 2.10 引入); 旧布局钩子只能挂模块级函数,
                #    且 _fsdp_param_group 按名导入 foreach_*, 需双命名空间条目
                #  - "2.10+"  新布局: allocate 抽象(PyTorch PR #155189, 2025-07 合入, 2.10 起)
                "2.6-2.9": {
                    "fsdp2": {
                        "decompose": [
                            # 激活值: FSDP 模块前向窗口 (pre_forward 描述 / post_forward 撤销)
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_state",
                                "FSDPState",
                                "_pre_forward",
                                [("fsdp2", COMPONENT), ("activation", PROCESS)],
                            ],
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_state",
                                "FSDPState",
                                "_post_forward",
                                [("fsdp2", COMPONENT), ("activation", PROCESS)],
                            ],
                            # 激活值(反向): FSDP 模块反向段窗口 (pre_backward 描述 / post_backward 撤销)
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_state",
                                "FSDPState",
                                "_pre_backward",
                                [("fsdp2", COMPONENT), ("backward", PROCESS)],
                            ],
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_param_group",
                                "FSDPParamGroup",
                                "post_backward",
                                [("fsdp2", COMPONENT), ("backward", PROCESS)],
                            ],
                            # 分片权重: fully_shard() 包装时申请
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_param",
                                "FSDPParam",
                                "_init_sharded_param",
                                [("fsdp2", COMPONENT), ("sharded_weight", DETAIL_1)],
                            ],
                            # all-gather 输出缓冲: 旧布局在 foreach_all_gather 窗口内申请(含算子内部),双命名空间
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_collectives",
                                "",
                                "foreach_all_gather",
                                [("fsdp2", COMPONENT), ("all_gather_output", DETAIL_1)],
                            ],
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_param_group",
                                "",
                                "foreach_all_gather",
                                [("fsdp2", COMPONENT), ("all_gather_output", DETAIL_1)],
                            ],
                            # reduce-scatter 输入/输出缓冲: 旧布局在 foreach_reduce 窗口内内联申请,双命名空间
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_collectives",
                                "",
                                "foreach_reduce",
                                [("fsdp2", COMPONENT), ("gradient", DETAIL_1)],
                            ],
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_param_group",
                                "",
                                "foreach_reduce",
                                [("fsdp2", COMPONENT), ("gradient", DETAIL_1)],
                            ],
                            # 单卡/扩展路径的 per-param all-gather 缓冲 (world_size==1 分支, 方法缺失时静默跳过)
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_param",
                                "FSDPParam",
                                "init_all_gather_outputs",
                                [("fsdp2", COMPONENT), ("all_gather_output", DETAIL_1)],
                            ],
                        ]
                    }
                },
                "2.10+": {
                    "fsdp2": {
                        "decompose": [
                            # 激活值: FSDP 模块前向窗口 (pre_forward 描述 / post_forward 撤销)
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_state",
                                "FSDPState",
                                "_pre_forward",
                                [("fsdp2", COMPONENT), ("activation", PROCESS)],
                            ],
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_state",
                                "FSDPState",
                                "_post_forward",
                                [("fsdp2", COMPONENT), ("activation", PROCESS)],
                            ],
                            # 激活值(反向): FSDP 模块反向段窗口 (pre_backward 描述 / post_backward 撤销)
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_state",
                                "FSDPState",
                                "_pre_backward",
                                [("fsdp2", COMPONENT), ("backward", PROCESS)],
                            ],
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_param_group",
                                "FSDPParamGroup",
                                "post_backward",
                                [("fsdp2", COMPONENT), ("backward", PROCESS)],
                            ],
                            # 分片权重: fully_shard() 包装时申请
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_param",
                                "FSDPParam",
                                "_init_sharded_param",
                                [("fsdp2", COMPONENT), ("sharded_weight", DETAIL_1)],
                            ],
                            # all-gather 输出缓冲: 前向/反向 unshard (新布局 allocate 抽象)
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_collectives",
                                "DefaultAllGather",
                                "allocate",
                                [("fsdp2", COMPONENT), ("all_gather_output", DETAIL_1)],
                            ],
                            # reduce-scatter 输入/输出缓冲: 反向梯度归约
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_collectives",
                                "DefaultReduceScatter",
                                "allocate",
                                [("fsdp2", COMPONENT), ("gradient", DETAIL_1)],
                            ],
                            # 单卡/扩展路径的 per-param all-gather 缓冲 (world_size==1 分支, 方法缺失时静默跳过)
                            [
                                "torch.distributed.fsdp._fully_shard._fsdp_param",
                                "FSDPParam",
                                "init_all_gather_outputs",
                                [("fsdp2", COMPONENT), ("all_gather_output", DETAIL_1)],
                            ],
                        ]
                    }
                },
            },
        }

    def _resolve_version_keys(self, version_map: Dict[str, Dict], version: str) -> List[str]:
        """
        版本键并集解析: 返回所有匹配的版本键(精确键优先, 其余按声明顺序)
        多组件框架(如 pytorch 的 fsdp1/fsdp2)可分别登记在互不重叠的版本键下,
        查询版本时返回全部命中键, 由 get_hook_entries 按组件并集汇总
        """
        if version in version_map:
            return [version]
        return [key for key in version_map if _match_version(version, key)]

    def get_hook_entries(
        self, framework: str, version: str, component: str, hook_type: str
    ) -> List[List[Union[str, Callable]]]:
        """
        获取指定框架/版本/组件/钩子类型对应的劫持目标列表
        :param framework: 框架名(如 vllm_ascend/pytorch)
        :param version: 框架版本(如 11.0),支持精确版本/区间键匹配(如传 2.11.0 命中 2.10+)
        :param component: 组件名(如 model_runner/memory_manager/fsdp1/fsdp2)
        :param hook_type: 钩子类型(如 decompose/snapshot)
        :return: 劫持目标列表 [[module, class_name, method_name, mark?], ...], decompose 条目
                 第4元素为 str(范围标记标签)或 callable(回调标记), 3元素旧条目向后兼容
        """
        if framework not in self.hijack_mapping:
            print(f"[msmemscope] Error: 框架 '{framework}' 不存在！当前支持的框架: {list(self.hijack_mapping.keys())}")
            return []

        version_map = self.hijack_mapping[framework]
        resolved_keys = self._resolve_version_keys(version_map, version)
        if not resolved_keys:
            print(
                f"[msmemscope] Error: 框架 '{framework}' 下的版本 '{version}' 不存在！"
                f"当前支持的版本: {list(version_map.keys())}"
            )
            return []

        # 组件校验: 至少一个匹配键中包含该组件
        if not any(component in version_map[key] for key in resolved_keys):
            components = sorted({c for key in resolved_keys for c in version_map[key]})
            print(
                f"[msmemscope] Error: 框架 '{framework}' 版本 '{version}' 下的组件 '{component}' 不存在！"
                f"当前支持的组件: {components}"
            )
            return []

        # 钩子类型校验: 该组件在任一匹配键中存在该钩子类型
        if not any(hook_type in version_map[key].get(component, {}) for key in resolved_keys):
            hook_types = sorted({ht for key in resolved_keys for ht in version_map[key].get(component, {})})
            print(
                f"[msmemscope] Error: 框架 '{framework}' 版本 '{version}' 组件 '{component}' 下的钩子类型 '{hook_type}' 不存在！"
                f"当前支持的类型: {hook_types}"
            )
            return []

        # 多版本键并集: 组件表可能登记在多个匹配键下(如 fsdp1 在 2.6+, fsdp2 在 2.6-2.9/2.10+),
        # 全部命中后按目标三元组(module, class, method)去重; 组件可能只存在于部分匹配键
        entries: List[List[Union[str, Callable]]] = []
        seen = set()
        for key in resolved_keys:
            for entry in version_map[key].get(component, {}).get(hook_type, []):
                target = (entry[0], entry[1], entry[2])
                if target in seen:
                    continue
                seen.add(target)
                entries.append(entry)
        return entries

    def get_hooklet_list(self, framework: str, version: str, component: str, hook_type: str) -> List[MemScopeHooklet]:
        """
        生成指定框架/版本/组件/钩子类型对应的MemScopeHooklet列表
        """
        hooklet_list = []
        hook_entries = self.get_hook_entries(framework, version, component, hook_type)

        for entry in hook_entries:
            module, class_name, method_name = entry[0], entry[1], entry[2]
            # 第4元素按类型分派: list=范围标记分级标签(owner), callable=回调标记, 缺失则向后兼容
            owner = []
            callback = None
            if len(entry) > 3:
                mark = entry[3]
                if isinstance(mark, list):
                    owner = mark
                elif callable(mark):
                    callback = mark
                else:
                    print(
                        f"[msmemscope] Error: 条目第4元素必须为 list(范围标记分级标签)或 callable(回调标记), "
                        f"收到 {type(mark).__name__} ({module}@{class_name}@{method_name}), 跳过该条目"
                    )
                    continue
            hooklet_unit = MemScopeHooklet(
                hook_type=hook_type,
                module=module,
                class_name=class_name,
                method_name=method_name,
                owner=owner,
                callback=callback,
            )
            hooklet_list.append(hooklet_unit)

        return hooklet_list


# 全局单例实例
memscope_hijack_map = MemScopeHijackMap()
