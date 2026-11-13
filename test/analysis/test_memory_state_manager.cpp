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
        MemoryStateManager::GetInstance().hostTensorTotal_ = 0;
        MemoryStateManager::GetInstance().deviceUsedCache_.fill(-1);
        MemoryStateManager::GetInstance().processUsedCache_.fill(-1);
        // 清理display memory新增统计（峰值/池级占用,进程全程语义,跨用例必清）
        MemoryStateManager::GetInstance().halPeak_.clear();
        MemoryStateManager::GetInstance().hostPeak_ = 0;
        MemoryStateManager::GetInstance().hostTensorPeak_ = 0;
        MemoryStateManager::GetInstance().poolUsage_.clear();
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
        MemoryStateManager::GetInstance().hostTensorTotal_ = 0;
        MemoryStateManager::GetInstance().deviceUsedCache_.fill(-1);
        MemoryStateManager::GetInstance().processUsedCache_.fill(-1);
        MemoryStateManager::GetInstance().halPeak_.clear();
        MemoryStateManager::GetInstance().hostPeak_ = 0;
        MemoryStateManager::GetInstance().hostTensorPeak_ = 0;
        MemoryStateManager::GetInstance().poolUsage_.clear();
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

// HOST 内存事件模型：poolType=HOST、device=DEVICE_ID_CPU、eventSubType=HOST。
// 锁页内存（offload pinned）isPinned=true；CPU tensor 数据内存 isPinned=false，二者仅以 isPinned 区分。
std::shared_ptr<MemoryEvent> CreateHostMalloc(uint64_t addr, int64_t size, bool isPinned = true)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HOST;
    event->poolType = PoolType::HOST;
    event->addr = addr;
    event->device = DEVICE_ID_CPU;
    event->pid = 1;
    event->size = size;
    event->isPinned = isPinned;
    return event;
}

std::shared_ptr<MemoryEvent> CreateHostFree(uint64_t addr, int64_t size, bool isPinned = true)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = EventSubType::HOST;
    event->poolType = PoolType::HOST;
    event->addr = addr;
    event->device = DEVICE_ID_CPU;
    event->pid = 1;
    event->size = size;
    event->isPinned = isPinned;
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

// AddEvent 内完成统计累计并回填事件字段（生产路径：事件处理阶段一 UpdateMemoryEventState）。
// 按值传参:左值/右值(CreateXxx临时对象)均接受——T&与T重载对左值实参重载决议歧义
// (C++规则:subsequence比较排除lvalue transformation,identity与copy不可区分);
// shared_ptr拷贝与原对象共享所有权,AddEvent回填字段原对象可见,与原引用版本等价
void Handle(std::shared_ptr<MemoryEvent> event)
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
    pool->processUsed = 300;  // 报告层预置值（dcmi_get_npu_proc_mem_info 查询缓存），统计累计不得改写
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

// HOST 锁页内存（poolType=HOST、isPinned=true）：used=HOST累计、processUsed=进程VmRSS
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

    // HOST 累计与 DEVICE 维度隔离（poolType=HOST 不进入 halUsed_）
    auto dev = CreateHalMalloc(0x3000, 0, 100);
    Handle(dev);
    EXPECT_EQ(MemoryStateManager::GetInstance().hostUsed_, 50);
    EXPECT_EQ(MemoryStateManager::GetInstance().halUsed_[0], 100);
}

// CPU tensor 数据内存（poolType=HOST、isPinned=false）：used=活跃CPU tensor累计、processUsed=进程VmRSS，与锁页 used 隔离
TEST_F(MemoryStateManagerTest, cpu_tensor_events_update_used_and_process_used)
{
    auto t1 = CreateHostMalloc(0x1000, 100, false);
    Handle(t1);
    EXPECT_EQ(t1->used, 100);
    EXPECT_GT(t1->processUsed, 0);  // 进程 VmRSS 必然大于 0
    EXPECT_EQ(MemoryStateManager::GetInstance().hostTensorTotal_, 100);
    EXPECT_EQ(MemoryStateManager::GetInstance().hostUsed_, 0);  // 与锁页内存隔离

    auto t2 = CreateHostMalloc(0x2000, 50, false);
    Handle(t2);
    EXPECT_EQ(t2->used, 150);

    auto f1 = CreateHostFree(0x1000, 100, false);
    Handle(f1);
    EXPECT_EQ(f1->used, 50);
    EXPECT_GT(f1->processUsed, 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().hostTensorTotal_, 50);
    EXPECT_EQ(MemoryStateManager::GetInstance().hostUsed_, 0);
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

// ---------- display memory 数据源: 峰值进程全程 / 池级占用 / 卡号集合 ----------

// 峰值语义=进程全程: TRACE_START(ResetUsageBaseline)只重建current,峰值保留历史最大
TEST_F(MemoryStateManagerTest, hal_peak_is_process_wide_across_baseline_reset)
{
    Handle(CreateHalMalloc(0x1000, 0, 100));
    Handle(CreateHalMalloc(0x2000, 0, 200));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHalUsed(0), 300);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHalPeak(0), 300);

    // free到200: current下降,peak保留
    Handle(CreateHalFree(0x1000, 0, 100));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHalUsed(0), 200);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHalPeak(0), 300);

    // TRACE_START基线重建: current按存量块(0x2000)重算仍200,峰值不动
    MemoryStateManager::GetInstance().ResetUsageBaseline();
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHalUsed(0), 200);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHalPeak(0), 300);

    // 未记录设备: 0
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHalUsed(7), 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHalPeak(7), 0);
}

// 锁页/CPU tensor 分流与峰值(进程全程;free只降current)
TEST_F(MemoryStateManagerTest, host_pinned_and_tensor_peaks)
{
    Handle(CreateHostMalloc(0x1000, 2000, true));   // 锁页内存
    Handle(CreateHostMalloc(0x2000, 3000, false));  // CPU tensor数据内存
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHostPinnedUsed(), 2000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHostPinnedPeak(), 2000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHostTensorUsed(), 3000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHostTensorPeak(), 3000);

    // free锁页块: used归零,peak保留;tensor不受影响
    Handle(CreateHostFree(0x1000, 2000, true));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHostPinnedUsed(), 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHostPinnedPeak(), 2000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHostTensorUsed(), 3000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetHostTensorPeak(), 3000);
}

// 其余池占用: 逐池逐设备 current/peak;FREE(自带size)扣减;peak进程全程
TEST_F(MemoryStateManagerTest, generic_pool_usage_tracked_per_pool_per_device)
{
    // pta dev0: +1000 → current/peak 1000
    Handle(CreatePoolEvent(EventBaseType::MALLOC, 0x1000, 0, 1000, 1, 1, PoolType::PTA_CACHING));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::PTA_CACHING, 0), 1000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolPeak(PoolType::PTA_CACHING, 0), 1000);

    // 同池不同卡互不影响;不同池同卡互不影响
    Handle(CreatePoolEvent(EventBaseType::MALLOC, 0x2000, 1, 500, 1, 1, PoolType::PTA_CACHING));
    Handle(CreatePoolEvent(EventBaseType::MALLOC, 0x3000, 0, 700, 1, 1, PoolType::MINDSPORE));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::PTA_CACHING, 1), 500);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::PTA_CACHING, 0), 1000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::MINDSPORE, 0), 700);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolPeak(PoolType::MINDSPORE, 0), 700);

    // 未记录的池/设备返回0(含INVALID)
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::ATB, 0), 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolPeak(PoolType::ATB, 0), 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::INVALID, 0), 0);

    // FREE(自带size)扣减: current归零,peak保留
    Handle(CreatePoolEvent(EventBaseType::FREE, 0x1000, 0, 1000, 1, 1, PoolType::PTA_CACHING));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::PTA_CACHING, 0), 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolPeak(PoolType::PTA_CACHING, 0), 1000);

    // TRACE_START基线重建: current按存量块重算,peak保留
    MemoryStateManager::GetInstance().ResetUsageBaseline();
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::PTA_CACHING, 1), 500);  // 0x2000存量块
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::PTA_CACHING, 0), 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolPeak(PoolType::PTA_CACHING, 0), 1000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::MINDSPORE, 0), 700);
}

// 池级负数截断: 释放量超过current → 截断0,峰值不受影响
TEST_F(MemoryStateManagerTest, generic_pool_usage_negative_truncated)
{
    Handle(CreatePoolEvent(EventBaseType::MALLOC, 0x1000, 0, 1000, 1, 1, PoolType::PTA_WORKSPACE));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::PTA_WORKSPACE, 0), 1000);

    // FREE size 2000 > current 1000 → 截断0
    Handle(CreatePoolEvent(EventBaseType::FREE, 0x1000, 0, 2000, 1, 1, PoolType::PTA_WORKSPACE));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::PTA_WORKSPACE, 0), 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolPeak(PoolType::PTA_WORKSPACE, 0), 1000);

    // 截断后再次分配: current从0起,peak仍为历史最大
    Handle(CreatePoolEvent(EventBaseType::MALLOC, 0x2000, 0, 300, 1, 1, PoolType::PTA_WORKSPACE));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolCurrent(PoolType::PTA_WORKSPACE, 0), 300);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetPoolPeak(PoolType::PTA_WORKSPACE, 0), 1000);
}

// GetUsedDeviceList: hal有数据(used/peak任一) ∪ device/process缓存非-1,升序去重;
// 池级占用设备不入集合
TEST_F(MemoryStateManagerTest, get_used_device_list_union_and_dedup)
{
    EXPECT_TRUE(MemoryStateManager::GetInstance().GetUsedDeviceList().empty());

    // hal dev0有数据 → [0];缓存: dev2=deviceUsed, dev5=processUsed(同设备双缓存只出现一次)
    Handle(CreateHalMalloc(0x1000, 0, 100));
    MemoryStateManager::GetInstance().UpdateDeviceUsedCache(2, 100);
    MemoryStateManager::GetInstance().UpdateDeviceUsedCache(5, 200);
    MemoryStateManager::GetInstance().UpdateProcessUsedCache(5, 300);
    // 池级占用设备(dev9)不进入卡号集合
    Handle(CreatePoolEvent(EventBaseType::MALLOC, 0x2000, 9, 100, 1, 1, PoolType::ATB));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetUsedDeviceList(), (std::vector<int32_t>{0, 2, 5}));

    // hal free到0: used清零但peak仍在 → dev0仍列出(hal有数据=used/peak任一)
    Handle(CreateHalFree(0x1000, 0, 100));
    EXPECT_EQ(MemoryStateManager::GetInstance().GetUsedDeviceList(), (std::vector<int32_t>{0, 2, 5}));

    // 缓存清为-1 → 从列表移除
    MemoryStateManager::GetInstance().UpdateDeviceUsedCache(2, -1);
    MemoryStateManager::GetInstance().UpdateProcessUsedCache(5, -1);
    MemoryStateManager::GetInstance().UpdateDeviceUsedCache(5, -1);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetUsedDeviceList(), (std::vector<int32_t>{0}));
}

// ==================== 标签边界 / 负数截断 / 缓存非法设备 / 查找兜底分支补充 ====================

// OwnerLabelManager：越界 level 的读写均被安全忽略，正常 level 按顺序用 '@' 拼接
TEST_F(MemoryStateManagerTest, owner_label_manager_boundary)
{
    OwnerLabelManager labelMgr;
    labelMgr.AddLabel(OwnerLevel::FRAMEWORK, "pta");
    labelMgr.AddLabel(OwnerLevel::DETAIL_1, "weight");
    EXPECT_EQ(labelMgr.GetLabel(OwnerLevel::FRAMEWORK), "pta");
    EXPECT_EQ(labelMgr.GetLabel(OwnerLevel::DETAIL_1), "weight");
    EXPECT_EQ(labelMgr.GetOwnerStr(), "pta@weight");

    // 越界 level：GetLabel 返回空串，AddLabel 不写入任何位置
    EXPECT_EQ(labelMgr.GetLabel(OwnerLevel::OWNER_LEVEL_NUM), "");
    labelMgr.AddLabel(OwnerLevel::OWNER_LEVEL_NUM, "ignored");
    EXPECT_EQ(labelMgr.GetLabel(OwnerLevel::OWNER_LEVEL_NUM), "");
    EXPECT_EQ(labelMgr.GetOwnerStr(), "pta@weight");
}

// HOST 锁页/CPU tensor 无匹配 MALLOC 的 FREE 导致累计为负：截断为 0 且事件 used 回填 0
TEST_F(MemoryStateManagerTest, host_negative_accumulation_truncated_to_zero)
{
    auto pinnedFree = CreateHostFree(0x1000, 100, true);
    Handle(pinnedFree);
    EXPECT_EQ(pinnedFree->used, 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().hostUsed_, 0);

    auto tensorFree = CreateHostFree(0x2000, 200, false);
    Handle(tensorFree);
    EXPECT_EQ(tensorFree->used, 0);
    EXPECT_EQ(MemoryStateManager::GetInstance().hostTensorTotal_, 0);
}

// GetState(MemoryEvent)：key 未命中后按 pid + 地址区间扫描，覆盖命中/pid 不同/device 不同/未命中
TEST_F(MemoryStateManagerTest, get_state_containment_scan_paths)
{
    auto bigEvent = CreateMalloc(PoolType::PTA_CACHING, 0xC000, 0, 1, 1, 1000, 4096);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(bigEvent), nullptr);

    // 地址落在已分配区间内（device 缺失，不参与 device 过滤）
    auto innerFree = CreateFree(PoolType::PTA_CACHING, 0xC100, GD_INVALID_NUM, 1, 2, 2000);
    MemoryState* matched = MemoryStateManager::GetInstance().GetState(innerFree);
    ASSERT_NE(matched, nullptr);
    EXPECT_EQ(matched->events[0]->eventType, EventBaseType::MALLOC);

    // pid 不同：跳过
    auto otherPid = CreateFree(PoolType::PTA_CACHING, 0xC100, GD_INVALID_NUM, 2, 3, 3000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetState(otherPid), nullptr);

    // device 有效且与块不同：跳过
    auto otherDevice = CreateFree(PoolType::PTA_CACHING, 0xC100, 1, 1, 4, 4000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetState(otherDevice), nullptr);

    // 地址不在任何区间：遍历完返回 nullptr（0xE000 已越过 [0xC000, 0xD000] 覆盖范围）
    auto outside = CreateFree(PoolType::PTA_CACHING, 0xE000, GD_INVALID_NUM, 1, 5, 5000);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetState(outside), nullptr);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0xC000});
}

// PromoteShadowStates：待处理 key 在回调中被删除后，后续处理跳过该 key
TEST_F(MemoryStateManagerTest, promote_shadow_states_vanished_key_skipped)
{
    auto stateA = CreateMalloc(PoolType::PTA_CACHING, 0xA000, 0, 1, 1, 1000);
    auto stateB = CreateMalloc(PoolType::PTA_CACHING, 0xB000, 0, 1, 2, 2000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(stateA), nullptr);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(stateB), nullptr);
    MemoryStateManager::GetInstance().GetState(stateA)->shadowState = ShadowState::SHADOW_FREED;
    MemoryStateManager::GetInstance().GetState(stateB)->shadowState = ShadowState::SHADOW_FREED;

    int dumpCount = 0;
    auto& statesMap = MemoryStateManager::GetInstance().poolsMap_[PoolType::PTA_CACHING].statesMap;
    auto eraseOther = [&statesMap](const MemoryStateKey& key, MemoryState* current)
    {
        auto it = statesMap.find(key);
        if (it != statesMap.end() && &it->second != current)
        {
            statesMap.erase(it);
        }
    };
    auto dumpFunc = [&](MemoryState* state)
    {
        ++dumpCount;
        // 模拟回调中删除了另一个待处理 key（不能删除 current，否则迭代器失效）
        eraseOther(MemoryStateKey{1, 0, 0xA000}, state);
        eraseOther(MemoryStateKey{1, 0, 0xB000}, state);
    };
    MemoryStateManager::GetInstance().PromoteShadowStates(dumpFunc);

    // 第一个 SHADOW_FREED 落盘时删除另一个 key，第二个 key 查找失败被跳过
    EXPECT_EQ(dumpCount, 1);
    EXPECT_TRUE(statesMap.empty());
}
