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

#include "event.h"
#include "memory_state_manager.h"

using namespace MemScope;

namespace
{
std::shared_ptr<MemoryEvent> CreateMalloc(PoolType poolType, uint64_t addr, int32_t device, uint64_t pid,
                                          uint64_t id, uint64_t timestamp, uint64_t size = 1024)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->poolType = poolType;
    event->addr = addr;
    event->device = device;
    event->pid = pid;
    event->id = id;
    event->timestamp = timestamp;
    event->size = static_cast<int64_t>(size);
    event->kernelIndex = 7;
    return event;
}

std::shared_ptr<MemoryEvent> CreateFree(PoolType poolType, uint64_t addr, int32_t device, uint64_t pid,
                                        uint64_t id, uint64_t timestamp)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->poolType = poolType;
    event->addr = addr;
    event->device = device;
    event->pid = pid;
    event->id = id;
    event->timestamp = timestamp;
    event->size = 0;
    return event;
}
}  // namespace

class MemoryStateManagerTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        // 清理MSM残留状态（单例跨用例共享）
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
    }

    void TearDown() override
    {
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
    }
};

TEST_F(MemoryStateManagerTest, key_includes_device_dimension)
{
    // 同pid同addr不同device是两个独立块
    auto event0 = CreateMalloc(PoolType::PTA_CACHING, 0x1000, 0, 1, 1, 1000);
    auto event1 = CreateMalloc(PoolType::PTA_CACHING, 0x1000, 1, 1, 2, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(event0), nullptr);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(event1), nullptr);

    // device=0的FREE只能匹配device=0的块
    auto free0 = CreateFree(PoolType::PTA_CACHING, 0x1000, 0, 1, 3, 2000);
    MemoryState* state0 = MemoryStateManager::GetInstance().AddEvent(free0);
    ASSERT_NE(state0, nullptr);
    EXPECT_EQ(state0->events[0]->eventType, EventBaseType::MALLOC);
    EXPECT_EQ(free0->device, 0);

    // device=1的块仍在（未被device=0的FREE误匹配）
    auto free1 = CreateFree(PoolType::PTA_CACHING, 0x1000, 1, 1, 4, 3000);
    MemoryState* stateByKey = MemoryStateManager::GetInstance().GetState(free1);
    ASSERT_NE(stateByKey, nullptr);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0x1000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 1, 0x1000});
}

TEST_F(MemoryStateManagerTest, malloc_conflict_returns_nullptr)
{
    auto event1 = CreateMalloc(PoolType::ATB, 0x2000, 0, 1, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(event1), nullptr);
    // 同key二次MALLOC返回nullptr
    auto event2 = CreateMalloc(PoolType::ATB, 0x2000, 0, 1, 2, 2000);
    EXPECT_EQ(MemoryStateManager::GetInstance().AddEvent(event2), nullptr);
    // 不同device不冲突
    auto event3 = CreateMalloc(PoolType::ATB, 0x2000, 1, 1, 3, 2000);
    EXPECT_NE(MemoryStateManager::GetInstance().AddEvent(event3), nullptr);

    MemoryStateManager::GetInstance().DeteleState(PoolType::ATB, MemoryStateKey{1, 0, 0x2000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::ATB, MemoryStateKey{1, 1, 0x2000});
}

TEST_F(MemoryStateManagerTest, free_without_malloc_creates_ghost_state)
{
    auto freeEvent = CreateFree(PoolType::HAL, 0x3000, 0, 1, 1, 1000);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(freeEvent);
    ASSERT_NE(state, nullptr);
    // 幽灵state：仅含当前FREE事件
    EXPECT_EQ(state->events.size(), 1u);
    EXPECT_EQ(state->events[0]->eventType, EventBaseType::FREE);

    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 0, 0x3000});
}

TEST_F(MemoryStateManagerTest, free_event_device_missing_uses_fuzzy_match)
{
    // HAL块分配于device=2
    auto mallocEvent = CreateMalloc(PoolType::HAL, 0x4000, 2, 1, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(mallocEvent), nullptr);

    // FREE事件device缺失（GD_INVALID_NUM）：模糊匹配回填
    auto freeEvent = CreateFree(PoolType::HAL, 0x4000, GD_INVALID_NUM, 1, 2, 2000);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(freeEvent);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->events[0]->eventType, EventBaseType::MALLOC);
    EXPECT_EQ(freeEvent->device, 2);  // 已回填为分配时device
    EXPECT_EQ(freeEvent->size, static_cast<int64_t>(1024));  // HAL free缺size，从MALLOC回填

    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 2, 0x4000});
}

TEST_F(MemoryStateManagerTest, query_live_blocks_filters_by_pool_device_pid)
{
    auto npuEvent = CreateMalloc(PoolType::PTA_CACHING, 0x5000, 0, 1, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent), nullptr);
    auto atbEvent = CreateMalloc(PoolType::ATB, 0x6000, 0, 2, 2, 2000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(atbEvent), nullptr);
    auto halEvent = CreateMalloc(PoolType::HAL, 0x7000, 0, 1, 3, 3000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent), nullptr);

    // 全部池
    auto all = MemoryStateManager::GetInstance().QueryLiveBlocks(LiveBlockFilter{});
    EXPECT_EQ(all.size(), 3u);

    // 仅HAL池
    LiveBlockFilter halFilter;
    halFilter.poolTypes = {PoolType::HAL};
    auto halBlocks = MemoryStateManager::GetInstance().QueryLiveBlocks(halFilter);
    ASSERT_EQ(halBlocks.size(), 1u);
    EXPECT_EQ(halBlocks[0].addr, 0x7000);

    // 按device过滤
    LiveBlockFilter deviceFilter;
    deviceFilter.poolTypes = {PoolType::PTA_CACHING, PoolType::ATB, PoolType::HAL};
    deviceFilter.device = 0;
    EXPECT_EQ(MemoryStateManager::GetInstance().QueryLiveBlocks(deviceFilter).size(), 3u);

    // 按pid过滤
    LiveBlockFilter pidFilter;
    pidFilter.poolTypes = {PoolType::PTA_CACHING, PoolType::ATB, PoolType::HAL};
    pidFilter.pid = 2;
    auto pidBlocks = MemoryStateManager::GetInstance().QueryLiveBlocks(pidFilter);
    ASSERT_EQ(pidBlocks.size(), 1u);
    EXPECT_EQ(pidBlocks[0].addr, 0x6000);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0x5000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::ATB, MemoryStateKey{2, 0, 0x6000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 0, 0x7000});
}

TEST_F(MemoryStateManagerTest, query_live_blocks_excludes_ghost_and_shadow)
{
    // 幽灵state（FREE无匹配）
    auto ghostFree = CreateFree(PoolType::PTA_CACHING, 0x8000, 0, 1, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(ghostFree), nullptr);

    // SHADOW_CREATED（影子期创建未转正）
    auto shadowEvent = CreateMalloc(PoolType::PTA_CACHING, 0x9000, 0, 1, 2, 2000);
    shadowEvent->isShadowEvent = true;
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(shadowEvent), nullptr);

    // SHADOW_FREED（正常申请+影子释放）
    auto promotedEvent = CreateMalloc(PoolType::PTA_CACHING, 0xA000, 0, 1, 3, 3000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(promotedEvent), nullptr);
    MemoryState* state = MemoryStateManager::GetInstance().GetState(promotedEvent);
    ASSERT_NE(state, nullptr);
    state->shadowState = ShadowState::SHADOW_FREED;

    // SHADOW_PROMOTED（影子期创建已转正）——参与存活判定
    auto shadowPromotedEvent = CreateMalloc(PoolType::PTA_CACHING, 0xB000, 0, 1, 4, 4000);
    shadowPromotedEvent->isShadowEvent = true;
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(shadowPromotedEvent), nullptr);
    MemoryState* promotedState = MemoryStateManager::GetInstance().GetState(shadowPromotedEvent);
    ASSERT_NE(promotedState, nullptr);
    promotedState->shadowState = ShadowState::SHADOW_PROMOTED;

    LiveBlockFilter filter;
    filter.poolTypes = {PoolType::PTA_CACHING};
    auto blocks = MemoryStateManager::GetInstance().QueryLiveBlocks(filter);
    // 仅SHADOW_PROMOTED块存活
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].addr, 0xB000);
    EXPECT_EQ(blocks[0].shadowState, ShadowState::SHADOW_PROMOTED);

    // excludeShadowCreated=false时SHADOW_CREATED与SHADOW_FREED也参与（同一过滤位）
    LiveBlockFilter includeFilter;
    includeFilter.poolTypes = {PoolType::PTA_CACHING};
    includeFilter.excludeShadowCreated = false;
    auto allBlocks = MemoryStateManager::GetInstance().QueryLiveBlocks(includeFilter);
    ASSERT_EQ(allBlocks.size(), 3u);  // SHADOW_CREATED + SHADOW_FREED + SHADOW_PROMOTED（幽灵state仍排除）

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0x8000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0x9000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0xA000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0xB000});
}

TEST_F(MemoryStateManagerTest, free_containment_match_respects_device)
{
    // 大块覆盖范围 [0xC000, 0xC000+4096)
    auto bigEvent = CreateMalloc(PoolType::MINDSPORE, 0xC000, 1, 1, 1, 1000, 4096);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(bigEvent), nullptr);

    // 相同pid+addr范围，但device不同 → containment不匹配（不同device块）
    auto freeOtherDevice = CreateFree(PoolType::MINDSPORE, 0xC100, 0, 1, 2, 2000);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(freeOtherDevice);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->events[0]->eventType, EventBaseType::FREE);  // 幽灵state

    // 同device containment匹配
    auto freeSameDevice = CreateFree(PoolType::MINDSPORE, 0xC100, 1, 1, 3, 3000);
    MemoryState* matched = MemoryStateManager::GetInstance().AddEvent(freeSameDevice);
    ASSERT_NE(matched, nullptr);
    EXPECT_EQ(matched->events[0]->eventType, EventBaseType::MALLOC);

    MemoryStateManager::GetInstance().DeteleState(PoolType::MINDSPORE, MemoryStateKey{1, 1, 0xC000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::MINDSPORE, MemoryStateKey{1, 0, 0xC100});
}
