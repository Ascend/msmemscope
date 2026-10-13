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

"""控制通道IT目标进程。

由msmemscope wrapper预加载libascend_leaks.so启动,控制通道监听线程随.so加载常驻。
本脚本将自身pid写入pidfile后,持续小规模alloc/free并sleep保持进程活跃,
供attach用例取pid/发控制字(当前套件装配=NPU钩子链,无host事件流)。
"""

import argparse
import os
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pidfile", default="target.pid")
    parser.add_argument("--sleep", type=int, default=300)
    args = parser.parse_args()

    with open(args.pidfile, "w") as f:
        f.write(str(os.getpid()) + "\n")

    buf = []
    deadline = time.time() + args.sleep
    while time.time() < deadline:
        buf.append(b"x" * 4096)
        if len(buf) > 64:
            buf.clear()
        time.sleep(0.5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
