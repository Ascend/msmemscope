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

#include "event_router.h"

#include <iostream>
#include <memory>

#include "analysis/event_dispatcher.h"
#include "analysis/memory_state_manager.h"
#include "log.h"

namespace MemScope
{

// =============================================================================
// EventRouter: 无状态事件路由单例，CLI和Python API模式共用
// =============================================================================

EventRouter& EventRouter::Instance()
{
    static EventRouter router;
    return router;
}

void EventRouter::Route(std::shared_ptr<EventBase> event) { EventHandler(event); }

// =============================================================================
// EventHandler三阶段拆分
// =============================================================================

// 阶段1: 更新内存块状态追踪
MemoryState* UpdateMemoryState(std::shared_ptr<EventBase> event)
{
    if (event == nullptr)
    {
        return nullptr;
    }

    MemoryState* state = nullptr;
    if (event->eventType == EventBaseType::MALLOC || event->eventType == EventBaseType::FREE ||
        event->eventType == EventBaseType::ACCESS)
    {
        auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event);
        if (memEvent && !MemoryStateManager::GetInstance().AddEvent(memEvent))
        {
            // 添加事件失败时，表明对应位置已存在事件，需先清空事件列表
            std::shared_ptr<EventBase> cleanUpEvent =
                std::make_shared<CleanUpEvent>(memEvent->poolType, memEvent->pid, memEvent->addr);
            EventHandler(cleanUpEvent);  // 最大递归深度为2，因为这里传入事件的类型为CLEAN_UP
            MemoryStateManager::GetInstance().AddEvent(memEvent);  // 再次尝试添加
        }
        if (memEvent)
        {
            state = MemoryStateManager::GetInstance().GetState(memEvent);
        }

        // 影子事件处理：不落盘、不分析，仅维护State
        if (memEvent && memEvent->isShadowEvent)
        {
            if (event->eventType == EventBaseType::FREE && state != nullptr)
            {
                if (state->shadowState == ShadowState::SHADOW_CREATED)
                {
                    // 影子分配 + 影子释放 = 直接消亡，不留痕迹
                    MemoryStateManager::GetInstance().DeteleState(event->poolType,
                                                                  MemoryStateKey{event->pid, event->addr});
                }
                else if (state->shadowState == ShadowState::NORMAL ||
                         state->shadowState == ShadowState::SHADOW_PROMOTED)
                {
                    // 正常分配/已转正影子块 + 影子释放 → 标记SHADOW_FREED待下次start或exit时处理
                    state->shadowState = ShadowState::SHADOW_FREED;
                }
            }
            // 影子MALLOC: state已创建且已标记SHADOW_CREATED（在MemoryStateManager::AddEvent中）
            // 返回nullptr给EventHandler，跳过dispatch和cleanup
            return nullptr;
        }
    }
    else if (event->eventType == EventBaseType::MEMORY_OWNER || event->eventType == EventBaseType::CLEAN_UP)
    {
        state = MemoryStateManager::GetInstance().GetState(event);

        if (event->eventType == EventBaseType::CLEAN_UP && state != nullptr)
        {
            if (state->shadowState == ShadowState::SHADOW_CREATED)
            {
                // 影子申请未转正 + CLEAN_UP → 直接消亡，不留痕迹
                MemoryStateManager::GetInstance().DeteleState(event->poolType, MemoryStateKey{event->pid, event->addr});
                return nullptr;  // 跳过dispatch和cleanup
            }

            if (state->shadowState != ShadowState::SHADOW_FREED)
            {
                // NORMAL / SHADOW_PROMOTED → 补一条FREE记录防止累计内存异常增长
                auto freeEvent = std::make_shared<MemoryEvent>();
                freeEvent->eventType = EventBaseType::FREE;
                freeEvent->poolType = event->poolType;
                freeEvent->name = "N/A";
                freeEvent->pid = event->pid;
                freeEvent->addr = event->addr;
                freeEvent->size = static_cast<int64_t>(state->size);
                freeEvent->isShadowEvent = true;
                if (!state->events.empty())
                {
                    freeEvent->device = state->events[0]->device;
                    freeEvent->eventSubType = state->events[0]->eventSubType;
                }

                state->events.push_back(freeEvent);
            }
        }
    }

    return state;
}

// 阶段2: 分发给注册的分析器
void DispatchToAnalyzers(std::shared_ptr<EventBase> event, MemoryState* state)
{
    // 影子事件跳过分析器分发（已在UpdateMemoryState中处理完毕）
    if (auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event))
    {
        if (memEvent->isShadowEvent)
        {
            return;
        }
    }
    EventDispatcher::GetInstance().DispatchEvent(event, state);
}

// 阶段3: 清理已完成生命周期的内存块状态
void CleanupMemoryState(std::shared_ptr<EventBase> event)
{
    // 内存块生命周期结束，删除相关缓存数据
    if (event->eventType == EventBaseType::FREE || event->eventType == EventBaseType::CLEAN_UP)
    {
        // 影子事件的状态生命周期已在UpdateMemoryState中处理完毕，此处跳过
        if (auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event))
        {
            if (memEvent->isShadowEvent)
            {
                return;
            }
        }
        MemoryStateManager::GetInstance().DeteleState(event->poolType, MemoryStateKey{event->pid, event->addr});
    }
}

// 编排函数
void EventHandler(std::shared_ptr<EventBase> event)
{
    if (event == nullptr)
    {
        return;
    }

    MemoryState* state = UpdateMemoryState(event);
    DispatchToAnalyzers(event, state);
    CleanupMemoryState(event);
}

}  // namespace MemScope
