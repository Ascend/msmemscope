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

import logging
import os
from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.utils import  ColorText


class SampleTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            SampleTestCase1("sample_test1", work_path, "."),
            SampleTestCase2("sample_test2", work_path, "."),
        ]
        _ = list(map(self.register, test_cases))
        '''
        self._requires = [DeviceCountRequire(threshold),
                            NotNone(lambda env: env.device_count),
                            GreaterEqualThan(lambda env: env.device_count, threshold)]
        '''
    def __str__(self):
        return f"msleaks test suite. suite name: {self.name}"

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()


class SampleTestCase1(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"sample test case. case name: {self.name}, " \
               f"case path: {self._real_path}"

    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")

        result = Result(False, [], [])

        #具体测试逻辑

        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()


class SampleTestCase2(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"sample test case. case name: {self.name}, " \
               f"case path: {self._real_path}"

    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")

        result = Result(False, [], [])

        #具体测试逻辑

        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()