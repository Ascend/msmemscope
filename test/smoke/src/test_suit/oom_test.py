#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

"""
OOM 分析冒烟测试。
验证 --analysis=oom:K 模式下 dump CSV 包含 OOM_DETAIL 事件。
目标：1 分钟内完成。
"""

import logging
import os
import re

import pandas as pd

from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.utils import ColorText


class OOMTestSuite(TestSuite):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time: int):
        super().__init__(name, config, work_path, cmd, max_time)
        test_cases = [
            OOMTestCase("check_oom_dump", work_path),
        ]
        _ = list(map(self.register, test_cases))

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()

    def __str__(self):
        return f"oom test suite. suite name: {self.name}"


class OOMTestCase(BaseTest):
    """验证 OOM dump CSV 包含三类 OOM_DETAIL 事件。"""

    def __init__(self, name: str, work_path: str):
        super().__init__(name)
        self._work_path = work_path

    def __str__(self):
        return f"oom test case. case name: {self.name}"

    def run(self) -> Result:
        super().run()
        logging.debug("run %s", self)
        print(f"{ColorText.run_test} {self}")

        csv_path = self._find_dump_csv()
        if not csv_path:
            self._report(Result(False, ["dump CSV exists"], ["not found"]))
            return Result(False, ["dump CSV exists"], ["not found"])

        return self._verify_csv(csv_path)

    def _find_dump_csv(self):
        for root, _dirs, files in os.walk(self._work_path):
            for f in files:
                if re.match(r"memscope_dump_\d+\.csv", f):
                    return os.path.join(root, f)
        return None

    def _verify_csv(self, csv_path: str) -> Result:
        try:
            df = pd.read_csv(csv_path)

            # 1. 必须有 OOM_DETAIL 事件
            oom_df = df[df["Event"] == "OOM_DETAIL"]
            if len(oom_df) == 0:
                self._report(Result(False, ["OOM_DETAIL events"], ["0"]))
                return Result(False, ["OOM_DETAIL events"], ["0"])

            event_types = oom_df["Event Type"].value_counts().to_dict()
            logging.info("OOM_DETAIL events: %s", event_types)

            # 2. 必须包含三种 Event Type
            for required in ["OOM_TRIGGER", "OOM_RECENT_ALLOC", "OOM_TOP_ALLOC"]:
                if required not in event_types:
                    self._report(
                        Result(False, [f"contains {required}"], [str(list(event_types.keys()))])
                    )
                    return Result(
                        False, [f"contains {required}"], [str(list(event_types.keys()))]
                    )

            # 3. OOM_TRIGGER Attr 含必要字段
            trigger_rows = oom_df[oom_df["Event Type"] == "OOM_TRIGGER"]
            for _, row in trigger_rows.iterrows():
                attr = str(row["Attr"])
                if "func" not in attr or "req_size" not in attr:
                    self._report(Result(False, ["func,req_size in Attr"], [attr]))
                    return Result(False, ["func,req_size in Attr"], [attr])

            self._report(Result(True, [], []))
            return Result(True, [], [])
        except Exception as e:
            logging.error("Error verifying OOM CSV %s: %s", csv_path, str(e))
            self._report(Result(False, ["CSV verification"], [str(e)]))
            return Result(False, ["CSV verification"], [str(e)])

    def _report(self, result: Result):
        self.report(result)

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()
