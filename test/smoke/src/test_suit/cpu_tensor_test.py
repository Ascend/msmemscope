#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

"""
CPU tensor memory collection smoke test.
Verifies --device=cpu mode generates HOST events in device_cpu/ dump CSV.
"""

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
        test_cases = [
            CpuTensorTestCase("check_cpu_tensor_dump", work_path),
        ]
        _ = list(map(self.register, test_cases))

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()

    def __str__(self):
        return f"cpu tensor test suite. suite name: {self.name}"


class CpuTensorTestCase(BaseTest):
    """Verify device_cpu/ CSV contains HOST MALLOC/FREE events."""

    def __init__(self, name: str, work_path: str):
        super().__init__(name)
        self._work_path = work_path

    def __str__(self):
        return f"cpu tensor test case. case name: {self.name}"

    def _find_cpu_csv(self):
        """Find CSV files under device_cpu/ directory."""
        dump_dir = os.path.join(self._work_path, "memscopeDumpResults")
        if not os.path.exists(dump_dir):
            return []
        cpu_csv_paths = []
        for root, _dirs, files in os.walk(dump_dir):
            for f in files:
                if f.startswith("memscope_dump_") and f.endswith(".csv"):
                    full_path = os.path.join(root, f)
                    if "device_cpu" in root:
                        cpu_csv_paths.append(full_path)
        return cpu_csv_paths

    def _verify_csv(self, csv_paths):
        """Verify HOST events exist with proper fields."""
        if not csv_paths:
            return Result(False, ["CSV files in device_cpu/"], ["none found"])

        all_has_host = True
        total_host_count = 0
        for csv_path in csv_paths:
            try:
                df = pd.read_csv(csv_path)
                host_df = df[df["Event Type"] == "HOST"]
                host_count = len(host_df)
                total_host_count += host_count

                if host_count == 0:
                    logging.error("No HOST events in %s", csv_path)
                    all_has_host = False
                    continue

                # Verify HOST events contain both MALLOC and FREE
                event_counts = host_df["Event"].value_counts().to_dict()
                if "MALLOC" not in event_counts:
                    logging.error("No HOST MALLOC events in %s", csv_path)
                    all_has_host = False
                if "FREE" not in event_counts:
                    logging.error("No HOST FREE events in %s", csv_path)
                    all_has_host = False

                logging.info("%s: %d HOST events (MALLOC=%d, FREE=%d)",
                             os.path.basename(csv_path), host_count,
                             event_counts.get("MALLOC", 0), event_counts.get("FREE", 0))

            except Exception as e:
                logging.error("Error reading %s: %s", csv_path, str(e))
                return Result(False, ["valid CSV"], [str(e)])

        if not all_has_host or total_host_count == 0:
            return Result(False, ["HOST events with MALLOC/FREE"], ["missing"])

        return Result(True, [], [])

    def run(self) -> Result:
        super().run()
        logging.debug("run %s", self)
        print(f"{ColorText.run_test} {self}")

        csv_paths = self._find_cpu_csv()
        result = self._verify_csv(csv_paths)
        self.report(result)
        return result

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()
