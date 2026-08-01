/* -------------------------------------------------------------------------
 * This file is part of the MindStudio project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 *
 * MindStudio is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 */

#ifndef FRAMEWORK_EVENT_ROUTER_H
#define FRAMEWORK_EVENT_ROUTER_H

#include <memory>

#include "event.h"
#include "memory_state_manager.h"

namespace MemScope
{

/*
 * EventRouter类主要功能：
 * 事件路由：接收事件 → EventHandler → 分发
 * 无状态单例，CLI和Python API模式共用
 * 替代了Process::SendEvent中硬编码的switch-case分发逻辑
 */
class EventRouter
{
   public:
    static EventRouter& Instance();
    void Route(std::shared_ptr<EventBase> event);

   private:
    EventRouter() = default;
};

// EventHandler三阶段拆分
// 阶段1: 更新内存块状态追踪（含影子事件处理），返回MemoryState*供后续阶段使用
MemoryState* UpdateMemoryState(std::shared_ptr<EventBase> event);
// 阶段2: 通过EventDispatcher分发给注册的分析器
void DispatchToAnalyzers(std::shared_ptr<EventBase> event, MemoryState* state);
// 阶段3: 清理已完成生命周期的内存块状态
void CleanupMemoryState(std::shared_ptr<EventBase> event);
// 编排函数
void EventHandler(std::shared_ptr<EventBase> event);

}  // namespace MemScope

#endif
