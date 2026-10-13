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
from typing import Optional, Any, TypeVar, Generic
import subprocess

from .singleton import Singleton
from .utils import PrintBorder, ColorText


class SymbolChecker(metaclass=Singleton):
    def __init__(self, path, cann_version):
        self._env_path = path
        self._cann_version = cann_version
        self._is_right_symbol = self._check()

    @property
    def right_symbol(self):
        return self._is_right_symbol

    @staticmethod
    def _check_symbols_with_nm(so_path, symbols):
        """
        使用nm命令检查so文件中的符号
        :param so_path: so文件路径
        :param symbols: 要检查的符号列表
        :return: 存在的符号列表
        """
        try:
            # 运行nm命令获取所有符号
            result = subprocess.run(['nm', '-D', so_path], 
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            
            if result.returncode != 0:
                print(f"Error running nm: {result.stderr}")
                return []
                
            # 检查每个符号是否存在
            existing_symbols = []
            nm_output = result.stdout
            for symbol in symbols:
                if symbol in nm_output:
                    existing_symbols.append(symbol)
            
            return existing_symbols
        except Exception as e:
            print(f"Error: {e}")
            return []

    def _check_symbol_in_abi_0_so(self):
        symbols_to_check = [
            "_ZN3atb6Runner7ExecuteERNS_17RunnerVariantPackE",
            "_ZN3atb9StoreUtil15SaveLaunchParamEPvRKN3Mki11LaunchParamERKSs",
            "_ZN3atb5Probe17IsSaveTensorAfterEv",
            "_ZN3atb5Probe18IsSaveTensorBeforeEv",
            "_ZN3atb5Probe16IsTensorNeedSaveERKSt6vectorIlSaIlEERKSs",
            "_ZN3atb5Probe21IsExecuteCountInRangeEm",
            "_ZN3atb5Probe16IsSaveTensorDescEv",
            "_ZNK3atb6Runner16GetOperationNameEv",
            "_ZNK3atb6Runner16GetSaveTensorDirEv",
            "_ZN3Mki11LaunchParam12GetInTensorsEv",
            "_ZN3Mki11LaunchParam13GetOutTensorsEv",
        ]

        if self._cann_version == "old":
            so_file = self._env_path + "/nnal/atb/latest/atb/cxx_abi_0/lib/libatb.so"
        else:
            so_file = self._env_path + "/nnal/atb/8.5.0/atb/cxx_abi_0/lib/libatb.so"

        found_symbols = self._check_symbols_with_nm(so_file, symbols_to_check)

        for symbol in symbols_to_check:
            if symbol not in found_symbols:
                print(f"{ColorText.run_failed} {symbol} not defined in {so_file}")
                return False

        return True
    
    def _check_symbol_in_abi_1_so(self):
        symbols_to_check = [
            "_ZN3atb6Runner7ExecuteERNS_17RunnerVariantPackE",
            "_ZN3atb9StoreUtil15SaveLaunchParamEPvRKN3Mki11LaunchParamERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
            "_ZN3atb5Probe17IsSaveTensorAfterEv",
            "_ZN3atb5Probe18IsSaveTensorBeforeEv",
            "_ZN3atb5Probe16IsTensorNeedSaveERKSt6vectorIlSaIlEERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
            "_ZN3atb5Probe21IsExecuteCountInRangeEm",
            "_ZN3atb5Probe16IsSaveTensorDescEv",
            "_ZNK3atb6Runner16GetOperationNameB5cxx11Ev",
            "_ZNK3atb6Runner16GetSaveTensorDirB5cxx11Ev",
            "_ZN3Mki11LaunchParam12GetInTensorsEv",
            "_ZN3Mki11LaunchParam13GetOutTensorsEv",
        ]

        if self._cann_version == "old":
            so_file = self._env_path + "/nnal/atb/latest/atb/cxx_abi_1/lib/libatb.so"
        else:
            so_file = self._env_path + "/nnal/atb/8.5.0/atb/cxx_abi_1/lib/libatb.so"

        found_symbols = self._check_symbols_with_nm(so_file, symbols_to_check)

        for symbol in symbols_to_check:
            if symbol not in found_symbols:
                print(f"{ColorText.run_failed} {symbol} not defined in {so_file}")
                return False

        return True

    @staticmethod
    def _check_code_snippet(header_path, snippet):
        """
        检查头文件中是否包含特定代码段（简单字符串匹配）
        :param header_path: 头文件路径
        :param snippet: 要查找的代码段
        :return: 是否找到
        """
        with open(header_path, 'r', encoding='utf-8') as f:
            content = f.read()
        return snippet in content

    def _check(self):
        with PrintBorder("check symbol in so", "check symbol in so done"):
            result = (self._check_symbol_in_abi_0_so() and self._check_symbol_in_abi_1_so())
            return result
