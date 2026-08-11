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

from functools import wraps
from ._msmemscope import _describer

# 标签级别常量(与 C++ OwnerLevel 枚举对齐, 见 csrc/analysis/event.h):
# 分级模型 框架@组件@流程@细化标签, 各级允许省略/多个(按序拼接)
FRAMEWORK = 0
COMPONENT = 1
PROCESS = 2
DETAIL_1 = 3
DETAIL_2 = 4
USER_DEFINED_1 = 5
USER_DEFINED_2 = 6
USER_DEFINED_3 = 7


# ---------------------------------------------------------------------------
# 内部接口(带级别): 供框架钩子(hijack_map/taggers/optimizer_step_hook)使用
# ---------------------------------------------------------------------------
def describe_label(label: str, level: int) -> None:
    """
    内部接口: 上报系统标签(带级别), 存放于 DescribeTrace 数据区1对应级别的标签栈。
    同级别同标签嵌套由 C++ 侧计数管理, 同级别不同标签嵌套输出取最新。
    """
    _describer.describe_label(label, level)


def undescribe_label(label: str, level: int) -> None:
    """内部接口: 撤销系统标签(带级别), 计数归零时出栈"""
    _describer.undescribe_label(label, level)


def describe_addr(addr: int, labels: list) -> None:
    """
    内部接口: 地址直标(一次携带多级标签), labels 为 [(label:str, level:int), ...]。
    直接更新目标内存块的 owner, 同级别重复时以地址标签为准。
    """
    _describer.describe_addr(addr, labels)


# ---------------------------------------------------------------------------
# 用户接口(无级别): 用户范围标签存放于 DescribeTrace 数据区2(栈序映射
# USER_DEFINED_1..3, 超出3个静默丢弃); 用户地址直标默认落在 USER_DEFINED_1 槽
# ---------------------------------------------------------------------------
class Describe:
    def __call__(self, obj=None, owner=''):
        if obj is not None:
            if isinstance(obj, int):
                address = obj
            else:
                address = self._get_address(obj)
            _describer.describe_addr(address, [(owner, USER_DEFINED_1)])
            return None
        else:
            return DescribeContext(owner)

    @staticmethod
    def _get_address(tensor):
        try:
            return tensor.data_ptr()
        except AttributeError as error:
            raise NotImplementedError("Unsupported tensor type") from error


class DescribeContext:
    def __init__(self, owner):
        self.owner = owner

    def __enter__(self):
        _describer.describe(self.owner)
        return self

    def __exit__(self, exc_type=None, exc_val=None, exc_tb=None):
        _describer.undescribe(self.owner)

    def __call__(self, func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            with self:
                return func(*args, **kwargs)

        return wrapper


describer = Describe()
