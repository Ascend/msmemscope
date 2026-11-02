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

"""NPU smoke for msMemScope Triton ACCESS path (assert + dump).

Worker subprocess so C++ dump Fflush-on-exit lands before parent checks ACCESS
(same pattern as test_triton_e2e_gate.py).
"""

from __future__ import annotations

import csv
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

_OUT = Path("./output")
_TOL = 1e-5
_KERNEL_TAG = "msm_access_smoke_axpy"

WORKER = r'''
import json, sys
from pathlib import Path
import torch, torch_npu, triton, triton.language as tl, msmemscope

cfg = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
out_dir = Path(cfg["out"])
tol = float(cfg["tol"])

@triton.jit
def msm_access_smoke_axpy(src_a_ptr, src_b_ptr, dst_ptr, n_elems, BLOCK: tl.constexpr):
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

msmemscope.config(events="alloc,free,access,launch", level="kernel,op",
                  output=str(out_dir), data_format="csv")
device = torch.device("npu:0")
torch.npu.set_device(device)
a = torch.randn(4096, device=device, dtype=torch.float32)
b = torch.randn(4096, device=device, dtype=torch.float32)
msmemscope.start()
dst = torch.empty_like(a)
n = int(a.numel())
block = 1024
grid = (min(65535, triton.cdiv(n, block)),)
msm_access_smoke_axpy[grid](a, b, dst, n, BLOCK=block)
expect = {"a": hex(int(a.data_ptr())), "b": hex(int(b.data_ptr())),
          "dst": hex(int(dst.data_ptr()))}
torch.npu.synchronize()
msmemscope.stop()
ref = a + b
max_diff = float((dst - ref).abs().max().item())
meta = {"max_diff": max_diff, "tol": tol, "expect": expect, "ok_num": max_diff <= tol}
print("MSM_SMOKE_META:" + json.dumps(meta), flush=True)
sys.exit(0 if meta["ok_num"] else 2)
'''


def _norm_ptr(v: str) -> str:
    s = (v or "").strip().lower()
    if s.startswith("0x"):
        return hex(int(s, 16))
    try:
        return hex(int(s))
    except ValueError:
        return s


def _dump_has_kernel_access(out_dir: Path, expect: dict) -> bool:
    need = {
        (_norm_ptr(expect["a"]), "READ"),
        (_norm_ptr(expect["b"]), "READ"),
        (_norm_ptr(expect["dst"]), "WRITE"),
    }
    found = set()
    for path in out_dir.rglob("memscope_dump_*.csv"):
        try:
            with path.open(encoding="utf-8", errors="ignore", newline="") as fh:
                for row in csv.DictReader(fh):
                    if (row.get("Event") or "").strip() != "ACCESS":
                        continue
                    if _KERNEL_TAG not in (row.get("Name") or ""):
                        continue
                    ptr = _norm_ptr(row.get("Ptr") or "")
                    typ = (row.get("Event Type") or "").strip().upper()
                    if typ in ("READ", "WRITE"):
                        found.add((ptr, typ))
        except OSError:
            continue
    return need.issubset(found)


def main() -> int:
    shutil.rmtree(_OUT, ignore_errors=True)
    _OUT.mkdir(parents=True, exist_ok=True)

    cfg = {"out": str(_OUT.resolve()), "tol": _TOL}
    with tempfile.NamedTemporaryFile("w", suffix="_smoke_cfg.json", delete=False, encoding="utf-8") as cf:
        json.dump(cfg, cf)
        cfg_path = cf.name
    with tempfile.NamedTemporaryFile("w", suffix="_smoke_worker.py", delete=False, encoding="utf-8") as wf:
        wf.write(WORKER)
        worker_path = wf.name
    try:
        proc = subprocess.run(
            [sys.executable, worker_path, cfg_path],
            cwd=str(Path.cwd()),
            capture_output=True,
            text=True,
            check=False,
            env=os.environ.copy(),
        )
    finally:
        for p in (cfg_path, worker_path):
            try:
                os.unlink(p)
            except OSError:
                pass

    sys.stdout.write(proc.stdout or "")
    sys.stderr.write(proc.stderr or "")
    meta = None
    for line in (proc.stdout or "").splitlines():
        if line.startswith("MSM_SMOKE_META:"):
            meta = json.loads(line.split(":", 1)[1])
    if proc.returncode != 0 or meta is None:
        print(f"FAIL: smoke worker rc={proc.returncode}", file=sys.stderr)
        return proc.returncode or 1
    print(f"max diff vs torch add: {meta['max_diff']}")
    if not meta.get("ok_num"):
        print(f"FAIL: numerical mismatch max_diff={meta['max_diff']} > {_TOL}", file=sys.stderr)
        return 1
    if not _dump_has_kernel_access(_OUT, meta["expect"]):
        print(
            f"FAIL: missing Triton ACCESS Ptr/Type for {_KERNEL_TAG} under {_OUT.resolve()} "
            f"(expect {meta['expect']})",
            file=sys.stderr,
        )
        return 1
    print("PASS: numerical OK + ACCESS dump present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
