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
import sqlite3

from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.file_system import WorkingDir
from ..utils.utils import ColorText


class MultirankConfig:
    """多Rank测试配置常量，集中管理便于维护"""
    # 文件生成数量配置
    FILE_GEN_COUNT_CSV_DB = 3
    FILE_GEN_COUNT_LOG = 1
    # CSV列名配置
    CSV_COLUMNS = (
        "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,"
        "Ptr,Attr,Call Stack(Python),Call Stack(C)"
    )
    # 日志校验阈值
    LOG_THRESHOLDS = {
        "LEAK_COUNT": 20,
        "MSTX_START_COUNT": 4,
        "STEP_INNER_COUNT": 100
    }
    # 数据校验阈值（按模式区分）
    DATA_THRESHOLDS = {
        "multirank_cmd_test": {
            "system_num": 6,
            "mstx_num": 202,
            "op_threshold": {"min": 16000, "max": 16500},
            "kernel_threshold": {"min": 16000, "max": 16500},
            "hal_threshold": {"min": 200, "max": 360},
            "pta_threshold": {"min": 9200, "max": 9300}
        },
        "default": {  # API模式
            "system_num": 9,
            "mstx_num": 202,
            "op_threshold": {"min": 16000, "max": 16500},
            "kernel_threshold": {"min": 16000, "max": 16500},
            "hal_threshold": {"min": 200, "max": 360},
            "pta_threshold": {"min": 8700, "max": 8800}
        }
    }
    # Torch版本阈值
    TORCH_VERSION_THRESHOLD = "2.3.0"
    TORCH_DATA_LENGTH_THRESHOLD = {"min": 15000, "max": 17000}

class MultirankTestSuite(TestSuite):
    """多Rank测试套件基类，抽离公共逻辑"""
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time, test_case_name: str):
        super().__init__(name, config, work_path, cmd, max_time)
        # 子类只需传入不同的test_case_name即可
        test_cases = [
            MultirankTestCase(test_case_name, name, work_path, ""),
            #MultirankTestCase("check_log", work_path, ""),
            #MultirankAnaLyzerModuleTestCase("check_npu_leaks", work_path, ""),
            #MultirankAnaLyzerModuleTestCase("check_leaks_warning", work_path, ""),
            #MultirankAnaLyzerModuleTestCase("check_gap_analysis", work_path, ""),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return f"msmemscope test suite. suite name: {self.name}, " \
               f"suite work path: {self._work_path}"

    def set_up(self):
        super().set_up()
        # 确保工作目录存在
        os.makedirs(self._work_path, exist_ok=True)
        
        # 清理旧文件
        with WorkingDir(self._work_path):
            if os.path.exists('memscopeDumpResults'):
                shutil.rmtree('memscopeDumpResults')

            for suffix in ['.log', '.json']:
                for file in os.listdir("."):
                    if file.endswith(suffix):
                        os.remove(file)

    def tear_down(self):
        super().tear_down()

class MultirankCsvTestSuite(MultirankTestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time, "check_dump_csv")

class MultirankDbTestSuite(MultirankTestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name, config, work_path, cmd, max_time, "check_dump_db")


class MultirankTestCase(BaseTest):
    def __init__(self, name: str, case_name: str, real_path: str, golden_path: str):
        super().__init__(name)
        self.case_name = case_name
        self._golden_path = golden_path
        self._real_path = real_path
        # 获取当前模式的阈值配置
        self.thresholds = MultirankConfig.DATA_THRESHOLDS.get(
            self.case_name, MultirankConfig.DATA_THRESHOLDS["default"]
        )

    def __str__(self):
        return f"case name: {self.name}, case path: {self._real_path}"

    def _check_event_count(self, event_counts, event_name, expected_value, file_path):
        """Event计数校验方法"""
        if event_counts.get(event_name, None) is None:
            logging.error(f"{event_name} key not found in Event_counts (file: {file_path})")
            return Result(False, [f"{event_name} key not found", -1], [-1])
        
        if event_counts[event_name] != expected_value:
            logging.error(f"{event_name} count error (file: {file_path}). Expected: {expected_value}, Actual: {event_counts[event_name]}")
            return Result(False, [f"{event_name}: ", expected_value], [event_counts[event_name]])
        return Result(True, [], [])

    def _check_threshold_range(self, count, threshold, name, file_path):
        """范围阈值校验方法"""
        if count < threshold["min"] or count > threshold["max"]:
            logging.error(f"{name} count out of range (file: {file_path}). Min: {threshold['min']}, Max: {threshold['max']}, Actual: {count}")
            return Result(False, [f"{name} min: ", threshold['min'], f"{name} max: ", threshold['max']], [count])
        return Result(True, [], [])

    def _validate_data_frame(self, df, column, file_paths):
        """校验DataFrame的列和长度"""
        # 校验列名
        if list(df.columns) != column.split(','):
            logging.error(f"Column mismatch (files: {file_paths}). Expected: {column}, Actual: {df.columns}")
            return Result(False, [column], [df.columns])
        
        # 校验torch版本对应的长度阈值
        if version.parse(torch.__version__) < version.parse(MultirankConfig.TORCH_VERSION_THRESHOLD):
            min_th = MultirankConfig.TORCH_DATA_LENGTH_THRESHOLD["min"]
            max_th = MultirankConfig.TORCH_DATA_LENGTH_THRESHOLD["max"]
            if len(df) < min_th or len(df) > max_th:
                logging.error(f"Data length error (files: {file_paths}). Min: {min_th}, Max: {max_th}, Actual: {len(df)}")
                return Result(False, ["min: ", min_th, "max: ", max_th], [len(df)])
        return Result(True, [], [])

    # 公共文件查找方法
    def _find_files(self, dir_path, file_patterns):
        """通用文件查找方法：递归查找符合条件的文件"""
        file_names = []
        file_paths = []
        if not os.path.exists(dir_path):
            logging.error(f"Directory {dir_path} not exist")
            return file_names, file_paths
        
        for root, dirs, files in os.walk(dir_path):
            for file in files:
                if all(pattern(file) for pattern in file_patterns):
                    full_path = os.path.join(root, file)
                    file_names.append(file)
                    file_paths.append(full_path)
        return file_names, file_paths

    # CSV/DB核心校验逻辑
    def comp_memscope_contents(self, file_paths, is_db=False):
        """统一的CSV/DB内容校验方法"""
        # 处理CSV/DB文件读取
        dfs = []
        for file in file_paths:
            try:
                if is_db:
                    # DB文件处理
                    conn = sqlite3.connect(file)
                    df = pd.read_sql_query("SELECT * FROM memscope_dump", conn)
                    conn.close()
                    if df.empty:
                        logging.error(f"SQLite file {file} has no data in memscope_dump table")
                        return Result(False, ["Non-empty data expected"], ["Empty table"])
                else:
                    # CSV文件处理
                    df = pd.read_csv(file)
                dfs.append(df)
            except Exception as e:
                logging.error(f"Error reading {file}: {str(e)}")
                return Result(False, [f"Read error: {str(e)}"], [])
        
        # 处理空数据情况（修复原代码潜在bug）
        if not dfs:
            logging.error(f"No valid data found in files: {file_paths}")
            return Result(False, ["Valid data expected"], ["No data"])
        
        # 合并数据
        data = pd.concat(dfs, ignore_index=True)

        if not is_db:
            # 校验列和长度
            validate_result = self._validate_data_frame(data, MultirankConfig.CSV_COLUMNS, file_paths)
            if not validate_result.success:
                return validate_result


        # 开始事件计数校验
        event_counts = data['Event'].value_counts()
        event_type_counts = data['Event Type'].value_counts()
        
        # 逐个校验事件类型
        check_items = [
            ("SYSTEM", self.thresholds["system_num"], event_counts),
            ("MSTX", self.thresholds["mstx_num"], event_counts),
        ]
        for event_name, expected, counts in check_items:
            result = self._check_event_count(counts, event_name, expected, file_paths)
            if not result.success:
                return result
        
        # 范围阈值校验
        range_check_items = [
            ("KERNEL_LAUNCH", self.thresholds["kernel_threshold"], event_counts),
            ("HAL", self.thresholds["hal_threshold"], event_type_counts),
            ("PTA", self.thresholds["pta_threshold"], event_type_counts),
            ("OP_LAUNCH", self.thresholds["op_threshold"], event_counts),
        ]
        for name, threshold, counts in range_check_items:
            count = counts.get(name, 0)
            result = self._check_threshold_range(count, threshold, name, file_paths)
            if not result.success:
                return result

        return Result(True, [], [])

    def comp_memscope_csv_contents(self, file_paths, column):
        return self.comp_memscope_contents(file_paths, is_db=False)

    def comp_memscope_db_contents(self, file_paths):
        return self.comp_memscope_contents(file_paths, is_db=True)

    @staticmethod
    def count_substring(data, phase, name) -> int:
        count = 0
        for entry in data:
            if entry.get("ph") == phase and name in entry.get("name"):
                count += 1
        return count

    def compare_log(self):
        logging.info("checking log...")
        FILE_GEN_COUNT = 1
        FILE_GEN_DIR = self._real_path
        LEAK_COUNT, MSTX_START_COUNT, STEP_INNER_COUNT = 20, 4, 100
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

            if leak_count < LEAK_COUNT:
                logging.error("msmemscope detect failed")
                return Result(False, [LEAK_COUNT], [leak_count])
            mstx_start_count = file_text.count("mstxMarkA")
            step_inner_count = file_text.count("step start") 
            if mstx_start_count != MSTX_START_COUNT or step_inner_count != STEP_INNER_COUNT:
                logging.error("mstx detect failed")
                return Result(False, ["mstx_start_count", MSTX_START_COUNT, "step_inner_count", STEP_INNER_COUNT], [mstx_start_count, step_inner_count])

        logging.info("check finish")
        return Result(True, [], [])

    def compare_memscope_csv(self):
        logging.info("checking csv...")
        FILE_GEN_DIR = os.path.join(self._real_path, 'memscopeDumpResults')
        
        # 查找CSV文件
        file_patterns = [lambda f: f.endswith('.csv'), lambda f: f.startswith('memscope_dump')]
        csv_files, csv_file_paths = self._find_files(FILE_GEN_DIR, file_patterns)
        
        # 校验文件数量
        if len(csv_files) != MultirankConfig.FILE_GEN_COUNT_CSV_DB:
            logging.error(f"Failed to generate {MultirankConfig.FILE_GEN_COUNT_CSV_DB} CSV files. Actual: {len(csv_files)}")
            return Result(False, [MultirankConfig.FILE_GEN_COUNT_CSV_DB], [len(csv_files)])
        
        # 校验文件名格式
        for csv_file in csv_files:
            if not re.match('memscope_dump_\d{1,20}\.csv', csv_file):
                logging.error(f"CSV file name {csv_file} does not match convention")
                return Result(False, [], [])

        # 调用统一校验方法
        result = self.comp_memscope_csv_contents(csv_file_paths, MultirankConfig.CSV_COLUMNS)
        if not result.success:
            return result
        
        logging.info("check finish")
        return Result(True, [], [])

    def compare_memscope_db(self):
        logging.info("checking db...")
        FILE_GEN_DIR = os.path.join(self._real_path, 'memscopeDumpResults')
        
        # 查找DB文件
        file_patterns = [lambda f: f.endswith('.db'), lambda f: f.startswith('memscope_dump')]
        db_files, db_file_paths = self._find_files(FILE_GEN_DIR, file_patterns)
        
        # 校验文件数量
        if len(db_files) != MultirankConfig.FILE_GEN_COUNT_CSV_DB:
            logging.error(f"Failed to generate {MultirankConfig.FILE_GEN_COUNT_CSV_DB} DB files. Actual: {len(db_files)}")
            return Result(False, [MultirankConfig.FILE_GEN_COUNT_CSV_DB], [len(db_files)])
        
        # 校验文件名格式
        for db_file in db_files:
            if not re.match('memscope_dump_\d{1,20}\.db', db_file):
                logging.error(f"DB file name {db_file} does not match convention")
                return Result(False, [], [])

        # 调用统一校验方法
        result = self.comp_memscope_db_contents(db_file_paths)
        if not result.success:
            return result
        
        logging.info("check finish")
        return Result(True, [], [])

    def run(self) -> Result:
        """简化的run方法"""
        super().run()
        logging.debug(f"run {self}")
        print(f"{ColorText.run_test} {self}")

        result_map = {
            "check_log": self.compare_log,
            "check_dump_csv": self.compare_memscope_csv,
            "check_dump_db": self.compare_memscope_db
        }
        # 执行对应校验方法
        result_func = result_map.get(self._name, lambda: Result(False, [], []))
        result = result_func()
        
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()

class MultirankAnaLyzerModuleTestCase(BaseTest):
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
        NPU_LEAK_COUNT = 48
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
        NPU_LEAK_WARNING_COUNT = 2300
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
            npu_leak_count = file_text.count("Please check if there is leaks in Pytorch Caching memory pool.")

            if npu_leak_count <= NPU_LEAK_WARNING_COUNT:
                logging.error("leaks warning detect failed")
                return Result(False, [NPU_LEAK_WARNING_COUNT], [npu_leak_count])

        logging.info("check finish")
        return Result(True, [], [])
    
    def check_gap_analysis(self):
        logging.info("checking gap analysis...")
        FILE_GEN_DIR = self._real_path
        DEVICE_COUNT = 2
        MIN_GAP_STEP = 2
        MAX_GAP_STEP_DEVICE_0 = 3
        MAX_GAP_STEP_DEVICE_1 = 49
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

                        if alloc_percent > 50.0:
                            if max_gap_step != MAX_GAP_STEP_DEVICE_1:
                                logging.error("Device1 MaxGap step Error.")
                                return Result(False, [MAX_GAP_STEP_DEVICE_1], [max_gap_step])
                        else:
                            if max_gap_step != MAX_GAP_STEP_DEVICE_0:
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