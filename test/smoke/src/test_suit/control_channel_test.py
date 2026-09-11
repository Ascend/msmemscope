#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

"""控制通道IT(smoke)套件。

覆盖attach全流程: 交互会话内start/stop/step/display各命令回显、tab补全交互
(pty)、REGISTER后socket文件删除与exit无残留、重复attach拒绝、非法目标拒绝
(无libascend_leaks.so进程/不存在的pid)、目标进程退出主动提示(仅exit/help可用)。

目标进程由suite cmd启动: msmemscope wrapper预加载libascend_leaks.so的python
常驻进程(控制通道监听线程随.so加载)。用例经`msmemscope --pid <pid>`附加。
"""

import logging
import os
import pty
import select
import signal
import subprocess
import time

from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.utils import ColorText

# 控制端入口(用例cwd为workbench,较套件cmd的workbench/<work_path>浅一级,故为../而非../../)
MSMEM_SCOPE = "../msmemscope/output/bin/msmemscope"
# 控制协议socket路径模板(control_protocol.cpp)
SOCKET_PATH_TEMPLATE = "/tmp/msmemscope_socket_{pid}"

# 目标pid文件: 套件cmd(_run_cmd)在workbench/<work_path>下启动目标并写target.pid;
# 用例在workbench下运行,经<work_path>/target.pid取pid
TARGET_PIDFILE = "target.pid"

# 交互attach的accept等待(env可调;正常attach毫秒级,仅异常场景用满)
DEFAULT_ATTACH_TIMEOUT_MS = 15000
# 重复attach用例: 短超时快速暴露already attached
DUPLICATE_ATTACH_TIMEOUT_MS = 3000


def _attach_env(timeout_ms):
    """控制端env: 关闭调试禁用项,收紧accept超时(env名见control_protocol.h)。"""
    env = dict(os.environ)
    env.pop("MSMEMSCOPE_DISABLE_CONTROL_CHANNEL", None)
    env["MSMEMSCOPE_ATTACH_TIMEOUT_MS"] = str(timeout_ms)
    return env


class PtySession:
    """pty交互会话: master侧读写,逐模式累积消费输出。"""

    def __init__(self, argv, env):
        master, slave = pty.openpty()
        self._master = master
        self._proc = subprocess.Popen(
            argv, stdin=slave, stdout=slave, stderr=subprocess.STDOUT,
            env=env, close_fds=True,
        )
        os.close(slave)
        self._buf = b""

    def _read_more(self):
        if self._proc.poll() is not None:
            return False
        r, _, _ = select.select([self._master], [], [], 0.5)
        if not r:
            return True
        try:
            chunk = os.read(self._master, 4096)
        except OSError:
            return False
        if not chunk:
            return False
        self._buf += chunk
        return True

    def expect(self, pattern, timeout=25.0):
        """读取输出直到出现pattern(消费至pattern末尾);超时抛AssertionError。"""
        if isinstance(pattern, str):
            pattern = pattern.encode()
        deadline = time.time() + timeout
        while pattern not in self._buf:
            if time.time() > deadline:
                raise AssertionError(
                    "expect {!r} timeout; got: {!r}".format(pattern, self._buf[-2048:])
                )
            if not self._read_more():
                break
        idx = self._buf.find(pattern)
        if idx < 0:
            raise AssertionError(
                "expect {!r} not found (process rc={}); got: {!r}".format(
                    pattern, self._proc.poll(), self._buf[-2048:]
                )
            )
        self._buf = self._buf[idx + len(pattern):]
        return pattern

    def expect_any(self, patterns, timeout=25.0):
        """任一pattern出现即返回之(step等输出因环境取值的分支断言)。"""
        patterns = [p.encode() if isinstance(p, str) else p for p in patterns]
        deadline = time.time() + timeout
        matched = None
        while matched is None:
            for p in patterns:
                if p in self._buf:
                    matched = p
                    break
            if matched is not None:
                break
            if time.time() > deadline:
                raise AssertionError(
                    "expect_any {!r} timeout; got: {!r}".format(patterns, self._buf[-2048:])
                )
            if not self._read_more():
                break
        if matched is None:
            raise AssertionError(
                "expect_any {!r} not found (process rc={}); got: {!r}".format(
                    patterns, self._proc.poll(), self._buf[-2048:]
                )
            )
        idx = self._buf.find(matched)
        self._buf = self._buf[idx + len(matched):]
        return matched

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        os.write(self._master, data)

    def wait(self, timeout=15.0):
        try:
            return self._proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            raise AssertionError("pty session did not exit within {}s".format(timeout))

    def close(self):
        try:
            if self._proc.poll() is None:
                self._proc.kill()
                self._proc.wait(timeout=5)
        except Exception:
            pass
        try:
            os.close(self._master)
        except OSError:
            pass


def _run_attach(pid, command, timeout_ms=DEFAULT_ATTACH_TIMEOUT_MS, timeout_s=30):
    """单次attach执行控制字;返回(returncode, 合并stdout+stderr文本)。"""
    argv = [MSMEM_SCOPE, "--pid", str(pid)]
    if command:
        argv += ["--command", command]
    p = subprocess.run(
        argv, capture_output=True, text=True,
        env=_attach_env(timeout_ms), timeout=timeout_s,
    )
    return p.returncode, p.stdout + p.stderr


class ControlChannelTestSuite(TestSuite):
    """控制通道IT套件(目标进程由cmd后台启动,用例逐个attach)。"""

    def __init__(self, name: str, config, work_path: str, cmd: str, max_time: int):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            ControlChannelTestCase("interactive_full_flow", work_path),
            ControlChannelTestCase("single_shot_commands", work_path),
            ControlChannelTestCase("duplicate_attach_rejected", work_path),
            ControlChannelTestCase("unsupported_target_rejected", work_path),
            ControlChannelTestCase("nonexistent_pid_rejected", work_path),
            # 目标进程退出用例会杀掉目标,须最后执行(套件tear_down幂等回收)
            ControlChannelTestCase("interactive_target_exit", work_path),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return (
            f"control channel test suite. suite name: {self.name}, "
            f"suite work path: {self._work_path}"
        )

    @property
    def pidfile_path(self):
        return os.path.join(self._work_path, TARGET_PIDFILE)

    def set_up(self):
        super().set_up()
        self._target_pid = None

    def tear_down(self):
        # 回收目标进程与残留文件(套件级清理,失败路径同样执行)
        if self._target_pid:
            try:
                os.kill(self._target_pid, signal.SIGTERM)
                deadline = time.time() + 5
                while time.time() < deadline:
                    try:
                        os.kill(self._target_pid, 0)
                    except OSError:
                        break
                    time.sleep(0.2)
                else:
                    os.kill(self._target_pid, signal.SIGKILL)
            except OSError:
                pass
        for path in (
            self.pidfile_path,
            os.path.join(self._work_path, "target.log"),
            SOCKET_PATH_TEMPLATE.format(pid=self._target_pid or -1),
        ):
            try:
                os.remove(path)
            except OSError:
                pass
        super().tear_down()

    def wait_target_pid(self, timeout=25.0):
        """等待目标进程pidfile就绪(套件cmd后台启动后异步写入)。"""
        if self._target_pid:
            return self._target_pid
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with open(self.pidfile_path) as f:
                    self._target_pid = int(f.read().strip())
                    return self._target_pid
            except (OSError, ValueError):
                time.sleep(0.5)
        raise AssertionError("target pidfile {} not ready".format(self.pidfile_path))


class ControlChannelTestCase(BaseTest):
    """控制通道IT用例,按*name*分派。"""

    def __init__(self, name: str, work_path: str):
        super().__init__(name)
        self._work_path = work_path

    def __str__(self):
        return f"control channel test case. case name: {self.name}"

    @property
    def _suite(self):
        return self.parent

    @property
    def _target_pid(self):
        return self._suite.wait_target_pid()

    @property
    def _socket_path(self):
        return SOCKET_PATH_TEMPLATE.format(pid=self._target_pid)

    def run(self) -> Result:
        super().run()
        logging.debug("run %s", self)
        print(f"{ColorText.run_test} {self}")

        dispatch = {
            "interactive_full_flow": self._test_interactive_full_flow,
            "single_shot_commands": self._test_single_shot_commands,
            "duplicate_attach_rejected": self._test_duplicate_attach_rejected,
            "unsupported_target_rejected": self._test_unsupported_target_rejected,
            "nonexistent_pid_rejected": self._test_nonexistent_pid_rejected,
            "interactive_target_exit": self._test_interactive_target_exit,
        }

        result = dispatch.get(self._name, lambda: Result(False, [], []))()
        self.report(result)
        return result

    # ------------------------------------------------------------------
    # Case 1 — attach全流程(pty交互): 补全/启动/step/巡检/停/退出/无残留
    # ------------------------------------------------------------------
    def _test_interactive_full_flow(self):
        sess = PtySession([MSMEM_SCOPE, "--pid", str(self._target_pid)],
                          _attach_env(DEFAULT_ATTACH_TIMEOUT_MS))
        try:
            # 建联横幅(REGISTER已完成)
            sess.expect("[msmemscope] Target check passed (pid={},".format(self._target_pid))
            sess.expect("[msmemscope] Attached to pid {}.".format(self._target_pid))
            sess.expect("Type 'help' for control words, 'exit' to detach.")
            # REGISTER后socket文件已删除(会话仍活跃)
            if os.path.exists(self._socket_path):
                return Result(False, ["socket removed after REGISTER"],
                              ["{} still exists".format(self._socket_path)])

            # tab补全: "disp<TAB>"唯一候选display直接补全(提示符含pid)
            sess.send("disp\t")
            sess.expect("msmemscope[{}]> display".format(self._target_pid))
            # "display h<TAB>"多候选(hook/host_leak公共前缀=ho>已输入h)→扩展公共前缀
            sess.send(" h\t")
            sess.expect("msmemscope[{}]> display ho".format(self._target_pid))
            # 续输"ok<回车>"→display hook回显(目标经wrapper装配NPU钩子链;
            # 非host模式不预加载host钩子so,故Host行not loaded;逐行断言规避pty \r\n)
            sess.send("ok\r")
            sess.expect("Host hook: not loaded")
            sess.expect("NPU hooks: loaded")

            # 启动/step/停止状态机(目标以collect-mode=deferred启动,attach时未开trace)
            sess.send("start\r")
            sess.expect("tracing started")
            # 幂等分支: 已使能态重复start回显提示,非错误
            sess.send("start\r")
            sess.expect("tracing already in progress")
            sess.send("step\r")
            sess.expect_any([" recorded", "step failed:"])
            sess.send("display memory summary\r")
            # 结构断言: 空数据或任一设备行/宿主行均合法(格式细节由UT覆盖)
            sess.expect_any(["(no device data collected yet)", "Device ", "Host pinned:"])
            sess.send("stop\r")
            sess.expect("tracing stopped")

            # exit→正常退出(rc 0);socket仍无残留
            sess.send("exit\r")
            rc = sess.wait()
            if rc != 0:
                return Result(False, ["interactive exit rc 0"], [rc])
            if os.path.exists(self._socket_path):
                return Result(False, ["socket removed after exit"],
                              ["{} still exists".format(self._socket_path)])
            return Result(True, [], [])
        finally:
            sess.close()

    # ------------------------------------------------------------------
    # Case 2 — 单次attach执行控制字: 回显/白名单拒绝
    # ------------------------------------------------------------------
    def _test_single_shot_commands(self):
        pid = self._target_pid

        rc, out = _run_attach(pid, "display config")
        if rc != 0:
            return Result(False, ["display config rc 0"], [rc, out])
        for key in ("analysis: ", "output_dir: ", "log_level: "):
            if key not in out:
                return Result(False, ["display config contains {}".format(key)], [out])

        rc, out = _run_attach(pid, "display analyzer")
        if rc != 0:
            return Result(False, ["display analyzer rc 0"], [rc, out])
        if "Analyzers: " not in out or "control_channel" in out:
            return Result(False, ["Analyzers: list excludes control_channel"], [out])

        rc, out = _run_attach(pid, "display hook")
        if rc != 0 or "Host hook: not loaded" not in out:
            return Result(False, ["display hook echoes Host hook status"], [rc, out])

        # 非白名单控制字: 控制端本地拒绝并回显原因(rc 1=无效控制字失败退出码)
        rc, out = _run_attach(pid, "frobnicate")
        if rc != 1 or "invalid control word: frobnicate" not in out:
            return Result(False, ["invalid control word echoed (rc 1)"], [rc, out])

        # -c help: 本地打印控制字清单(不发帧),rc 0
        rc, out = _run_attach(pid, "help")
        if rc != 0 or "enable tracing (same as msmemscope.start())" not in out:
            return Result(False, ["-c help rc 0 with control word list"], [rc, out])

        # -c exit: 本地直接退出,rc 0
        rc, out = _run_attach(pid, "exit")
        if rc != 0:
            return Result(False, ["-c exit rc 0"], [rc, out])

        return Result(True, [], [])

    # ------------------------------------------------------------------
    # Case 3 — 重复attach拒绝: 会话活跃期间第二控制端accept超时,首会话不受扰
    # ------------------------------------------------------------------
    def _test_duplicate_attach_rejected(self):
        sess = PtySession([MSMEM_SCOPE, "--pid", str(self._target_pid)],
                          _attach_env(DEFAULT_ATTACH_TIMEOUT_MS))
        try:
            sess.expect("[msmemscope] Attached to pid")

            # 第二控制端: 业务侧会话活跃→SIGUSR1被忽略→accept超时(短env快速暴露)
            rc, out = _run_attach(self._target_pid, "display hook",
                                  timeout_ms=DUPLICATE_ATTACH_TIMEOUT_MS)
            if rc == 0:
                return Result(False, ["second attach rejected (rc != 0)"], [rc, out])
            if "attach timed out after {}ms".format(DUPLICATE_ATTACH_TIMEOUT_MS) not in out:
                return Result(False, ["attach timed out message"], [rc, out])
            if "already attached" not in out:
                return Result(False, ["message hints already attached"], [rc, out])

            # 首会话未被干扰,仍可收发
            sess.send("display hook\r")
            sess.expect("Host hook: not loaded")
            sess.send("exit\r")
            rc = sess.wait()
            if rc != 0:
                return Result(False, ["first session exit rc 0"], [rc])
            if os.path.exists(self._socket_path):
                return Result(False, ["socket removed after exit"],
                              ["{} still exists".format(self._socket_path)])
            return Result(True, [], [])
        finally:
            sess.close()

    # ------------------------------------------------------------------
    # Case 4 — 无libascend_leaks.so的进程拒绝attach(规则3)
    # ------------------------------------------------------------------
    def _test_unsupported_target_rejected(self):
        # 纯净env启动探针进程(排除用户shell已source msmemscope环境的LD_PRELOAD干扰)
        clean_env = {k: v for k, v in os.environ.items()
                     if k not in ("LD_PRELOAD", "LD_LIBRARY_PATH")}
        sleeper = subprocess.Popen(["sleep", "60"], env=clean_env)
        try:
            time.sleep(1)  # 确保目标进程就绪(/proc/<pid>可见)
            rc, out = _run_attach(sleeper.pid, "display hook")
            if rc == 0:
                return Result(False, ["unsupported target rejected (rc != 0)"], [rc, out])
            if "does not have libascend_leaks.so loaded (unsupported process)" not in out:
                return Result(False, ["maps check error message"], [rc, out])
            return Result(True, [], [])
        finally:
            try:
                sleeper.kill()
                sleeper.wait(timeout=5)
            except Exception:
                pass

    # ------------------------------------------------------------------
    # Case 5 — 不存在的pid拒绝attach(规则1)
    # ------------------------------------------------------------------
    def _test_nonexistent_pid_rejected(self):
        # 99999999 > pid_max(默认4194304),必然不存在
        pid = 99999999
        rc, out = _run_attach(pid, "display hook")
        if rc == 0:
            return Result(False, ["nonexistent pid rejected (rc != 0)"], [rc, out])
        if "process {} does not exist".format(pid) not in out:
            return Result(False, ["pid liveness error message"], [rc, out])
        return Result(True, [], [])

    # ------------------------------------------------------------------
    # Case 6 — 目标进程退出: 主动提示(无需输入)+仅exit/help可用
    # ------------------------------------------------------------------
    def _test_interactive_target_exit(self):
        sess = PtySession([MSMEM_SCOPE, "--pid", str(self._target_pid)],
                          _attach_env(DEFAULT_ATTACH_TIMEOUT_MS))
        try:
            sess.expect("[msmemscope] Attached to pid")

            # 杀掉目标进程:控制端空闲等待期间主动提示(不需输入任何命令)
            os.kill(self._target_pid, signal.SIGTERM)
            sess.expect("target process exited")

            # 非exit/help控制字:仅回显提示,会话不退出、不报错
            sess.send("display hook\r")
            sess.expect("target process exited")

            # help仍可用;exit正常收尾(rc 0)
            sess.send("help\r")
            sess.expect("enable tracing (same as msmemscope.start())")
            sess.send("exit\r")
            rc = sess.wait()
            if rc != 0:
                return Result(False, ["interactive exit rc 0 after target exit"], [rc])
            return Result(True, [], [])
        finally:
            sess.close()

    # -- lifecycle -----------------------------------------------------------

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()
