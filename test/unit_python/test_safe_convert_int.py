#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
"""
safe_convert_int 单元测试: 安全转换 + context 定位信息(WARNING)

本次改动: 新增 context 参数; 失败时合并为单个 WARNING 输出并返回 None。
运行: cd test/unit_python && python -m unittest -v test_safe_convert_int
不依赖 _msmemscope 编译产物(经由 analyzer_loader 按需加载)
"""

import contextlib
import io
import unittest

from analyzer_loader import load_analyzer


class TestSafeConvertInt(unittest.TestCase):
    def setUp(self):
        self.uf = load_analyzer("utility_function")

    def _capture(self, input_value, context=""):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            result = self.uf.safe_convert_int(input_value, context=context)
        return result, buf.getvalue()

    def test_valid_integer_string(self):
        self.assertEqual(self.uf.safe_convert_int("123"), 123)
        self.assertIsInstance(self.uf.safe_convert_int("123"), int)

    def test_valid_int_value(self):
        self.assertEqual(self.uf.safe_convert_int(456), 456)

    def test_valid_with_context_no_output(self):
        result, output = self._capture("789", context="line 2")
        self.assertEqual(result, 789)
        self.assertEqual(output, "")

    def test_invalid_values_return_none(self):
        for bad in ("abc", "", "1.5", None):
            self.assertIsNone(self.uf.safe_convert_int(bad))

    def test_failure_warns_with_context(self):
        result, output = self._capture("abc", context="line 3, device d0")
        self.assertIsNone(result)
        self.assertIn("WARNING: Cannot convert", output)
        self.assertIn(repr("abc"), output)
        self.assertIn("line 3, device d0", output)

    def test_failure_warns_without_context(self):
        result, output = self._capture("abc")
        self.assertIsNone(result)
        self.assertIn("WARNING: Cannot convert", output)
        self.assertNotIn(" (", output)

    def test_none_value_warns(self):
        result, output = self._capture(None)
        self.assertIsNone(result)
        self.assertIn("WARNING: Cannot convert", output)


if __name__ == "__main__":
    unittest.main()