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
#define private public
#include "memory_state_manager.h"
#undef private
#include "process.h"
#include "record_info.h"

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
        // 清理统计累计残留（HAL/HOST 累计 + 整卡/本进程用量缓存）
        MemoryStateManager::GetInstance().halUsed_.clear();
        MemoryStateManager::GetInstance().hostUsed_ = 0;
        MemoryStateManager::GetInstance().deviceUsedCache_.fill(-1);
        MemoryStateManager::GetInstance().processUsedCache_.fill(-1);
    }

    void TearDown() override
    {
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
        MemoryStateManager::GetInstance().halUsed_.clear();
        MemoryStateManager::GetInstance().hostUsed_ = 0;
        MemoryStateManager::GetInstance().deviceUsedCache_.fill(-1);
        MemoryStateManager::GetInstance().processUsedCache_.fill(-1);
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

// ---------- 统计累计（used/processUsed 回填）与 TRACE_START 基线测试 ----------
// 统计回填在 AddEvent 内完成（事件处理阶段一，先于所有分析器）；TRACE_START 基线经
// ResetUsageBaseline 从存量块重建（EventHandler::HandleTraceEvent 调用，事件处理单线程）

std::shared_ptr<MemoryEvent> CreateHalMalloc(uint64_t addr, int32_t device, int64_t size)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HAL;
    event->poolType = PoolType::HAL;
    event->addr = addr;
    event->device = device;
    event->pid = 1;
    event->size = size;
    return event;
}

std::shared_ptr<MemoryEvent> CreateHalFree(uint64_t addr, int32_t device, int64_t size)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = EventSubType::HAL;
    event->poolType = PoolType::HAL;
    event->addr = addr;
    event->device = device;
    event->pid = 1;
    event->size = size;  // AddEvent 匹配到 MALLOC 后回填的值，此处为回填后大小
    return event;
}

// HOST 内存事件模型：poolType=HAL、device=DEVICE_ID_CPU、eventSubType=HOST_PINNED
std::shared_ptr<MemoryEvent> CreateHostMalloc(uint64_t addr, int64_t size)
{
    auto event = CreateHalMalloc(addr, DEVICE_ID_CPU, size);
    event->eventSubType = EventSubType::HOST_PINNED;
    return event;
}

std::shared_ptr<MemoryEvent> CreateHostFree(uint64_t addr, int64_t size)
{
    auto event = CreateHalFree(addr, DEVICE_ID_CPU, size);
    event->eventSubType = EventSubType::HOST_PINNED;
    return event;
}

std::shared_ptr<MemoryEvent> CreatePoolEvent(EventBaseType type, uint64_t addr, int32_t device, int64_t size,
                                             int64_t used, int64_t total, PoolType poolType)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = type;
    event->eventSubType = poolType == PoolType::PTA_CACHING      ? EventSubType::PTA_CACHING
                          : poolType == PoolType::PTA_WORKSPACE  ? EventSubType::PTA_WORKSPACE
                          : poolType == PoolType::ATB            ? EventSubType::ATB
                                                                 : EventSubType::MINDSPORE;
    event->poolType = poolType;
    event->addr = addr;
    event->device = device;
    event->pid = 1;
    event->size = size;
    event->used = used;   // 报告时已填（totalAllocated），统计累计不得改写
    event->total = total; // 报告时已填（totalReserved），统计累计不得改写
    return event;
}

// AddEvent 内完成统计累计并回填事件字段（生产路径：事件处理阶段一 UpdateMemoryEventState）
void Handle(std::shared_ptr<MemoryEvent>& event)
{
    MemoryStateManager::GetInstance().AddEvent(event);
}

// HAL MALLOC 序列按设备累计：used = 本进程 HAL 维度活跃累计
TEST_F(MemoryStateManagerTest, hal_malloc_accumulates_per_device)
{
    auto e1 = CreateHalMalloc(0x1000, 0, 100);
    auto e2 = CreateHalMalloc(0x2000, 0, 200);
    auto e3 = CreateHalMalloc(0x3000, 0, 300);
    Handle(e1);
    EXPECT_EQ(e1->used, 100);
    Handle(e2);
    EXPECT_EQ(e2->used, 300);
    Handle(e3);
    EXPECT_EQ(e3->used, 600);

    // 按设备隔离：卡1 的累计不影响卡0
    auto e4 = CreateHalMalloc(0x4000, 1, 50);
    Handle(e4);
    EXPECT_EQ(e4->used, 50);
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 600);
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[1], 50);
}

// HAL MALLOC+FREE 累计（100/300/200）
TEST_F(MemoryStateManagerTest, hal_malloc_free_accumulation)
{
    auto e1 = CreateHalMalloc(0x1000, 0, 100);
    auto e2 = CreateHalMalloc(0x2000, 0, 200);
    auto f1 = CreateHalFree(0x1000, 0, 100);
    Handle(e1);
    Handle(e2);
    EXPECT_EQ(e2->used, 300);
    Handle(f1);
    EXPECT_EQ(f1->used, 200);
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 200);
}

// 重复 FREE 累计为负 → 截断为 0（曲线不出现负数毛刺）
TEST_F(MemoryStateManagerTest, negative_accumulation_truncated_to_zero)
{
    auto f1 = CreateHalFree(0x1000, 0, 100);
    Handle(f1);
    EXPECT_EQ(f1->used, 0);  // 截断而非 -100
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 0);

    // 截断后继续累计正常
    auto e1 = CreateHalMalloc(0x2000, 0, 100);
    Handle(e1);
    EXPECT_EQ(e1->used, 100);
}

// 影子事件不累计、不回填
TEST_F(MemoryStateManagerTest, shadow_events_are_skipped)
{
    auto e1 = CreateHalMalloc(0x1000, 0, 100);
    e1->isShadowEvent = true;
    Handle(e1);
    EXPECT_EQ(e1->used, 0);  // 未回填
    EXPECT_TRUE(MemoryStateManager::GetInstance().halUsed_.empty());

    auto f1 = CreateHalFree(0x1000, 0, 100);
    f1->isShadowEvent = true;
    Handle(f1);
    EXPECT_TRUE(MemoryStateManager::GetInstance().halUsed_.empty());
    EXPECT_EQ(MemoryStateManager::GetInstance().hostUsed_, 0);
}

// 池事件 used/total/processUsed/deviceUsed 均不被改写（used/total 报告时已填，HealthAnalyzer 依赖；
// processUsed/deviceUsed 由报告层按设备读查询缓存填值）
TEST_F(MemoryStateManagerTest, pool_events_preserve_used_total_and_fill_process_used)
{
    auto pool = CreatePoolEvent(EventBaseType::MALLOC, 0x2000, 0, 1024, 512, 2048, PoolType::PTA_CACHING);
    pool->deviceUsed = 42;    // 报告层预置值，统计累计不得改写
    pool->processUsed = 300;  // 报告层预置值（aclrtGetMemInfo 查询缓存），统计累计不得改写
    Handle(pool);
    EXPECT_EQ(pool->used, 512);  // 未动
    EXPECT_EQ(pool->total, 2048);  // 未动
    EXPECT_EQ(pool->processUsed, 300);  // 保持报告层预置值
    EXPECT_EQ(pool->deviceUsed, 42);  // 报告层字段保持

    auto free = CreatePoolEvent(EventBaseType::FREE, 0x2000, 0, 1024, 0, 2048, PoolType::PTA_CACHING);
    free->processUsed = 300;
    Handle(free);
    EXPECT_EQ(free->used, 0);
    EXPECT_EQ(free->total, 2048);
    EXPECT_EQ(free->processUsed, 300);
}

// HOST（poolType=HAL、device=DEVICE_ID_CPU）：used=HOST累计、processUsed=进程VmRSS
TEST_F(MemoryStateManagerTest, host_events_update_used_and_process_used)
{
    auto h1 = CreateHostMalloc(0x1000, 100);
    Handle(h1);
    EXPECT_EQ(h1->used, 100);
    EXPECT_GT(h1->processUsed, 0);  // 进程 VmRSS 必然大于 0

    auto h2 = CreateHostMalloc(0x2000, 50);
    Handle(h2);
    EXPECT_EQ(h2->used, 150);

    auto f1 = CreateHostFree(0x1000, 100);
    Handle(f1);
    EXPECT_EQ(f1->used, 50);
    EXPECT_GT(f1->processUsed, 0);

    // HOST 累计与 DEVICE 维度隔离（DEVICE_ID_CPU 不进入 halUsed_）
    auto dev = CreateHalMalloc(0x3000, 0, 100);
    Handle(dev);
    EXPECT_EQ(MemoryStateManager::GetInstance().hostUsed_, 50);
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 100);
}

// TRACE_START 从存量块初始化基线（卡0 100B、卡1 50B、HOST 30B）
TEST_F(MemoryStateManagerTest, trace_start_resets_baseline_from_live_blocks)
{
    auto b0 = CreateHalMalloc(0x1000, 0, 100);
    auto b1 = CreateHalMalloc(0x2000, 1, 50);
    auto h0 = CreateHostMalloc(0x3000, 30);
    MemoryStateManager::GetInstance().AddEvent(b0);
    MemoryStateManager::GetInstance().AddEvent(b1);
    MemoryStateManager::GetInstance().AddEvent(h0);

    MemoryStateManager::GetInstance().ResetUsageBaseline();
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 100);
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[1], 50);
    EXPECT_EQ(MemoryStateManager::GetInstance().hostUsed_, 30);

    // 基线后增量：事件 used = 基线和 + 新分配量
    auto e1 = CreateHalMalloc(0x4000, 0, 20);
    Handle(e1);
    EXPECT_EQ(e1->used, 120);
}

// 多次 TRACE_START 重置而非叠加（无漂移）
TEST_F(MemoryStateManagerTest, trace_start_twice_resets_not_accumulates)
{
    auto b0 = CreateHalMalloc(0x1000, 0, 100);
    MemoryStateManager::GetInstance().AddEvent(b0);

    MemoryStateManager::GetInstance().ResetUsageBaseline();
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 100);

    // 存量块被释放后再次 start：基线应重置为当前存量（0）
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 0, 0x1000});
    MemoryStateManager::GetInstance().ResetUsageBaseline();
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 0);  // 无叠加漂移
}

// SHADOW_PROMOTED 块计入基线、SHADOW_CREATED 排除（基线防影子期累计失真）
TEST_F(MemoryStateManagerTest, trace_start_baseline_includes_promoted_shadow_blocks)
{
    // 影子期申请（isShadowEvent=true → SHADOW_CREATED）
    auto shadow = CreateHalMalloc(0x1000, 0, 100);
    shadow->isShadowEvent = true;
    MemoryStateManager::GetInstance().AddEvent(shadow);
    // 转正：SHADOW_CREATED → SHADOW_PROMOTED（start() 流程在 TRACE_START 前执行）
    MemoryStateManager::GetInstance().PromoteShadowStates([](MemoryState*) {});
    // 未转正的影子块（保持 SHADOW_CREATED）
    auto pending = CreateHalMalloc(0x2000, 1, 50);
    pending->isShadowEvent = true;
    MemoryStateManager::GetInstance().AddEvent(pending);

    MemoryStateManager::GetInstance().ResetUsageBaseline();
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 100);  // 转正块计入（防失真）
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_.count(1), 0);  // 未转正影子块排除
}

// SHADOW_FREED 块不计入基线
TEST_F(MemoryStateManagerTest, trace_start_baseline_excludes_shadow_freed_blocks)
{
    // 正常块（NORMAL），影子期释放 → SHADOW_FREED（走事件管线标记，影子事件不参与分析分发）
    auto alloc = CreateHalMalloc(0x1000, 0, 100);
    MemoryStateManager::GetInstance().AddEvent(alloc);
    auto shadowFree = CreateHalFree(0x1000, 0, 100);
    shadowFree->isShadowEvent = true;
    EventHandler(shadowFree);  // HandleShadowEvent 标记 SHADOW_FREED

    MemoryStateManager::GetInstance().ResetUsageBaseline();
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 0);  // SHADOW_FREED 不计入
}

// 统计累计不改写报告层设置的 deviceUsed/processUsed（由采集层查询后写入缓存，池事件读取）
TEST_F(MemoryStateManagerTest, device_used_not_modified_by_usage_update)
{
    auto hal = CreateHalMalloc(0x1000, 0, 100);
    hal->deviceUsed = 1234;
    hal->processUsed = 1235;
    Handle(hal);
    EXPECT_EQ(hal->deviceUsed, 1234);
    EXPECT_EQ(hal->processUsed, 1235);
    EXPECT_EQ(hal->used, 100);

    auto pool = CreatePoolEvent(EventBaseType::MALLOC, 0x2000, 0, 100, 50, 200, PoolType::PTA_CACHING);
    pool->deviceUsed = 5678;
    pool->processUsed = 5679;
    Handle(pool);
    EXPECT_EQ(pool->deviceUsed, 5678);
    EXPECT_EQ(pool->processUsed, 5679);

    auto host = CreateHostMalloc(0x3000, 10);
    host->deviceUsed = 999;
    Handle(host);
    EXPECT_EQ(host->deviceUsed, 999);
    EXPECT_EQ(host->processUsed, static_cast<int64_t>(Utility::GetProcessVmRss()));  // HOST 事件仍回填 VmRSS
}
