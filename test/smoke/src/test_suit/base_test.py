#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

from abc import ABC, abstractmethod
import subprocess
from typing import Optional
from functools import reduce
from ..utils.file_system import WorkingDir
import logging
import re
import os
import time
from ..utils.result import Result
from ..utils.utils import ColorText, PrintBorder
from ..utils.env_checker import EnvChecker
from ..utils.functional import as_arg, uncurry


class BaseTest(ABC):
    def __init__(self, name: str):
        self._name = name
        self._requires = []
        self.parent: Optional[BaseTest] = None

    def __str__(self):
        """ test case description that should be overloaded by derived test case class
        """
        return f"base test case. case name: {self.name}"

    @abstractmethod
    def run(self) -> Result:
        pass

    @abstractmethod
    def set_up(self):
        pass

    @abstractmethod
    def tear_down(self):
        pass

    @property
    def name(self) -> str:

        return f"{self.parent.name}.{self._name}" if self.parent else self._name

    def report(self, result: Result):
        if result.success:
            print(f"{ColorText.run_ok} {self.name}")
        else:
            print(f"= expected: {result.expected}\n"
                  f"= got: {result.got}")
            print(f"{ColorText.run_failed} {self.name}")

    def __enter__(self):
        self.set_up()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.tear_down()


class TestSuite(BaseTest):
    def __init__(self, name: str, config, work_path: str, cmd: str, max_time):
        super().__init__(name)
        self._test_cases = []
        self._config = config
        self._work_path = work_path
        self._cmd = cmd
        self._max_time = max_time
    def register(self, test_case: BaseTest):

        test_case.parent = self
        if not self._filter_test_case(test_case):
            return

        self._test_cases.append(test_case)

    def _run_cmd(self) -> bool:
        if self._cmd == []:
            return True

        if not os.path.exists(self._work_path):
            os.mkdir(self._work_path)

        with WorkingDir(self._work_path):
            with open('output.txt', 'w') as f:
                start_time = time.time()
                print(f"{ColorText.run_test} {self._cmd}")
                process = subprocess.Popen(self._cmd, stdout=f, stderr=subprocess.STDOUT)
                while process.poll() is None:
                    if time.time() - start_time > self._max_time:
                        process.terminate()
                        print(f"{ColorText.run_failed} {self._cmd} time out")
                        return False

        if process.returncode != 0:
            print(f"{ColorText.run_failed} {self._cmd} run failed\n")
            return False

        print(f"{ColorText.run_ok} {self._cmd}")
        return True

    def run(self) -> Result:

        def _reduce_requires(requires):
            result = map(as_arg(EnvChecker().env), requires)
            return reduce(lambda a, b: a or b, result, None)

        def _run_test_case(test_case):
            with test_case:
                return test_case.run()

        reason = _reduce_requires(self._requires)
        if reason is not None:
            print(f"{ColorText.run_warn} drop {self.name}. reason: {reason}")
            return Result(True, None, None)
        
        print(f"{ColorText.run_test} {self}")

        if not self._test_cases:
            return Result(True, [], [])

        if not self._run_cmd():
            print(f"{ColorText.run_failed} {self.name} run failed")
            return Result(False, [], [])

        logging.debug(f"run {self}")
        
        with PrintBorder(f"{len(self._test_cases)} tests from {self._name}"):
            result = self.reduce(list(map(_run_test_case, self._test_cases)))

        return result

    def reduce(self, results) -> Result:
        total_success = all(map(lambda r: r.success, results))
        total_expected = list(map(lambda r: r.expected, results))
        total_got = list(map(lambda r: r.got, results))
        return Result(total_success, total_expected, total_got)

    def _filter_test_case(self, test_case: BaseTest) -> bool:

        if self._config.filter is None:
            return True

        case_name = test_case.name

        return re.search(self._config.filter, case_name) is not None
