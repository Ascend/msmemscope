#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
"""
inefficient.py 文件读取单元测试: 跳过 ID 无法解析的脏记录行

本次改动: _read_csv_file/_read_db_file 中按行过滤 ID 非法记录,
被过滤行不进入 OriginEvent 列表, 剩余行 row_num 保持原始物理行号。
运行: cd test/unit_python && python -m unittest -v test_inefficient_analyzer
不依赖 _msmemscope 编译产物(经由 analyzer_loader 按需加载)
"""

import contextlib
import io
import os
import sqlite3
import tempfile
import unittest
from pathlib import Path

from analyzer_loader import load_analyzer

_HEADERS = [
    "ID", "Event", "Event Type", "Name", "Timestamp(ns)",
    "Process Id", "Thread Id", "Device Id", "Ptr", "Attr",
    "Call Stack(Python)", "Call Stack(C)",
]

_VALID_ROW_1 = "1,MALLOC,PTA,x,100,1,1,d0,0x10,size:1024,py,cpp"
_DIRTY_ROW = "abc,MALLOC,PTA,y,200,1,1,d0,0x20,size:2048,py,cpp"
_VALID_ROW_3 = "3,FREE,ATB,z,300,1,1,d0,0x10,,,"


class TestInefficientFileReading(unittest.TestCase):
    def setUp(self):
        self.ineff = load_analyzer("inefficient")
        self.analyzer = self.ineff.InefficientAnalyzer()

    def test_csv_dirty_rows_skipped_and_row_num_kept(self):
        fd, path = tempfile.mkstemp(suffix=".csv")
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write(",".join(_HEADERS) + "\n")
            f.write(_VALID_ROW_1 + "\n")
            f.write(_DIRTY_ROW + "\n")
            f.write(_VALID_ROW_3 + "\n")
        self.addCleanup(os.unlink, path)

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            events = self.analyzer._read_csv_file(path)

        # 脏行(物理第3行)被跳过, 剩余行保持原始物理行号
        self.assertEqual([e.event_id for e in events], [1, 3])
        self.assertEqual([e.row_num for e in events], [2, 4])
        self.assertTrue(all(isinstance(e.event_id, int) for e in events))
        self.assertIn("WARNING: Cannot convert", buf.getvalue())
        self.assertIn("line 3", buf.getvalue())

    def test_csv_all_valid_rows_loaded(self):
        fd, path = tempfile.mkstemp(suffix=".csv")
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write(",".join(_HEADERS) + "\n")
            f.write(_VALID_ROW_1 + "\n")
            f.write(_VALID_ROW_3 + "\n")
        self.addCleanup(os.unlink, path)

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            events = self.analyzer._read_csv_file(path)

        self.assertEqual([e.row_num for e in events], [2, 3])

    def test_db_dirty_rows_skipped_and_row_num_kept(self):
        fd, path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.addCleanup(os.unlink, path)
        conn = sqlite3.connect(path)
        self.addCleanup(conn.close)
        # dump 表列为宽松 TEXT, 以模拟真实导出中出现的脏 ID
        conn.execute(
            "CREATE TABLE memscope_dump (%s)" % ",".join(f'"{h}" TEXT' for h in _HEADERS))
        conn.executemany(
            "INSERT INTO memscope_dump VALUES (%s)" % ",".join("?" * len(_HEADERS)),
            [tuple(_VALID_ROW_1.split(",")), tuple(_DIRTY_ROW.split(",")), tuple(_VALID_ROW_3.split(","))])
        conn.commit()

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            # 与 _read_db_file 的 Path 类型约定保持一致(file_to_events 中即传入 Path)
            events = self.analyzer._read_db_file(Path(path))

        # DB 无表头, 行号从 1 开始; 脏行(第2行)被跳过
        self.assertEqual([e.event_id for e in events], [1, 3])
        self.assertEqual([e.row_num for e in events], [1, 3])
        self.assertTrue(all(isinstance(e.event_id, int) for e in events))
        self.assertIn("WARNING: Cannot convert", buf.getvalue())

    def test_db_all_valid_rows_loaded(self):
        fd, path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.addCleanup(os.unlink, path)
        conn = sqlite3.connect(path)
        self.addCleanup(conn.close)
        conn.execute(
            "CREATE TABLE memscope_dump (%s)" % ",".join(f'"{h}" TEXT' for h in _HEADERS))
        conn.executemany(
            "INSERT INTO memscope_dump VALUES (%s)" % ",".join("?" * len(_HEADERS)),
            [tuple(_VALID_ROW_1.split(",")), tuple(_VALID_ROW_3.split(","))])
        conn.commit()

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            events = self.analyzer._read_db_file(Path(path))

        self.assertEqual([e.row_num for e in events], [1, 2])


if __name__ == "__main__":
    unittest.main()