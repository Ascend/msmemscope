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

"""Triton ACCESS via hijacker + memscope-aten-* MSTX.

Known tensor_kinds → precise R/W; missing/mismatch/unknown → UNKNOWN ACCESS
(is_read=False;is_write=False). Kwargs NPU tensors skip ACCESS. Lazy triton
hijack via module POST_HOOK (no force-import). Diagnostics use stderr.
"""

from __future__ import annotations

import sys
import threading
import types as _types
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple

from .hijacker.hijack_utility import PRE_HOOK, POST_HOOK, hijacker, release

_SKIP_ACCESS_WARNED: Set[str] = set()
_MSTX_UNAVAILABLE_WARNED = False

_NPU_LAUNCHER_MODULE = "triton.backends.ascend.driver"
_NPU_LAUNCHER_CLS = "NPULauncher"
_NPU_LAUNCHER_FUNC = "__call__"
_NPU_LAUNCHER_TARGETS = ((_NPU_LAUNCHER_MODULE, _NPU_LAUNCHER_CLS),)

try:
    import mstx
except Exception:  # pragma: no cover
    mstx = None

try:
    import torch
except Exception:  # pragma: no cover
    torch = None

TENSOR_KIND_INPUT = 0
TENSOR_KIND_OUTPUT = 1
TENSOR_KIND_INOUT = 2
_KNOWN_TENSOR_KINDS = frozenset((TENSOR_KIND_INPUT, TENSOR_KIND_OUTPUT, TENSOR_KIND_INOUT))

_LAUNCH_META_INDEX = 6
_PACKED_META_INDEX = 5
_KERNEL_ARGS_START = 9
_PACKED_TO_KERNEL_ARGS = 4
_SCAN_ARG_LIMIT = 20

_EXCLUDED_TENSOR_SUBCLASSES: Optional[Tuple[type, ...]] = None
_EXCLUDED_TENSOR_SUBCLASSES_RESOLVED = False


def _diag(msg: str) -> None:
    print(f"[msmemscope] {msg}", file=sys.stderr)


def _excluded_tensor_subclasses() -> Tuple[type, ...]:
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


def _append_access_mark(
    marks: List[str],
    mark_fn,
    tensor: Any,
    kernel_name: str,
    *,
    is_read: bool,
    is_write: bool,
    is_output: bool,
) -> None:
    try:
        data_ptr = int(tensor.data_ptr())
        tensor_size = calculate_tensor_size(tensor)
    except Exception:
        return  # nosec B110
    if data_ptr == 0:
        return
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
        meta_hint = f" ({_metadata_summary(packed_metadata)})" if packed_metadata is not None else ""
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
        key = f"{kernel_name}|{detail}|unknown"
        if key not in _SKIP_ACCESS_WARNED:
            _SKIP_ACCESS_WARNED.add(key)
            _diag(
                f"Triton {detail} for {kernel_name}; "
                "emitting ACCESS with UNKNOWN direction (is_read=False;is_write=False)."
            )
        for tensor in npu_tensors:
            _append_access_mark(
                marks, mark_fn, tensor, kernel_name, is_read=False, is_write=False, is_output=False
            )
        return marks

    for tensor, kind, kind_missing in iter_tensor_args_with_kinds(kernel_args, tensor_kinds):
        flags = None if kind_missing else kind_to_access_flags(kind)
        if flags is None:
            _append_access_mark(
                marks, mark_fn, tensor, kernel_name, is_read=False, is_write=False, is_output=False
            )
            continue
        is_read, is_write, is_output = flags
        _append_access_mark(
            marks, mark_fn, tensor, kernel_name, is_read=is_read, is_write=is_write, is_output=is_output
        )
    return marks


def emit_triton_end_event(kernel_name: str, *, mark_fn=None) -> str:
    if mark_fn is None:
        mark_fn = _default_mark
    end = build_end_mark(kernel_name)
    mark_fn(end)
    return end


def _default_mark(msg: str) -> None:
    if mstx is None:
        return
    mstx.mark(msg, None)


def _ensure_mstx() -> bool:
    global mstx, _MSTX_UNAVAILABLE_WARNED
    if mstx is not None:
        return True
    try:
        import mstx as _mstx  # type: ignore

        mstx = _mstx
    except Exception:
        mstx = None
    if mstx is None and not _MSTX_UNAVAILABLE_WARNED:
        _MSTX_UNAVAILABLE_WARNED = True
        _diag(
            "mstx is unavailable; Triton collector will not install launch hooks. "
            "Install/ensure mstx for collection."
        )
    return mstx is not None


def _parse_launch_args(args: tuple, kwargs: Optional[dict] = None) -> Optional[Tuple[Any, Any, tuple, bool]]:
    packed_metadata = None
    launch_metadata = None
    kernel_args: Optional[list] = None

    if len(args) >= _KERNEL_ARGS_START and _is_packed_metadata(args[_PACKED_META_INDEX]):
        packed_metadata = args[_PACKED_META_INDEX]
        launch_metadata = args[_LAUNCH_META_INDEX]
        kernel_args = list(args[_KERNEL_ARGS_START:])

    if kernel_args is None:
        scan_limit = min(len(args), _SCAN_ARG_LIMIT)
        for i in range(scan_limit):
            if not _is_packed_metadata(args[i]):
                continue
            packed_metadata = args[i]
            if i + _PACKED_TO_KERNEL_ARGS <= len(args):
                launch_metadata = args[i + 1] if i + 1 < len(args) else None
                kernel_args = list(args[i + _PACKED_TO_KERNEL_ARGS :])
            else:
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
        self.collect_launch = True
        self.collect_access = True
        self._hooked = False
        self._hooked_targets: Set[Tuple[str, str]] = set()
        self._handlers: List[Any] = []
        self._import_hooks: List[Any] = []
        self._lock = threading.RLock()
        self._tls = threading.local()
        self.stats = {
            "kernels_seen": 0,
            "launch_with_access": 0,
            "launch_with_unknown_access": 0,
            "skip_kwargs": 0,
            "no_tensor": 0,
            "access_gated": 0,
            "emit_failed": 0,
            "access_emitted": 0,
            "access_skipped": 0,
            "enable_calls": 0,
        }

    def __del__(self):
        is_finalizing = getattr(sys, "is_finalizing", lambda: False)
        if (sys is not None) and (not is_finalizing()) and self.enabled:
            try:
                self.disable()
            except Exception:
                pass  # nosec B110

    def enable(self, collect_launch: bool = True, collect_access: bool = True) -> None:
        with self._lock:
            self.collect_launch = bool(collect_launch)
            self.collect_access = bool(collect_access)
            self.enabled = True
            self.stats["enable_calls"] = self.stats.get("enable_calls", 0) + 1
            if not _ensure_mstx():
                self._uninstall_hijacks()
                return
            self._ensure_import_hooks()
            self._try_install_hijacks()

    def disable(self) -> None:
        global _MSTX_UNAVAILABLE_WARNED
        with self._lock:
            self._uninstall_hijacks()
            self._uninstall_import_hooks()
            self.enabled = False
            self.collect_launch = True
            self.collect_access = True
            _SKIP_ACCESS_WARNED.clear()
            _MSTX_UNAVAILABLE_WARNED = False

    def _bump_stat(self, key: str) -> None:
        with self._lock:
            self.stats[key] = self.stats.get(key, 0) + 1

    def _on_driver_imported(self, module) -> None:  # pylint: disable=unused-argument
        with self._lock:
            if self.enabled and mstx is not None:
                self._try_install_hijacks()

    def _ensure_import_hooks(self) -> None:
        if self._import_hooks:
            return
        try:
            self._import_hooks.append(
                hijacker(
                    stub=self._on_driver_imported,
                    module=_NPU_LAUNCHER_MODULE,
                    action=POST_HOOK,
                    priority=50,
                )
            )
        except Exception as exc:
            _diag(f"failed to register Triton driver import hook: {exc}")

    def _uninstall_import_hooks(self) -> None:
        for handler in self._import_hooks:
            try:
                release(handler)
            except Exception:
                pass  # nosec B110
        self._import_hooks = []

    def _active_launcher_targets(self) -> List[Tuple[str, str]]:
        """Resolve hijack targets without force-importing triton when absent.

        When triton is already loaded, resolve ``driver.active.launcher_cls`` via
        a normal import (lazy proxy needs this) — the live class is often
        ``ascend.NPULauncher``, distinct from ``triton.backends.ascend.driver.NPULauncher``.
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

        cls = None
        if "triton" in sys.modules or "triton.runtime.driver" in sys.modules:
            try:
                from triton.runtime import driver as triton_driver

                active = getattr(triton_driver, "active", None)
                cls = getattr(active, "launcher_cls", None) if active is not None else None
            except Exception:
                cls = None
        if cls is None or id(cls) in seen_cls:
            return targets

        mod_name = getattr(cls, "__module__", None) or "ascend"
        cls_name = getattr(cls, "__name__", None) or _NPU_LAUNCHER_CLS
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
        return targets

    def _try_install_hijacks(self) -> None:
        if _NPU_LAUNCHER_MODULE not in sys.modules and "triton" not in sys.modules:
            return
        targets = self._active_launcher_targets()
        if not targets:
            return
        # Refresh if live launcher (e.g. ascend.NPULauncher) appeared after first hook.
        if self._hooked:
            want = set(targets)
            if getattr(self, "_hooked_targets", None) == want:
                return
            self._uninstall_hijacks()

        handlers: List[Any] = []
        try:
            for module, cls_name in targets:
                if module not in sys.modules and module != "ascend":
                    if module == _NPU_LAUNCHER_MODULE:
                        continue
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
            self._hooked_targets = set(targets) if handlers else set()
        except Exception as exc:
            _diag(f"failed to register Triton NPULauncher hijack: {exc}")
            for handler in handlers:
                try:
                    release(handler)
                except Exception:
                    pass  # nosec B110
            self._handlers = []
            self._hooked = False
            self._hooked_targets = set()

    def _uninstall_hijacks(self) -> None:
        for handler in self._handlers:
            try:
                release(handler)
            except Exception as exc:
                _diag(f"failed to release Triton NPULauncher hijack: {exc}")
        self._handlers = []
        self._hooked = False
        self._hooked_targets = set()
        self._tls_clear()

    def _tls_clear(self) -> None:
        for attr in ("need_end", "kernel_name"):
            if hasattr(self._tls, attr):
                delattr(self._tls, attr)

    def _pre_npu_launcher(self, *args, **kws):
        self._tls.need_end = False
        self._tls.kernel_name = "triton.unknown"
        if not self.enabled:
            return args, kws
        try:
            launch_args = args[1:] if args else ()
            parsed = _parse_launch_args(launch_args, kws)
            if parsed is None:
                return args, kws
            packed_metadata, launch_metadata, kernel_args, kwargs_has_npu = parsed
            kernel_name = extract_kernel_name(launch_metadata, packed_metadata, fallback="unknown")
            self._bump_stat("kernels_seen")
            self._tls.kernel_name = kernel_name
            want_launch = self.collect_launch or self.collect_access
            want_access = self.collect_access
            if not want_launch and not want_access:
                return args, kws
            if want_launch or want_access:
                self._tls.need_end = True

            if not want_access:
                _default_mark(build_begin_mark(kernel_name))
                self._bump_stat("access_gated")
                self._bump_stat("access_skipped")
                return args, kws

            if kwargs_has_npu:
                emit_triton_access_events(
                    kernel_name,
                    kernel_args,
                    None,
                    packed_metadata=packed_metadata,
                    skip_access=True,
                    skip_access_reason="NPU tensor(s) passed via kwargs; order vs tensor_kinds untrusted",
                )
                self._bump_stat("skip_kwargs")
                self._bump_stat("access_skipped")
                return args, kws

            tensor_kinds = extract_tensor_kinds(packed_metadata)
            npu_count = len(_collect_npu_tensors(kernel_args))
            usable, _reason = _kinds_usable(npu_count, tensor_kinds)
            emitted = emit_triton_access_events(
                kernel_name,
                kernel_args,
                tensor_kinds,
                packed_metadata=packed_metadata,
                skip_access=False,
            )
            if npu_count == 0 or len(emitted) <= 1:
                self._bump_stat("no_tensor")
                self._bump_stat("access_skipped")
            elif not usable:
                self._bump_stat("launch_with_unknown_access")
                self._bump_stat("access_emitted")
            else:
                self._bump_stat("launch_with_access")
                self._bump_stat("access_emitted")
        except Exception as exc:
            kname = getattr(self._tls, "kernel_name", "triton.unknown")
            key = f"{kname}|emit_failed|{type(exc).__name__}"
            if key not in _SKIP_ACCESS_WARNED:
                _SKIP_ACCESS_WARNED.add(key)
                _diag(
                    f"Triton ACCESS collection failed in PRE_HOOK for {kname} "
                    f"({type(exc).__name__}); continuing kernel launch without ACCESS marks."
                )
            self._bump_stat("emit_failed")
        return args, kws

    def _post_npu_launcher(self, ret, *args, **kws):
        try:
            if self.enabled and getattr(self._tls, "need_end", False):
                if self.collect_launch or self.collect_access:
                    emit_triton_end_event(getattr(self._tls, "kernel_name", "triton.unknown"))
        except Exception:
            pass  # nosec B110
        finally:
            self._tls_clear()
        return ret


triton_collector = TritonCollector()


def enable_triton_collector(collect_launch: bool = True, collect_access: bool = True) -> None:
    triton_collector.enable(collect_launch=collect_launch, collect_access=collect_access)


def enable_triton_collector_with_access() -> None:
    triton_collector.enable(collect_launch=True, collect_access=True)


def enable_triton_collector_launch_only() -> None:
    triton_collector.enable(collect_launch=True, collect_access=False)


def disable_triton_collector() -> None:
    triton_collector.disable()


def get_triton_collector_stats() -> Dict[str, int]:
    with triton_collector._lock:
        return dict(triton_collector.stats)
