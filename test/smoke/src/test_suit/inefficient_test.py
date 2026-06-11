#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

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


class InefficientTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time)
        # 传入name区分命令行和api,显存拆解样例只校验attr
        test_cases = [
            InefficientTestCase("check_dump", name, work_path, "")
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
            json_files = [name for name in os.listdir(".") if name.endswith('.json')]
            for file in json_files:
                os.remove(file)
    def tear_down(self):
        super().tear_down()


class InefficientTestCase(BaseTest):
    def __init__(self, name: str, case_name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self.case_name = case_name
        self._golden_path = golden_path
        self._real_path = real_path

    def __str__(self):
        return f"case name: {self.name}, " \
               f"case path: {self._real_path}"

    def comp_memscope_csv_contents(self, file_paths, column):
        # 由于多卡冒烟涉及多个文件，必须合并后统计
        dfs = []
        for file in file_paths:
            try:
                # 读取所有CSV文件,进行合并
                df = pd.read_csv(file)
                dfs.append(df)
            except Exception as e:
                logging.error(f"Error reading {file}: {str(e)}")
        
        # 合并所有 DataFrame（按行叠加）
        if dfs:
            data = pd.concat(dfs, ignore_index=True)
        
        # 校验条件 1、低效显存的个数是否符合
        ATTR_VALID_RULES = [
                "inefficient_type",                     
                "inefficient_type:early_allocation",
                "inefficient_type:late_deallocation",
                "inefficient_type:temporary_idleness"
            ]
        if self.case_name == "msmemscope_inefficient_cmd_test":
            ATTR_VALID_THRESHOLD = {
                "inefficient_type": {"min": 2928, "max": 2928},
                "inefficient_type:early_allocation": {"min": 1822, "max": 1822},
                "inefficient_type:late_deallocation": {"min": 1010, "max": 1010},
                "inefficient_type:temporary_idleness": {"min": 96, "max": 96},
            }
        else :
            ATTR_VALID_THRESHOLD = {
                "inefficient_type": {"min": 2728, "max": 2728},
                "inefficient_type:early_allocation": {"min": 1822, "max": 1822},
                "inefficient_type:late_deallocation": {"min": 810, "max": 810},
                "inefficient_type:temporary_idleness": {"min": 96, "max": 96},
            }

        if list(data.columns) != column.split(','):
            logging.error("the sum data column of %s is error, please check your dump file")
            return Result(False, [column], [data.columns])

        if 'Attr' not in data.columns:
            logging.error("ATTR column not found in data")
            return Result(False, ["ATTR column missing"], [])

        # pytorch版本小于2.3,无法进行低效显存判断,这里算通过,但是会提示warn
        if version.parse(torch.__version__) < version.parse("2.3.0"):
            logging.warning(f"PyTorch version {torch.__version__} is below minimum required version 2.3.0 for memory profiling")
            return Result(True, ["PyTorch version requirement not met", f"Required: >=2.3.0", f"Current: {torch.__version__}"], [])

        for check_element in ATTR_VALID_RULES:
            actual_count = data["Attr"].str.count(check_element).sum()
            if check_element in ATTR_VALID_THRESHOLD:
                lower_bound = ATTR_VALID_THRESHOLD[check_element]["min"]
                upper_bound = ATTR_VALID_THRESHOLD[check_element]["max"]
                if not (lower_bound <= actual_count <= upper_bound):
                    logging.error(f"{check_element} does not match the expectation, please check your dump file")
                    return Result(False, [f"{check_element} does not match the expectation", -1], [-1])
            else:
                logging.error(f"{check_element} does not exist, please check your dump file")
                return Result(False, [f"{check_element} does not exist", -1], [-1])

        return Result(True, [], [])

    def compare_memscope_csv(self):

        FILE_GEN_COUNT = 3
        FILE_GEN_DIR = os.path.join(self._real_path, 'memscopeDumpResults')
        logging.info("checking csv...")
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

        # 多卡冒烟当前落盘3个csv文件,先查看文件个数、格式是否满足
        if len(new_csv_files_names) != FILE_GEN_COUNT:
            logging.error("Failed to generate %d CSV files", FILE_GEN_COUNT)
            return Result(False, [FILE_GEN_COUNT], [len(new_csv_files_names)])
        
        for i in range(FILE_GEN_COUNT):
            if not re.match('memscope_dump_\d{1,20}\.csv', new_csv_files_names[i]):
                logging.error("A CSV file matching naming convention memscope_dump could not be found")
                return Result(False, [], [])

        column = ("ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,"
                "Ptr,Attr,Call Stack(Python),Call Stack(C)")
        
        result = self.comp_memscope_csv_contents(new_csv_files_paths, column)
        if not result.success:
            return result
        logging.info("check finish")
        return Result(True, [], [])

    def run(self) -> Result:
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")

        result = Result(False, [], [])
        if self._name == "check_dump":
            result = self.compare_memscope_csv()
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()