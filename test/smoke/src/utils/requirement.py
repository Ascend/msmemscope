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

from abc import ABC, abstractmethod
from typing import Any, Optional, Callable

from .env_checker import Environment, EnvItem


class Require(ABC):
    """ requirement before test case run. test case will be dropped if requirement unsatisfied
    """

    def __init__(self, env_getter: Callable[[Environment], EnvItem]):
        """
        @param env_getter: getter method for get specific envitem from environment
        """
        self._env_getter = env_getter

    @abstractmethod
    def __call__(self, env: Environment) -> Optional[str]:
        """ [interface] run require check
        @param env: environment probed by env_checker
        @return: None -> requirement satisfied
                 str  -> requirement unsatisfied, reason return
        """
        pass


class NotNone(Require):
    """ describe one envitem should NOT be None
    """
    def __call__(self, env: Environment) -> Optional[str]:
        item = self._env_getter(env)
        if item.value is not None:
            return None
        return f"{item.attr.name} expected not None"


class GreaterEqualThan(Require):
    """ describe one envitem should greater than or equal to threshold
    """
    def __init__(self, env_getter: Callable[[Environment], EnvItem], threshold: Any):
        super().__init__(env_getter)
        self._threshold = threshold

    def __call__(self, env: Environment) -> Optional[str]:
        item = self._env_getter(env)
        if item.value is None:
            return f"{item.attr.name} expected not None"
        if item.value < self._threshold:
            return f"{item.attr.name} expected greater than or equal to {self._threshold}, but got {item.value}"
        return None


class DeviceCountRequire(Require):
    def __init__(self, threshold: Any):
        self._require = GreaterEqualThan(lambda env: env.device_count, threshold)

    def __call__(self, env: Environment) -> Optional[str]:
        return self._require(env)
