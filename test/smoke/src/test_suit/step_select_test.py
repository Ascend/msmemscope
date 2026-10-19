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
import shutil
import os
import re
import csv
import pandas
from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.utils import  ColorText
from ..utils.file_system import WorkingDir

SELECT_ONE_STEPS_RECORD_NUMS = 235
SELECT_FIRST_STEPS_RECORD_NUMS = 60
SELECT_LAST_STEPS_RECORD_NUMS = 44
SELECT_MULTI_STEPS_RECORD_NUMS = 132

class SelectOneStepTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            SelectOneStepTest("select_one_step", work_path, "."),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msleaks test suite. suite name: {self.name}"

    def set_up(self):
        super().set_up()

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            if os.path.exists('leaksDumpResults'):
                shutil.rmtree('leaksDumpResults')

        for filename in os.listdir(self._work_path):
            if filename.endswith('.log'):
                file_path = os.path.join(self._work_path, filename)
                os.remove(file_path)

    def tear_down(self):
        super().tear_down()

class SelectFirstStepTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            SelectFirstStepTest("select_first_step", work_path, "."),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msleaks test suite. suite name: {self.name}"

    def set_up(self):
        super().set_up()

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            if os.path.exists('leaksDumpResults'):
                shutil.rmtree('leaksDumpResults')

        for filename in os.listdir(self._work_path):
            if filename.endswith('.log'):
                file_path = os.path.join(self._work_path, filename)
                os.remove(file_path)

    def tear_down(self):
        super().tear_down()

class SelectLastStepTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            SelectLastStepTest("select_lastt_step", work_path, "."),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msleaks test suite. suite name: {self.name}"

    def set_up(self):
        super().set_up()

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            if os.path.exists('leaksDumpResults'):
                shutil.rmtree('leaksDumpResults')

        for filename in os.listdir(self._work_path):
            if filename.endswith('.log'):
                file_path = os.path.join(self._work_path, filename)
                os.remove(file_path)

    def tear_down(self):
        super().tear_down()

class SelectMultiStepTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            SelectMultiStepTest("select_multi_step", work_path, "."),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msleaks test suite. suite name: {self.name}"

    def set_up(self):
        super().set_up()

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            if os.path.exists('leaksDumpResults'):
                shutil.rmtree('leaksDumpResults')

        for filename in os.listdir(self._work_path):
            if filename.endswith('.log'):
                file_path = os.path.join(self._work_path, filename)
                os.remove(file_path)

    def tear_down(self):
        super().tear_down()

def check_dump_record_nums(file, column, nums):
    data = pandas.read_csv(file)
    if list(data.columns) != column.split(','):
        logging.error("the column of %s is error", file)
        return Result(False, [column], [data.columns])

    if len(data) != nums:
        logging.error("the length of %s is error", file)
        return Result(False, nums, [len(data)])
    return Result(True, [], [])

def check_select_steps_dump_record(real_path, nums):

    FILE_GEN_COUNT = 1
    FILE_GEN_DIR = os.path.join(real_path, 'leaksDumpResults/dump')
    logging.info("checking csv...")
    if not os.path.exists(FILE_GEN_DIR):
        logging.error("directory %s not exist", FILE_GEN_DIR)
        return Result(False, [], [])
    new_csv_files = [name for name in os.listdir(FILE_GEN_DIR) if name.endswith('.csv') and name.startswith('leaks')]

    if len(new_csv_files) != FILE_GEN_COUNT:
        logging.error("Failed to generate %d CSV files", FILE_GEN_COUNT)
        return Result(False, [FILE_GEN_COUNT], [len(new_csv_files)])

    for i in range(FILE_GEN_COUNT):
        if not re.match('leaks_dump_\d{1,20}\.csv', new_csv_files[i]):
            logging.error("A CSV file matching naming convention leaks could not be found")
            return Result(False, [], [])

        new_file = os.path.join(FILE_GEN_DIR, new_csv_files[i])
        column = ("Record Index,Timestamp(ns),Event,Event Type,Process Id,Thread Id,Device Id,"
                "Kernel Index,Flag,Addr,Size(byte),Total Allocated(byte),Total Reserved(byte)")
        result = check_dump_record_nums(new_file, column, nums)
        if not result.success:
            return result

    logging.info("check finish")
    return Result(True, [], [])

class SelectOneStepTest(BaseTest):
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

        return check_select_steps_dump_record(self._real_path, SELECT_ONE_STEPS_RECORD_NUMS)

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()

class SelectFirstStepTest(BaseTest):
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

        return check_select_steps_dump_record(self._real_path, SELECT_FIRST_STEPS_RECORD_NUMS)

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()

class SelectLastStepTest(BaseTest):
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

        return check_select_steps_dump_record(self._real_path, SELECT_LAST_STEPS_RECORD_NUMS)

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()

class SelectMultiStepTest(BaseTest):
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

        return check_select_steps_dump_record(self._real_path, SELECT_MULTI_STEPS_RECORD_NUMS)

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()