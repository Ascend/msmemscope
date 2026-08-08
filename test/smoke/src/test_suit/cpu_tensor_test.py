#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

"""CPU tensor smoke test: verify device_cpu/ CSV contains HOST MALLOC/FREE events."""

import logging
import os
import re

import pandas as pd

from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.utils import ColorText


class CpuTensorTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time: int):
        super().__init__(name, config, work_path, cmd, max_time)
        _ = list(map(self.register, [CpuTensorTestCase("check_cpu_dump", work_path)]))

    def __str__(self):
        return f"cpu tensor smoke. suite: {self.name}"


class CpuTensorTestCase(BaseTest):
    def __init__(self, name: str, work_path: str):
        super().__init__(name)
        self._work_path = work_path

    def run(self) -> Result:
        super().run()
        dump_dir = os.path.join(self._work_path, "memscopeDumpResults")
        cpu_csv = []
        for root, _dirs, files in os.walk(dump_dir) if os.path.exists(dump_dir) else []:
            for f in files:
                if f.startswith("memscope_dump_") and f.endswith(".csv") and "device_cpu" in root:
                    cpu_csv.append(os.path.join(root, f))

        if not cpu_csv:
            self.report(Result(False, ["CSV in device_cpu/"], ["none"]))
            return Result(False, [], [])

        try:
            df = pd.read_csv(cpu_csv[0])
            host = df[df["Event Type"] == "HOST"]
            malloc_cnt = (host["Event"] == "MALLOC").sum()
            free_cnt = (host["Event"] == "FREE").sum()
            ok = malloc_cnt > 0 and free_cnt > 0
            logging.info("HOST events: MALLOC=%d, FREE=%d", malloc_cnt, free_cnt)
            result = Result(ok, ["HOST MALLOC/FREE > 0"], [f"MALLOC={malloc_cnt}, FREE={free_cnt}"])
        except Exception as e:
            result = Result(False, ["valid CSV"], [str(e)])
        self.report(result)
        return result

    def set_up(self):
        pass
    def tear_down(self):
        pass
