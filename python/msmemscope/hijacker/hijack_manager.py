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
        self.registered_handlers = []

    def init_framework_hooks(self, framework: str, version: str, component: str, hook_type: str):
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
                print(f"[msmemscope] Error: [{idx + 1}/{len(self.registered_handlers)}] 注册劫持函数失败,错误:{str(e)}")
                return
        print(
            f"[msmemscope] Info: 框架 '{framework}' 下的版本 '{version}' 的 {component} 组件 {hook_type} 劫持函数注册成功"
        )

    def cleanup_framework_hooks(self):
        if not self.registered_handlers:
            print("[msmemscope] Info: 无已注册的劫持处理器，无需释放")
            return
        for idx, handler in enumerate(self.registered_handlers):
            try:
                release(handler)
            except Exception as e:
                print(f"[msmemscope] Error: [{idx + 1}/{len(self.registered_handlers)}] 释放劫持函数失败,错误:{str(e)}")
        self.registered_handlers.clear()
        print("[msmemscope] Info: 所有劫持函数handler均已释放")


# 生成单例实例
memscope_hijack_manager = MemScopeHijackManager()
