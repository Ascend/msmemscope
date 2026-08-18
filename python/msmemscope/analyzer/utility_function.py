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

from typing import Any, Optional


def safe_convert_int(input_value: Any, context: str = "") -> Optional[int]:
    """将输入安全转换为 int。

    转换失败时不抛异常，输出 WARNING 并返回 None，由调用方决定是否跳过该条记录。
    :param input_value: 待转换的值
    :param context: 附加定位信息（如行号、设备号），可选
    :return: 转换后的 int；失败时为 None
    """
    try:
        return int(input_value)
    except (ValueError, TypeError):
        suffix = f" ({context})" if context else ""
        print(f"WARNING: Cannot convert {input_value!r} to int{suffix}, this record will be skipped")
        return None
