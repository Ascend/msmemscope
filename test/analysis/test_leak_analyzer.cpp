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
#include <string>
#include <vector>

#include "bit_field.h"
#include "config_info.h"
#include "event.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#define private public
#include "leak_analyzer.h"
#undef private
#include "memory_state_manager.h"
#include "record_info.h"

using namespace MemScope;

namespace
{
// 默认开启泄漏分析所需的最小配置（ALLOC+FREE，非--steps，非DEFERRED）
void SetLeakConfig()
{
    Config config{};
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    config.analysisType = analysisBit.getValue();
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 0;
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    ConfigManager::Instance().SetConfig(config);
}

std::shared_ptr<MemoryEvent> CreateNpuMalloc(uint64_t addr, int32_t device, uint64_t pid, uint64_t id,
                                             uint64_t timestamp, uint64_t size = 1024)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::PTA_CACHING;
    event->poolType = PoolType::PTA_CACHING;
    event->addr = addr;
    event->device = device;
    event->pid = pid;
    event->id = id;
    event->timestamp = timestamp;
    event->size = static_cast<int64_t>(size);
    event->used = static_cast<int64_t>(size);
    event->kernelIndex = 7;
    return event;
}

std::shared_ptr<MemoryEvent> CreateNpuFree(uint64_t addr, int32_t device, uint64_t pid, uint64_t id,
                                           uint64_t timestamp)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = EventSubType::PTA_CACHING;
    event->poolType = PoolType::PTA_CACHING;
    event->addr = addr;
    event->device = device;
    event->pid = pid;
    event->id = id;
    event->timestamp = timestamp;
    event->size = 0;
    event->used = 0;
    return event;
}

std::shared_ptr<MemoryEvent> CreateHalMalloc(uint64_t addr, int32_t device, uint64_t pid, uint64_t id,
                                             uint64_t timestamp, uint64_t size = 1024)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HAL;
    event->poolType = PoolType::HAL;
    event->addr = addr;
    event->device = device;
    event->pid = pid;
    event->id = id;
    event->timestamp = timestamp;
    event->size = static_cast<int64_t>(size);
    return event;
}

std::shared_ptr<MemoryEvent> CreateHalFree(uint64_t addr, int32_t device, uint64_t pid, uint64_t id,
                                           uint64_t timestamp)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = EventSubType::HAL;
    event->poolType = PoolType::HAL;
    event->addr = addr;
    event->device = device;
    event->pid = pid;
    event->id = id;
    event->timestamp = timestamp;
    event->size = 0;
    return event;
}

// MSTX step start打点（带引号形态）
std::shared_ptr<MstxEvent> CreateMstxStart(uint64_t stepId, uint64_t rangeId, int32_t device,
                                           uint64_t timestamp, uint64_t eventId)
{
    auto event = std::make_shared<MstxEvent>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = EventSubType::MSTX_RANGE_START;
    event->device = device;
    event->stepId = stepId;
    event->rangeId = rangeId;
    event->streamId = 0;
    event->timestamp = timestamp;
    event->id = eventId;
    event->name = "\"step start\"";
    return event;
}

std::shared_ptr<MstxEvent> CreateMstxEnd(uint64_t stepId, uint64_t rangeId, int32_t device,
                                         uint64_t timestamp, uint64_t eventId)
{
    auto event = std::make_shared<MstxEvent>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = EventSubType::MSTX_RANGE_END;
    event->device = device;
    event->stepId = stepId;
    event->rangeId = rangeId;
    event->streamId = 0;
    event->timestamp = timestamp;
    event->id = eventId;
    return event;
}
}  // namespace

class LeakAnalyzerTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        SetLeakConfig();
        LeakAnalyzer::GetInstance();
        // 清理单例跨用例残留状态
        LeakAnalyzer::GetInstance().leakMemSums_.clear();
        LeakAnalyzer::GetInstance().halClients_.clear();
        LeakAnalyzer::GetInstance().stepTracker_.deviceStates_.clear();
        LeakAnalyzer::GetInstance().stepTracker_.stepSource_ = StepSource::NONE;
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
    }

    void TearDown() override
    {
        ConfigManager::Instance().SetConfig(Config{});
        LeakAnalyzer::GetInstance().leakMemSums_.clear();
        LeakAnalyzer::GetInstance().halClients_.clear();
        LeakAnalyzer::GetInstance().stepTracker_.deviceStates_.clear();
        LeakAnalyzer::GetInstance().stepTracker_.stepSource_ = StepSource::NONE;
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
    }
};

TEST_F(LeakAnalyzerTest, npu_malloc_then_free_no_state_residual)
{
    auto mallocEvent = CreateNpuMalloc(0x1000, 0, 1, 1, 1000);
    MemoryStateManager::GetInstance().AddEvent(mallocEvent);
    auto freeEvent = CreateNpuFree(0x1000, 0, 1, 2, 2000);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(freeEvent);
    ASSERT_NE(state, nullptr);

    // FREE被MSM匹配到已有块（非幽灵state）
    EXPECT_EQ(state->events[0]->eventType, EventBaseType::MALLOC);

    // 直接驱动分析器，不触发告警路径（free匹配成功无告警）
    std::shared_ptr<EventBase> baseEvent = freeEvent;
    LeakAnalyzer::GetInstance().EventHandle(baseEvent, state);

    // 清理
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x1000});
}

TEST_F(LeakAnalyzerTest, npu_free_without_malloc_is_ghost_state)
{
    auto freeEvent = CreateNpuFree(0x9999, 0, 1, 1, 1000);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(freeEvent);
    ASSERT_NE(state, nullptr);
    // 幽灵state：仅含当前FREE事件
    EXPECT_EQ(state->events.size(), 1u);
    EXPECT_EQ(state->events[0]->eventType, EventBaseType::FREE);

    std::shared_ptr<EventBase> baseEvent = freeEvent;
    LeakAnalyzer::GetInstance().EventHandle(baseEvent, state);

    // 清理幽灵state
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x9999});
}

TEST_F(LeakAnalyzerTest, npu_malloc_conflict_returns_nullptr_and_residual_cleanup)
{
    auto mallocEvent1 = CreateNpuMalloc(0x2000, 0, 1, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(mallocEvent1), nullptr);

    // 同key二次MALLOC：AddEvent返回nullptr（MALLOC冲突），由EventRouter生成RESIDUAL_BLOCK
    auto mallocEvent2 = CreateNpuMalloc(0x2000, 0, 1, 2, 2000);
    EXPECT_EQ(MemoryStateManager::GetInstance().AddEvent(mallocEvent2), nullptr);

    // RESIDUAL_BLOCK告警路径：device由冲突的memEvent回填
    auto cleanUpEvent = std::make_shared<CleanUpEvent>(EventSubType::RESIDUAL_BLOCK, PoolType::PTA_CACHING, 1, 0x2000);
    cleanUpEvent->device = mallocEvent2->device;
    MemoryState* state = MemoryStateManager::GetInstance().GetState(cleanUpEvent);
    std::shared_ptr<EventBase> baseEvent = cleanUpEvent;
    LeakAnalyzer::GetInstance().EventHandle(baseEvent, state);

    // 清理
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x2000});
}

TEST_F(LeakAnalyzerTest, hal_double_free_reports_warning_path)
{
    auto mallocEvent = CreateHalMalloc(0x3000, 0, 1, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(mallocEvent), nullptr);

    auto freeEvent = CreateHalFree(0x3000, 0, 1, 2, 2000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(freeEvent), nullptr);
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 0, 0x3000});

    // 第二次FREE：无匹配 → 幽灵state，触发free error告警路径
    auto freeEvent2 = CreateHalFree(0x3000, 0, 1, 3, 3000);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(freeEvent2);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> baseEvent = freeEvent2;
    LeakAnalyzer::GetInstance().EventHandle(baseEvent, state);

    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 0, 0x3000});
}

TEST_F(LeakAnalyzerTest, query_unfreed_records_merges_npu_and_hal)
{
    // NPU块（device=1）
    auto npuEvent = CreateNpuMalloc(0x4000, 1, 1, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent), nullptr);
    // HAL块（pid=1）
    auto halEvent = CreateHalMalloc(0x5000, 0, 1, 2, 2000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent), nullptr);
    // 其他device的NPU块不参与
    auto npuOtherDevice = CreateNpuMalloc(0x6000, 2, 1, 3, 3000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuOtherDevice), nullptr);

    auto records = LeakAnalyzer::GetInstance().QueryUnfreedRecords(1, 1);
    ASSERT_EQ(records.size(), 2u);
    // NPU device过滤 + HAL pid过滤
    bool hasNpu = false;
    bool hasHal = false;
    for (const auto& rec : records)
    {
        if (rec.ptr == 0x4000)
        {
            hasNpu = true;
        }
        if (rec.ptr == 0x5000)
        {
            hasHal = true;
        }
    }
    EXPECT_TRUE(hasNpu);
    EXPECT_TRUE(hasHal);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 1, 0x4000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 0, 0x5000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 2, 0x6000});
}

TEST_F(LeakAnalyzerTest, mstx_step_events_drive_leak_check)
{
    // step1 开始（boundary step1, ts=1000）
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);

    // step2 开始（boundary step2, ts=2000）
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 3);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);

    // step2内分配块（ts=2100），step3结束时仍未释放 → 泄漏候选
    // （step1内分配的块allocStep=1会被SkipCheck跳过，永不会告警）
    auto npuEvent = CreateNpuMalloc(0x7000, 0, 1, 4, 2100);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent), nullptr);

    // step2 结束 → CheckNpuLeak(device=0, stepId=2)：allocStep=2，duration=0，阈值1不告警
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 5);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);

    // step3 开始（boundary step3, ts=3000）
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3000, 6);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);

    // step3 结束 → CheckNpuLeak(device=0, stepId=3)：duration=1 ≥ 阈值1 → 产生泄漏候选
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 7);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);

    // 泄漏候选集合：addr=0x7000的块在device=0表中被记录
    const auto& leakTable = LeakAnalyzer::GetInstance().leakMemSums_[0];
    bool found = false;
    for (const auto& pair : leakTable)
    {
        if (pair.first.ptr == 0x7000)
        {
            found = true;
            EXPECT_EQ(pair.first.leakStepId, 2u);      // 分配step（step2内分配）
            EXPECT_EQ(pair.second.kernelIndex, 7u);
            EXPECT_EQ(pair.second.leakSize, static_cast<int64_t>(1024));
        }
    }
    EXPECT_TRUE(found);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0x7000});
}

TEST_F(LeakAnalyzerTest, step1_allocs_are_skipped_by_duration_check)
{
    // step1开始，step1内分配
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    auto npuEvent = CreateNpuMalloc(0x8000, 0, 1, 2, 1100);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent), nullptr);

    // step2开始、结束：CheckNpuLeak(stepId=2)，allocStep=1 ≤ skipSteps_=1 → 跳过
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 3);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 4);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);

    // step3开始、结束：allocStep=1仍被SkipCheck跳过
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3000, 5);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 6);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);

    // step1内分配不产生泄漏候选
    EXPECT_TRUE(LeakAnalyzer::GetInstance().leakMemSums_[0].empty());

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0x8000});
}
