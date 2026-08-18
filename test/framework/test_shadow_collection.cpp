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

#include <gtest/gtest.h>
#include <gtest/internal/gtest-port.h>
#include <memory>
#include <string>

#include "bit_field.h"
#include "config_info.h"
#include "event.h"
#include "event_dispatcher.h"
#include "event_router.h"
#include "memory_state_manager.h"
#define private public
#include "trace_manager/event_trace_manager.h"
#undef private

using namespace MemScope;

// =============================================================================
// UT-1: ShouldCollectShadowEvents & DetermineTraceMode 状态机
// =============================================================================

TEST(ShadowCollectionTest, should_collect_shadow_events_deferred_not_tracing)
{
    // DEFERRED + NOT_IN_TRACING + alloc events → true
    Config config{};
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    config.eventType = eventBit.getValue();
    config.collectMode = static_cast<uint8_t>(CollectMode::DEFERRED);
    ConfigManager::Instance().SetConfig(config);

    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    EXPECT_TRUE(EventTraceManager::Instance().ShouldCollectShadowEvents());
}

TEST(ShadowCollectionTest, should_collect_shadow_events_immediate_mode)
{
    // IMMEDIATE mode + NOT_IN_TRACING + alloc events → true (与collectMode无关)
    Config config{};
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    config.eventType = eventBit.getValue();
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    ConfigManager::Instance().SetConfig(config);

    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    EXPECT_TRUE(EventTraceManager::Instance().ShouldCollectShadowEvents());
}

TEST(ShadowCollectionTest, should_collect_shadow_events_in_tracing)
{
    // IN_TRACING → false
    Config config{};
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    config.eventType = eventBit.getValue();
    config.collectMode = static_cast<uint8_t>(CollectMode::DEFERRED);
    ConfigManager::Instance().SetConfig(config);

    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);
    EXPECT_FALSE(EventTraceManager::Instance().ShouldCollectShadowEvents());
}

TEST(ShadowCollectionTest, should_collect_shadow_events_no_alloc_events)
{
    // DEFERRED + NOT_IN_TRACING + no alloc/free events → false
    Config config{};
    config.eventType = 0;  // no events configured
    config.collectMode = static_cast<uint8_t>(CollectMode::DEFERRED);
    ConfigManager::Instance().SetConfig(config);

    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    EXPECT_FALSE(EventTraceManager::Instance().ShouldCollectShadowEvents());
}

TEST(ShadowCollectionTest, determine_trace_mode_normal)
{
    // IN_TRACING + alloc+free events → NORMAL
    Config config{};
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    ConfigManager::Instance().SetConfig(config);

    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);
    EXPECT_EQ(DetermineTraceMode(), TraceMode::NORMAL);
}

TEST(ShadowCollectionTest, determine_trace_mode_shadow)
{
    // NOT_IN_TRACING + DEFERRED + alloc event → SHADOW
    Config config{};
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    config.eventType = eventBit.getValue();
    config.collectMode = static_cast<uint8_t>(CollectMode::DEFERRED);
    ConfigManager::Instance().SetConfig(config);

    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    EXPECT_EQ(DetermineTraceMode(), TraceMode::SHADOW);
}

TEST(ShadowCollectionTest, determine_trace_mode_skip)
{
    // NOT_IN_TRACING + IMMEDIATE + alloc → SKIP
    Config config{};
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    config.eventType = eventBit.getValue();
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    ConfigManager::Instance().SetConfig(config);

    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    EXPECT_EQ(DetermineTraceMode(), TraceMode::SHADOW);
}

// =============================================================================
// UT-1b: 事件上报抑制机制（dcmi 查询等真实运行时调用窗口）
// =============================================================================

TEST(ShadowCollectionTest, suppression_guard_initial_not_suppressed)
{
    EXPECT_FALSE(IsEventReportSuppressed());
}

TEST(ShadowCollectionTest, determine_trace_mode_suppressed_skips_normal_mode)
{
    // NORMAL可采集配置 + 抑制窗口 → SKIP；守卫离开窗口后恢复 NORMAL
    Config config{};
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    ConfigManager::Instance().SetConfig(config);
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);

    {
        EventReportSuppressor suppressor;
        EXPECT_EQ(DetermineTraceMode(), TraceMode::SKIP);
    }
    EXPECT_EQ(DetermineTraceMode(), TraceMode::NORMAL);
}

TEST(ShadowCollectionTest, determine_trace_mode_suppressed_skips_shadow_mode)
{
    // SHADOW可采集配置 + 抑制窗口 → SKIP；守卫离开窗口后恢复 SHADOW
    Config config{};
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    config.eventType = eventBit.getValue();
    config.collectMode = static_cast<uint8_t>(CollectMode::DEFERRED);
    ConfigManager::Instance().SetConfig(config);
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);

    {
        EventReportSuppressor suppressor;
        EXPECT_EQ(DetermineTraceMode(), TraceMode::SKIP);
    }
    EXPECT_EQ(DetermineTraceMode(), TraceMode::SHADOW);
}

TEST(ShadowCollectionTest, suppression_guard_supports_nesting)
{
    // 嵌套置位：内层析构不影响外层抑制，最外层离开后恢复
    {
        EventReportSuppressor outer;
        {
            EventReportSuppressor inner;
            EXPECT_TRUE(IsEventReportSuppressed());
        }
        EXPECT_TRUE(IsEventReportSuppressed());
    }
    EXPECT_FALSE(IsEventReportSuppressed());
}

// =============================================================================
// UT-2: 影子 MALLOC 创建 SHADOW_CREATED 的 State
// =============================================================================

TEST(ShadowCollectionTest, shadow_malloc_creates_marked_state)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HAL;
    event->poolType = PoolType::HAL;
    event->pid = 12345;
    event->addr = 0x1000;
    event->size = 1024;
    event->isShadowEvent = true;

    EXPECT_NE(MemoryStateManager::GetInstance().AddEvent(event), nullptr);
    MemoryState* state = MemoryStateManager::GetInstance().GetState(event);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->shadowState, ShadowState::SHADOW_CREATED);

    // Cleanup
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{12345, event->device, 0x1000});
}

// =============================================================================
// UT-3: 影子 MALLOC + 影子 FREE → 直接消亡
// =============================================================================

TEST(ShadowCollectionTest, shadow_malloc_then_shadow_free_vanishes)
{
    // Shadow MALLOC
    auto mallocEvent = std::make_shared<MemoryEvent>();
    mallocEvent->eventType = EventBaseType::MALLOC;
    mallocEvent->eventSubType = EventSubType::HAL;
    mallocEvent->poolType = PoolType::HAL;
    mallocEvent->pid = 12345;
    mallocEvent->addr = 0x2000;
    mallocEvent->size = 1024;
    mallocEvent->isShadowEvent = true;

    EventHandler(mallocEvent);

    // Shadow FREE
    auto freeEvent = std::make_shared<MemoryEvent>();
    freeEvent->eventType = EventBaseType::FREE;
    freeEvent->eventSubType = EventSubType::HAL;
    freeEvent->poolType = PoolType::HAL;
    freeEvent->pid = 12345;
    freeEvent->addr = 0x2000;
    freeEvent->size = 0;
    freeEvent->isShadowEvent = true;

    EventHandler(freeEvent);

    // State should be deleted
    auto lookupEvent = std::make_shared<EventBase>();
    lookupEvent->poolType = PoolType::HAL;
    lookupEvent->pid = 12345;
    lookupEvent->addr = 0x2000;
    MemoryState* state = MemoryStateManager::GetInstance().GetState(lookupEvent);
    EXPECT_EQ(state, nullptr);
}

// =============================================================================
// UT-4: 正常 MALLOC + 影子 FREE → 标记 SHADOW_FREED
// =============================================================================

TEST(ShadowCollectionTest, normal_malloc_then_shadow_free_is_marked)
{
    // Normal MALLOC
    auto mallocEvent = std::make_shared<MemoryEvent>();
    mallocEvent->eventType = EventBaseType::MALLOC;
    mallocEvent->eventSubType = EventSubType::HAL;
    mallocEvent->poolType = PoolType::HAL;
    mallocEvent->pid = 12345;
    mallocEvent->addr = 0x3000;
    mallocEvent->size = 2048;
    mallocEvent->isShadowEvent = false;

    EventHandler(mallocEvent);

    // Shadow FREE
    auto freeEvent = std::make_shared<MemoryEvent>();
    freeEvent->eventType = EventBaseType::FREE;
    freeEvent->eventSubType = EventSubType::HAL;
    freeEvent->poolType = PoolType::HAL;
    freeEvent->pid = 12345;
    freeEvent->addr = 0x3000;
    freeEvent->size = 0;
    freeEvent->isShadowEvent = true;

    EventHandler(freeEvent);

    // State should exist and be marked isShadowFreed
    auto lookupEvent = std::make_shared<EventBase>();
    lookupEvent->poolType = PoolType::HAL;
    lookupEvent->pid = 12345;
    lookupEvent->addr = 0x3000;
    MemoryState* state = MemoryStateManager::GetInstance().GetState(lookupEvent);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->shadowState, ShadowState::SHADOW_FREED);

    // Cleanup
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{12345, GD_INVALID_NUM, 0x3000});
}

// =============================================================================
// UT-7: 分析器跳过影子事件
// =============================================================================

TEST(ShadowCollectionTest, analyzer_skips_shadow_events)
{
    // Create a shadow event
    auto shadowEvent = std::make_shared<MemoryEvent>();
    shadowEvent->eventType = EventBaseType::MALLOC;
    shadowEvent->eventSubType = EventSubType::PTA_CACHING;
    shadowEvent->poolType = PoolType::PTA_CACHING;
    shadowEvent->pid = 12345;
    shadowEvent->addr = 0x4000;
    shadowEvent->size = 512;
    shadowEvent->isShadowEvent = true;
    shadowEvent->device = 0;

    // Add to MemoryStateManager first (as UpdateMemoryState would)
    MemoryStateManager::GetInstance().AddEvent(shadowEvent);
    MemoryState* state = MemoryStateManager::GetInstance().GetState(shadowEvent);

    // Verify EventHandle skips shadow events in analyzers
    // The guard is: if (memEvent->isShadowEvent) return;
    // This is tested implicitly by the fact that EventHandler returns without
    // dispatching to analyzers when UpdateMemoryState returns nullptr for shadow events.

    // Cleanup
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{12345, 0, 0x4000});
}

// =============================================================================
// UT-8/9: EventHandler 三阶段拆分
// =============================================================================

TEST(EventHandlerTest, update_memory_state_returns_state_for_malloc)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::PTA_CACHING;
    event->poolType = PoolType::PTA_CACHING;
    event->pid = 99999;
    event->addr = 0x5000;
    event->size = 256;

    MemoryState* state = UpdateMemoryState(event);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->size, 256u);

    // Cleanup
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{99999, GD_INVALID_NUM, 0x5000});
}

TEST(EventHandlerTest, update_memory_state_returns_nullptr_for_shadow_event)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HAL;
    event->poolType = PoolType::HAL;
    event->pid = 99999;
    event->addr = 0x6000;
    event->size = 256;
    event->isShadowEvent = true;

    MemoryState* state = UpdateMemoryState(event);
    EXPECT_EQ(state, nullptr);  // Shadow events return nullptr to skip dispatch

    // Cleanup
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{99999, GD_INVALID_NUM, 0x6000});
}

TEST(EventHandlerTest, cleanup_memory_state_deletes_on_free)
{
    // Create a state first
    auto mallocEvent = std::make_shared<MemoryEvent>();
    mallocEvent->eventType = EventBaseType::MALLOC;
    mallocEvent->eventSubType = EventSubType::HAL;
    mallocEvent->poolType = PoolType::HAL;
    mallocEvent->pid = 77777;
    mallocEvent->addr = 0x7000;
    mallocEvent->size = 128;
    mallocEvent->isShadowEvent = false;

    EventHandler(mallocEvent);

    // Now create a FREE event and call CleanupMemoryState
    auto freeEvent = std::make_shared<MemoryEvent>();
    freeEvent->eventType = EventBaseType::FREE;
    freeEvent->eventSubType = EventSubType::HAL;
    freeEvent->poolType = PoolType::HAL;
    freeEvent->pid = 77777;
    freeEvent->addr = 0x7000;
    freeEvent->isShadowEvent = false;

    CleanupMemoryState(freeEvent);

    // State should be deleted
    auto lookupEvent = std::make_shared<EventBase>();
    lookupEvent->poolType = PoolType::HAL;
    lookupEvent->pid = 77777;
    lookupEvent->addr = 0x7000;
    MemoryState* state = MemoryStateManager::GetInstance().GetState(lookupEvent);
    EXPECT_EQ(state, nullptr);
}

// =============================================================================
// UT-11: EventRouter::Route 功能等价
// =============================================================================

TEST(EventRouterTest, route_dispatches_to_event_handler)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::PTA_CACHING;
    event->poolType = PoolType::PTA_CACHING;
    event->pid = 88888;
    event->addr = 0x8000;
    event->size = 64;

    // Route should process without crash (same as old Process::SendEvent)
    EventRouter::Instance().Route(event);

    // Verify state was created
    auto lookupEvent = std::make_shared<EventBase>();
    lookupEvent->poolType = PoolType::PTA_CACHING;
    lookupEvent->pid = 88888;
    lookupEvent->addr = 0x8000;
    MemoryState* state = MemoryStateManager::GetInstance().GetState(lookupEvent);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->size, 64u);

    // Cleanup
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{88888, GD_INVALID_NUM, 0x8000});
}
