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

import os
from dataclasses import dataclass
from typing import Optional, Any, TypeVar, Generic
from enum import Enum, auto

from .singleton import Singleton
from .utils import PrintBorder, ColorText
from .functional import uncurry
from .platform import DeviceName


T = TypeVar("T")


class EnvItemProp(Enum):
    REQUIRED = auto()
    FALLBACK = auto()
    DROP = auto()


@dataclass
class EnvItemAttr(Generic[T]):
    name: str
    prop: EnvItemProp
    default: Optional[T]


@dataclass
class EnvItem(Generic[T]):
    value: Optional[T]
    attr: EnvItemAttr[T]


@dataclass
class Environment:
    ascend_home_path: EnvItem[str]
    device_count: EnvItem[int]
    device_name: EnvItem[DeviceName]


class EnvChecker(metaclass=Singleton):
    def __init__(self, path):
        self._toolkit_path = path + "/ascend-toolkit/latest"
        self._env = self._check()

    @property
    def env(self):
        return self._env


    def _check(self) -> Optional[Environment]:
        _checks = [
            (EnvItemAttr("ASCEND_HOME_PATH", EnvItemProp.REQUIRED, None), self._check_ascend_home_path),
            (EnvItemAttr("Device count", EnvItemProp.DROP, None), self._check_device_count),
            (EnvItemAttr("Device name", EnvItemProp.FALLBACK, DeviceName.Ascend910B1), self._check_device_name),
        ]

        def _run_check(_attr: EnvItemAttr, _check) -> Optional[EnvItem]:
            print(f"{ColorText.run_test} check {_attr.name}")
            value = _check()
            if value is not None:
                print(f"{ColorText.run_ok} {_attr.name} check ok. value: {value}")
                return EnvItem(value, _attr)
            if _attr.prop is EnvItemProp.DROP:
                print(f"{ColorText.run_warn} {_attr.name} missing. some test cases will be dropped")
                return EnvItem(value, _attr)
            if _attr.prop is EnvItemProp.FALLBACK and _attr.default is not None:
                print(f"{ColorText.run_warn} {_attr.name} missing. fallback to {_attr.default}")
                return EnvItem(_attr.default, _attr)

            print(f"{ColorText.run_failed} {_attr.name} check failed")
            return None

        with PrintBorder("check environment", "check environment done"):
            args = tuple(map(uncurry(_run_check), _checks))
            if any(map(lambda x: x is None, args)):
                return None
            return Environment(*args)

    def _check_ascend_home_path(self) -> Optional[str]:
        path = os.getenv("ASCEND_HOME_PATH")
        return path

    def _check_device_count(self) -> Optional[int]:
        try:
            import torch
            import torch_npu
        except ModuleNotFoundError:
            return None
        return torch.npu.device_count()

    def _check_device_name(self) -> Optional[DeviceName]:
        return DeviceName.Ascend910B1
