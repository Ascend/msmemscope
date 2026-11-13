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

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>
#include <gtest/gtest.h>

#include "framework/event_router.h"
#include "analysis/event_dispatcher.h"
#include "analysis/memory_state_manager.h"

using namespace MemScope;

namespace {
constexpr uint64_t UT_ADDR_SHADOW = 0x5A5A00010000ULL;
constexpr uint64_t UT_ADDR_PROC_EXIT = 0x5A5A00020000ULL;
constexpr int32_t UT_DEVICE = 3;

// 探测订阅者：探针id复用废弃占位HAL_ANALYZER，析构时自动退订，避免影响其他用例
class RouterProbe {
public:
    using OnEvent = std::function<void(std::shared_ptr<EventBase>&, MemoryState*)>;

    RouterProbe(const std::vector<EventBaseType>& types, const OnEvent& onEvent)
    {
        EventDispatcher::GetInstance().Subscribe(SubscriberId::HAL_ANALYZER, types,
                                                 EventDispatcher::Priority::Low, onEvent, "ut_router_probe");
    }
    ~RouterProbe()
    {
        EventDispatcher::GetInstance().UnSubscribe(SubscriberId::HAL_ANALYZER);
    }
    RouterProbe(const RouterProbe&) = delete;
    RouterProbe& operator=(const RouterProbe&) = delete;
};

std::shared_ptr<MemoryEvent> MakeMalloc(uint64_t pid, uint64_t addr, int64_t size, bool shadow)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HOST;
    event->poolType = PoolType::HOST;
    event->pid = pid;
    event->device = UT_DEVICE;
    event->addr = addr;
    event->size = size;
    event->isShadowEvent = shadow;
    return event;
}
}  // namespace

// 空事件直接返回；非MemoryEvent的MALLOC事件在状态更新阶段返回nullptr，但仍会分发给分析器
TEST(EventRouterTest, null_event_and_non_memory_event)
{
    int calls = 0;
    MemoryState* receivedState = reinterpret_cast<MemoryState*>(1);
    RouterProbe probe({EventBaseType::MALLOC}, [&calls, &receivedState](std::shared_ptr<EventBase>&,
                                                                        MemoryState* state) {
        ++calls;
        receivedState = state;
    });

    EventHandler(nullptr);
    EXPECT_EQ(calls, 0);

    auto rawEvent = std::make_shared<EventBase>();
    rawEvent->eventType = EventBaseType::MALLOC;
    rawEvent->poolType = PoolType::HOST;
    rawEvent->pid = static_cast<uint64_t>(::getpid());
    rawEvent->device = UT_DEVICE;
    rawEvent->addr = 0x5A5A00030000ULL;

    EventHandler(rawEvent);
    EXPECT_EQ(calls, 1);
    // dynamic_pointer_cast<MemoryEvent>失败，状态更新返回nullptr
    EXPECT_EQ(receivedState, nullptr);
}

// 影子申请未转正即收到CLEAN_UP：状态直接消亡，不留痕
TEST(EventRouterTest, cleanup_deletes_shadow_created_state)
{
    const uint64_t pid = static_cast<uint64_t>(::getpid());
    EventRouter::Instance().Route(MakeMalloc(pid, UT_ADDR_SHADOW, 64, true));

    auto cleanUp = std::make_shared<CleanUpEvent>(EventSubType::RESIDUAL_BLOCK, PoolType::HOST, pid,
                                                  UT_ADDR_SHADOW);
    cleanUp->device = UT_DEVICE;

    MemoryState* state = MemoryStateManager::GetInstance().GetState(cleanUp);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->shadowState, ShadowState::SHADOW_CREATED);

    EventRouter::Instance().Route(cleanUp);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetState(cleanUp), nullptr);
}

// 正常块在PROC_EXIT时补FREE事件，时间戳取上次stop时间+1
TEST(EventRouterTest, proc_exit_appends_free_event_with_stop_timestamp)
{
    const uint64_t pid = static_cast<uint64_t>(::getpid());
    EventRouter::Instance().Route(MakeMalloc(pid, UT_ADDR_PROC_EXIT, 128, false));

    int calls = 0;
    size_t eventCount = 0;
    uint64_t appendedTimestamp = 0;
    EventBaseType appendedType = EventBaseType::INVALID;
    RouterProbe probe({EventBaseType::CLEAN_UP}, [&calls, &eventCount, &appendedTimestamp,
                                                   &appendedType](std::shared_ptr<EventBase>&,
                                                                  MemoryState* state) {
        ++calls;
        if (state == nullptr) {
            return;
        }
        eventCount = state->events.size();
        if (!state->events.empty()) {
            appendedType = state->events.back()->eventType;
            appendedTimestamp = state->events.back()->timestamp;
        }
    });

    MemoryStateManager::GetInstance().SetLastStopTimestamp(777);
    auto cleanUp = std::make_shared<CleanUpEvent>(EventSubType::PROC_EXIT, PoolType::HOST, pid,
                                                  UT_ADDR_PROC_EXIT);
    cleanUp->device = UT_DEVICE;
    EventRouter::Instance().Route(cleanUp);
    MemoryStateManager::GetInstance().ClearLastStopTimestamp();

    EXPECT_EQ(calls, 1);
    // 原MALLOC事件 + 补的一条FREE记录
    EXPECT_EQ(eventCount, 2U);
    EXPECT_EQ(appendedType, EventBaseType::FREE);
    EXPECT_EQ(appendedTimestamp, 778U);
    // CLEAN_UP后状态被清理
    EXPECT_EQ(MemoryStateManager::GetInstance().GetState(cleanUp), nullptr);
}
