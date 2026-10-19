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


class Llama2_7bTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            Llama2_7bTestCase("check_dump", work_path, ""),
            Llama2_7bTestCase("check_log", work_path, ""),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msleaks test suite. suite name: {self.name}, " \
               f"suite work path: {self._work_path}"

    def set_up(self):
        super().set_up()

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            if os.path.exists('leaksDumpResults'):
                shutil.rmtree('leaksDumpResults')
            log_files = [name for name in os.listdir(".") if name.endswith('.log')]
            for file in log_files:
                os.remove(file)
    def tear_down(self):
        super().tear_down()


class Llama2_7bTestCase(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"case name: {self.name}, " \
               f"case path: {self._real_path}"

    def comp_leaks_csv_contents(self, file, column):
        data = pd.read_csv(file)
        if list(data.columns) != column.split(','):
            logging.error("the column of %s is error", file)
            return Result(False, [column], [data.columns])

        threshold = 300000

        if len(data) < threshold:
            logging.error("the length of %s is error", file)
            return Result(False, [threshold], [len(data)])
        return Result(True, [], [])

    def compare_log(self):
        logging.info("checking log...")
        FILE_GEN_COUNT = 1
        FILE_GEN_DIR = self._real_path
        LEAK_COUNT, MSTX_START_COUNT, STEP_INNER_COUNT = 36, 8, 20
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
            leak_count = file_text.count("Leak memory in Malloc operator")

            if leak_count != LEAK_COUNT:
                logging.error("msleaks detect failed")
                return Result(False, [LEAK_COUNT], [leak_count])
            mstx_start_count = file_text.count("mstxMarkA")
            step_inner_count = file_text.count("step start")
            if mstx_start_count != MSTX_START_COUNT or step_inner_count != STEP_INNER_COUNT:
                logging.error("mstx detect failed")
                return Result(False, ["mstx_start_count", MSTX_START_COUNT, "step_inner_count", STEP_INNER_COUNT], [mstx_start_count, step_inner_count])

        logging.info("check finish")
        return Result(True, [], [])

    def compare_leaks_csv(self):

        FILE_GEN_COUNT = 1
        FILE_GEN_DIR = os.path.join(self._real_path, 'leaksDumpResults/dump')
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
            result = self.comp_leaks_csv_contents(new_file, column)
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
            result = self.compare_leaks_csv()
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()