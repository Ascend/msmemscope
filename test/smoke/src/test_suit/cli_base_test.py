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

"""
CLI base test suite for msmemscope --load-api-env / --unload-api-env subcommands.

Covers the three most essential scenarios defined in
docs/rfc/2026-07-14-one-click-env-setup.md Section 4 (Test Design).
"""

import logging
import os
import re
import shutil
import subprocess
import tempfile

from .base_test import BaseTest, TestSuite
from ..utils.result import Result
from ..utils.utils import ColorText

# ---------------------------------------------------------------------------
# Constants matching the hook .so list in output/bin/msmemscope
# ---------------------------------------------------------------------------
HOOK_SOS = [
    "libleaks_ascend_hal_hook.so",
    "libascend_mstx_hook.so",
    "libascend_kernel_hook.so",
    "libatb_abi_0_hook.so",
    "libatb_abi_1_hook.so",
]

# Resolve repo root relative to this source file.
# test/smoke/src/test_suit/cli_base_test.py  ->  4 levels up  ->  repo root
_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "..")
)
_SCRIPT_SRC = os.path.join(_REPO_ROOT, "output", "bin", "msmemscope")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _run_bash_combined(script_text):
    """Run a snippet of bash; return (returncode, stdout+stderr combined)."""
    p = subprocess.run(
        ["bash", "-c", script_text],
        capture_output=True,
        text=True,
        timeout=30,
    )
    return p.returncode, p.stdout + p.stderr


def _count_in_colon_sep(value, substring):
    """Count entries in a colon-separated string that contain *substring*."""
    if not value:
        return 0
    return sum(1 for entry in value.split(":") if substring in entry)


def _extract_var(var_name, text):
    """Parse ``VAR_NAME=value`` from combined stdout+stderr of a bash run."""
    pattern = r"^{}=(.*)$".format(var_name)
    for line in text.splitlines():
        m = re.match(pattern, line.strip())
        if m:
            return m.group(1)
    return ""


# ---------------------------------------------------------------------------
# Test Suite
# ---------------------------------------------------------------------------

class CliBaseTestSuite(TestSuite):
    """Smoke-test suite for msmemscope --load-api-env / --unload-api-env.

    Creates a temporary fixture that mimics a full msmemscope installation.
    """

    def __init__(self, name: str, config, work_path: str, cmd: str, max_time: int):
        super().__init__(name, config, work_path, cmd, max_time)

        test_cases = [
            CliBaseTestCase("source_guard_direct_exec_errors", work_path),
            CliBaseTestCase("load_api_env_sets_ld_preload_and_library_path", work_path),
            CliBaseTestCase("unload_api_env_removes_only_msmemscope_entries", work_path),
        ]
        _ = list(map(self.register, test_cases))

    def __str__(self):
        return (
            f"cli_base test suite. suite name: {self.name}, "
            f"suite work path: {self._work_path}"
        )

    # -- fixture management --------------------------------------------------

    def set_up(self):
        super().set_up()
        self._fixture_dir = self._create_fixture()

    def tear_down(self):
        if hasattr(self, "_fixture_dir") and os.path.isdir(self._fixture_dir):
            shutil.rmtree(self._fixture_dir, ignore_errors=True)
        super().tear_down()

    def _create_fixture(self):
        """Build a minimal msmemscope installation tree and return its root."""
        fixture = tempfile.mkdtemp(prefix="msmemscope_cli_fixture_")

        bin_dir = os.path.join(fixture, "output", "bin")
        lib64_dir = os.path.join(fixture, "output", "lib64")
        for d in (bin_dir, lib64_dir):
            os.makedirs(d)

        # Copy the real msmemscope wrapper script
        shutil.copy(_SCRIPT_SRC, os.path.join(bin_dir, "msmemscope"))
        os.chmod(os.path.join(bin_dir, "msmemscope"), 0o755)

        # Dummy msmemscope.bin for normal-mode passthrough
        dummy_bin = os.path.join(bin_dir, "msmemscope.bin")
        with open(dummy_bin, "w") as f:
            f.write("#!/bin/bash\necho 'msmemscope.bin dummy called with args:' \"$@\"\n")
        os.chmod(dummy_bin, 0o755)

        # Dummy hook .so files (empty — only existence is checked)
        for so in HOOK_SOS:
            with open(os.path.join(lib64_dir, so), "w") as f:
                f.write("")

        logging.info("CLI fixture created at %s", fixture)
        return fixture

    @property
    def script_path(self):
        return os.path.join(self._fixture_dir, "output", "bin", "msmemscope")

    @property
    def lib64_path(self):
        return os.path.join(self._fixture_dir, "output", "lib64")


# ---------------------------------------------------------------------------
# Test Case
# ---------------------------------------------------------------------------

class CliBaseTestCase(BaseTest):
    """Single env-setup test case dispatched by *name*."""

    def __init__(self, name: str, work_path: str):
        super().__init__(name)
        self._work_path = work_path

    def __str__(self):
        return f"cli_base test case. case name: {self.name}"

    @property
    def _suite(self):
        return self.parent

    @property
    def _script(self):
        return self._suite.script_path

    @property
    def _lib64(self):
        return self._suite.lib64_path

    # -- dispatch ------------------------------------------------------------

    def run(self) -> Result:
        super().run()
        logging.debug("run %s", self)
        print(f"{ColorText.run_test} {self}")

        dispatch = {
            "source_guard_direct_exec_errors": self._test_source_guard,
            "load_api_env_sets_ld_preload_and_library_path": self._test_load_api_env,
            "unload_api_env_removes_only_msmemscope_entries": self._test_unload_api_env,
        }

        result = dispatch.get(self._name, lambda: Result(False, [], []))()
        self.report(result)
        return result

    # ------------------------------------------------------------------
    # Case 1 — source guard: direct execution (not sourced) must error
    # ------------------------------------------------------------------
    def _test_source_guard(self):
        rc, combined = _run_bash_combined(
            'bash "{}" --load-api-env'.format(self._script)
        )
        if rc == 0:
            return Result(False, ["non-zero exit"], [rc])
        if "must be sourced" not in combined.lower():
            return Result(False, ["message 'must be sourced'"], [combined.strip()])
        return Result(True, [], [])

    # ------------------------------------------------------------------
    # Case 2 — --load-api-env: correctly sets LD_PRELOAD + LD_LIBRARY_PATH
    # ------------------------------------------------------------------
    def _test_load_api_env(self):
        rc, combined = _run_bash_combined(
            'unset LD_PRELOAD; unset LD_LIBRARY_PATH; '
            'source "{}" --load-api-env; '
            'echo "PRELOAD=${{LD_PRELOAD}}"; '
            'echo "LIB_PATH=${{LD_LIBRARY_PATH}}"'.format(self._script)
        )
        if rc != 0:
            return Result(False, ["exit 0"], ["exit {}".format(rc), combined])

        preload = _extract_var("PRELOAD", combined)
        lib_path = _extract_var("LIB_PATH", combined)

        # LD_LIBRARY_PATH: lib64 must be prepended
        if not lib_path.startswith(self._lib64):
            return Result(
                False,
                ["LD_LIBRARY_PATH starts with {}".format(self._lib64)],
                [lib_path],
            )

        # LD_PRELOAD: all 5 hook .so paths must be present
        for so in HOOK_SOS:
            expected = os.path.join(self._lib64, so)
            if expected not in preload:
                return Result(
                    False, ["LD_PRELOAD contains {}".format(expected)], [preload]
                )

        # Exactly 5 msmemscope entries, nothing duplicated
        count = _count_in_colon_sep(preload, self._lib64)
        if count != 5:
            return Result(
                False,
                ["5 msmemscope entries in LD_PRELOAD", "got {}".format(count)],
                [preload],
            )

        return Result(True, [], [])

    # ------------------------------------------------------------------
    # Case 3 — --unload-api-env: removes only msmemscope entries,
    #           preserves other tools' entries
    # ------------------------------------------------------------------
    def _test_unload_api_env(self):
        rc, combined = _run_bash_combined(
            'export LD_LIBRARY_PATH="{}:/usr/local/lib:/opt/cann/lib64"; '
            'export LD_PRELOAD="{}/libascend_kernel_hook.so:{}/libascend_mstx_hook.so:/opt/other/hook.so"; '
            'source "{}" --unload-api-env; '
            'echo "PRELOAD=${{LD_PRELOAD}}"; '
            'echo "LIB_PATH=${{LD_LIBRARY_PATH}}"'.format(
                self._lib64, self._lib64, self._lib64, self._script
            )
        )
        if rc != 0:
            return Result(False, ["exit 0"], ["exit {}".format(rc), combined])

        preload = _extract_var("PRELOAD", combined)
        lib_path = _extract_var("LIB_PATH", combined)

        # msmemscope lib64 removed from LD_LIBRARY_PATH
        if self._lib64 in lib_path:
            return Result(
                False,
                ["{} removed from LD_LIBRARY_PATH".format(self._lib64)],
                [lib_path],
            )

        # msmemscope .so removed from LD_PRELOAD
        if self._lib64 in preload:
            return Result(
                False,
                ["{} removed from LD_PRELOAD".format(self._lib64)],
                [preload],
            )

        # Non-msmemscope entries survive
        for expected, actual, label in [
            ("/usr/local/lib", lib_path, "LD_LIBRARY_PATH"),
            ("/opt/cann/lib64", lib_path, "LD_LIBRARY_PATH"),
            ("/opt/other/hook.so", preload, "LD_PRELOAD"),
        ]:
            if expected not in actual:
                return Result(
                    False, ["{} in {}".format(expected, label)], [actual]
                )

        return Result(True, [], [])

    # -- lifecycle -----------------------------------------------------------

    def set_up(self):
        super().set_up()

    def tear_down(self):
        super().tear_down()
