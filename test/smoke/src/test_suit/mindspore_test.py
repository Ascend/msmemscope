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
import shutil
import re
import pandas as pd

from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.file_system import WorkingDir
from ..utils.utils import  ColorText


class HijackMindsporeTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            HijackMindsporeTestCase("check_dump", work_path, "."),
            #HijackMindsporeAnaLyzerModuleTestCase("check_npu_leaks", work_path, ""),
            #HijackMindsporeAnaLyzerModuleTestCase("check_leaks_warning", work_path, ""),
            #HijackMindsporeAnaLyzerModuleTestCase("check_gap_analysis", work_path, ""),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msmemscope test suite. suite name: {self.name}"

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
            json_files = [name for name in os.listdir(".") if name.endswith('.json')]
            for file in json_files:
                os.remove(file)

    def tear_down(self):
        super().tear_down()


class HijackMindsporeTestCase(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"sample test case. case name: {self.name}, " \
               f"case path: {self._real_path}"

    def check_memscope_csv_contents(self, file):
        data = pd.read_csv(file)

        threshold = 600

        if len(data) < threshold:
            logging.error("the length of %s is error", file)
            return Result(False, [threshold], [len(data)])
        return Result(True, [], [])

        Event_counts = data['Event'].value_counts()
        Event_type_counts = data['Event Type'].value_counts()

        if list(data.columns) != column.split(','):
            logging.error("the column of %s is error", file)
            return Result(False, [column], [data.columns])

        mstx_num = 10
        kernel_threshold = {"min":440, "max":465}
        hal_threshold = {"min":45, "max":60}
        ms_threshold = {"min":440, "max":465}


        if Event_counts['MSTX'] != mstx_num:
            logging.error("the length of %s is error", file)
            return Result(False, ["MSTX: ", mstx_num], [Event_counts['MSTX']])

        if Event_counts['KERNEL_LAUNCH'] < kernel_threshold['min'] or \
            Event_counts['KERNEL_LAUNCH'] > kernel_threshold['max']:
            logging.error("the length of %s is error", file)
            return Result(False, ["KERNEL_LAUNCH min: ", kernel_threshold['min'],
                "KERNEL_LAUNCH max: ", kernel_threshold['max']], [Event_counts['KERNEL_LAUNCH']])

        if Event_type_counts['HAL'] < hal_threshold['min'] or Event_type_counts['HAL'] > hal_threshold['max']:
            logging.error("the length of %s is error", file)
            return Result(False, ["HAL min: ", hal_threshold['min'], "HAL max: ", hal_threshold['max']],
                [Event_type_counts['HAL']])

        if Event_type_counts['MINDSPORE'] < ms_threshold['min'] or Event_type_counts['MINDSPORE'] > ms_threshold['max']:
            logging.error("the length of %s is error", file)
            return Result(False, ["MINDSPORE min: ", ms_threshold['min'], "MINDSPORE max: ", ms_threshold['max']],
                [Event_type_counts['MINDSPORE']])

        return Result(True, [], [])

    def check_memscope_csv(self):

        FILE_GEN_COUNT = 1
        FILE_GEN_DIR = os.path.join(self._real_path, 'memscopeDumpResults')
        logging.info("checking csv with hijack malloc and free...")
        if not os.path.exists(FILE_GEN_DIR):
            logging.error("directory %s not exist", FILE_GEN_DIR)
            return Result(False, [], [])
        

        # 递归查找所有子目录中的 CSV 文件,保存名称和位置
        new_csv_files_names = []
        new_csv_files_paths = []
        for root, dirs, files in os.walk(FILE_GEN_DIR):
            for file in files:
                if file.endswith('.csv') and file.startswith('memscope_dump'):
                    # 构建完整文件路径并添加到列表
                    full_path = os.path.join(root, file)
                    new_csv_files_names.append(file)
                    new_csv_files_paths.append(full_path)

        if len(new_csv_files_names) != FILE_GEN_COUNT:
            logging.error("Failed to generate %d CSV files", FILE_GEN_COUNT)
            return Result(False, [FILE_GEN_COUNT], [len(new_csv_files_names)])
    
        for i in range(FILE_GEN_COUNT):
            if not re.match('memscope_dump_\d{1,20}\.csv', new_csv_files_names[i]):
                logging.error("No csv file.")
                return Result(False, [], [])


            result = self.check_memscope_csv_contents(new_csv_files_paths[i])
            if not result.success:
                return result
        logging.info("check finish")
        return Result(True, [], [])

    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self.name}")

        result = Result(False, [], [])

        if self._name == "check_dump":
            result = self.check_memscope_csv()
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()


class HijackMindsporeAnaLyzerModuleTestCase(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"case name: {self.name}, " \
               f"case path: {self._real_path}"

    def check_npu_leaks(self):
        logging.info("checking npu leaks...")
        FILE_GEN_COUNT = 1
        FILE_GEN_DIR = self._real_path
        NPU_LEAK_COUNT = 4
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
            npu_leak_count = file_text.count("------leaks")

            if npu_leak_count != NPU_LEAK_COUNT:
                logging.error("npu leaks detect failed")
                return Result(False, [NPU_LEAK_COUNT], [npu_leak_count])

        logging.info("check finish")
        return Result(True, [], [])

    def check_leaks_warning(self):
        logging.info("checking leaks warning...")
        FILE_GEN_COUNT = 1
        FILE_GEN_DIR = self._real_path
        NPU_LEAK_WARNING_COUNT = 45
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
            npu_leak_count = file_text.count("Please check if there is leaks in Mindspore memory pool.")

            if npu_leak_count <= NPU_LEAK_WARNING_COUNT:
                logging.error("leaks warning detect failed")
                return Result(False, [NPU_LEAK_WARNING_COUNT], [npu_leak_count])

        logging.info("check finish")
        return Result(True, [], [])
    
    def check_gap_analysis(self):
        logging.info("checking gap analysis...")
        FILE_GEN_DIR = self._real_path
        DEVICE_COUNT = 1
        MIN_GAP_STEP = 2
        MAX_GAP_STEP_DEVICE_0 = 4
        if not os.path.exists(FILE_GEN_DIR):
            logging.error("directory %s not exist", FILE_GEN_DIR)
            return Result(False, [], [])
        new_output_files = [name for name in os.listdir(FILE_GEN_DIR) if name.endswith('.txt') and name[0] == 'o']

        real_output_file = os.path.join(FILE_GEN_DIR, new_output_files[0])
        device_count = 0
        with open(real_output_file, 'r') as f:
            for line in f:
                if line.startswith("MinGap"):
                    device_count += 1
                    parts = line.strip().split()
                    if parts:
                        min_gap_step = int(parts[-1])
                        
                        if min_gap_step != MIN_GAP_STEP:
                            logging.error("MinGap step Error.")
                            return Result(False, [MIN_GAP_STEP], [min_gap_step])
                    
                    else:
                        logging.error("GapAnalysis error .")
                        return Result(False, [], [])
                
                if line.startswith("MaxGap"):
                    parts = line.strip().split()
                    if parts:
                        max_gap_step = int(parts[-1])
                        alloc_percent = float(parts[1])

                        if max_gap_step < MAX_GAP_STEP_DEVICE_0:
                            logging.error("Device0 MaxGap step Error.")
                            return Result(False, [MAX_GAP_STEP_DEVICE_0], [max_gap_step])                            
                    
                    else:
                        logging.error("GapAnalysis error .")
                        return Result(False, [], [])
            
            if device_count != DEVICE_COUNT:
                logging.error("Device count error.")
                return Result(False, [DEVICE_COUNT], [device_count])
        
        return Result(True, [], [])

    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")
        
        #具体测试逻辑
        result = Result(False, [], [])
        if self._name == "check_npu_leaks":
            result = self.check_npu_leaks()
        if self._name == "check_leaks_warning":
            result = self.check_leaks_warning()
        if self._name == "check_gap_analysis":
            result = self.check_gap_analysis()
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()