#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
"""
test/unit_python 统一 UT 执行入口(与 test/smoke/run_st.py 同级对应)

自动发现本目录下所有 test_*.py 单元测试并逐个执行(每个文件在独立子进程
中运行, 避免各测试文件 import 期对 sys.modules 的 mock 注入互相污染),
执行完成后汇总结果并返回进程退出码。

用法(等价于逐个执行 `python -m unittest -v <module>`):
    cd test/unit_python && python run_ut.py                    # 执行全部
    python run_ut.py -f test_hijack_map test_taggers           # 执行指定文件
    python run_ut.py -q                                        # 仅汇总, 不展示用例详情
"""

import argparse
import glob
import os
import subprocess
import sys
import time

_BASE_DIR = os.path.dirname(os.path.abspath(__file__))


def _normalize_module(name: str) -> str:
    """兼容传入文件名/模块名/带路径的写法, 统一为无后缀模块名"""
    return os.path.splitext(os.path.basename(name))[0]


def discover_test_modules() -> list[str]:
    """发现本目录下所有 test_*.py 测试模块(排序保证执行顺序稳定)"""
    files = sorted(glob.glob(os.path.join(_BASE_DIR, "test_*.py")))
    return [_normalize_module(f) for f in files]


def run_module(module_name: str, quiet: bool) -> bool:
    """以 unittest 在子进程中运行单个测试模块(与各文件头注释的运行方式一致)"""
    cmd = [sys.executable, "-m", "unittest", "-v", module_name]
    devnull = subprocess.DEVNULL if quiet else None
    proc = subprocess.run(cmd, cwd=_BASE_DIR, stdout=devnull, stderr=devnull)
    return proc.returncode == 0


def report_summary(results: list[tuple[str, bool, float]]) -> bool:
    """打印每个文件的通过/失败/耗时并返回整体是否全部通过"""
    total = len(results)
    passed = sum(1 for _, ok, _ in results if ok)

    print(f"\n{'=' * 64}")
    print(f"{'UT 文件':<32}{'结果':<8}{'耗时':>10}")
    print("-" * 64)
    for module, ok, elapsed in results:
        print(f"{module:<32}{'PASS' if ok else 'FAIL':<8}{elapsed:>8.2f}s")
    print("-" * 64)
    if passed == total:
        print(f"全部通过: {total} 个测试文件")
    else:
        print(f"失败 {total - passed}/{total} 个测试文件")
    print(f"{'=' * 64}")
    return passed == total


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="test/unit_python 统一 UT 执行入口")
    parser.add_argument("-f", "--file", nargs="+", metavar="MODULE",
                        help="仅执行指定的测试模块(可带 .py 后缀), 默认执行全部 test_*.py")
    parser.add_argument("-q", "--quiet", action="store_true",
                        help="不输出用例详情, 仅输出每个文件的通过/失败汇总")
    args = parser.parse_args(argv)

    if args.file:
        modules = [_normalize_module(m) for m in args.file]
        missing = [m for m in modules if not os.path.exists(os.path.join(_BASE_DIR, m + ".py"))]
        if missing:
            print(f"未找到测试文件: {', '.join(missing)} (搜索目录: {_BASE_DIR})")
            return 2
    else:
        modules = discover_test_modules()
        if not modules:
            print(f"未发现任何 test_*.py 测试文件 (搜索目录: {_BASE_DIR})")
            return 2

    print(f"执行 {len(modules)} 个 UT 文件 (目录: {_BASE_DIR})")
    results = []
    for module in modules:
        start = time.time()
        ok = run_module(module, args.quiet)
        results.append((module, ok, time.time() - start))

    return 0 if report_summary(results) else 1


if __name__ == "__main__":
    sys.exit(main())
