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

from .hijacker.hijack_utility import hijacker, release, POST_HOOK

_cpu_blocks: Dict[int, Tuple[int, weakref.finalize]] = {}  # data_ptr -> (nbytes, finalizer)
_handlers: List = []  # registered hijack handlers
_pending_enable = False  # deferred enable in flight (waiting for torch_npu init)
_torch_npu_hook = None  # torch_npu module POST_HOOK (deterministic import-complete signal)
_PURE_CPU_GRACE = 1.0  # seconds to wait for torch_npu before assuming pure-CPU mode

_torch_module = None

# torch 模块级工厂函数：直接分配 CPU tensor 的入口。torch.randn/empty/zeros 等
# 底层走 C++ at::*，不经过 torch.tensor，因此必须逐个 hook 才能覆盖首个 NPU
# 算子之前由这些工厂创建的 CPU tensor。frombuffer 包装外部 buffer（如 mmap 页对齐
# 内存），用于 aclrtHostRegisterV2 双通道场景。
_CPU_TENSOR_FACTORIES = [
    "empty",
    "zeros",
    "ones",
    "full",
    "rand",
    "randn",
    "randint",
    "randperm",
    "arange",
    "linspace",
    "logspace",
    "eye",
    "empty_like",
    "zeros_like",
    "ones_like",
    "full_like",
    "rand_like",
    "randn_like",
    "randint_like",
    "frombuffer",
]


def _torch():
    """Lazy torch import: this module must stay importable before torch exists so the
    C++ trigger thread can install the torch_npu import hook before torch is imported.
    """
    global _torch_module
    if _torch_module is None:
        import torch  # pylint: disable=import-outside-toplevel

        _torch_module = torch
    return _torch_module


def _is_cpu_tensor(t) -> bool:
    """Check if a value is a CPU tensor."""
    return isinstance(t, _torch().Tensor) and t.device.type == "cpu"


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


def _module_importing(name):
    """Return True if module ``name`` is currently mid-import."""
    mod = sys.modules.get(name)
    spec = getattr(mod, "__spec__", None)
    return bool(getattr(spec, "_initializing", False))


def _torch_imported() -> bool:
    """Return True once torch has finished importing (module body executed)."""
    try:
        return "torch" in sys.modules and not _module_importing("torch")
    except Exception:
        return False


def _npu_initialized() -> bool:
    """Return True only when it is safe to monkey-patch ``torch.Tensor`` now.

    Patching is unsafe:
      * while ``torch`` or ``torch_npu`` is mid-import — patching mid-import races
        with torch_npu's own torch patching and produced the 507033 "device setup
        error";
      * while ``torch_npu`` has not been imported at all — a later
        ``import torch_npu`` re-patches ``torch.tensor`` and clobbers our hooks.

    ``ModuleSpec._initializing`` is False only once the module body has fully
    executed. Requiring ``torch_npu`` to be present AND not mid-import makes the
    immediate-registration path safe; the torch_npu import POST_HOOK handles the
    "about to be imported" case deterministically.
    """
    try:
        return _torch_imported() and "torch_npu" in sys.modules and not _module_importing("torch_npu")
    except Exception:
        return False


def _on_torch_npu_imported(module):  # pylint: disable=unused-argument
    """torch_npu module POST_HOOK: register hooks once the import has completed.

    The hijacker fires this synchronously right after torch_npu's module body
    executes (``HiJackerLoader.exec_module`` runs ``exec_post_hook`` after the
    body), which is the safe point — torch_npu has finished patching torch. No
    ``_npu_initialized`` gate here: at this instant ``_initializing`` is still
    True (the import machinery resets it only after ``exec_module`` returns), so
    the gate would wrongly block registration.
    """
    _register_handlers()


def _install_torch_npu_hook():
    """Install a one-shot torch_npu import POST_HOOK (idempotent).

    This is the deterministic "import finished" signal: the hook fires
    synchronously right after torch_npu's module body executes.
    """
    global _torch_npu_hook
    if _torch_npu_hook is not None:
        return
    # If torch_npu is mid-import, the hijacker would fire the POST_HOOK immediately
    # (before its body finishes), registering too early. Defer to the polling
    # thread, which waits for torch_npu to finish importing.
    if _module_importing("torch_npu"):
        return
    try:
        _torch_npu_hook = hijacker(stub=_on_torch_npu_imported, module="torch_npu", action=POST_HOOK)
    except Exception as exc:
        print(f"[msmemscope] Warning: failed to register torch_npu import hook: {exc}")


def _register_handlers():
    """Register the torch hijack hooks (idempotent)."""
    global _pending_enable
    if _handlers:
        return
    _torch()  # ensure torch imported before hijacker resolves "torch" module
    targets = [
        ("torch", "", "tensor", _on_cpu_tensor_created),
        ("torch", "Tensor", "to", _on_cpu_tensor_created),
        ("torch", "Tensor", "cpu", _on_cpu_tensor_created),
    ]
    targets += [("torch", "", name, _on_cpu_tensor_created) for name in _CPU_TENSOR_FACTORIES]
    for module, cls, func, stub in targets:
        try:
            _handlers.append(hijacker(stub=stub, module=module, cls=cls, function=func, action=POST_HOOK))
        except Exception as exc:
            print(f"[msmemscope] Warning: failed to register CPU tensor hook ({module}@{cls}@{func}): {exc}")
    _pending_enable = False


def _deferred_enable():
    """Fallback to the torch_npu import POST_HOOK.

    Covers two cases the POST_HOOK alone misses:
      * pure-CPU mode (torch imported, torch_npu never imported) — the POST_HOOK
        never fires, so register once torch is fully imported and torch_npu has
        stayed absent for a short grace period;
      * the rare race where the POST_HOOK was installed while torch_npu was
        mid-import (hook skipped) — poll until torch_npu finishes importing.
    """
    global _pending_enable
    deadline = time.monotonic() + 60.0
    torch_imported_at = None
    while _pending_enable and not _handlers and time.monotonic() < deadline:
        if _npu_initialized():
            break
        now = time.monotonic()
        if torch_imported_at is None and _torch_imported():
            torch_imported_at = now
        if (
            torch_imported_at is not None
            and "torch_npu" not in sys.modules
            and now - torch_imported_at >= _PURE_CPU_GRACE
        ):
            break
        time.sleep(0.001)
    if not _pending_enable or _handlers:
        return
    _pending_enable = False
    _register_handlers()


def enable_cpu_tensor_collect():
    """Register hooks: torch.tensor / Tensor.to / Tensor.cpu (idempotent)."""
    global _pending_enable
    if _handlers:
        return
    if _npu_initialized():
        _register_handlers()
        return
    if _pending_enable:
        return
    _pending_enable = True
    _install_torch_npu_hook()
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
    global _pending_enable, _torch_npu_hook
    _pending_enable = False
    for handler in _handlers:
        try:
            release(handler)
        except Exception as exc:
            print(f"[msmemscope] Warning: failed to release CPU tensor hook: {exc}")
    _handlers.clear()
    if _torch_npu_hook is not None:
        try:
            release(_torch_npu_hook)
        except Exception as exc:
            print(f"[msmemscope] Warning: failed to release torch_npu import hook: {exc}")
        _torch_npu_hook = None
    # Flush pending CPU tensor frees to the C++ address book before clearing, so a
    # repeated start/stop cycle doesn't leave stale hostPtrs_ entries (finding #2).
    for ptr in list(_cpu_blocks.keys()):
        _on_storage_freed(ptr)
    _cpu_blocks.clear()
