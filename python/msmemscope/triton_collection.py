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

"""Collect Triton kernel memory access events for msMemScope.

Triton kernels bypass ``__torch_dispatch__``, so AtenCollector cannot see their
device-memory touches. This module intercepts the Ascend Triton launch path via
the repo ``hijacker`` (PRE_HOOK for begin/ACCESS, POST_HOOK for end), infers R/W
from compile-time ``tensor_kinds``, and emits the same ``memscope-aten-*`` MSTX
protocol consumed by AtenManager.

R/W correctness: ACCESS marks are emitted only when ``tensor_kinds`` aligns with
positional NPU tensor arguments and every kind is a known value (0/1/2). Missing,
mismatched, unknown kinds, or NPU tensors passed via kwargs skip ACCESS
(begin/end still emitted). Never invent R/W when kinds are untrusted.

ABI: launch args are anchored on ``packed_metadata`` at fixed index 5
(``args[9:]`` = non-constexpr kernel args). ``launch_metadata`` (index 6) is
optional — triton-ascend 3.2.2 often passes ``None`` when no enter hook is set.

Diagnostic messages use stderr prints (not ``warnings.warn``) so ``-W error``
cannot abort kernel launch.
"""

from __future__ import annotations

import sys
import threading
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple

from .hijacker.hijack_utility import PRE_HOOK, POST_HOOK, hijacker, release

# Dedup skip-ACCESS diagnostics: (kernel_name, reason) → already reported
_SKIP_ACCESS_WARNED: Set[str] = set()

# Canonical Ascend Triton launcher (hijacker target). Deferred import is handled
# by HiJackerPathFinder — no builtins.__import__ patching.
#
# Runtime note: triton-ascend often exposes the live class as builtin
# ``ascend.NPULauncher`` (via ``driver.active.launcher_cls``), which is a
# *different* object from ``triton.backends.ascend.driver.NPULauncher``.
# We register both the canonical Python path and the active class host module.
_NPU_LAUNCHER_MODULE = "triton.backends.ascend.driver"
_NPU_LAUNCHER_CLS = "NPULauncher"
_NPU_LAUNCHER_FUNC = "__call__"
_NPU_LAUNCHER_TARGETS = ((_NPU_LAUNCHER_MODULE, _NPU_LAUNCHER_CLS),)

try:
    import mstx
except Exception:  # pragma: no cover - mstx always present in tool runtime
    mstx = None

try:
    import torch
except Exception:  # pragma: no cover
    torch = None

# Ascend IR / launcher conventions (see triton-ascend driver.cpp TENSOR_KIND_*)
TENSOR_KIND_INPUT = 0
TENSOR_KIND_OUTPUT = 1
TENSOR_KIND_INOUT = 2
_KNOWN_TENSOR_KINDS = frozenset((TENSOR_KIND_INPUT, TENSOR_KIND_OUTPUT, TENSOR_KIND_INOUT))

# CompiledKernel.run / NPULauncher.__call__ positional layout before kernel args
# grid0,grid1,grid2,stream,function,packed_metadata,launch_metadata,enter,exit,*args
_LAUNCH_META_INDEX = 6
_PACKED_META_INDEX = 5
_KERNEL_ARGS_START = 9
_PACKED_TO_KERNEL_ARGS = 4  # packed, launch, enter, exit
_SCAN_ARG_LIMIT = 20

# Lazily resolved once: FakeTensor / FunctionalTensor classes to exclude.
_EXCLUDED_TENSOR_SUBCLASSES: Optional[Tuple[type, ...]] = None
_EXCLUDED_TENSOR_SUBCLASSES_RESOLVED = False


def _diag(msg: str) -> None:
    """Non-fatal diagnostic — never use warnings.warn (breaks under -W error)."""
    print(f"[msmemscope] {msg}", file=sys.stderr)


def _excluded_tensor_subclasses() -> Tuple[type, ...]:
    """Resolve FakeTensor/FunctionalTensor once (hot path avoids per-call import).

    Only caches permanently when at least one class was found; empty failure
    leaves unresolved so a later torch import can retry (review P2-e).
    """
    global _EXCLUDED_TENSOR_SUBCLASSES, _EXCLUDED_TENSOR_SUBCLASSES_RESOLVED
    if _EXCLUDED_TENSOR_SUBCLASSES_RESOLVED:
        return _EXCLUDED_TENSOR_SUBCLASSES or ()
    types: List[type] = []
    try:
        from torch._subclasses.fake_tensor import FakeTensor

        types.append(FakeTensor)
    except Exception:
        pass  # nosec B110
    try:
        from torch._subclasses.functional_tensor import FunctionalTensor

        types.append(FunctionalTensor)
    except Exception:
        pass  # nosec B110
    if types:
        _EXCLUDED_TENSOR_SUBCLASSES = tuple(types)
        _EXCLUDED_TENSOR_SUBCLASSES_RESOLVED = True
        return _EXCLUDED_TENSOR_SUBCLASSES
    return ()


def calculate_tensor_size(tensor: Any) -> int:
    """Logical element count * element size (matches aten_collection)."""
    numel_fn = getattr(tensor, "numel", None)
    elem_fn = getattr(tensor, "element_size", None)
    if callable(numel_fn) and callable(elem_fn):
        try:
            return int(numel_fn()) * int(elem_fn())
        except Exception:
            pass  # nosec B110
    numel = 1
    for dim in tensor.shape:
        numel *= int(dim)
    return numel * int(tensor.itemsize)


def is_npu_tensor(value: Any) -> bool:
    if torch is not None and isinstance(value, torch.Tensor):
        # Align with aten_collection: ignore FakeTensor / FunctionalTensor subclasses.
        excluded = _excluded_tensor_subclasses()
        if excluded and isinstance(value, excluded):
            return False
        return bool(getattr(value, "is_npu", False))
    if getattr(value, "is_npu", False) and callable(getattr(value, "data_ptr", None)):
        return True
    return False


def is_known_tensor_kind(kind: Optional[int]) -> bool:
    return kind is not None and int(kind) in _KNOWN_TENSOR_KINDS


def kind_to_access_flags(kind: Optional[int]) -> Optional[Tuple[bool, bool, bool]]:
    """Map known tensor_kind to (is_read, is_write, is_output). None if unknown.

    INOUT sets ``is_output=True`` intentionally so ``--watch`` includes the tensor
    (Aten in-place inputs use ``is_output=False``; dumped Event Type is still WRITE).
    """
    if kind == TENSOR_KIND_INPUT:
        return True, False, False
    if kind == TENSOR_KIND_OUTPUT:
        return False, True, True
    if kind == TENSOR_KIND_INOUT:
        return True, True, True
    return None


def extract_tensor_kinds(packed_metadata: Any) -> Optional[Sequence[int]]:
    if packed_metadata is None:
        return None
    kinds = None
    if isinstance(packed_metadata, dict):
        kinds = packed_metadata.get("tensor_kinds")
    else:
        kinds = getattr(packed_metadata, "tensor_kinds", None)
        if kinds is None and hasattr(packed_metadata, "get") and callable(packed_metadata.get):
            try:
                kinds = packed_metadata.get("tensor_kinds")
            except TypeError:
                kinds = None
            except Exception:
                kinds = None
    if kinds is None:
        return None
    try:
        return [int(k) for k in kinds]
    except Exception:
        return None


def extract_kernel_name(launch_metadata: Any, packed_metadata: Any = None, fallback: str = "unknown") -> str:
    name = None
    if isinstance(launch_metadata, dict):
        name = launch_metadata.get("name")
    elif launch_metadata is not None:
        raw_data = None
        try:
            raw_data = object.__getattribute__(launch_metadata, "data")
        except Exception:
            raw_data = None
        if isinstance(raw_data, dict):
            name = raw_data.get("name")
        if name is None and hasattr(launch_metadata, "get") and callable(launch_metadata.get):
            try:
                meta = launch_metadata.get()
                if isinstance(meta, dict):
                    name = meta.get("name")
            except TypeError:
                try:
                    got = launch_metadata.get("name")
                    if got is not None:
                        name = got
                except Exception:
                    name = None
            except Exception:
                name = None
    if name is None and packed_metadata is not None:
        if isinstance(packed_metadata, dict):
            name = packed_metadata.get("name") or packed_metadata.get("kernel_name")
        else:
            name = getattr(packed_metadata, "name", None) or getattr(packed_metadata, "kernel_name", None)
    if not name:
        name = fallback
    name = str(name)
    if not name.startswith("triton."):
        name = f"triton.{name}"
    return name


def build_access_mark(
    *,
    data_ptr: int,
    is_write: bool,
    is_read: bool,
    is_output: bool,
    name: str,
    shape: Any,
    dtype: Any,
    tensor_size: int,
    device: Any,
) -> str:
    return (
        f"memscope-aten-ac:ptr={data_ptr};is_write={is_write};is_read={is_read};"
        f"is_output={is_output};name={name};shape={shape};dtype={dtype};"
        f"tensor_size={tensor_size};device={device}"
    )


def build_begin_mark(name: str) -> str:
    return f"memscope-aten-b: name={name}"


def build_end_mark(name: str) -> str:
    return f"memscope-aten-e: name={name}"


def _is_packed_metadata(obj: Any) -> bool:
    """True if obj looks like Ascend ``pack_metadata`` (tensor_kinds and/or kernel_name/hash)."""
    if obj is None:
        return False
    if isinstance(obj, dict):
        kinds = obj.get("tensor_kinds")
        if isinstance(kinds, (list, tuple)):
            return True
        return ("kernel_name" in obj) or ("hash" in obj)
    kinds = getattr(obj, "tensor_kinds", None)
    if isinstance(kinds, (list, tuple)):
        return True
    if getattr(obj, "kernel_name", None) is not None or getattr(obj, "hash", None) is not None:
        return True
    return False


def _is_launch_metadata(obj: Any) -> bool:
    """Optional enrichment only — ``None`` is common on triton-ascend 3.2.2."""
    if obj is None:
        return False
    if isinstance(obj, dict):
        name = obj.get("name")
        return (isinstance(name, str) and bool(name)) or ("grid" in obj)
    try:
        raw_data = object.__getattribute__(obj, "data")
    except Exception:
        raw_data = None
    if isinstance(raw_data, dict) and ("name" in raw_data or "grid" in raw_data):
        return True
    if hasattr(obj, "get") and callable(obj.get) and not isinstance(obj, (str, bytes, list, tuple, dict)):
        return True
    return False


def _collect_npu_tensors(kernel_args: Sequence[Any]) -> List[Any]:
    return [arg for arg in kernel_args if is_npu_tensor(arg)]


def iter_tensor_args_with_kinds(
    kernel_args: Sequence[Any],
    tensor_kinds: Optional[Sequence[int]],
) -> Iterable[Tuple[Any, Optional[int], bool]]:
    """Yield (tensor, kind, kind_missing) for NPU tensor kernel arguments.

    ``tensor_kinds`` indexes only NPU/pointer args (scalars skipped), matching
    triton-ascend packed_metadata convention.
    """
    kind_idx = 0
    kinds_len = len(tensor_kinds) if tensor_kinds is not None else 0
    for arg in kernel_args:
        if not is_npu_tensor(arg):
            continue
        kind: Optional[int] = None
        kind_missing = True
        if tensor_kinds is not None and kind_idx < kinds_len:
            kind = int(tensor_kinds[kind_idx])
            kind_missing = False
        kind_idx += 1
        yield arg, kind, kind_missing


def _kinds_usable(npu_count: int, tensor_kinds: Optional[Sequence[int]]) -> Tuple[bool, str]:
    if tensor_kinds is None:
        return False, "missing"
    if len(tensor_kinds) != npu_count:
        return False, "length_mismatch"
    for k in tensor_kinds:
        if not is_known_tensor_kind(k):
            return False, "unknown_kind"
    return True, ""


def _metadata_summary(packed_metadata: Any) -> str:
    if isinstance(packed_metadata, dict):
        return f"keys={list(packed_metadata.keys())}"
    return f"type={type(packed_metadata).__name__}"


def emit_triton_access_events(
    kernel_name: str,
    kernel_args: Sequence[Any],
    tensor_kinds: Optional[Sequence[int]],
    *,
    mark_fn=None,
    packed_metadata: Any = None,
    skip_access: bool = False,
    skip_access_reason: str = "",
) -> List[str]:
    """Emit begin + access MSTX marks. Returns the mark strings (for UT)."""
    if mark_fn is None:
        mark_fn = _default_mark

    marks: List[str] = []
    begin = build_begin_mark(kernel_name)
    mark_fn(begin)
    marks.append(begin)

    if skip_access:
        key = f"{kernel_name}|{skip_access_reason or 'ACCESS skipped'}"
        if key not in _SKIP_ACCESS_WARNED:
            _SKIP_ACCESS_WARNED.add(key)
            _diag(
                f"Triton {skip_access_reason or 'ACCESS skipped'} for {kernel_name}; "
                "emitting begin/end only (R/W order not trusted)."
            )
        return marks
    npu_tensors = _collect_npu_tensors(kernel_args)
    usable, reason = _kinds_usable(len(npu_tensors), tensor_kinds)

    if not usable:
        meta_hint = ""
        if packed_metadata is not None:
            meta_hint = f" ({_metadata_summary(packed_metadata)})"
        if reason == "missing":
            detail = f"tensor_kinds missing{meta_hint}"
        elif reason == "length_mismatch":
            detail = (
                f"tensor_kinds length mismatch{meta_hint}: "
                f"kinds={len(tensor_kinds) if tensor_kinds is not None else 0} "
                f"tensors={len(npu_tensors)}"
            )
        else:
            detail = f"unknown tensor_kinds value(s){meta_hint}: {list(tensor_kinds)}"
        key = f"{kernel_name}|{detail}"
        if key not in _SKIP_ACCESS_WARNED:
            _SKIP_ACCESS_WARNED.add(key)
            _diag(f"Triton {detail} for {kernel_name}; skipping ACCESS marks.")
        return marks

    for tensor, kind, kind_missing in iter_tensor_args_with_kinds(kernel_args, tensor_kinds):
        flags = None if kind_missing else kind_to_access_flags(kind)
        if flags is None:
            continue
        is_read, is_write, is_output = flags
        try:
            data_ptr = int(tensor.data_ptr())
            tensor_size = calculate_tensor_size(tensor)
        except Exception:
            continue  # nosec B112
        if data_ptr == 0:
            continue
        msg = build_access_mark(
            data_ptr=data_ptr,
            is_write=is_write,
            is_read=is_read,
            is_output=is_output,
            name=kernel_name,
            shape=tensor.shape,
            dtype=tensor.dtype,
            tensor_size=tensor_size,
            device=tensor.device,
        )
        mark_fn(msg)
        marks.append(msg)
    return marks


def emit_triton_end_event(kernel_name: str, *, mark_fn=None) -> str:
    if mark_fn is None:
        mark_fn = _default_mark
    end = build_end_mark(kernel_name)
    mark_fn(end)
    return end


def _default_mark(msg: str) -> None:
    if mstx is None:
        _diag(
            "mstx is unavailable; Triton ACCESS mark dropped "
            f"(msg prefix={msg[:48]!r}). Install/ensure mstx for collection."
        )
        return
    mstx.mark(msg, None)


def _parse_launch_args(args: tuple, kwargs: Optional[dict] = None) -> Optional[Tuple[Any, Any, tuple, bool]]:
    """Return (packed, launch, kernel_args, kwargs_has_npu) if layout is recognizable.

    Anchored on ``packed_metadata`` (fixed index 5 on the standard Ascend launch ABI).
    ``launch_metadata`` may be ``None`` (triton-ascend 3.2.2 without enter hook).

    ``kwargs_has_npu`` means keyword NPU tensors were present; ACCESS must be skipped
    because append-order would not match ``tensor_kinds``.
    """
    packed_metadata = None
    launch_metadata = None
    kernel_args: Optional[list] = None

    if len(args) >= _KERNEL_ARGS_START and _is_packed_metadata(args[_PACKED_META_INDEX]):
        packed_metadata = args[_PACKED_META_INDEX]
        # May be None / LazyDict / dict; name extraction tolerates all of these.
        launch_metadata = args[_LAUNCH_META_INDEX]
        kernel_args = list(args[_KERNEL_ARGS_START:])

    if kernel_args is None:
        scan_limit = min(len(args), _SCAN_ARG_LIMIT)
        for i in range(scan_limit):
            if not _is_packed_metadata(args[i]):
                continue
            packed_metadata = args[i]
            # Standard relative layout: packed, launch, enter, exit, *kernel_args
            if i + _PACKED_TO_KERNEL_ARGS <= len(args):
                launch_metadata = args[i + 1] if i + 1 < len(args) else None
                kernel_args = list(args[i + _PACKED_TO_KERNEL_ARGS :])
            else:
                # Degenerate short tuple (UT): packed, optional launch, then tensors
                rest = list(args[i + 1 :])
                if rest and _is_launch_metadata(rest[0]):
                    launch_metadata = rest.pop(0)
                elif rest and rest[0] is None:
                    launch_metadata = rest.pop(0)
                while rest and rest[0] is None:
                    rest.pop(0)
                kernel_args = rest
            break

    if kernel_args is None:
        return None

    kwargs_has_npu = False
    if kwargs:
        for value in kwargs.values():
            if is_npu_tensor(value):
                kwargs_has_npu = True
                break

    return packed_metadata, launch_metadata, tuple(kernel_args), kwargs_has_npu


class TritonCollector:
    """Hijack NPULauncher.__call__ via repo hijacker (PRE=b/ac, POST=e)."""

    def __init__(self) -> None:
        self.enabled = False
        self._hooked = False
        self._handlers: List[Any] = []
        self._lock = threading.RLock()
        self._tls = threading.local()
        self.stats = {
            "kernels_seen": 0,
            "access_emitted": 0,
            "access_skipped": 0,
            "enable_calls": 0,  # bumped only by enable_triton_collector (C++ or Python)
        }

    def __del__(self):
        is_finalizing = getattr(sys, "is_finalizing", lambda: False)
        if (sys is not None) and (not is_finalizing()) and self.enabled:
            try:
                self.disable()
            except Exception:
                pass  # nosec B110

    def enable(self) -> None:
        with self._lock:
            self.enabled = True
            self.stats["enable_calls"] = self.stats.get("enable_calls", 0) + 1
            self._install_hijacks()

    def disable(self) -> None:
        with self._lock:
            self._uninstall_hijacks()
            self.enabled = False
            _SKIP_ACCESS_WARNED.clear()

    def _bump_stat(self, key: str) -> None:
        with self._lock:
            self.stats[key] = self.stats.get(key, 0) + 1

    def _active_launcher_targets(self) -> List[Tuple[str, str]]:
        """Return (module, class_name) pairs to hijack, including live launcher_cls.

        Dedupes by class *identity*: the live ``ascend.NPULauncher`` must be
        registered when it is a different object from the canonical Python
        ``triton.backends.ascend.driver.NPULauncher``, but the same class must
        not be hooked twice via two module paths (breaks the hijacker chain).
        """
        targets: List[Tuple[str, str]] = list(_NPU_LAUNCHER_TARGETS)
        seen_keys = set(targets)
        seen_cls: Set[int] = set()
        for module, cls_name in targets:
            try:
                mod = sys.modules.get(module)
                obj = getattr(mod, cls_name, None) if mod is not None else None
                if obj is not None:
                    seen_cls.add(id(obj))
            except Exception:
                pass  # nosec B110
        try:
            from triton.runtime import driver as triton_driver

            active = getattr(triton_driver, "active", None)
            cls = getattr(active, "launcher_cls", None) if active is not None else None
        except Exception:
            cls = None
        if cls is None:
            return targets
        if id(cls) in seen_cls:
            return targets

        mod_name = getattr(cls, "__module__", None) or "ascend"
        cls_name = getattr(cls, "__name__", None) or _NPU_LAUNCHER_CLS
        # Builtin / extension classes may not live in sys.modules; host them so
        # hijacker can resolve module→class→__call__ the same way as Python paths.
        import types as _types

        host = sys.modules.get(mod_name)
        if host is None:
            host = _types.ModuleType(mod_name)
            sys.modules[mod_name] = host
        if getattr(host, cls_name, None) is not cls:
            try:
                setattr(host, cls_name, cls)
            except Exception as exc:
                _diag(f"cannot host active launcher {mod_name}.{cls_name}: {exc}")
                return targets
        key = (mod_name, cls_name)
        if key not in seen_keys:
            targets.append(key)
            seen_keys.add(key)
        return targets

    def _install_hijacks(self) -> None:
        if self._hooked:
            return
        handlers: List[Any] = []
        try:
            for module, cls_name in self._active_launcher_targets():
                handlers.append(
                    hijacker(
                        stub=self._pre_npu_launcher,
                        module=module,
                        cls=cls_name,
                        function=_NPU_LAUNCHER_FUNC,
                        action=PRE_HOOK,
                        priority=50,
                    )
                )
                handlers.append(
                    hijacker(
                        stub=self._post_npu_launcher,
                        module=module,
                        cls=cls_name,
                        function=_NPU_LAUNCHER_FUNC,
                        action=POST_HOOK,
                        priority=50,
                    )
                )
            self._handlers = handlers
            self._hooked = bool(handlers)
            if not handlers:
                _diag("no Triton NPULauncher hijack targets registered")
        except Exception as exc:
            _diag(f"failed to register Triton NPULauncher hijack: {exc}")
            for handler in handlers:
                try:
                    release(handler)
                except Exception:
                    pass  # nosec B110
            self._handlers = []
            self._hooked = False

    def _uninstall_hijacks(self) -> None:
        for handler in self._handlers:
            try:
                release(handler)
            except Exception as exc:
                _diag(f"failed to release Triton NPULauncher hijack: {exc}")
        self._handlers = []
        self._hooked = False
        self._tls_clear()

    def _tls_clear(self) -> None:
        for attr in ("need_end", "kernel_name"):
            if hasattr(self._tls, attr):
                delattr(self._tls, attr)

    def _pre_npu_launcher(self, *args, **kws):
        """PRE_HOOK: emit begin + ACCESS. Must never raise into hijacker wrapper."""
        self._tls.need_end = False
        self._tls.kernel_name = "triton.unknown"
        if not self.enabled:
            return args, kws
        try:
            # Method hijack: args[0] is launcher instance; ABI starts at args[1:].
            launch_args = args[1:] if args else ()
            parsed = _parse_launch_args(launch_args, kws)
            if parsed is None:
                return args, kws
            packed_metadata, launch_metadata, kernel_args, kwargs_has_npu = parsed
            kernel_name = extract_kernel_name(
                launch_metadata,
                packed_metadata,
                fallback="unknown",
            )
            tensor_kinds = extract_tensor_kinds(packed_metadata)
            if kwargs_has_npu:
                skip_access = True
                skip_reason = "NPU tensor(s) passed via kwargs; order vs tensor_kinds untrusted"
            else:
                skip_access = False
                skip_reason = ""
            self._bump_stat("kernels_seen")
            self._tls.kernel_name = kernel_name
            self._tls.need_end = True
            emitted = emit_triton_access_events(
                kernel_name,
                kernel_args,
                tensor_kinds,
                packed_metadata=packed_metadata,
                skip_access=skip_access,
                skip_access_reason=skip_reason,
            )
            if len(emitted) <= 1:
                self._bump_stat("access_skipped")
            else:
                self._bump_stat("access_emitted")
        except Exception:
            _diag("Triton ACCESS collection failed in PRE_HOOK; continuing kernel launch without ACCESS marks.")
            # If begin was already emitted (need_end True), keep it so POST_HOOK
            # still pairs end; do not leave a dangling begin for AtenManager.
        return args, kws

    def _post_npu_launcher(self, ret, *args, **kws):
        """POST_HOOK: emit end after Python launch returns. Must never raise."""
        try:
            if self.enabled and getattr(self._tls, "need_end", False):
                emit_triton_end_event(getattr(self._tls, "kernel_name", "triton.unknown"))
        except Exception:
            pass  # nosec B110
        finally:
            self._tls_clear()
        return ret


triton_collector = TritonCollector()


def enable_triton_collector() -> None:
    """Enabled from C++ EventTraceManager when tracing starts (v1 kwargs-skip path)."""
    triton_collector.enable()


def disable_triton_collector() -> None:
    """Disabled from C++ EventTraceManager when tracing stops."""
    triton_collector.disable()


def get_triton_collector_stats() -> Dict[str, int]:
    """Return a shallow copy of collector counters (for debugging / UT)."""
    with triton_collector._lock:
        return dict(triton_collector.stats)
