#!/bin/bash
# Copyright Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

# 控制通道IT: 启动attach目标(后台常驻,pid写入target.pid),脚本自身立即退出。
# 后台python继承msmemscope wrapper注入的LD_PRELOAD→libascend_leaks.so加载
# →控制通道监听线程常驻;setsid脱离进程组,避免wrapper退出清理波及。

DIR="$(cd "$(dirname "$0")" && pwd)"
setsid nohup python "$DIR/test_control_channel_target.py" --pidfile target.pid --sleep 300 \
    > target.log 2>&1 < /dev/null &
exit 0
