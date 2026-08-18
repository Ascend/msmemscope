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

import sys
import threading
import time
import traceback
import weakref
from typing import Dict, List, Tuple

import torch

from .hijacker.hijack_utility import hijacker, release, POST_HOOK

_cpu_blocks: Dict[int, Tuple[int, weakref.finalize]] = {}  # data_ptr -> (nbytes, finalizer)
_handlers: List = []  # registered hijack handlers
_pending_enable = False  # deferred enable in flight (waiting for torch_npu init)


def _is_cpu_tensor(t) -> bool:
    """Check if a value is a CPU tensor."""
    return isinstance(t, torch.Tensor) and t.device.type == "cpu"


def _format_py_stack() -> str:
    """Format the current call stack as a self-quoted CSV field.

    Mirrors the C++ ``PythonCallstack`` format (``"file(line): func\\n..."``)
    used by device-side events, so the dump CSV stays well-formed.
    """
    frames = list(traceback.extract_stack())
    frames.reverse()  # C++ walks from the innermost frame outward
    return '"' + "\n".join(f"{f.filename}({f.lineno}): {f.name}" for f in frames) + '\n"'


def _on_cpu_tensor_created(ret, *args, **kwargs):  # pylint: disable=unused-argument
    """POST_HOOK callback: report MALLOC when return value is a CPU tensor."""
    if not _is_cpu_tensor(ret):
        return ret
    try:
        import msmemscope._msmemscope as _cpp  # pylint: disable=no-name-in-module

        storage = ret.untyped_storage()
        ptr = storage.data_ptr()
        if ptr == 0:  # empty tensor, skip
            return ret
        if ptr in _cpu_blocks:  # same-channel dedup (no-op return self / shared storage)
            return ret
        size = storage.nbytes()
        stack = _format_py_stack()
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
        import msmemscope._msmemscope as _cpp  # pylint: disable=no-name-in-module

        _cpp._report_cpu_tensor(ptr, block[0], False, "")
    except Exception as exc:
        print(f"[msmemscope] Warning: failed to report CPU tensor free: {exc}")


def _npu_initialized() -> bool:
    """Return True when it is safe to monkey-patch ``torch.Tensor``.

    torch_npu opens its device lazily (aclInit / rtSetDevice). Installing the
    CPU tensor hijack hooks while that initialization is in flight races with
    it and makes ``torch.npu.set_device`` fail with error 507033 ("device
    retain error"). We therefore wait until the NPU device is initialized (or
    confirm there is no torch_npu at all) before patching torch.
    """
    try:
        if "torch_npu" not in sys.modules:
            return True  # no torch_npu -> nothing to race with
        npu = getattr(torch, "npu", None)
        if npu is None:
            return False  # torch_npu is still being imported
        initialized = getattr(npu, "is_initialized", None)
        if callable(initialized) and not initialized():
            return False  # present but device not opened yet
        return True
    except Exception:
        return False


def _register_handlers():
    """Register the torch hijack hooks (idempotent)."""
    if _handlers:
        return
    targets = [
        ("torch", "", "tensor", _on_cpu_tensor_created),
        ("torch", "Tensor", "to", _on_cpu_tensor_created),
        ("torch", "Tensor", "cpu", _on_cpu_tensor_created),
    ]
    for module, cls, func, stub in targets:
        try:
            _handlers.append(hijacker(stub=stub, module=module, cls=cls, function=func, action=POST_HOOK))
        except Exception as exc:
            print(f"[msmemscope] Warning: failed to register CPU tensor hook ({module}@{cls}@{func}): {exc}")


def _deferred_enable():
    """Poll until the NPU device is initialized, then install the hooks."""
    global _pending_enable
    deadline = time.monotonic() + 60.0
    while _pending_enable and not _npu_initialized() and time.monotonic() < deadline:
        time.sleep(0.001)
    if not _pending_enable:
        return  # disabled, or already registered by on_device_ready()
    _pending_enable = False
    _register_handlers()


def enable_cpu_tensor_collect():
    """Register hooks: torch.tensor / Tensor.to / Tensor.cpu (idempotent)."""
    global _pending_enable
    if _handlers:
        return
    if _npu_initialized():
        _register_handlers()
    elif not _pending_enable:
        _pending_enable = True
        threading.Thread(target=_deferred_enable, daemon=True).start()


def on_device_ready():
    """Called from C++ after ``aclrtSetDevice`` completes.

    The NPU device is retained at this point, so monkey-patching
    ``torch.Tensor`` is safe. This closes the CLI deferral window in which
    tensors created right after ``set_device`` (before the first NPU op) were
    otherwise missed by the ``is_initialized()``-based polling in
    ``enable_cpu_tensor_collect``.
    """
    global _pending_enable
    _pending_enable = False
    _register_handlers()


def disable_cpu_tensor_collect():
    """Unregister hooks and clear state (idempotent)."""
    global _pending_enable
    _pending_enable = False
    for handler in _handlers:
        try:
            release(handler)
        except Exception as exc:
            print(f"[msmemscope] Warning: failed to release CPU tensor hook: {exc}")
    _handlers.clear()
    _cpu_blocks.clear()
