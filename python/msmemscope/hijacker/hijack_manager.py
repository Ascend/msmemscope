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
from .hijack_map import memscope_hijack_map
from .hijack_utility import hijacker, release, PRE_HOOK, POST_HOOK


class MemScopeHijackManager:
    _instance = None

    def __new__(cls, *args, **kwargs):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __init__(self):
        self.registered_handlers = []  # 手动注册（init_framework_hooks）handler
        self.auto_registered_handlers = []  # 自动使能注册 handler（disable_decompose_hooks 释放）
        self._enabled_targets = set()  # 已注册目标集合（跨调用幂等）
        self._manual_decompose_init_called = False  # 会话级手动注册标记（互斥，见 3.3.1）

    def init_framework_hooks(self, framework: str, version: str, component: str, hook_type: str):
        if hook_type == "decompose":
            self._manual_decompose_init_called = True  # 手动 decompose 注册置位标记，自动使能让位
        # 所有函数的hooklet管理器
        hooklet_list = memscope_hijack_map.get_hooklet_list(framework, version, component, hook_type)
        if not hooklet_list:
            return
        for idx, hooklet_unit in enumerate(hooklet_list):
            try:
                # 调用hijacker注册劫持
                pre_handler = hijacker(
                    stub=hooklet_unit.prehook_func,
                    module=hooklet_unit.module,
                    cls=hooklet_unit.class_name,
                    function=hooklet_unit.method_name,
                    action=PRE_HOOK,
                    priority=hooklet_unit.priority,
                )
                self.registered_handlers.append(pre_handler)

                post_handler = hijacker(
                    stub=hooklet_unit.posthook_func,
                    module=hooklet_unit.module,
                    cls=hooklet_unit.class_name,
                    function=hooklet_unit.method_name,
                    action=POST_HOOK,
                    priority=hooklet_unit.priority,
                )
                self.registered_handlers.append(post_handler)

            except Exception as e:
                print(
                    f"[msmemscope] Error: [{idx + 1}/{len(self.registered_handlers)}] "
                    f"Failed to register hijack function, error: {str(e)}"
                )
                return
        print(
            f"[msmemscope] Info: Hijack functions registered successfully for framework '{framework}', "
            f"version '{version}', component {component}, hook type {hook_type}"
        )

    def cleanup_framework_hooks(self):
        # 公开语义不变：全量释放（含自动使能注册的 handler，见 3.3.1 自动/手动分账）
        handlers = self.registered_handlers + self.auto_registered_handlers
        if not handlers:
            print("[msmemscope] Info: No registered hijack handlers, nothing to release")
            return
        for idx, handler in enumerate(handlers):
            try:
                release(handler)
            except Exception as e:
                print(
                    f"[msmemscope] Error: [{idx + 1}/{len(handlers)}] "
                    f"Failed to release hijack function, error: {str(e)}"
                )
        self.registered_handlers.clear()
        self.auto_registered_handlers.clear()
        self._enabled_targets.clear()
        print("[msmemscope] Info: All hijack function handlers have been released")

    def _enable_decompose_hooks(self):
        """遍历 hijack_map 中所有 decompose 条目，去重后统一注册（幂等、
        累积到 auto_registered_handlers）
        """
        if self._manual_decompose_init_called:
            print("[msmemscope] Info: Manual call to init_framework_hooks(decompose) detected, skipping auto-enable")
            return
        # 收集: framework/版本键/component → decompose 条目列表
        pending = []  # [(hooklet_unit), ...] 跨框架/版本键/组件
        seen = set()  # 本次调用内 (module, class_name, method_name) 去重
        hijack_mapping = memscope_hijack_map.hijack_mapping
        for framework, version_map in hijack_mapping.items():
            for version, component_map in version_map.items():
                for component, hook_type_map in component_map.items():
                    if "decompose" not in hook_type_map:
                        continue
                    for unit in memscope_hijack_map.get_hooklet_list(framework, version, component, "decompose"):
                        target = (unit.module, unit.class_name, unit.method_name)
                        if target in seen or target in self._enabled_targets:
                            continue
                        seen.add(target)
                        pending.append(unit)
        if not pending:
            return
        # 统一注册: 单条目失败仅跳过该条目, 不影响其他
        for unit in pending:
            try:
                self.auto_registered_handlers.append(
                    hijacker(
                        stub=unit.prehook_func,
                        module=unit.module,
                        cls=unit.class_name,
                        function=unit.method_name,
                        action=PRE_HOOK,
                        priority=unit.priority,
                    )
                )
                self.auto_registered_handlers.append(
                    hijacker(
                        stub=unit.posthook_func,
                        module=unit.module,
                        cls=unit.class_name,
                        function=unit.method_name,
                        action=POST_HOOK,
                        priority=unit.priority,
                    )
                )
                self._enabled_targets.add((unit.module, unit.class_name, unit.method_name))
            except Exception as e:
                print(
                    f"[msmemscope] Warning: Failed to register decompose hook for {unit.identifier}, "
                    f"skipped, error: {str(e)}"
                )
        print(f"[msmemscope] Info: All decompose hooks registered, {len(self._enabled_targets)} targets in total")

    def _disable_decompose_hooks(self):
        """释放自动使能注册的全部 handler（不影响手动注册的 handler；空列表时静默返回）"""
        if not self.auto_registered_handlers:
            return
        for idx, handler in enumerate(self.auto_registered_handlers):
            try:
                release(handler)
            except Exception as e:
                print(
                    f"[msmemscope] Warning: [{idx + 1}/{len(self.auto_registered_handlers)}] "
                    f"Failed to release auto hook, error: {str(e)}"
                )
        self.auto_registered_handlers.clear()
        self._enabled_targets.clear()


# 生成单例实例
memscope_hijack_manager = MemScopeHijackManager()


def enable_decompose_hooks():
    """供 C++ 侧 HandleWithDecompose 调用：全量注册 decompose 钩子
    （幂等；手动注册后自动让位）
    """
    memscope_hijack_manager._enable_decompose_hooks()


def disable_decompose_hooks():
    """供 C++ 侧 HandleWithDecompose 调用：清理自动使能注册的 decompose 钩子
    （不影响手动注册）
    """
    memscope_hijack_manager._disable_decompose_hooks()
