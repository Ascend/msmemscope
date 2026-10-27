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

"""NPU smoke: Triton vector-add + ACCESS dump check (fails hard on regression)."""

from __future__ import annotations

import sys
from pathlib import Path

import torch
import torch_npu  # noqa: F401
import triton
import triton.language as tl
import msmemscope

_OUT = Path("./output")
_TOL = 1e-5


@triton.jit
def add_kernel(x_ptr, y_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    num_core = tl.num_programs(0)
    num_blocks = tl.cdiv(n_elements, BLOCK_SIZE)
    for block_idx in range(pid, num_blocks, num_core):
        offsets = block_idx * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
        mask = offsets < n_elements
        x = tl.load(x_ptr + offsets, mask=mask)
        y = tl.load(y_ptr + offsets, mask=mask)
        tl.store(out_ptr + offsets, x + y, mask=mask)


def triton_add(x: torch.Tensor, y: torch.Tensor) -> torch.Tensor:
    out = torch.empty_like(x)
    n_elements = x.numel()
    BLOCK_SIZE = 1024
    grid = (min(65535, triton.cdiv(n_elements, BLOCK_SIZE)),)
    add_kernel[grid](x, y, out, n_elements, BLOCK_SIZE=BLOCK_SIZE)
    return out


def _dump_has_triton_access(out_dir: Path) -> bool:
    if not out_dir.is_dir():
        return False
    for path in out_dir.rglob("*"):
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        if "triton.add_kernel" in text or "triton." in text and "ACCESS" in text.upper():
            if "add_kernel" in text or "memscope-aten-ac" in text or ",WRITE" in text or "WRITE" in text:
                return True
        # CSV dumps often contain kernel name + access type columns
        if "add_kernel" in text and ("WRITE" in text or "READ" in text):
            return True
    return False


def main() -> int:
    msmemscope.config(
        events="alloc,free,access,launch",
        level="kernel,op",
        output=str(_OUT),
        data_format="csv",
    )
    device = torch.device("npu:0")
    torch.npu.set_device(device)

    x = torch.randn(4096, device=device, dtype=torch.float32)
    y = torch.randn(4096, device=device, dtype=torch.float32)

    msmemscope.start()
    out = triton_add(x, y)
    torch.npu.synchronize()
    msmemscope.stop()

    ref = x + y
    max_diff = (out - ref).abs().max().item()
    print(f"max diff vs torch add: {max_diff}")
    if max_diff > _TOL:
        print(f"FAIL: numerical mismatch max_diff={max_diff} > {_TOL}", file=sys.stderr)
        return 1
    if not _dump_has_triton_access(_OUT):
        print(
            f"FAIL: no Triton ACCESS evidence under {_OUT.resolve()}",
            file=sys.stderr,
        )
        return 1
    print("PASS: numerical OK + ACCESS dump present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
