#!/usr/bin/env python3
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

"""C++ gate E2E: event/level matrix x multi-kernel, no manual enable.

Requires rebuilt msmemscope with HandleWithTritonCollect (not Aten bridge).

Each state runs in a **subprocess** so msmemscope.config(output=...) is not sticky.
"""

# pylint: disable=duplicate-code,subprocess-run-check

from __future__ import annotations

import argparse
import csv
import os
import shutil
import subprocess
import sys
import traceback
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, List, Tuple


E2E_KERNELS = (
    "msm_e2e_pair_sum",
    "msm_e2e_pair_prod",
    "msm_e2e_clamp_pos",
    "msm_e2e_const_write",
    "msm_e2e_inplace_gain",
)

# Hardcoded per-kernel ACCESS (Ptr order = launch NPU arg order).
EXPECTED_RW = {
    "msm_e2e_pair_sum": ("READ", "READ", "WRITE"),
    "msm_e2e_pair_prod": ("READ", "READ", "WRITE"),
    "msm_e2e_clamp_pos": ("READ", "WRITE"),
    "msm_e2e_const_write": ("WRITE",),
    "msm_e2e_inplace_gain": ("WRITE",),
}


def _write_report(path: Path, text: str) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _assert_no_aten_bridge() -> None:
    import msmemscope.aten_collection as aten

    src = Path(aten.__file__).read_text(encoding="utf-8", errors="replace")
    if "msmemscope Triton bridge for #129" in src:
        raise RuntimeError("Aten bridge still installed - E2E must use rebuilt HandleWithTritonCollect")


def _norm_ptr(p) -> str:
    s = str(p).strip().lower()
    if s.startswith("0x"):
        return hex(int(s, 16))
    try:
        return hex(int(s))
    except Exception:
        return s


def _find_dumps(root: Path) -> List[Path]:
    return sorted(root.glob("**/device_*/dump/memscope_dump_*.csv"))


def _latest_dump(*roots: Path) -> Path:
    dumps: List[Path] = []
    for r in roots:
        if r.exists():
            dumps.extend(_find_dumps(r))
    dumps.extend(_find_dumps(Path("memscopeDumpResults")))
    dumps.extend(_find_dumps(Path(".")))
    if not dumps:
        raise FileNotFoundError(f"no dump under {[str(r) for r in roots]}")
    return max(dumps, key=lambda p: p.stat().st_mtime)


def _short_name(name: str) -> str:
    if "triton." in name:
        return name.split("triton.", 1)[-1]
    return name


def _is_e2e_kernel(name: str) -> bool:
    short = _short_name(name)
    return any(k in short for k in E2E_KERNELS)


def _collect_access_by_kernel(dump: Path) -> Dict[str, List[Tuple[str, str]]]:
    rows = list(csv.DictReader(dump.open(encoding="utf-8", errors="replace")))
    by: Dict[str, List[Tuple[str, str]]] = defaultdict(list)
    for r in rows:
        if r.get("Event") != "ACCESS":
            continue
        name = r.get("Name", "")
        if not _is_e2e_kernel(name):
            continue
        short = _short_name(name)
        for k in E2E_KERNELS:
            if k in short:
                by[k].append((_norm_ptr(r.get("Ptr", "")), r.get("Event Type", "").strip()))
                break
    return dict(by)


def _collect_launch_hits(dump: Path) -> int:
    """Count OP_LAUNCH / ATEN_START|END rows for E2E kernels (one hit per CSV row)."""
    rows = list(csv.DictReader(dump.open(encoding="utf-8", errors="replace")))
    n = 0
    for r in rows:
        name = r.get("Name", "")
        if not _is_e2e_kernel(name):
            continue
        blob = " ".join(
            (
                r.get("Event") or "",
                r.get("Event Type") or "",
                r.get("Attr") or "",
            )
        ).upper()
        if "ATEN_START" in blob or "ATEN_END" in blob or "OP_LAUNCH" in blob:
            n += 1
    return n


def _count_any_e2e(dump: Path) -> int:
    rows = list(csv.DictReader(dump.open(encoding="utf-8", errors="replace")))
    return sum(1 for r in rows if _is_e2e_kernel(r.get("Name") or ""))


WORKER = r'''
import json, sys
from pathlib import Path
import torch, torch_npu, triton, triton.language as tl
import msmemscope
from msmemscope import triton_collection as tc

@triton.jit
def msm_e2e_pair_sum(a_ptr, b_ptr, c_ptr, n_elems, BLOCK: tl.constexpr):
    i = tl.program_id(0)
    offs = i * BLOCK + tl.arange(0, BLOCK)
    m = offs < n_elems
    ta = tl.load(a_ptr + offs, mask=m)
    tb = tl.load(b_ptr + offs, mask=m)
    tl.store(c_ptr + offs, ta + tb, mask=m)

@triton.jit
def msm_e2e_pair_prod(a_ptr, b_ptr, c_ptr, n_elems, BLOCK: tl.constexpr):
    i = tl.program_id(0)
    offs = i * BLOCK + tl.arange(0, BLOCK)
    m = offs < n_elems
    ta = tl.load(a_ptr + offs, mask=m)
    tb = tl.load(b_ptr + offs, mask=m)
    tl.store(c_ptr + offs, ta * tb, mask=m)

@triton.jit
def msm_e2e_clamp_pos(src_ptr, dst_ptr, n_elems, BLOCK: tl.constexpr):
    i = tl.program_id(0)
    offs = i * BLOCK + tl.arange(0, BLOCK)
    m = offs < n_elems
    v = tl.load(src_ptr + offs, mask=m)
    tl.store(dst_ptr + offs, tl.maximum(v, 0.0), mask=m)

@triton.jit
def msm_e2e_const_write(dst_ptr, n_elems, fill_v, BLOCK: tl.constexpr):
    i = tl.program_id(0)
    offs = i * BLOCK + tl.arange(0, BLOCK)
    m = offs < n_elems
    tl.store(dst_ptr + offs, fill_v, mask=m)

@triton.jit
def msm_e2e_inplace_gain(buf_ptr, n_elems, gain, BLOCK: tl.constexpr):
    i = tl.program_id(0)
    offs = i * BLOCK + tl.arange(0, BLOCK)
    m = offs < n_elems
    v = tl.load(buf_ptr + offs, mask=m)
    tl.store(buf_ptr + offs, v * gain, mask=m)

cfg = json.loads(sys.argv[1])
out_dir = Path(cfg["out_dir"])
out_dir.mkdir(parents=True, exist_ok=True)
level = cfg.get("level", "kernel,op")
device = torch.device("npu:0")
torch.npu.set_device(device)
before = tc.get_triton_collector_stats().get("enable_calls", 0)
msmemscope.config(
    events=cfg["events"],
    level=level,
    output=str(out_dir.resolve()),
    data_format="csv",
)
msmemscope.start()
after = tc.get_triton_collector_stats().get("enable_calls", 0)

n, block = 1024, 256
grid = (triton.cdiv(n, block),)
ptrs = {}

xa = torch.randn(n, device=device)
ya = torch.randn(n, device=device)
oa = torch.empty_like(xa)
ptrs["msm_e2e_pair_sum"] = [int(xa.data_ptr()), int(ya.data_ptr()), int(oa.data_ptr())]
msm_e2e_pair_sum[grid](xa, ya, oa, n, BLOCK=block)

xm = torch.randn(n, device=device)
ym = torch.randn(n, device=device)
om = torch.empty_like(xm)
ptrs["msm_e2e_pair_prod"] = [int(xm.data_ptr()), int(ym.data_ptr()), int(om.data_ptr())]
msm_e2e_pair_prod[grid](xm, ym, om, n, BLOCK=block)

xr = torch.randn(n, device=device)
or_ = torch.empty_like(xr)
ptrs["msm_e2e_clamp_pos"] = [int(xr.data_ptr()), int(or_.data_ptr())]
msm_e2e_clamp_pos[grid](xr, or_, n, BLOCK=block)

of = torch.empty(n, device=device)
ptrs["msm_e2e_const_write"] = [int(of.data_ptr())]
msm_e2e_const_write[grid](of, n, 1.5, BLOCK=block)

xi = torch.randn(n, device=device)
ptrs["msm_e2e_inplace_gain"] = [int(xi.data_ptr())]
msm_e2e_inplace_gain[grid](xi, n, 2.0, BLOCK=block)

torch.npu.synchronize()
assert torch.allclose(oa, xa + ya)
assert torch.allclose(om, xm * ym)
assert torch.allclose(or_, torch.relu(xr))
assert torch.allclose(of, torch.full_like(of, 1.5))

msmemscope.stop()
emitted_before = tc.get_triton_collector_stats().get("access_emitted", 0)
msm_e2e_pair_sum[grid](xa, ya, oa, n, BLOCK=block)
torch.npu.synchronize()
emitted_after = tc.get_triton_collector_stats().get("access_emitted", 0)
print(json.dumps({
    "before": before,
    "after": after,
    "ptrs": ptrs,
    "out_dir": str(out_dir.resolve()),
    "level": level,
    "post_stop_emitted_before": emitted_before,
    "post_stop_emitted_after": emitted_after,
}))
'''




def _run_worker(events: str, out_dir: Path, *, level: str = "kernel,op") -> dict:
    import json
    import tempfile

    cfg = json.dumps({"events": events, "out_dir": str(out_dir.resolve()), "level": level})
    env = os.environ.copy()
    with tempfile.NamedTemporaryFile(
        mode="w",
        suffix="_e2e_worker.py",
        prefix="msmemscope_",
        delete=False,
        encoding="utf-8",
    ) as tf:
        tf.write(WORKER)
        worker_path = tf.name
    try:
        proc = subprocess.run(
            [sys.executable, worker_path, cfg],
            cwd=str(Path.cwd()),
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
    finally:
        try:
            os.unlink(worker_path)
        except OSError:
            pass
    if proc.returncode != 0:
        raise RuntimeError(f"worker failed rc={proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    lines = [ln for ln in proc.stdout.splitlines() if ln.strip().startswith("{")]
    if not lines:
        raise RuntimeError(f"no json from worker\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    return json.loads(lines[-1])


def _check_post_stop(meta: dict, lines: List[str], label: str) -> None:
    ps_b = int(meta.get("post_stop_emitted_before", -1))
    ps_a = int(meta.get("post_stop_emitted_after", -2))
    if ps_b != ps_a:
        raise AssertionError(f"{label}: post-stop ACCESS emit grew {ps_b} -> {ps_a}")
    lines.append(f"  PASS: post-stop access_emitted stable ({ps_b})")


def _check_access_map(meta: dict, by_k: Dict[str, List[Tuple[str, str]]], label: str) -> None:
    ptrs = meta["ptrs"]
    for k, expected_rw in EXPECTED_RW.items():
        got = Counter(by_k.get(k, []))
        exp = Counter([(_norm_ptr(ptrs[k][i]), expected_rw[i]) for i in range(len(expected_rw))])
        if got != exp:
            raise AssertionError(f"{label}/{k}: ACCESS {got} != expected {exp}")
    # all five kernels must appear
    missing = [k for k in E2E_KERNELS if k not in by_k]
    if missing:
        raise AssertionError(f"{label}: missing ACCESS for {missing}")


def run_state(
    *,
    label: str,
    events: str,
    out_root: Path,
    level: str = "kernel,op",
    expect_access: bool,
    expect_launch: bool,
    expect_none: bool,
    expect_enable: bool = True,
) -> str:
    out_dir = (out_root / label).resolve()
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    meta = _run_worker(events, out_dir, level=level)
    enable_delta = int(meta["after"]) - int(meta["before"])
    dump = _latest_dump(out_dir, Path("memscopeDumpResults"))
    by_k = _collect_access_by_kernel(dump)
    launch_n = _collect_launch_hits(dump)
    any_e2e = _count_any_e2e(dump)

    lines = [
        f"[{label}] events={events} level={level}",
        f"  enable_calls delta={enable_delta} (before={meta['before']} after={meta['after']})",
        f"  dump={dump}",
        f"  access_kernels={ {k: [t for _, t in v] for k, v in by_k.items()} }",
        f"  launch_hits={launch_n} any_e2e_rows={any_e2e}",
    ]

    if expect_enable:
        if enable_delta < 1:
            raise AssertionError(
                f"{label}: enable_calls did not increase - C++ HandleWithTritonCollect "
                f"likely not invoked (before={meta['before']} after={meta['after']})"
            )
    else:
        if enable_delta != 0:
            raise AssertionError(f"{label}: gate should NOT enable, enable_calls+={enable_delta}")
        lines.append("  PASS: collector not enabled (gate off)")

    if expect_none:
        # No e2e ACCESS; launch rows may still exist for kernel-level tracing.
        if by_k:
            raise AssertionError(f"{label}: expected no e2e ACCESS, got {by_k}")
        if expect_enable is False and enable_delta != 0:
            raise AssertionError(f"{label}: enable_calls+={enable_delta}")
        lines.append("  PASS: no e2e ACCESS")
        _check_post_stop(meta, lines, label)
        return "\n".join(lines)

    if expect_access:
        _check_access_map(meta, by_k, label)
        lines.append(f"  PASS: ACCESS R/W OK for {len(E2E_KERNELS)} kernels via (Ptr, Type)")
    else:
        if by_k:
            raise AssertionError(f"{label}: expected no ACCESS, got {by_k}")
        lines.append("  PASS: no ACCESS (launch-only / gate-off)")

    if expect_launch:
        if launch_n < 1 and any_e2e < 1:
            raise AssertionError(f"{label}: expected OP_LAUNCH / ATEN_* for e2e kernels")
        lines.append("  PASS: triton launch/OP_LAUNCH present")
    elif not expect_none:
        # access_only / gate-off with enable: dump must not carry OP_LAUNCH rows
        if launch_n != 0:
            raise AssertionError(f"{label}: expect_launch=False but launch_hits={launch_n}")
        lines.append("  PASS: no OP_LAUNCH rows (events omit launch)")

    _check_post_stop(meta, lines, label)
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path("e2e_gate.log"))
    parser.add_argument("--workdir", type=Path, default=Path("./output_e2e_gate"))
    args = parser.parse_args()

    report: List[str] = []
    try:
        _assert_no_aten_bridge()
        report.append("bridge_check: OK (no Aten #129 bridge)")
        import triton
        import torch_npu

        report.append(f"env: triton={triton.__version__} torch_npu={getattr(torch_npu, '__version__', '?')}")
        report.append(f"kernels: {', '.join(E2E_KERNELS)} ({len(E2E_KERNELS)} per state)")

        if args.workdir.exists():
            shutil.rmtree(args.workdir)
        args.workdir.mkdir(parents=True)

        # --- event matrix (level=kernel,op) ---
        report.append(
            run_state(
                label="state_access",
                events="alloc,free,access,launch",
                out_root=args.workdir,
                expect_access=True,
                expect_launch=True,
                expect_none=False,
                expect_enable=True,
            )
        )
        report.append(
            run_state(
                label="state_access_only",
                events="alloc,free,access",
                out_root=args.workdir,
                expect_access=True,
                expect_launch=False,
                expect_none=False,
                expect_enable=True,
            )
        )
        report.append(
            run_state(
                label="state_launch_only",
                events="alloc,free,launch",
                out_root=args.workdir,
                expect_access=False,
                expect_launch=True,
                expect_none=False,
                expect_enable=True,
            )
        )
        report.append(
            run_state(
                label="state_alloc_free_only",
                events="alloc,free",
                out_root=args.workdir,
                expect_access=False,
                expect_launch=False,
                expect_none=True,
                expect_enable=False,
            )
        )
        # --- level matrix ---
        report.append(
            run_state(
                label="state_level_op_only",
                events="alloc,free,access,launch",
                level="op",
                out_root=args.workdir,
                expect_access=True,
                expect_launch=True,
                expect_none=False,
                expect_enable=True,
            )
        )
        report.append(
            run_state(
                label="state_level_kernel_only",
                events="alloc,free,access,launch",
                level="kernel",
                out_root=args.workdir,
                expect_access=False,
                expect_launch=False,
                expect_none=True,
                expect_enable=False,
            )
        )

        n_states = 6
        report.append(f"TOTAL: E2E gate {n_states}/{n_states} PASS ({len(E2E_KERNELS)} kernels x states + post-stop)")
        text = "\n".join(report) + "\n"
        try:
            _write_report(args.out, text)
        except OSError as exc:
            print(f"WARN: failed to write report {args.out}: {exc}", file=sys.stderr)
        print(text)
        return 0
    except Exception as exc:
        report.append(f"FAIL: {exc}")
        report.append(traceback.format_exc())
        text = "\n".join(report) + "\n"
        try:
            _write_report(args.out, text)
        except OSError as write_exc:
            print(f"WARN: failed to write report {args.out}: {write_exc}", file=sys.stderr)
        print(text)
        return 1


if __name__ == "__main__":
    sys.exit(main())
