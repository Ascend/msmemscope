#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
"""
memscope_dump CSV 数据一致性检查工具。

对 pd.concat 合并后的 DataFrame 执行以下检查项：
1. used 一致性检查：将 PTA 与 PTA_WORKSPACE 视为同一内存池，
   按设备对 MALLOC/FREE 事件按时间累积内存（MALLOC 累加 size、FREE 累减 size），
   与 Attr 中的 used 字段对比，输出最大差值。
2. allocation_id 配对检查：allocation_id 在 MALLOC 时分配且唯一，FREE 携带同一 id，
   同一 id 的 MALLOC 与 FREE 应成对出现，报告悬空申请/释放。
"""

import logging
import re
from typing import Dict, Tuple

import numpy as np
import pandas as pd

from .result import Result

# PTA 与 PTA_WORKSPACE 视为同一个内存池
TARGET_EVENT_TYPES = ("PTA", "PTA_WORKSPACE")
TARGET_EVENTS = ("MALLOC", "FREE")

# 匹配 {key:value,key:value,...} 中的 key:value 对
_ATTR_PATTERN = re.compile(r"(\w+):(.*?)(?=,\s*\w+:|$)")


def parse_attr(attr_str) -> Dict:
    """解析 Attr 字段内容 {key:value,...} 为 dict。

    - key 为字母/数字/下划线组成的标识符
    - value 为数字（含负数、0x十六进制）或字符串，空值为 None
    """
    if not isinstance(attr_str, str) or not attr_str.strip():
        return {}
    attr_str = attr_str.strip()
    if attr_str.startswith("{") and attr_str.endswith("}"):
        attr_str = attr_str[1:-1]
    if not attr_str:
        return {}
    result = {}
    for match in _ATTR_PATTERN.finditer(attr_str):
        key = match.group(1)
        value = match.group(2).strip()
        if value == "":
            result[key] = None
            continue
        try:
            result[key] = int(value, 16) if value.lower().startswith("0x") else int(value)
        except ValueError:
            result[key] = value
    return result


def check_used_consistency(data: pd.DataFrame,
                           event_types: Tuple[str, ...] = TARGET_EVENT_TYPES) -> Result:
    """检查项 1：内存累积值与 used 字段的一致性。

    按 Device Id 分组，对 MALLOC/FREE 事件按 Timestamp(ns) 升序累积内存，
    当 Attr 含 used 字段时比较 |累积值 - used|，输出最大差值。
    """
    if not isinstance(data, pd.DataFrame) or data.empty:
        logging.info("dump csv check: empty data, skip used consistency check")
        return Result(True, [], [])

    mask = data["Event"].isin(TARGET_EVENTS) & data["Event Type"].isin(event_types)
    df = data[mask].copy()
    if df.empty:
        logging.info("dump csv check: no %s events, skip used consistency check", event_types)
        return Result(True, [], [])

    required = {"Timestamp(ns)", "Attr", "Device Id"}
    missing = required - set(data.columns)
    if missing:
        logging.error("dump csv check: required columns %s not found in data", missing)
        return Result(False, [sorted(required)], [data.columns.tolist()])

    df["Timestamp(ns)"] = pd.to_numeric(df["Timestamp(ns)"], errors="coerce")
    df = df.dropna(subset=["Timestamp(ns)"]).sort_values(["Device Id", "Timestamp(ns)"])

    attrs = df["Attr"].apply(parse_attr).apply(pd.Series)
    df = pd.concat([df, attrs], axis=1)

    # MALLOC 累加 size、FREE 累减 size（size 均为正数），缺失 size 的行不参与累积
    size_col = df.get("size")
    if size_col is None:
        size = pd.Series(0, index=df.index)
    else:
        size = pd.to_numeric(size_col, errors="coerce").fillna(0)
    delta = np.where(df["Event"] == "MALLOC", size, -size)
    df["accumulated"] = pd.Series(delta, index=df.index).groupby(df["Device Id"]).cumsum()

    used = pd.to_numeric(df.get("used"), errors="coerce") if "used" in df.columns else None
    if used is None:
        logging.info("dump csv check: no attr contains 'used', skip used consistency check")
        return Result(True, [], [])
    checked = used.notna()
    if not checked.any():
        logging.info("dump csv check: no attr contains 'used', skip used consistency check")
        return Result(True, [], [])

    diff = (df.loc[checked, "accumulated"] - used[checked]).abs()
    max_diff = diff.max()
    worst = df.loc[diff.idxmax()]

    logging.info("dump csv check: %s events=%d, checked used=%d, max diff=%s",
                 event_types, len(df), int(checked.sum()), max_diff)
    if max_diff > 0:
        logging.error("dump csv check: used mismatch, max diff=%s, ID=%s, Event=%s, "
                      "Event Type=%s, Timestamp(ns)=%s, accumulated=%s, used=%s",
                      max_diff, worst.get("ID"), worst.get("Event"), worst.get("Event Type"),
                      worst.get("Timestamp(ns)"), worst["accumulated"], worst.get("used"))
        return Result(False, ["max used diff = 0"], [max_diff])
    return Result(True, [], [])


def check_allocation_pairs(data: pd.DataFrame,
                           event_types: Tuple[str, ...] = TARGET_EVENT_TYPES) -> Result:
    """检查项 2：allocation_id 配对校验。

    同一 allocation_id 的 MALLOC 与 FREE 应一一对应，
    MALLOC 多于 FREE 为悬空申请，FREE 多于 MALLOC 为悬空释放。
    """
    if not isinstance(data, pd.DataFrame) or data.empty:
        logging.info("dump csv check: empty data, skip allocation_id check")
        return Result(True, [], [])

    mask = data["Event"].isin(TARGET_EVENTS) & data["Event Type"].isin(event_types)
    df = data[mask].copy()
    if df.empty:
        logging.info("dump csv check: no %s events, skip allocation_id check", event_types)
        return Result(True, [], [])

    if "Attr" not in df.columns:
        logging.error("dump csv check: required column 'Attr' not found in data")
        return Result(False, [["Attr"]], [data.columns.tolist()])

    attrs = df["Attr"].apply(parse_attr)
    df["allocation_id"] = attrs.apply(lambda d: d.get("allocation_id"))
    df["size"] = attrs.apply(lambda d: d.get("size"))
    df["addr"] = attrs.apply(lambda d: d.get("addr"))
    df = df[df["allocation_id"].notna()]
    if df.empty:
        logging.info("dump csv check: no attr contains 'allocation_id', skip allocation_id check")
        return Result(True, [], [])

    # 按 allocation_id 统计 MALLOC/FREE 次数
    counts = df.groupby("allocation_id")["Event"].value_counts().unstack(fill_value=0)
    for event in TARGET_EVENTS:
        if event not in counts.columns:
            counts[event] = 0

    dangling_malloc_ids = counts.index[counts["MALLOC"] > counts["FREE"]].tolist()
    dangling_free_ids = counts.index[counts["FREE"] > counts["MALLOC"]].tolist()

    for alloc_id in dangling_malloc_ids:
        group = df[df["allocation_id"] == alloc_id]
        excess_malloc = group[group["Event"] == "MALLOC"].tail(
            int(len(group[group["Event"] == "MALLOC"]) - len(group[group["Event"] == "FREE"])))
        for _, row in excess_malloc.iterrows():
            logging.error("dump csv check: dangling MALLOC, allocation_id=%s, ID=%s, "
                          "Timestamp(ns)=%s, size=%s, addr=%s, Device Id=%s",
                          alloc_id, row.get("ID"), row.get("Timestamp(ns)"),
                          row.get("size"), row.get("addr"), row.get("Device Id"))

    for alloc_id in dangling_free_ids:
        group = df[df["allocation_id"] == alloc_id]
        excess_free = group[group["Event"] == "FREE"].tail(
            int(len(group[group["Event"] == "FREE"]) - len(group[group["Event"] == "MALLOC"])))
        for _, row in excess_free.iterrows():
            logging.error("dump csv check: dangling FREE, allocation_id=%s, ID=%s, "
                          "Timestamp(ns)=%s, size=%s, addr=%s, Device Id=%s",
                          alloc_id, row.get("ID"), row.get("Timestamp(ns)"),
                          row.get("size"), row.get("addr"), row.get("Device Id"))

    if dangling_malloc_ids or dangling_free_ids:
        logging.error("dump csv check: allocation_id mismatch, dangling MALLOC=%d, dangling FREE=%d",
                      len(dangling_malloc_ids), len(dangling_free_ids))
        return Result(False, ["MALLOC count == FREE count per allocation_id"],
                      [f"dangling MALLOC: {len(dangling_malloc_ids)}, "
                       f"dangling FREE: {len(dangling_free_ids)}"])
    logging.info("dump csv check: allocation_id pairs OK, total ids=%d", len(counts))
    return Result(True, [], [])


def check_memscope_dump(data: pd.DataFrame) -> Result:
    """对单个 memscope_dump 数据执行全部一致性检查。

    注意：allocation_id 只在同一文件内唯一，多文件数据须分别检查，
    合并后再检查会因不同文件 allocation_id 重复而误报。
    """
    result = check_used_consistency(data)
    if not result.success:
        return result
    return check_allocation_pairs(data)


def check_memscope_dump_files(dfs) -> Result:
    """对 pd.concat 合并前的各个 DataFrame 分别执行全部一致性检查。

    不同 CSV 文件（不同进程/设备）中的 allocation_id 可能重复，
    内存累计与配对校验均须按文件独立进行。
    """
    for df in dfs:
        result = check_memscope_dump(df)
        if not result.success:
            return result
    return Result(True, [], [])
