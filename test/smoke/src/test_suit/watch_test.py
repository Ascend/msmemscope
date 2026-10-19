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

import os
import shutil
import logging
import re
import pandas as pd
from packaging import version
import torch

from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.file_system import WorkingDir
from ..utils.utils import ColorText

WATCH_ATB_HASH_DUMP_NUMS = 2
WATCH_ATB_BIN_DUMP_NUMS = 2
WATCH_ATB_PYTHON_DUMP_NUMS = 0
WATCH_ATEN_HASH_DUMP_NUMS = 55
WATCH_ATEN_BIN_DUMP_NUMS = 55
WATCH_ATEN_PYTHON_DUMP_NUMS = 10

class WatchATBHashTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            WatchATBHashTestCase("check_dump", work_path, ""),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msmemscope test suite. suite name: {self.name}, " \
               f"suite work path: {self._work_path}"

    def set_up(self):
        super().set_up()

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            if os.path.exists('memscopeDumpResults'):
                shutil.rmtree('memscopeDumpResults')
            log_files = [name for name in os.listdir(".") if name.endswith('.log')]
            for file in log_files:
                os.remove(file)

    def tear_down(self):
        super().tear_down()


class WatchATBBinTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            WatchATBBinTestCase("check_dump", work_path, ""),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msmemscope test suite. suite name: {self.name}, " \
               f"suite work path: {self._work_path}"

    def set_up(self):
        super().set_up()

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            if os.path.exists('memscopeDumpResults'):
                shutil.rmtree('memscopeDumpResults')
            log_files = [name for name in os.listdir(".") if name.endswith('.log')]
            for file in log_files:
                os.remove(file)

    def tear_down(self):
        super().tear_down()


class WatchATENHashTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            WatchATENHashTestCase("check_dump", work_path, ""),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msmemscope test suite. suite name: {self.name}, " \
               f"suite work path: {self._work_path}"

    def set_up(self):
        super().set_up()

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            if os.path.exists('memscopeDumpResults'):
                shutil.rmtree('memscopeDumpResults')
            log_files = [name for name in os.listdir(".") if name.endswith('.log')]
            for file in log_files:
                os.remove(file)

    def tear_down(self):
        super().tear_down()


class WatchATENBinTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            WatchATENBinTestCase("check_dump", work_path, ""),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msmemscope test suite. suite name: {self.name}, " \
               f"suite work path: {self._work_path}"

    def set_up(self):
        super().set_up()

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            if os.path.exists('memscopeDumpResults'):
                shutil.rmtree('memscopeDumpResults')
            log_files = [name for name in os.listdir(".") if name.endswith('.log')]
            for file in log_files:
                os.remove(file)
    
    def tear_down(self):
        super().tear_down()


def check_watch_dump_nums(file, column, nums, python_nums):
    data = pd.read_csv(file)
    if list(data.columns) != column.split(','):
        logging.error("the column of %s is error", file)
        return Result(False, [column], [data.columns])

    if len(data) != nums:
        logging.error("the length of %s is error", file)
        return Result(False, nums, [len(data)])

    python_count = data['Tensor info'].str.contains('memscopeStWatch').sum()
    if python_count != python_nums:
        logging.error("the python watch length of %s is error", file)
        return Result(False, python_nums, [python_count])

    return Result(True, [], [])


def compare_watch_dump_csv(real_path, nums, python_nums):

    FILE_GEN_COUNT = 1
    FILE_GEN_DIR = os.path.join(real_path, 'memscopeDumpResults')
    logging.info("checking compare csv...")
    if not os.path.exists(FILE_GEN_DIR):
        logging.error("directory %s not exist", FILE_GEN_DIR)
        return Result(False, [], [])
    
    # 递归查找所有子目录中的 CSV 文件,保存名称和位置
    new_csv_files_names = []
    new_csv_files_paths = []
    for root, dirs, files in os.walk(FILE_GEN_DIR):
        for file in files:
            if file.endswith('.csv') and file.startswith('watch_dump_data_check_sum'):
                # 构建完整文件路径并添加到列表
                full_path = os.path.join(root, file)
                new_csv_files_names.append(file)
                new_csv_files_paths.append(full_path)

    if len(new_csv_files_names) != FILE_GEN_COUNT:
        logging.error("Failed to generate %d CSV files", FILE_GEN_COUNT)
        return Result(False, [FILE_GEN_COUNT], [len(new_csv_files_names)])

    for i in range(FILE_GEN_COUNT):
        if not re.match('watch_dump_data_check_sum_\d{1,20}\.csv', new_csv_files_names[i]):
            logging.error("A CSV file matching naming convention leaks could not be found")
            return Result(False, [], [])

        column = ("Tensor info,Check data sum")
        result = check_watch_dump_nums(new_csv_files_paths[i], column, nums, python_nums)
        
        if not result.success:
            return result
    logging.info("check finish")
    return Result(True, [], [])


def check_watch_dump_bin(real_path, nums, python_nums):

    FILE_GEN_DIR = os.path.join(real_path, 'memscopeDumpResults')
    logging.info("checking bin file...")
    if not os.path.exists(FILE_GEN_DIR):
        logging.error("directory %s not exist", FILE_GEN_DIR)
        return Result(False, [], [])

    # 递归查找所有子目录中的 bin 文件,保存名称和位置
    new_csv_files_names = []
    new_csv_files_paths = []
    for root, dirs, files in os.walk(FILE_GEN_DIR):
        for file in files:
            if file.endswith('.bin'):
                # 构建完整文件路径并添加到列表
                full_path = os.path.join(root, file)
                new_csv_files_names.append(file)
                new_csv_files_paths.append(full_path)

    if len(new_csv_files_names) != nums:
        logging.error("The number of generated bin files is incorrect.")
        return Result(False, [nums], [len(new_csv_files_names)])

    python_count = sum(1 for name in new_csv_files_names if 'memscopeStWatch' in name)
    if python_count != python_nums:
        logging.error("the python watch length of %s is error", file)
        return Result(False, python_nums, [python_count])
    
    logging.info("check finish")
    return Result(True, [], [])


class WatchATBHashTestCase(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"case name: {self.name}, " \
               f"case path: {self._real_path}"
    
    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")

        result = compare_watch_dump_csv(self._real_path, WATCH_ATB_HASH_DUMP_NUMS, WATCH_ATB_PYTHON_DUMP_NUMS)
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()


class WatchATBBinTestCase(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"case name: {self.name}, " \
               f"case path: {self._real_path}"
    
    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")

        result = check_watch_dump_bin(self._real_path, WATCH_ATB_BIN_DUMP_NUMS, WATCH_ATB_PYTHON_DUMP_NUMS)
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()


class WatchATENHashTestCase(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"case name: {self.name}, " \
               f"case path: {self._real_path}"
    
    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")

        result = compare_watch_dump_csv(self._real_path, WATCH_ATEN_HASH_DUMP_NUMS, WATCH_ATEN_PYTHON_DUMP_NUMS)
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()


class WatchATENBinTestCase(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"case name: {self.name}, " \
               f"case path: {self._real_path}"
    
    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")

        result = check_watch_dump_bin(self._real_path, WATCH_ATEN_BIN_DUMP_NUMS, WATCH_ATEN_PYTHON_DUMP_NUMS)
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()