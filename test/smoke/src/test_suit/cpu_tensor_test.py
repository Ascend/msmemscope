#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

"""CPU tensor smoke test: verify device_cpu/ CSV contains HOST MALLOC/FREE events."""

import logging
import os

import pandas as pd

from .base_test import BaseTest, TestSuite
from ..utils.result import Result


def _find_cpu_csv(work_path):
    """Return the latest device_cpu memory-event CSV path, or None.

    The dump filename carries a timestamp (``memscope_dump_YYYYMMDDHHMMSS.csv``), so
    ``max`` picks the most recent run even when old dumps accumulate in the workbench.
    """
    dump_dir = os.path.join(work_path, "memscopeDumpResults")
    if not os.path.exists(dump_dir):
        return None
    csvs = []
    for root, _dirs, files in os.walk(dump_dir):
        for f in files:
            if f.startswith("memscope_dump_") and f.endswith(".csv") and "device_cpu" in root:
                csvs.append(os.path.join(root, f))
    return max(csvs) if csvs else None


def _load_cpu_df(work_path):
    path = _find_cpu_csv(work_path)
    if path is None:
        return None, None
    return path, pd.read_csv(path)


class CpuTensorTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time: int):
        super().__init__(name, config, work_path, cmd, max_time)
        _ = list(map(self.register, [
            CpuTensorDumpTestCase("check_cpu_dump", work_path),
            CpuTensorCapturedTestCase("check_cpu_tensor_captured", work_path),
        ]))

    def set_up(self):
        pass

    def tear_down(self):
        pass

    def __str__(self):
        return f"cpu tensor smoke. suite: {self.name}"


class CpuTensorDumpTestCase(BaseTest):
    """device_cpu/ CSV exists and carries HOST MALLOC/FREE events."""

    def __init__(self, name: str, work_path: str):
        super().__init__(name)
        self._work_path = work_path

    def run(self) -> Result:
        super().run()
        path, df = _load_cpu_df(self._work_path)
        if path is None:
            self.report(Result(False, ["CSV in device_cpu/"], ["none"]))
            return Result(False, [], [])

        try:
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


class CpuTensorCapturedTestCase(BaseTest):
    """CPU tensors (attr ``total:``) are captured with a Python call stack, dedup intact.

    The smoke script creates exactly three CPU tensors (``torch.tensor`` / ``.to("cpu")`` /
    ``.cpu()``); an already-CPU ``.cpu()`` and a ``.view()`` must not add events, and an
    empty tensor must be skipped — so the CPU-tensor MALLOC/FREE count stays at 3.
    """

    def __init__(self, name: str, work_path: str):
        super().__init__(name)
        self._work_path = work_path

    def run(self) -> Result:
        super().run()
        path, df = _load_cpu_df(self._work_path)
        if path is None:
            self.report(Result(False, ["CSV in device_cpu/"], ["none"]))
            return Result(False, [], [])

        try:
            host = df[df["Event Type"] == "HOST"]
            cpu_malloc = host[(host["Event"] == "MALLOC") & host["Attr"].str.contains("total:", na=False)]
            cpu_free = host[(host["Event"] == "FREE") & host["Attr"].str.contains("total:", na=False)]
            malloc_cnt = len(cpu_malloc)
            free_cnt = len(cpu_free)
            stack = cpu_malloc["Call Stack(Python)"]
            stack_ok = stack.notna().all() and (stack != "").all()
            ok = malloc_cnt == 3 and free_cnt == 3 and stack_ok
            logging.info("CPU tensor events: MALLOC=%d, FREE=%d, stack_ok=%s", malloc_cnt, free_cnt, stack_ok)
            result = Result(ok,
                            ["3 CPU tensor MALLOC/FREE with Python stack"],
                            [f"MALLOC={malloc_cnt}, FREE={free_cnt}, stack_ok={stack_ok}"])
        except Exception as e:
            result = Result(False, ["valid CSV"], [str(e)])
        self.report(result)
        return result

    def set_up(self):
        pass

    def tear_down(self):
        pass
