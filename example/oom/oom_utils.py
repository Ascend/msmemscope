# -------------------------------------------------------------------------
# This file is part of the MindStudio project.
# Copyright (c) 2025 Huawei Technologies Co.,Ltd.
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
OOM 测试工具库：提供 OOM 触发、dump 文件验证等公共方法。
"""

import os
import sys
import re
import glob
import logging
from typing import List, Dict, Optional, Tuple

import torch
import torch_npu

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("oom_test")


def get_device_memory_gb(device_id: int) -> float:
    """获取指定 NPU 卡的总显存（GB）。"""
    prop = torch.npu.get_device_properties(device_id)
    return prop.total_memory / (1024 ** 3)


def trigger_oom(device_id: int, target_fraction: float = 0.95) -> int:
    """
    在指定 NPU 卡上触发 OOM。

    通过循环分配大块 tensor 填满 NPU 显存，直到 halMemAlloc 返回 OOM 错误。
    PyTorch 捕获 HAL 层 OOM 后抛出 RuntimeError，但 msmemscope 的 HAL hook
    在此之前已经拦截并记录了 OOM 事件。

    Args:
        device_id: NPU 卡号
        target_fraction: 目标占用比例，用于控制触发 OOM 前的预分配量

    Returns:
        int: 成功分配的 tensor 数量
    """
    device = torch.device(f"npu:{device_id}")
    torch.npu.set_device(device)

    # 清理前次测试可能残留的显存状态
    torch.npu.empty_cache()
    torch.npu.synchronize(device)

    total_mem = torch.npu.get_device_properties(device).total_memory
    logger.info("NPU %d: total memory = %.1f GB", device_id, total_mem / (1024 ** 3))

    tensors = []
    # 每次分配 1GB (float32: 4 bytes per element)
    chunk_elements = 256 * 1024 * 1024

    try:
        while True:
            t = torch.zeros(chunk_elements, dtype=torch.float32, device=device)
            tensors.append(t)
            allocated_gb = len(tensors) * 1.0
            if len(tensors) % 5 == 0:
                logger.info("  allocated %d GB on npu:%d...", int(allocated_gb), device_id)
    except RuntimeError as e:
        allocated_gb = len(tensors) * 1.0
        logger.info("OOM triggered on npu:%d after allocating ~%.0f GB (%d chunks): %s",
                    device_id, allocated_gb, len(tensors), str(e).split("\n")[0])
    except Exception as e:
        logger.warning("Unexpected error during OOM trigger on npu:%d: %s", device_id, str(e)[:200])

    # 释放内存以便后续测试
    n_chunks = len(tensors)
    del tensors
    torch.npu.empty_cache()
    torch.npu.synchronize(device)

    return n_chunks


def do_training_work(device_id: int, steps: int = 3) -> None:
    """
    在指定 NPU 卡上跑一小段训练，用于在 OOM 前产生正常的 alloc/free 记录。

    这些 alloc/free 事件会被 HalAnalyzer/StepInnerAnalyzer 记录，
    使得 OOM 分析器在 OOM 时能查询到"最近申请未释放"的内存块。
    """
    device = torch.device(f"npu:{device_id}")
    torch.npu.set_device(device)

    class TinyModel(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.fc = torch.nn.Linear(512, 512)

        def forward(self, x):
            return self.fc(x)

    model = TinyModel().to(device)
    optimizer = torch.optim.SGD(model.parameters(), lr=0.01)
    criterion = torch.nn.MSELoss()

    for step in range(steps):
        x = torch.randn(64, 512, device=device)
        y = torch.randn(64, 512, device=device)
        out = model(x)
        loss = criterion(out, y)
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

    torch.npu.synchronize(device)
    logger.info("Training work done on npu:%d (%d steps)", device_id, steps)


def find_dump_csvs(output_dir: str) -> List[str]:
    """
    递归搜索 output_dir 下所有 memscope_dump_*.csv 文件，返回完整路径列表。
    """
    pattern = os.path.join(output_dir, "**", "memscope_dump_*.csv")
    files = sorted(glob.glob(pattern, recursive=True))
    return files


def parse_dump_csv(csv_path: str) -> "pd.DataFrame":
    """读取单个 dump CSV 文件，返回 DataFrame。"""
    import pandas as pd
    return pd.read_csv(csv_path)


def filter_oom_events(df: "pd.DataFrame") -> "pd.DataFrame":
    """从 DataFrame 中筛选出 Event == 'OOM_DETAIL' 的行。"""
    return df[df["Event"] == "OOM_DETAIL"].copy()


def parse_oom_attr(attr_str: str) -> Dict[str, str]:
    """
    解析 dump CSV 中 Attr 列的 JSON-like 字符串。

    格式示例:
      "{func:halMemAlloc,req_size:1073741824,flag:0,ret:7}"
      "{pool:PTA,ptr:0x7f...,size:524288000,timestamp:...,step:5,kernel:42,client:12345}"
    """
    # 去掉首尾的 { } 和引号
    inner = attr_str.strip().strip('"').strip("{").strip("}")
    result = {}
    for pair in inner.split(","):
        if ":" not in pair:
            continue
        key, _, val = pair.partition(":")
        result[key.strip()] = val.strip()
    return result


class OOMDumpVerifier:
    """
    验证 dump CSV 中的 OOM_DETAIL 事件是否符合设计文档预期。

    注意：一次测试中可能触发多次 OOM（PyTorch 在 HAL OOM 后会尝试释放缓存
    并重试），因此验证逻辑以 OOM 触发次数 N 为基准：
      - RECENT_ALLOC 总数 ≤ N × K
      - TOP_ALLOC 总数 ≤ N × K
      - 排序验证基于相邻 OOM_TRIGGER 分组
    """

    def __init__(self, csv_path: str):
        self.csv_path = csv_path
        self.df = parse_dump_csv(csv_path)
        self.oom_df = filter_oom_events(self.df)
        self.errors: List[str] = []
        self.warnings: List[str] = []

    @property
    def trigger_count(self) -> int:
        """OOM 触发次数。"""
        return len(self.oom_df[self.oom_df["Event Type"] == "OOM_TRIGGER"])

    def verify_all(self, expected_k: int = 10) -> bool:
        """执行全部验证项，返回是否全部通过。"""
        checks = [
            self.check_csv_columns,
            self.check_oom_events_exist,
            self.check_oom_trigger_fields,
            self.check_oom_recent_alloc_count,
            self.check_oom_top_alloc_count,
            self.check_oom_memrecord_fields,
            self.check_oom_recent_alloc_order,
            self.check_oom_top_alloc_order,
        ]

        all_passed = True
        for check_fn in checks:
            name = check_fn.__name__
            try:
                if not check_fn(expected_k):
                    all_passed = False
                    logger.error("[FAIL] %s", name)
                else:
                    logger.info("[PASS] %s", name)
            except Exception as e:
                all_passed = False
                logger.error("[FAIL] %s: %s", name, str(e))

        return all_passed

    def check_csv_columns(self, _k: int = 0) -> bool:
        """检查 CSV 列名是否符合预期。"""
        expected_cols = ["ID", "Event", "Event Type", "Name", "Timestamp(ns)",
                         "Process Id", "Thread Id", "Device Id",
                         "Ptr", "Attr", "Call Stack(Python)", "Call Stack(C)"]
        actual_cols = list(self.df.columns)
        if actual_cols != expected_cols:
            self.errors.append(f"CSV columns mismatch: expected {expected_cols}, got {actual_cols}")
            return False
        return True

    def check_oom_events_exist(self, _k: int = 0) -> bool:
        """检查 OOM_DETAIL 事件是否存在。"""
        if len(self.oom_df) == 0:
            self.errors.append("No OOM_DETAIL events found in CSV")
            return False
        logger.info("  Found %d OOM_DETAIL events", len(self.oom_df))
        return True

    def check_oom_trigger_fields(self, _k: int = 0) -> bool:
        """检查 OOM_TRIGGER 事件的 Attr 是否包含必要字段。"""
        trigger_df = self.oom_df[self.oom_df["Event Type"] == "OOM_TRIGGER"]
        if len(trigger_df) == 0:
            self.errors.append("No OOM_TRIGGER event found")
            return False

        required_fields = ["func", "req_size", "flag"]
        for _, row in trigger_df.iterrows():
            attr = parse_oom_attr(str(row["Attr"]))
            for field in required_fields:
                if field not in attr:
                    self.errors.append(
                        f"OOM_TRIGGER missing field '{field}' in attr: {row['Attr']}")
                    return False
            if attr.get("func", "") not in ("halMemAlloc", "halMemCreate"):
                self.warnings.append(
                    f"OOM_TRIGGER func={attr.get('func')} (expected halMemAlloc/halMemCreate)")

        logger.info("  OOM_TRIGGER count: %d", len(trigger_df))
        return True

    def check_oom_recent_alloc_count(self, expected_k: int) -> bool:
        """检查 OOM_RECENT_ALLOC 事件数量，总数 ≤ OOM次数 × K。"""
        recent_df = self.oom_df[self.oom_df["Event Type"] == "OOM_RECENT_ALLOC"]
        count = len(recent_df)
        n_triggers = max(self.trigger_count, 1)
        max_expected = n_triggers * expected_k

        if count == 0:
            self.errors.append("No OOM_RECENT_ALLOC events found (expected > 0)")
            return False
        if count > max_expected:
            self.errors.append(
                f"OOM_RECENT_ALLOC count {count} exceeds {n_triggers} triggers × K={expected_k} = {max_expected}")
            return False
        logger.info("  OOM_RECENT_ALLOC count: %d (≤ %d triggers × K=%d)", count, n_triggers, expected_k)
        return True

    def check_oom_top_alloc_count(self, expected_k: int) -> bool:
        """检查 OOM_TOP_ALLOC 事件数量，总数 ≤ OOM次数 × K。"""
        top_df = self.oom_df[self.oom_df["Event Type"] == "OOM_TOP_ALLOC"]
        count = len(top_df)
        n_triggers = max(self.trigger_count, 1)
        max_expected = n_triggers * expected_k

        if count == 0:
            self.errors.append("No OOM_TOP_ALLOC events found (expected > 0)")
            return False
        if count > max_expected:
            self.errors.append(
                f"OOM_TOP_ALLOC count {count} exceeds {n_triggers} triggers × K={expected_k} = {max_expected}")
            return False
        logger.info("  OOM_TOP_ALLOC count: %d (≤ %d triggers × K=%d)", count, n_triggers, expected_k)
        return True

    def check_oom_memrecord_fields(self, _k: int = 0) -> bool:
        """检查 OOM_RECENT_ALLOC / OOM_TOP_ALLOC 事件的 Attr 包含必要字段。"""
        mem_df = self.oom_df[self.oom_df["Event Type"].isin(
            ["OOM_RECENT_ALLOC", "OOM_TOP_ALLOC"])]
        if len(mem_df) == 0:
            self.errors.append("No OOM memory record events found")
            return False

        required_fields = ["pool", "ptr", "size", "timestamp"]
        for _, row in mem_df.iterrows():
            attr = parse_oom_attr(str(row["Attr"]))
            for field in required_fields:
                if field not in attr:
                    self.errors.append(
                        f"OOM mem record (Event Type={row['Event Type']}) "
                        f"missing field '{field}' in attr: {row['Attr']}")
                    return False
        return True

    def check_oom_recent_alloc_order(self, expected_k: int) -> bool:
        """检查每个 OOM 分组内的 OOM_RECENT_ALLOC 是否按 timestamp 降序排列。"""
        groups = self._split_by_trigger("OOM_RECENT_ALLOC", expected_k)
        if not groups:
            return True

        for gi, group in enumerate(groups):
            if len(group) < 2:
                continue
            timestamps = []
            for _, row in group.iterrows():
                attr = parse_oom_attr(str(row["Attr"]))
                ts = int(attr.get("timestamp", "0"))
                timestamps.append(ts)

            for i in range(len(timestamps) - 1):
                if timestamps[i] < timestamps[i + 1]:
                    self.warnings.append(
                        f"OOM_RECENT_ALLOC group {gi}: timestamp not descending at pos {i}, "
                        f"ts[{i}]={timestamps[i]} < ts[{i + 1}]={timestamps[i + 1]}")
        return True

    def check_oom_top_alloc_order(self, expected_k: int) -> bool:
        """检查每个 OOM 分组内的 OOM_TOP_ALLOC 是否按 size 降序排列。"""
        groups = self._split_by_trigger("OOM_TOP_ALLOC", expected_k)
        if not groups:
            return True

        for gi, group in enumerate(groups):
            if len(group) < 2:
                continue
            sizes = []
            for _, row in group.iterrows():
                attr = parse_oom_attr(str(row["Attr"]))
                sz = int(attr.get("size", "0"))
                sizes.append(sz)

            for i in range(len(sizes) - 1):
                if sizes[i] < sizes[i + 1]:
                    self.warnings.append(
                        f"OOM_TOP_ALLOC group {gi}: size not descending at pos {i}, "
                        f"size[{i}]={sizes[i]} < size[{i + 1}]={sizes[i + 1]}")
        return True

    def _split_by_trigger(self, event_type: str, k: int) -> List["pd.DataFrame"]:
        """
        将指定类型的事件按 OOM_TRIGGER 分组。

        CSV 中事件顺序为：
          ... OOM_TRIGGER, RECENT_ALLOC[0..K-1], TOP_ALLOC[0..K-1], OOM_TRIGGER, ...

        取两个 OOM_TRIGGER 之间的 event_type 事件作为一组。
        """
        oom_df = self.oom_df.reset_index(drop=True)
        trigger_indices = oom_df[oom_df["Event Type"] == "OOM_TRIGGER"].index.tolist()
        if not trigger_indices:
            return []

        groups = []
        for i, ti in enumerate(trigger_indices):
            # 当前 OOM_TRIGGER 之后、下一个 OOM_TRIGGER 之前的事件
            start = ti + 1
            end = trigger_indices[i + 1] if i + 1 < len(trigger_indices) else len(oom_df)
            group = oom_df.iloc[start:end]
            group = group[group["Event Type"] == event_type]
            if len(group) > 0:
                groups.append(group)

        return groups

    def summary(self) -> str:
        """打印验证摘要。"""
        lines = [f"\n=== OOM Dump Verification: {self.csv_path} ==="]
        lines.append(f"Total rows: {len(self.df)}")
        lines.append(f"OOM_DETAIL rows: {len(self.oom_df)}")

        for etype in ["OOM_TRIGGER", "OOM_RECENT_ALLOC", "OOM_TOP_ALLOC"]:
            count = len(self.oom_df[self.oom_df["Event Type"] == etype])
            lines.append(f"  {etype}: {count}")

        if self.errors:
            lines.append(f"\nERRORS ({len(self.errors)}):")
            for e in self.errors:
                lines.append(f"  - {e}")
        if self.warnings:
            lines.append(f"\nWARNINGS ({len(self.warnings)}):")
            for w in self.warnings:
                lines.append(f"  - {w}")
        if not self.errors:
            lines.append("\nResult: ALL CHECKS PASSED")

        return "\n".join(lines)
