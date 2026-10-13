#!/usr/bin/env python3
# -*- coding: utf-8 -*-
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
"""
analyzer 模块按需加载 helper(test/unit_python 内部公共函数, 非测试文件)

msmemscope 顶层 __init__.py 依赖 C++ 扩展(_msmemscope), 无法直接 import。
此处以独立 spec 逐个加载 python/msmemscope/analyzer 下的被测模块,
并预先把相对导入依赖(base/utility_function)以真实文件加载进 sys.modules,
确保 `from .xxx import ...` 正常解析。
"""

import importlib.util
import os
import sys
import types

_ANALYZER_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "python", "msmemscope", "analyzer")
_MSMEMSCOPE_DIR = os.path.dirname(_ANALYZER_DIR)


def _get_or_create_module(name):
    mod = sys.modules.get(name)
    if mod is None:
        mod = types.ModuleType(name)
        sys.modules[name] = mod
    return mod


def _load_submodule(module_name):
    """以独立模块加载 analyzer 子模块; 已加载(且文件一致)时直接复用"""
    full_name = "msmemscope.analyzer." + module_name
    existing = sys.modules.get(full_name)
    if existing is not None and getattr(existing, "__file__", None) == os.path.join(_ANALYZER_DIR, module_name + ".py"):
        return existing
    spec = importlib.util.spec_from_file_location(full_name, os.path.join(_ANALYZER_DIR, module_name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[full_name] = module
    spec.loader.exec_module(module)
    return module


def load_analyzer(module_name):
    """加载 msmemscope.analyzer.<module_name>, 返回其模块对象"""
    msmemscope_pkg = _get_or_create_module("msmemscope")
    msmemscope_pkg.__path__ = [_MSMEMSCOPE_DIR]
    analyzer_pkg = _get_or_create_module("msmemscope.analyzer")
    analyzer_pkg.__path__ = [_ANALYZER_DIR]
    # 预加载相对导入依赖(与执行顺序无关, 缓存后可直接解析 from .xxx import)
    _load_submodule("base")
    _load_submodule("utility_function")
    return _load_submodule(module_name)