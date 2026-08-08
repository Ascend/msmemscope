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

import traceback
import weakref
from typing import Dict, List, Tuple

import torch

from .hijacker.hijack_utility import hijacker, release, POST_HOOK

_cpu_blocks: Dict[int, Tuple[int, weakref.finalize]] = {}  # data_ptr -> (nbytes, finalizer)
_handlers: List = []  # registered hijack handlers


def _is_cpu_tensor(t) -> bool:
    """Check if a value is a CPU tensor."""
    return isinstance(t, torch.Tensor) and t.device.type == "cpu"


def _on_cpu_tensor_created(ret, *args, **kwargs):  # pylint: disable=unused-argument
    """POST_HOOK callback: report MALLOC when return value is a CPU tensor."""
    if not _is_cpu_tensor(ret):
        return ret
    try:
        import msmemscope._msmemscope as _cpp

        storage = ret.untyped_storage()
        ptr = storage.data_ptr()
        if ptr == 0:  # empty tensor, skip
            return ret
        if ptr in _cpu_blocks:  # same-channel dedup (no-op return self / shared storage)
            return ret
        size = storage.nbytes()
        stack = "".join(traceback.format_stack())
        # Cross-channel dedup: only attach finalize if C++ accepts (returns True)
        if not _cpp._report_cpu_tensor(ptr, size, True, stack):
            return ret
        _cpu_blocks[ptr] = (size, weakref.finalize(storage, _on_storage_freed, ptr))
    except Exception as exc:
        print(f"[msmemscope] Warning: failed to report CPU tensor creation: {exc}")
    return ret


def _on_storage_freed(ptr):
    """weakref.finalize callback: storage is GC'd, report FREE."""
    block = _cpu_blocks.pop(ptr, None)
    if block is None:
        return
    try:
        import msmemscope._msmemscope as _cpp

        _cpp._report_cpu_tensor(ptr, block[0], False, "")
    except Exception as exc:
        print(f"[msmemscope] Warning: failed to report CPU tensor free: {exc}")


def enable_cpu_tensor_collect():
    """Register hooks: torch.tensor / Tensor.to / Tensor.cpu (idempotent)."""
    if _handlers:
        return
    targets = [
        ("torch", "", "tensor", _on_cpu_tensor_created),
        ("torch", "Tensor", "to", _on_cpu_tensor_created),
        ("torch", "Tensor", "cpu", _on_cpu_tensor_created),
    ]
    for module, cls, func, stub in targets:
        try:
            _handlers.append(
                hijacker(stub=stub, module=module, cls=cls, function=func, action=POST_HOOK)
            )
        except Exception as exc:
            print(
                f"[msmemscope] Warning: failed to register CPU tensor hook "
                f"({module}@{cls}@{func}): {exc}"
            )


def disable_cpu_tensor_collect():
    """Unregister hooks and clear state (idempotent)."""
    for handler in _handlers:
        try:
            release(handler)
        except Exception as exc:
            print(f"[msmemscope] Warning: failed to release CPU tensor hook: {exc}")
    _handlers.clear()
    _cpu_blocks.clear()
