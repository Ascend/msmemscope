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
// 阶段一：UpdateMemoryState更新内存块信息
// 阶段二：DispatchToAnalyzers事件分发给分析器处理
// 阶段三：CleanupMemoryState清理已消亡的内存块
// =============================================================================

// 影子FREE事件：按当前影子状态迁移生命周期
static void HandleShadowEvent(std::shared_ptr<EventBase> event, MemoryState* state)
{
    if (event->eventType != EventBaseType::FREE || state == nullptr)
    {
        return;
    }

    if (state->shadowState == ShadowState::SHADOW_CREATED)
    {
        // 影子分配 + 影子释放 = 直接消亡，不留痕迹
        MemoryStateManager::GetInstance().DeteleState(event->poolType,
                                                      MemoryStateKey{event->pid, event->device, event->addr});
    }
    else if (state->shadowState == ShadowState::NORMAL || state->shadowState == ShadowState::SHADOW_PROMOTED)
    {
        // 正常分配/已转正影子块 + 影子释放 → 标记SHADOW_FREED待下次start或exit时处理
        state->shadowState = ShadowState::SHADOW_FREED;
        // 影子释放事件时间戳刷为上次stop时间 + 1
        uint64_t stopTs = MemoryStateManager::GetInstance().GetLastStopTimestamp();
        if (stopTs > 0)
        {
            event->timestamp = stopTs + 1;
        }
    }
}

// CLEAN_UP时补一条FREE记录，防止累计内存异常增长
static void AppendCleanUpFreeEvent(std::shared_ptr<EventBase> event, MemoryState* state)
{
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
        if (state->events[0]->eventType == EventBaseType::MALLOC)
        {
            freeEvent->eventSubType = state->events[0]->eventSubType;
        }
    }

    // 析构时补的FREE事件，若当前不在trace状态，时间戳改为上次stop时间+1，防止短暂采集场景有进程退出时的记录，大大拉长时间轴跨度
    if (event->eventSubType == EventSubType::PROC_EXIT)
    {
        uint64_t stopTs = MemoryStateManager::GetInstance().GetLastStopTimestamp();
        if (stopTs > 0)
        {
            freeEvent->timestamp = stopTs + 1;
        }
    }

    state->events.push_back(freeEvent);
}

// MALLOC/FREE/ACCESS事件：维护内存块状态；影子事件不落盘、不分析，仅维护State
static MemoryState* UpdateMemoryEventState(std::shared_ptr<EventBase> event)
{
    auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event);
    if (memEvent == nullptr)
    {
        return nullptr;
    }

    MemoryStateManager& manager = MemoryStateManager::GetInstance();
    // AddEvent内部已定位到事件所属state并返回，避免调用方再GetState一次（二次加锁+二次查找）
    MemoryState* state = manager.AddEvent(memEvent);
    if (state == nullptr)
    {
        // 添加事件失败时，表明对应位置已存在事件，需先清空事件列表
        // MALLOC冲突命中key(pid, device, addr)，memEvent->device即冲突块的device，供DeteleState定位
        std::shared_ptr<EventBase> cleanUpEvent = std::make_shared<CleanUpEvent>(
            EventSubType::RESIDUAL_BLOCK, memEvent->poolType, memEvent->pid, memEvent->addr);
        cleanUpEvent->device = memEvent->device;
        EventHandler(cleanUpEvent);          // 最大递归深度为2，因为这里传入事件的类型为CLEAN_UP
        state = manager.AddEvent(memEvent);  // 再次尝试添加
    }

    // 影子事件处理：不落盘、不分析，仅维护State
    if (memEvent->isShadowEvent)
    {
        HandleShadowEvent(event, state);
        // 影子MALLOC: state已创建且已标记SHADOW_CREATED（在MemoryStateManager::AddEvent中）
        // 返回nullptr给EventHandler，跳过dispatch和cleanup
        return nullptr;
    }
    return state;
}

// CLEAN_UP事件：获取状态；CLEAN_UP时按影子状态补充FREE记录
static MemoryState* HandleCleanUpEvent(std::shared_ptr<EventBase> event)
{
    MemoryStateManager& manager = MemoryStateManager::GetInstance();
    MemoryState* state = manager.GetState(event);

    if (event->eventType != EventBaseType::CLEAN_UP || state == nullptr)
    {
        return state;
    }

    if (state->shadowState == ShadowState::SHADOW_CREATED)
    {
        // 影子申请未转正 + CLEAN_UP → 直接消亡，不留痕迹
        manager.DeteleState(event->poolType, MemoryStateKey{event->pid, event->device, event->addr});
        return nullptr;  // 跳过dispatch和cleanup
    }

    if (state->shadowState != ShadowState::SHADOW_FREED)
    {
        // NORMAL / SHADOW_PROMOTED → 补一条FREE记录防止累计内存异常增长
        AppendCleanUpFreeEvent(event, state);
    }
    return state;
}

// TRACE_START/TRACE_STOP事件：维护采集窗口时间戳
static MemoryState* HandleTraceEvent(std::shared_ptr<EventBase> event)
{
    if (event->eventSubType == EventSubType::TRACE_START)
    {
        MemoryStateManager::GetInstance().ClearLastStopTimestamp();
        // start时重建统计基线：晚于存量影子块转正，从当前存量块重新累计为事实（防影子期累计失真）
        MemoryStateManager::GetInstance().ResetUsageBaseline();
    }
    else if (event->eventSubType == EventSubType::TRACE_STOP)
    {
        MemoryStateManager::GetInstance().SetLastStopTimestamp(event->timestamp);
    }
    return nullptr;
}

// 阶段1: 更新内存块状态追踪，仅内存类事件需要，同时返回事件关联内存块
MemoryState* UpdateMemoryState(std::shared_ptr<EventBase> event)
{
    switch (event->eventType)
    {
        case EventBaseType::MALLOC:
        case EventBaseType::FREE:
        case EventBaseType::ACCESS:
        {
            return UpdateMemoryEventState(event);
        }
        case EventBaseType::MEMORY_OWNER:
        {
            return MemoryStateManager::GetInstance().GetState(event);
        }
        case EventBaseType::CLEAN_UP:
        {
            return HandleCleanUpEvent(event);
        }
        case EventBaseType::SYSTEM:
        {
            return HandleTraceEvent(event);
        }
        default:
        {
            return nullptr;
        }
    }
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
        MemoryStateManager::GetInstance().DeteleState(event->poolType,
                                                      MemoryStateKey{event->pid, event->device, event->addr});
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
