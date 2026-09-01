#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

import logging
import os
import shutil
import re
import pandas as pd
from pathlib import Path

from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.file_system import WorkingDir
from ..utils.utils import  ColorText
from ..utils.symbol_checker import SymbolChecker


class HijackAtbTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            HijackAtbTestCase("check_dump", work_path, ".")
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


class HijackAtbTestCase(BaseTest):
    def __init__(self, name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"sample test case. case name: {self.name}, " \
               f"case path: {self._real_path}"

    def check_memscope_csv_contents(self, file):
        data = pd.read_csv(file)

        threshold = 11

        if len(data) < threshold:
            logging.error("the length of %s is error", file)
            return Result(False, [threshold], [len(data)])
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
        result = Result(False, [], [])

        # 需要适配新老CANN包的环境变量
        ascend_path_str = Path(os.getenv("ASCEND_HOME_PATH", "")).as_posix().rstrip("/")

        if ascend_path_str.endswith("/ascend-toolkit/latest"):
            env_path = os.getenv("ASCEND_HOME_PATH")[:-len("/ascend-toolkit/latest")]
            cann_version = "old"
        else :
            env_path = os.getenv("ASCEND_HOME_PATH")[:-len("/cann")]
            cann_version = "new"

        symbol_checker = SymbolChecker(env_path, cann_version)
        if not symbol_checker.right_symbol:
            return result

        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self.name}")

        if self._name == "check_dump":
            result = self.check_memscope_csv()
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()