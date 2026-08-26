# -------------------------------------------------------------------------
# This file is part of the MindStudio project.
# Copyright (c) 2025 Huawei Technologies Co.,Ltd.
#
# MindStudio is licensed under Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#
#          http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
# -------------------------------------------------------------------------

"""CPU tensor collection smoke test. Usage: msmemscope --device=npu,cpu python test_cpu_tensor_smoke.py"""

import torch
import torch_npu
import mstx


def main():
    torch.npu.set_device(0)

    mstx_id = mstx.range_start("step start", None)

    x = torch.randn(16, 16).to("npu:0")
    torch.tensor([1.0, 2.0, 3.0])     # Method 1: torch.tensor  -> MALLOC (size 12)
    c1 = x.to("cpu")                   # Method 2: Tensor.to('cpu') -> MALLOC (size 1024)
    c2 = x.cpu()                       # Method 3: Tensor.cpu() -> MALLOC (size 1024)
    c1.cpu()                           # already CPU -> returns self, dedup (no MALLOC)
    c1.view(-1)                        # shared storage, not hooked (no MALLOC)
    torch.tensor([])                   # empty -> data_ptr 0, skipped (no MALLOC)

    mstx.range_end(mstx_id)
    torch.npu.synchronize()


if __name__ == "__main__":
    main()
