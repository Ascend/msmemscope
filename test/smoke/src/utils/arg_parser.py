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

from argparse import ArgumentParser


def create_arg_parser() -> ArgumentParser:
    parser = ArgumentParser(prog="msmemscope test framework",
                            description="test framework for msmemscope test cases")
    parser.add_argument("-f", "--filter",
                        help="filter test cases to run. regex is supported")
    parser.add_argument("-l", "--llama2_7b", action='store_true', help="enable modelLink test case")
    parser.add_argument("-s", "--select_steps_case", action='store_true', help="enable select steps test cases")
    return parser