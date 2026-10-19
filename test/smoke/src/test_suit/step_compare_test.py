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

from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.file_system import WorkingDir
from ..utils.utils import ColorText


class StepCompareTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            StepCompareTestCase("check_dump", work_path, ""),
            #StepCompareTestCase("check_log", work_path, ""),
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


class StepCompareTestCase(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"case name: {self.name}, " \
               f"case path: {self._real_path}"

    def compare_csv_contents(self, file):
        LINE_COUNT = 12
        first_column = ",,,Base,Compare\n"
        second_column = "Event,Name,Device Id,Allocated Memory(byte),Allocated Memory(byte),Diff Memory(byte)\n"
        with open(file, 'r') as f:
            first_line = f.readline()
            second_line = f.readline()

            if first_line != first_column:
                logging.error("the first column of %s is error", file)
                return Result(False, [first_column], [first_line])
            
            if second_line != second_column:
                logging.error("the second column of %s is error", file)
                return Result(False, [second_column], [second_line])

            line_count = len(f.readlines())
            if line_count != LINE_COUNT:
                logging.error("the length of %s is error", file)
                return Result(False, [LINE_COUNT], [line_count])
        return Result(True, [], [])

    def compare_log(self):
        logging.info("checking log...")
        FILE_GEN_COUNT = 1
        FILE_GEN_DIR = self._real_path
        if not os.path.exists(FILE_GEN_DIR):
            logging.error("directory %s not exist", FILE_GEN_DIR)
            return Result(False, [], [])
        new_log_files = [name for name in os.listdir(FILE_GEN_DIR) if name.endswith('.log') and name[0] == 'm']
    
        if len(new_log_files) != FILE_GEN_COUNT:
            logging.error("Failed to generate %d log files", FILE_GEN_COUNT)
            return Result(False, [FILE_GEN_COUNT], [len(new_log_files)])
 
        real_log_file = os.path.join(FILE_GEN_DIR, new_log_files[0])
        with open(real_log_file, 'r') as f:
 
            file_text = f.read()
            if "Start to compare memory data." not in file_text:
                logging.error("leaks step inter compare start detect failed!")
                return Result(False, [], ["Start to compare memory data."])
            
            if "The memory comparison has been completed in a total time of" not in file_text:
                logging.error("leaks step inter compare cost time detect failed!")
                return Result(False, [], ["The memory comparison has been completed in a total time of"])
 
        logging.info("check finish")
        return Result(True, [], [])

    def compare_step_difference_csv(self):

        FILE_GEN_COUNT = 1
        FILE_GEN_DIR = os.path.join(self._real_path, 'memscopeDumpResults')
        logging.info("checking compare csv...")
        if not os.path.exists(FILE_GEN_DIR):
            logging.error("directory %s not exist", FILE_GEN_DIR)
            return Result(False, [], [])
        
        # 递归查找所有子目录中的 CSV 文件,保存名称和位置
        new_csv_files_names = []
        new_csv_files_paths = []
        for root, dirs, files in os.walk(FILE_GEN_DIR):
            for file in files:
                if file.endswith('.csv') and file.startswith('memory_compare'):
                    # 构建完整文件路径并添加到列表
                    full_path = os.path.join(root, file)
                    new_csv_files_names.append(file)
                    new_csv_files_paths.append(full_path)

        if len(new_csv_files_names) != FILE_GEN_COUNT:
            logging.error("Failed to generate %d CSV files", FILE_GEN_COUNT)
            return Result(False, [FILE_GEN_COUNT], [len(new_csv_files_names)])
    
        for i in range(FILE_GEN_COUNT):
            if not re.match('memory_compare_\d{1,20}\.csv', new_csv_files_names[i]):
                logging.error("A CSV file matching naming convention compare could not be found")
                return Result(False, [], [])

            result = self.compare_csv_contents(new_csv_files_paths[i])
            if not result.success:
                return result
        logging.info("check finish")
        return Result(True, [], [])

    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")

        result = Result(False, [], [])
        if self._name == "check_log":
            result = self.compare_log()
        elif self._name == "check_dump":
            result = self.compare_step_difference_csv()
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()