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

"""NPU smoke for msMemScope Triton ACCESS path (assert + dump)."""

from __future__ import annotations

import csv
import shutil
import sys
from pathlib import Path

import torch
import torch_npu  # noqa: F401
import triton
import triton.language as tl
import msmemscope

_OUT = Path("./output")
_TOL = 1e-5
_KERNEL_TAG = "msm_access_smoke_axpy"


@triton.jit
def msm_access_smoke_axpy(
    src_a_ptr,
    src_b_ptr,
    dst_ptr,
    n_elems,
    BLOCK: tl.constexpr,
):
    """msMemScope ACCESS smoke: dst = src_a + src_b (grid-stride)."""
    lane = tl.program_id(0)
    stride = tl.num_programs(0)
    tiles = tl.cdiv(n_elems, BLOCK)
    tile = lane
    while tile < tiles:
        idx = tile * BLOCK + tl.arange(0, BLOCK)
        alive = idx < n_elems
        va = tl.load(src_a_ptr + idx, mask=alive)
        vb = tl.load(src_b_ptr + idx, mask=alive)
        tl.store(dst_ptr + idx, va + vb, mask=alive)
        tile += stride


def _launch_axpy(a: torch.Tensor, b: torch.Tensor) -> torch.Tensor:
    dst = torch.empty_like(a)
    n = int(a.numel())
    block = 1024
    grid = (min(65535, triton.cdiv(n, block)),)
    msm_access_smoke_axpy[grid](a, b, dst, n, BLOCK=block)
    return dst


def _norm_ptr(v: str) -> str:
    s = (v or "").strip().lower()
    if s.startswith("0x"):
        return hex(int(s, 16))
    try:
        return hex(int(s))
    except ValueError:
        return s


def _dump_has_kernel_access(out_dir: Path, expect: dict) -> bool:
    """Require ACCESS rows for this kernel with expected Ptr+Type pairs."""
    if not out_dir.is_dir():
        return False
    need = {(expect["a"], "READ"), (expect["b"], "READ"), (expect["dst"], "WRITE")}
    found = set()
    for path in out_dir.rglob("memscope_dump_*.csv"):
        try:
            with path.open(encoding="utf-8", errors="ignore", newline="") as fh:
                for row in csv.DictReader(fh):
                    # Dump schema: Event=ACCESS, Event Type=READ|WRITE, Ptr, Name
                    if (row.get("Event") or "").strip() != "ACCESS":
                        continue
                    name = row.get("Name") or ""
                    if _KERNEL_TAG not in name:
                        continue
                    ptr = _norm_ptr(row.get("Ptr") or "")
                    typ = (row.get("Event Type") or "").strip().upper()
                    if typ == "READ":
                        found.add((ptr, "READ"))
                    elif typ == "WRITE":
                        found.add((ptr, "WRITE"))
        except OSError:
            continue
    return need.issubset(found)


def main() -> int:
    shutil.rmtree(_OUT, ignore_errors=True)
    _OUT.mkdir(parents=True, exist_ok=True)

    msmemscope.config(
        events="alloc,free,access,launch",
        level="kernel,op",
        output=str(_OUT),
        data_format="csv",
    )
    device = torch.device("npu:0")
    torch.npu.set_device(device)

    a = torch.randn(4096, device=device, dtype=torch.float32)
    b = torch.randn(4096, device=device, dtype=torch.float32)

    msmemscope.start()
    out = _launch_axpy(a, b)
    expect = {
        "a": hex(int(a.data_ptr())),
        "b": hex(int(b.data_ptr())),
        "dst": hex(int(out.data_ptr())),
    }
    torch.npu.synchronize()
    msmemscope.stop()

    ref = a + b
    max_diff = (out - ref).abs().max().item()
    print(f"max diff vs torch add: {max_diff}")
    if max_diff > _TOL:
        print(f"FAIL: numerical mismatch max_diff={max_diff} > {_TOL}", file=sys.stderr)
        return 1
    if not _dump_has_kernel_access(_OUT, expect):
        print(
            f"FAIL: missing Triton ACCESS Ptr/Type for {_KERNEL_TAG} under {_OUT.resolve()} "
            f"(expect {expect})",
            file=sys.stderr,
        )
        return 1
    print("PASS: numerical OK + ACCESS dump present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
