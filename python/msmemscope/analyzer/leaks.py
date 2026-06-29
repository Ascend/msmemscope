# -------------------------------------------------------------------------
# This file is part of the MindStudio project.
# Copyright (c) 2025 Huawei Technologies Co.,Ltd.
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

import csv
import re
from collections import defaultdict
from pathlib import Path
from typing import Dict, List
from .base import BaseAnalyzer, AnalysisConfig
from .utility_function import safe_convert_int

BYTE_TO_MB = 1024 * 1024


class LeaksConfig(AnalysisConfig):
    def __init__(self, input_path: str, mstx_info: str, start_index: int):
        super().__init__(input_path=input_path)

        self.mstx_info = mstx_info
        self.start_index = start_index
        self.__post_init__()

    def __post_init__(self):
        super().__post_init__()
        # leaks_checker 功能只支持csv 这里需要额外校验
        path = Path(self.input_path)
        file_ext = path.suffix.lower()
        if file_ext != '.csv':
            raise ValueError(f"Unsupported file type: {file_ext}. Only files ending with .csv are supported.")
        # 校验成员函数
        if not isinstance(self.mstx_info, str):
            raise TypeError("mstx_info must be a string")
        if self.mstx_info.strip() == "":
            raise ValueError("mstx_info must not be empty or blank")
        if not isinstance(self.start_index, int) or self.start_index < 0:
            raise ValueError("start_index must be a non-negative integer")


class LeaksAnalyzer(BaseAnalyzer):
    def __init__(self):
        self.device_events: Dict[str, List[Dict[str, str]]] = defaultdict(list)
        self.leaks: List[str] = []
        self.config: LeaksConfig
        self.error: str

    def analyze(self, config: LeaksConfig):
        self.config = config
        if not self.read_file():
            if self.error:
                print("ERROR: Initialization failed:", self.error)
                return
        self.analyze_leaks()
        self.print_leaks_result()

    def read_file(self):
        """安全读取CSV文件"""
        try:
            with open(self.config.input_path, 'r', encoding='utf-8') as f:
                reader = csv.DictReader(f)
                for row in reader:
                    device_id = row['Device Id']
                    # 读取CSV文件按照设备号进行分组
                    self.device_events[device_id].append(row)
            return True
        except FileNotFoundError:
            self.error = "ERROR: File not found during access"
            return False
        except PermissionError:
            self.error = "ERROR: Permission denied during file access"
            return False
        except csv.Error as e:
            self.error = f"ERROR: CSV parsing error - {e}"
            return False
        except Exception as e:
            self.error = f"ERROR: Unexpected error - {e}"
            return False

    def analyze_leaks(self):
        """
        对设备进行泄漏分析
        :param mstx_info: MSTX事件筛选条件,由用户指定
        :param start_index: 起始打点索引
        :return: 泄漏告警列表
        """
        for device_id, events in self.device_events.items():
            if device_id == 'N/A':
                continue

            print(f"INFO: Starting to analyze data for device {device_id}...")
            # 按ID排序,需转为int
            events.sort(key=lambda x: safe_convert_int(x['ID']))

            # 查找指定内容的MSTX事件
            defined_mark_events = self._find_mstx_events(events)

            # 跟踪内存分配，allocation中维护申请未释放的内存
            allocations = {}

            if len(defined_mark_events) < 3:
                print(f"ERROR: Device {device_id} has fewer than 3 data points. Please re-collect the data points.")
                continue

            # 获取三个打点位置的事件
            try:
                point_a = defined_mark_events[self.config.start_index]
                point_b = defined_mark_events[self.config.start_index + 1]
                point_c = defined_mark_events[self.config.start_index + 2]
            except IndexError:
                print(
                    f"INFO: Start index {self.config.start_index} is out of bounds."
                    f" Device {device_id} has a total of {len(defined_mark_events)} MSTX events."
                )
                continue

            # 获取三个打点ID
            section1_start = safe_convert_int(point_a['ID'])
            section1_end = safe_convert_int(point_b['ID'])
            section2_end = safe_convert_int(point_c['ID'])

            # 分析内存泄漏
            for event in events:
                self._analyze_event(section1_start, section1_end, section2_end, allocations, event)

            if allocations:
                self.leaks.append(f"====== ERROR: Detected memory leaks on device {device_id} ======")
                for index, info in list(allocations.items()):
                    self.leaks.append(
                        f"Direct Hal memory leak of {info['size']} Mb(s) at {info['addr']} in Index {index}."
                    )
            else:
                self.leaks.append(f"No hal memory leaks detected on device {device_id}.")

    def _find_mstx_events(self, events):
        """查找符合条件的MSTX MARK事件"""
        defined_mark_events = []
        for event in events:
            if (
                event.get('Event') == 'MSTX'
                and event.get('Event Type') == 'Mark'
                and event.get('Name') == self.config.mstx_info
            ):
                defined_mark_events.append(event)

        return defined_mark_events

    def _analyze_event(self, section1_start, section1_end, section2_end, allocations, event):
        # 开放态上只处理Hal的泄漏
        if event.get('Event Type') != 'HAL':
            return

        event_id = event.get('ID')
        # 超出第三个打点位置的直接跳过
        if safe_convert_int(event_id) > section2_end:
            return

        # 记录MALLOC事件
        if section1_start <= safe_convert_int(event_id) <= section1_end and event.get('Event') == 'MALLOC':
            alloc_addr = event.get('Ptr', '')
            match = re.search(r'size:(\d+)', event.get('Attr', ''))  # 匹配 "size:数字"
            if match is None:
                raise ValueError("input data may broken")
            alloc_size = safe_convert_int(match.group(1)) / BYTE_TO_MB  # 提取数字并转为整数
            allocations[event_id] = {"addr": alloc_addr, "size": alloc_size}

        # 处理FREE事件
        if section1_start <= safe_convert_int(event_id) and event.get('Event') == 'FREE':
            free_addr = event.get('Ptr')
            # 不考虑double free
            for index, info in list(allocations.items()):
                if free_addr == info['addr']:
                    del allocations[index]

    def print_leaks_result(self):
        if self.leaks:
            print("\nLeak Analysis Results for the Custom Range:")
            for alert in self.leaks:
                print(f"{alert}")


# 对外暴露的便捷泄漏识别函数
def check_leaks(input_path: str, mstx_info: str, start_index: int):
    config = LeaksConfig(
        input_path=input_path,
        mstx_info=mstx_info,
        start_index=start_index,
    )

    return LeaksAnalyzer().analyze(config)
