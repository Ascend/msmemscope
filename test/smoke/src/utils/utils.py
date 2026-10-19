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


class ColorText:
    border = "\033[32m[----------]\033[0m"
    run_test = "\033[32m[ RUN      ]\033[0m"
    run_ok = "\033[32m[       OK ]\033[0m"
    run_failed = "\033[31m[  FAILED  ]\033[0m"
    run_warn = "\033[33m[   WARN   ]\033[0m"
    run_list = "\033[32m[   LIST   ]\033[0m"


class PrintBorder:
    def __init__(self, enter_text, exit_text=None):
        self._enter_text = enter_text
        self._exit_text = exit_text if exit_text is not None else enter_text

    def __enter__(self):
        print(f"{ColorText.border} {self._enter_text}")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        print(f"{ColorText.border} {self._exit_text}\n")
