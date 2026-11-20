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
"""输入文件大小上限环境变量测试。"""

import os
import unittest
from unittest import mock

from analyzer_loader import load_analyzer

GIB = 1024 ** 3


class TestMaxFileSize(unittest.TestCase):
    def setUp(self):
        self.base = load_analyzer("base")
        self.env = self.base.MAX_FILE_SIZE_ENV

    def test_default_limit(self):
        with mock.patch.dict(os.environ, {self.env: ""}):
            self.assertEqual(self.base._get_max_file_size_bytes(), 8 * GIB)

    def test_env_override(self):
        with mock.patch.dict(os.environ, {self.env: "2"}):
            self.assertEqual(self.base._get_max_file_size_bytes(), 2 * GIB)

    def test_invalid_env_fallback(self):
        with mock.patch.dict(os.environ, {self.env: "invalid"}):
            self.assertEqual(self.base._get_max_file_size_bytes(), 8 * GIB)


if __name__ == "__main__":
    unittest.main()
