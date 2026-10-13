# -------------------------------------------------------------------------
# This file is part of the MindStudio project.
# Copyright (c) 2026 Huawei Technologies Co.,Ltd.
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
from ._msmemscope import _record_function


class RecordFunction:
    """
    内存事件记录上下文管理器/装饰器:
    该类既可以通过Python上下文管理器协议,在代码块执行前后自动插入内存事件记录点,
    也可以作为装饰器使用，记录函数执行的内存事件。

    示例:
        作为上下文管理器:
        import msmemscope
        with msmemscope.RecordFunction("forward_pass"):
            output = model(input_data)

        作为装饰器:
        @msmemscope.RecordFunction("forward_pass")
        def forward_pass(data):
            return model(data)
    """

    def __init__(self, name: str):
        self.name = name

    def __enter__(self):
        _record_function.record_start(self.name)
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        _record_function.record_end(self.name)
        return False

    def __call__(self, func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            _record_function.record_start(self.name)

            try:
                result = func(*args, **kwargs)
                return result
            finally:
                _record_function.record_end(self.name)

        return wrapper
