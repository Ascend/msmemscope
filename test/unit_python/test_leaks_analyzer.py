#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
"""
leaks.py read_file 单元测试: CSV 列校验 + 跳过 ID 脏记录行

本次改动: 读取前校验 ID/Device Id 列是否存在; 按行过滤 ID 无法解析为 int 的记录。
运行: cd test/unit_python && python -m unittest -v test_leaks_analyzer
不依赖 _msmemscope 编译产物(经由 analyzer_loader 按需加载)
"""

import contextlib
import io
import os
import tempfile
import unittest
from types import SimpleNamespace

from analyzer_loader import load_analyzer


def _write_csv(header, rows):
    """写入临时 CSV, 返回文件路径(由调用方负责清理)"""
    fd, path = tempfile.mkstemp(suffix=".csv", dir=tempfile.gettempdir())
    with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
        f.write(header + "\n")
        f.writelines(rows)
    return path


class TestLeaksReadFile(unittest.TestCase):
    def setUp(self):
        self.leaks = load_analyzer("leaks")

    def _analyzer(self, header, rows):
        """构造带 input_path 配置的 LeaksAnalyzer, 捕获 stdout"""
        path = _write_csv(header, rows)
        analyzer = self.leaks.LeaksAnalyzer()
        analyzer.config = SimpleNamespace(input_path=path)
        self.addCleanup(os.unlink, path)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ok = analyzer.read_file()
        return analyzer, ok, buf.getvalue()

    def test_missing_id_header_rejected(self):
        analyzer, ok, _ = self._analyzer("Device Id,Event", ["d0,MALLOC\n"])
        self.assertFalse(ok)
        self.assertIn("missing headers ['ID']", analyzer.error)

    def test_missing_device_id_header_rejected(self):
        analyzer, ok, _ = self._analyzer("ID,Event", ["1,MALLOC\n"])
        self.assertFalse(ok)
        self.assertIn("missing headers ['Device Id']", analyzer.error)

    def test_empty_file_rejected_without_crash(self):
        analyzer, ok, _ = self._analyzer("", [])
        self.assertFalse(ok)
        self.assertIn("missing headers", analyzer.error)

    def test_dirty_id_row_skipped_with_warning(self):
        analyzer, ok, output = self._analyzer(
            "Device Id,ID,Event",
            ["d0,1,MALLOC\n", "d0,abc,FREE\n", "d0,3,MALLOC\n"],
        )
        self.assertTrue(ok)
        self.assertEqual(len(analyzer.device_events["d0"]), 2)
        self.assertIn("WARNING: Cannot convert", output)
        self.assertIn("line 3, device d0", output)

    def test_rows_grouped_by_device(self):
        analyzer, ok, _ = self._analyzer(
            "Device Id,ID,Event",
            ["d0,1,MALLOC\n", "d1,5,MALLOC\n", "d0,6,FREE\n"],
        )
        self.assertTrue(ok)
        self.assertEqual(len(analyzer.device_events["d0"]), 2)
        self.assertEqual(len(analyzer.device_events["d1"]), 1)


if __name__ == "__main__":
    unittest.main()