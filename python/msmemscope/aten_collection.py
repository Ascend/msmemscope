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

import functools
import sys
from contextlib import ExitStack
from collections.abc import Iterator
from typing import Any
import numpy as np
import mstx
from packaging import version
import torch
from torch.utils import _pytree as pytree
from torch.utils._python_dispatch import TorchDispatchMode

try:
    from torch._subclasses.fake_tensor import FakeTensor
    from torch._subclasses.functional_tensor import FunctionalTensor
except Exception:
    FakeTensor = torch.Tensor
    FunctionalTensor = torch.Tensor


is_version_support = version.parse(torch.__version__) > version.parse("2.3")
if is_version_support:

    def is_npu_tensor(value):
        return (
            isinstance(value, torch.Tensor)
            and value.is_npu
            and not isinstance(value, FakeTensor)
            and not isinstance(value, FunctionalTensor)
        )
else:
    print(f"[msmemscope]: Current Pytorch's version {torch.__version__} < 2.3, which doesn't support aten collection.")

    def is_npu_tensor(value):
        return False


def calculate_tensor_size(tensor: torch.Tensor):
    numel = np.prod(tensor.shape)
    element_size = tensor.itemsize
    size = numel * element_size

    return int(size)


def zip_arguments(schema: torch.FunctionSchema, args: tuple, kwargs: dict) -> Iterator:
    schema_args = schema.arguments[: len(args)]
    schema_kwargs = schema.arguments[len(args) :]

    yield from zip(schema_args, args)
    yield from ((arg, kwargs[arg.name]) for arg in schema_kwargs if arg.name in kwargs)


class ArgumentHandler:
    @classmethod
    def _handle_argument(
        cls,
        func,
        value: Any,
        is_write: bool,
        is_factory: bool,
        metadata_only: bool,
        is_output: bool = False,
    ) -> None:
        def _handle_tensor(
            func,
            value: Any,
            is_write: bool,
            is_factory: bool,
            metadata_only: bool,
            is_output: bool,
        ) -> None:
            is_read = False
            data_ptr = value.data_ptr()

            if not is_write and not metadata_only:
                is_read = True

            # 对于metadata_only分俩类处理，FACTORY FUNC和其它VIEW FUNC
            if metadata_only:
                if is_factory and is_output:
                    # 对于empty_like方法，为特例，由于其并未初始化，因此未涉及到写内存
                    if "empty_like" in func.__name__:
                        return
                    is_write = True
                else:
                    # VIEW FUNC不访问显存
                    return

            # 计算tensor大小
            tensor_size = calculate_tensor_size(value)
            mstx.mark(
                f"memscope-aten-ac:ptr={data_ptr};is_write={is_write};is_read={is_read};is_output={is_output};"
                f"name={func.__module__}.{func.__name__};shape={value.shape};"
                f"dtype={value.dtype};tensor_size={tensor_size};device={value.device}",
                None,
            )

        if isinstance(value, (list, tuple)):
            for t in value:
                if is_npu_tensor(t):
                    _handle_tensor(func, t, is_write, is_factory, metadata_only, is_output)
            return

        if is_npu_tensor(value):
            _handle_tensor(func, value, is_write, is_factory, metadata_only, is_output)
            return

    def parse_inputs(
        self,
        func,
        args: tuple,
        kwargs: dict,
        *,
        is_factory: bool,
    ) -> None:
        schema = func._schema
        for argument, value in zip_arguments(schema, args, kwargs):
            alias_info = argument.alias_info
            is_write = alias_info is not None and alias_info.is_write
            is_metadata_only = is_factory or (alias_info is not None and not alias_info.is_write)
            handle_arg = functools.partial(
                self._handle_argument,
                is_write=is_write,
                is_factory=is_factory,
                is_output=False,
                metadata_only=is_metadata_only,
            )
            pytree.tree_map_(handle_arg, func, value)

    def parse_outputs(self, func, outputs: Any, *, is_factory: bool) -> None:
        schema = func._schema
        for res, value in zip(schema.returns, (outputs,)):
            alias_info = res.alias_info
            is_metadata_only = is_factory or (alias_info is not None and not alias_info.is_write)
            handle_arg = functools.partial(
                self._handle_argument,
                is_write=not is_metadata_only,
                is_factory=is_factory,
                is_output=True,
                metadata_only=is_metadata_only,
            )
            pytree.tree_map_(handle_arg, func, value)


class MemoryDispatchMode(TorchDispatchMode):
    def __torch_dispatch__(self, func, types, args=(), kwargs=None):
        if kwargs is None:
            kwargs = {}

        # 匹配工厂函数
        is_factory = func._schema.name.startswith("new_") or func._schema.name.endswith("_like")
        # 获取aten算子执行开始事件
        mstx.mark(f"memscope-aten-b: name={func.__module__}.{func.__name__}", None)

        argument_handler = ArgumentHandler()
        argument_handler.parse_inputs(func, args, kwargs, is_factory=is_factory)
        # 算子执行
        outputs = func(*args, **kwargs)

        argument_handler.parse_outputs(func, outputs, is_factory=is_factory)
        # 获取aten算子执行结束事件
        mstx.mark(f"memscope-aten-e: name={func.__module__}.{func.__name__}", None)

        return outputs


class AtenCollector:
    def __init__(self) -> None:
        self.dispatch = MemoryDispatchMode()
        self.enabled = False
        self._context = ExitStack()

    def __del__(self):
        if (sys is not None) and (not sys.is_finalizing()) and self.enabled:
            self.disable()

    def enable(self):
        if self.enabled:
            return
        self._context.enter_context(self.dispatch)
        self.enabled = True

    def disable(self):
        if not self.enabled:
            return
        self._context.close()
        self.enabled = False


if is_version_support:
    aten_collector = AtenCollector()

    def enable_aten_collector():
        aten_collector.enable()

    def disable_aten_collector():
        aten_collector.disable()
