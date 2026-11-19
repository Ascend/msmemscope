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

from abc import ABC, abstractmethod
from pathlib import Path
import os

MAX_FILE_SIZE_GB = 8
MAX_FILE_SIZE_ENV = "MSMEMSCOPE_MAX_INPUT_FILE_SIZE_GB"
MAX_FILE_SIZE = MAX_FILE_SIZE_GB * 1024**3  # 默认最大文件大小8GiB
INPUT_STR_MAX_LEN = 4096  # 支持的最大文件路径长度


def _get_max_file_size_bytes() -> int:
    """读取环境变量配置的输入文件上限（字节），非法时回退默认值。"""
    value = os.environ.get(MAX_FILE_SIZE_ENV)
    if value is None or value.strip() == "":
        return MAX_FILE_SIZE
    try:
        limit_gb = int(value)
    except ValueError:
        limit_gb = 0
    if limit_gb <= 0 or limit_gb > (1 << 20):
        print(f"WARN: Invalid {MAX_FILE_SIZE_ENV} value {value!r}. Using the default {MAX_FILE_SIZE_GB}GiB.")
        return MAX_FILE_SIZE
    return limit_gb * 1024**3


class AnalysisConfig:
    def __init__(self, input_path: str):
        self.input_path = input_path

    def __post_init__(self):
        """包含对于输入文件路径的公共检查：包含文件存在性,可读性,文件大小,路径长度,是否为软链接,权限校验(group和other用户组不可写,属主为root或当前用户"""
        path = Path(self.input_path)
        current_uid = os.getuid()
        file_stat = path.stat()

        # 当输入路径不属于本用户时，添加风险提示
        if file_stat.st_uid != current_uid:
            if current_uid == 0:
                print("WARN: Process is running as user root. Please confirm the input path is trusted.")
            else:
                print("WARN: The input path is not owned by the current user. Please confirm it is trusted.")
        if not path.is_file():
            raise FileNotFoundError(f"File not found: {path} or not a regular file")
        if not os.access(str(path), os.R_OK):
            raise PermissionError(f"Read permission denied: {path}")
        max_file_size = _get_max_file_size_bytes()
        if file_stat.st_size > max_file_size:
            raise ValueError(
                f"File {path} is {file_stat.st_size} bytes, exceeding the maximum allowed size of "
                f"{max_file_size} bytes. Set {MAX_FILE_SIZE_ENV} to a larger value (unit: GiB) to raise the limit."
            )
        if len(str(path)) > INPUT_STR_MAX_LEN:
            raise ValueError(f"Max allowed length is {INPUT_STR_MAX_LEN} characters")
        # 目前只涉及csv和db文件的操作
        valid_extensions = {'.csv', '.db'}
        file_ext = path.suffix.lower()
        if file_ext not in valid_extensions:
            raise ValueError(f"Unsupported file type: {file_ext}.Only files ending with .csv or .db are supported.")


class BaseAnalyzer(ABC):
    """分析器基类:必须实现analyze"""

    @abstractmethod
    def analyze(self, config: AnalysisConfig):
        pass
