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

#include <dirent.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "bit_field.h"
#include "config_info.h"
#include "data_handler.h"
#include "event.h"
#include "securec.h"
// ConfigManager::config_为私有成员：测试需绕过SetConfig的默认化补齐（ALLOC会自动补FREE）
// 才能构造"仅采集ALLOC"的开关门控状态，故与event_trace_manager.h一并白盒引入；
// dump.h同理由（端到端落盘用例需清理handlerMap_）
#define private public
#include "dump.h"
#include "event_trace/trace_manager/event_trace_manager.h"
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

// 生成指定stepCount/采集模式/事件位的泄漏分析配置
Config MakeLeakConfig(uint8_t stepCount, CollectMode collectMode, bool allocOnly)
{
    Config config{};
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    config.analysisType = analysisBit.getValue();
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    if (!allocOnly)
    {
        eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    }
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = stepCount;
    config.collectMode = static_cast<uint8_t>(collectMode);
    return config;
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

// health_analyzer.cpp 的 ReportGap 打屏时会把 std::cout 置为 fixed/left，且只还原精度、
// 不还原浮点格式标志；同一进程内后续用例的 std::cout 会停留在 fixed 状态，使 ReportLeak
// 的 SUMMARY（long double 直接流输出）渲染成 "1.000000" 而非 "1"。本守卫在用例执行期间
// 复位浮点格式、结束后完整还原，避免断言结果取决于其他用例是否已经打过 Gap 表。
class ScopedDefaultFloatFormat
{
   public:
    ScopedDefaultFloatFormat() : flags_(std::cout.flags()), precision_(std::cout.precision())
    {
        std::cout.setf(std::ios::fmtflags(0), std::ios::floatfield);
        std::cout.precision(6);
    }
    ~ScopedDefaultFloatFormat()
    {
        std::cout.flags(flags_);
        std::cout.precision(precision_);
    }
    ScopedDefaultFloatFormat(const ScopedDefaultFloatFormat &) = delete;
    ScopedDefaultFloatFormat &operator=(const ScopedDefaultFloatFormat &) = delete;

   private:
    std::ios::fmtflags flags_;
    std::streamsize precision_;
};

class LeakAnalyzerTest : public ::testing::Test
{
   protected:
    // 隔离其他用例遗留在 std::cout 上的浮点格式（见 ScopedDefaultFloatFormat 注释）
    ScopedDefaultFloatFormat coutFormat_;
    void SetUp() override
    {
        SetLeakConfig();
        LeakAnalyzer::GetInstance();
        // 清理单例跨用例残留状态
        LeakAnalyzer::GetInstance().leakRecords_.clear();
        LeakAnalyzer::GetInstance().stepEndSnapshots_.clear();
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
        LeakAnalyzer::GetInstance().leakRecords_.clear();
        LeakAnalyzer::GetInstance().stepEndSnapshots_.clear();
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
// 判定①：连续两个非首个step结束时都存活的块被打标并记录
TEST_F(LeakAnalyzerTest, mstx_step_events_drive_leak_check)
{
    // step1 开始（boundary step1, ts=1000）
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);

    // step2 开始（boundary step2, ts=2000）
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 3);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);

    // step2内分配块（ts=2100），step2/step3结束时都存活 → 判定①命中
    auto npuEvent = CreateNpuMalloc(0x7000, 0, 1, 4, 2100);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);

    // step2 结束（首个快照，无配对不判定）
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 5);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);
    EXPECT_TRUE(LeakAnalyzer::GetInstance().leakRecords_.empty());

    // step3 开始（boundary step3, ts=3000）
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3000, 6);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);

    // step3 结束：配对(E2,E3)且E2为非首个step结束 → 与E2快照求交集命中
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 7);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);

    // MemoryState打标（粘性，落盘attr输出leak:true）
    EXPECT_TRUE(state->isLeak);

    // 累计表按allocationId记录：来源①、命中step3、分配step2
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 1u);
    auto it = records.find(state->allocationId);
    ASSERT_NE(it, records.end());
    EXPECT_EQ(it->second.ptr, 0x7000u);
    EXPECT_EQ(it->second.source, LeakSource::STEP_SUSPECTED);
    EXPECT_EQ(it->second.detectedStep, 3u);
    EXPECT_EQ(it->second.allocStep, 2u);
    EXPECT_EQ(it->second.kernelIndex, 7u);
    EXPECT_EQ(it->second.leakSize, 1024u);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                   MemoryStateKey{1, 0, 0x7000});
}

// 判定①排除首个step结束：配对(E1,E2)不判定，step1内存活的块不产生候选
TEST_F(LeakAnalyzerTest, first_step_end_pair_is_excluded_from_leak_check)
{
    // step1开始，step1内分配
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    auto npuEvent = CreateNpuMalloc(0x8000, 0, 1, 2, 1100);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);

    // step1结束（E1，首个快照）
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 3);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);

    // step2开始、结束：配对(E1,E2)为首个step结束配对 → 不判定
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 4);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 5);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);

    // 块在E1、E2结束时都存活，但配对起点为首个step结束 → 不产生候选
    EXPECT_TRUE(LeakAnalyzer::GetInstance().leakRecords_.empty());
    EXPECT_FALSE(state->isLeak);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x8000});
}

// 判定①只认快照事实：两个step结束之间新分配的块（未在上次快照中）不判定
TEST_F(LeakAnalyzerTest, block_allocated_between_step_ends_not_flagged)
{
    // E1、E2正常结束（本用例块尚未分配）
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 2);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 3);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 4);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);

    // E2之后、E3之前分配：E2快照中不存在该块
    auto npuEvent = CreateNpuMalloc(0x8800, 0, 1, 5, 3100);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);

    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3200, 6);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 7);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);

    // 配对(E2,E3)虽合法，但块不在E2快照中 → 不判定（修复旧逻辑按step差近似的误判）
    EXPECT_TRUE(LeakAnalyzer::GetInstance().leakRecords_.empty());
    EXPECT_FALSE(state->isLeak);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                   MemoryStateKey{1, 0, 0x8800});
}

// 判定①按分配时刻过滤初始化常驻块：初始化期（allocStep=0）与step1内（allocStep=1）分配且
// 一直存活的块（模型权重/框架常驻资源形态）不因后续配对(E2,E3)命中而误报，与判定②及基线
// skipSteps口径对齐；step2内分配的同形态块（allocStep=2）仍正常判定（正向对照）
TEST_F(LeakAnalyzerTest, init_resident_blocks_not_flagged_by_step_pair_check)
{
    // 初始化期分配（ts早于step1边界）与step1内分配的常驻块，之后一直不释放
    auto initBlock = CreateNpuMalloc(0x9000, 0, 1, 1, 500);
    MemoryState* initState = MemoryStateManager::GetInstance().AddEvent(initBlock);
    ASSERT_NE(initState, nullptr);
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 2);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    auto step1Block = CreateNpuMalloc(0x9100, 0, 1, 3, 1100);
    MemoryState* step1State = MemoryStateManager::GetInstance().AddEvent(step1Block);
    ASSERT_NE(step1State, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 4);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);

    // step2内分配的正向对照块及E2快照
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 5);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    auto step2Block = CreateNpuMalloc(0x9200, 0, 1, 6, 2100);
    MemoryState* step2State = MemoryStateManager::GetInstance().AddEvent(step2Block);
    ASSERT_NE(step2State, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 7);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);

    // 配对(E2,E3)起点合法，三个块在两次step结束时都存活、均交集命中：
    // 仅step2内分配的块（allocStep=2）打标，初始化期与step1内分配的常驻块被过滤
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3100, 8);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 9);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);

    EXPECT_FALSE(initState->isLeak);
    EXPECT_FALSE(step1State->isLeak);
    EXPECT_TRUE(step2State->isLeak);
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 1u);
    auto recordIt = records.find(step2State->allocationId);
    ASSERT_NE(recordIt, records.end());
    EXPECT_EQ(recordIt->second.source, LeakSource::STEP_SUSPECTED);
    EXPECT_EQ(recordIt->second.allocStep, 2u);
}

// 判定①要求stepId连续：打点缺失（E2后直接E4）不猜测配对，后续连续配对(E4,E5)正常判定
// 打点缺失（E3丢失）：E2后直接E4，配对按"该device相邻两次结束事件"进行（快照机制天然保证其间
// 无其他结束），跨度变宽只欠严不误报（被标块存活更久）。旧口径stepId算术相邻会静默吞掉此类
// 命中，且mstx打点的stepId为进程内全局自增，多device交叉打点下各device序列永不连续，
// 算术相邻会误杀整个多卡场景（见multi_device_interleaved_steps_flag_per_device）
TEST_F(LeakAnalyzerTest, lost_step_end_widens_pair_span_still_flags)
{
    // step2内分配的块，将存活到step5
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    auto npuEvent = CreateNpuMalloc(0x8900, 0, 1, 2, 1100);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 1200, 3);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);

    // step3结束打点缺失：E2之后直接E4。块在E2、E4结束时都存活 → 命中（跨度变宽），detectedStep=4
    std::shared_ptr<EventBase> start4 = CreateMstxStart(4, 400, 0, 2000, 4);
    LeakAnalyzer::GetInstance().EventHandle(start4, nullptr);
    std::shared_ptr<EventBase> end4 = CreateMstxEnd(4, 400, 0, 3000, 5);
    LeakAnalyzer::GetInstance().EventHandle(end4, nullptr);
    EXPECT_TRUE(state->isLeak);
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 1u);
    auto it = records.find(state->allocationId);
    ASSERT_NE(it, records.end());
    EXPECT_EQ(it->second.source, LeakSource::STEP_SUSPECTED);
    EXPECT_EQ(it->second.detectedStep, 4u);

    // 后续相邻配对(E4,E5)重复命中：累计表按allocationId去重，保留首次记录
    std::shared_ptr<EventBase> start5 = CreateMstxStart(5, 500, 0, 3100, 6);
    LeakAnalyzer::GetInstance().EventHandle(start5, nullptr);
    std::shared_ptr<EventBase> end5 = CreateMstxEnd(5, 500, 0, 4000, 7);
    LeakAnalyzer::GetInstance().EventHandle(end5, nullptr);
    EXPECT_EQ(LeakAnalyzer::GetInstance().leakRecords_.size(), 1u);
    EXPECT_EQ(it->second.detectedStep, 4u);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x8900});
}

// 地址复用：同地址先后两个块各自独立记录（allocationId区分），不互相混淆
TEST_F(LeakAnalyzerTest, address_reuse_creates_distinct_leak_records)
{
    // 块A：step2内分配，E2/E3结束时存活 → E3命中
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 2);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 3);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    auto mallocA = CreateNpuMalloc(0x9000, 0, 1, 4, 2100);
    MemoryState* stateA = MemoryStateManager::GetInstance().AddEvent(mallocA);
    ASSERT_NE(stateA, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 5);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3100, 6);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 7);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);
    ASSERT_EQ(LeakAnalyzer::GetInstance().leakRecords_.size(), 1u);

    // 块A释放并删除state（生产管线stage-3语义）；删除前捕获allocationId（state随erase析构）
    const uint64_t allocIdA = stateA->allocationId;
    auto freeA = CreateNpuFree(0x9000, 0, 1, 8, 4100);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(freeA), nullptr);
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0x9000});

    // 块B：同地址复用，step4内分配，E4/E5结束时存活 → E5命中
    std::shared_ptr<EventBase> start4 = CreateMstxStart(4, 400, 0, 4200, 9);
    LeakAnalyzer::GetInstance().EventHandle(start4, nullptr);
    auto mallocB = CreateNpuMalloc(0x9000, 0, 1, 10, 4300);
    MemoryState* stateB = MemoryStateManager::GetInstance().AddEvent(mallocB);
    ASSERT_NE(stateB, nullptr);
    std::shared_ptr<EventBase> end4 = CreateMstxEnd(4, 400, 0, 5000, 11);
    LeakAnalyzer::GetInstance().EventHandle(end4, nullptr);
    std::shared_ptr<EventBase> start5 = CreateMstxStart(5, 500, 0, 5100, 12);
    LeakAnalyzer::GetInstance().EventHandle(start5, nullptr);
    std::shared_ptr<EventBase> end5 = CreateMstxEnd(5, 500, 0, 6000, 13);
    LeakAnalyzer::GetInstance().EventHandle(end5, nullptr);

    // 两个块各有一条记录：allocationId不同，detectedStep不同，地址相同不混淆
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 2u);
    EXPECT_NE(allocIdA, stateB->allocationId);
    EXPECT_EQ(records.at(allocIdA).detectedStep, 3u);
    EXPECT_EQ(records.at(stateB->allocationId).detectedStep, 5u);
    EXPECT_EQ(records.at(allocIdA).ptr, 0x9000u);
    EXPECT_EQ(records.at(stateB->allocationId).ptr, 0x9000u);
    EXPECT_TRUE(stateB->isLeak);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x9000});
}

// 判定②：无step打点时，退出未释放块（NPU+HAL）全部打标并记录
TEST_F(LeakAnalyzerTest, exit_unfreed_blocks_marked_without_steps)
{
    auto npuEvent1 = CreateNpuMalloc(0xA000, 0, 1, 1, 1000);
    auto npuEvent2 = CreateNpuMalloc(0xB000, 0, 1, 2, 2000);
    auto halEvent = CreateHalMalloc(0xC000, 0, 1, 3, 3000);
    MemoryState* state1 = MemoryStateManager::GetInstance().AddEvent(npuEvent1);
    MemoryState* state2 = MemoryStateManager::GetInstance().AddEvent(npuEvent2);
    MemoryState* stateHal = MemoryStateManager::GetInstance().AddEvent(halEvent);
    ASSERT_NE(state1, nullptr);
    ASSERT_NE(state2, nullptr);
    ASSERT_NE(stateHal, nullptr);

    LeakAnalyzer::GetInstance().MarkExitUnfreedLeaks();

    // 三个退出未释放块全部标记：NPU两个 + HAL一个
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 3u);
    EXPECT_TRUE(state1->isLeak);
    EXPECT_TRUE(state2->isLeak);
    EXPECT_TRUE(stateHal->isLeak);
    EXPECT_EQ(records.at(state1->allocationId).source, LeakSource::EXIT_UNFREED);
    EXPECT_EQ(records.at(state2->allocationId).source, LeakSource::EXIT_UNFREED);
    EXPECT_EQ(records.at(stateHal->allocationId).source, LeakSource::EXIT_UNFREED);
    EXPECT_EQ(records.at(stateHal->allocationId).poolType, PoolType::HAL);
    // 无step边界时分配step为0
    EXPECT_EQ(records.at(state1->allocationId).allocStep, 0u);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0xA000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0xB000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 0, 0xC000});
}

// 判定②与判定①去重：①已命中的块退出时再次遇到，保留首次记录不重复计数
TEST_F(LeakAnalyzerTest, exit_marking_dedups_step_flagged_blocks)
{
    // ①路径：E2/E3命中
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 2);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 3);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    auto npuEvent = CreateNpuMalloc(0xA800, 0, 1, 4, 2100);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 5);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3100, 6);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 7);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);
    ASSERT_EQ(LeakAnalyzer::GetInstance().leakRecords_.size(), 1u);

    // ②路径：块仍存活，退出打标不新增记录、不覆盖来源
    LeakAnalyzer::GetInstance().MarkExitUnfreedLeaks();
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.at(state->allocationId).source, LeakSource::STEP_SUSPECTED);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0xA800});
}

// 判定②与基线口径对齐：存在step打点时，首个step结束点之前分配的块（NPU与HAL同规）退出不统计
TEST_F(LeakAnalyzerTest, exit_init_allocations_skipped_when_steps_exist)
{
    // 两个step：(E1,E2)配对被排除，无判定①记录，隔离验证判定②的step起点过滤
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 2);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 3);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 4);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);

    // E1前分配（allocStep=0）、step1内分配（allocStep=1）的NPU/HAL块退出时不统计；
    // step2内分配（allocStep=2）的块统计
    auto beforeStep = CreateNpuMalloc(0xA000, 0, 1, 5, 900);
    auto inStepOne = CreateNpuMalloc(0xB000, 0, 1, 6, 1100);
    auto halInStepOne = CreateHalMalloc(0xF000, 0, 1, 7, 1150);
    auto inStepTwo = CreateNpuMalloc(0xC000, 0, 1, 8, 2500);
    MemoryState* stateBefore = MemoryStateManager::GetInstance().AddEvent(beforeStep);
    MemoryState* stateInOne = MemoryStateManager::GetInstance().AddEvent(inStepOne);
    MemoryState* stateHalOne = MemoryStateManager::GetInstance().AddEvent(halInStepOne);
    MemoryState* stateInTwo = MemoryStateManager::GetInstance().AddEvent(inStepTwo);
    ASSERT_NE(stateBefore, nullptr);
    ASSERT_NE(stateInOne, nullptr);
    ASSERT_NE(stateHalOne, nullptr);
    ASSERT_NE(stateInTwo, nullptr);

    LeakAnalyzer::GetInstance().MarkExitUnfreedLeaks();

    // 仅首个step结束点之后分配的块入表
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.at(stateInTwo->allocationId).source, LeakSource::EXIT_UNFREED);
    EXPECT_TRUE(stateInTwo->isLeak);
    EXPECT_FALSE(stateBefore->isLeak);
    EXPECT_FALSE(stateInOne->isLeak);
    EXPECT_FALSE(stateHalOne->isLeak);
}

// 判定②HAL去重（真实torch退出时序）：判定①命中且PTA region已先于分析器析构被反注册，
// 同地址HAL块不重复入表也不打标——彻底跳过，退出统计与NPU层口径一致
TEST_F(LeakAnalyzerTest, exit_hal_matching_npu_record_skipped)
{
    // ①路径：0xA800在E2/E3命中
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 2);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 3);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    auto npuEvent = CreateNpuMalloc(0xA800, 0, 1, 4, 2100);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 5);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3100, 6);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 7);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);
    const uint64_t npuAllocId = state->allocationId;  // 删除state前捕获（state随erase析构）
    ASSERT_EQ(LeakAnalyzer::GetInstance().leakRecords_.size(), 1u);

    // 模拟torch退出时序：PTA region先于分析器析构被反注册（state删除，判定①累计记录保留）
    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0xA800});

    // 同地址HAL块存活到退出：不重复入表、不打标
    auto halEvent = CreateHalMalloc(0xA800, 0, 1, 8, 4200);
    MemoryState* halState = MemoryStateManager::GetInstance().AddEvent(halEvent);
    ASSERT_NE(halState, nullptr);
    LeakAnalyzer::GetInstance().MarkExitUnfreedLeaks();

    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 1u);  // 仍仅判定①记录，同地址HAL块未新增
    EXPECT_EQ(records.at(npuAllocId).source, LeakSource::STEP_SUSPECTED);
    EXPECT_FALSE(halState->isLeak);  // 彻底跳过：HAL行不携带leak标记
}

// 判定②HAL去重（无step场景）：与存活NPU块同地址的HAL块彻底跳过，独立地址HAL块正常兜底
TEST_F(LeakAnalyzerTest, exit_hal_shadowing_live_npu_addr_fully_skipped)
{
    auto npuEvent = CreateNpuMalloc(0xD000, 0, 1, 1, 1000);
    auto halSame = CreateHalMalloc(0xD000, 0, 1, 2, 1100);
    auto halOther = CreateHalMalloc(0xE000, 0, 1, 3, 1200);
    MemoryState* npuState = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    MemoryState* halSameState = MemoryStateManager::GetInstance().AddEvent(halSame);
    MemoryState* halOtherState = MemoryStateManager::GetInstance().AddEvent(halOther);
    ASSERT_NE(npuState, nullptr);
    ASSERT_NE(halSameState, nullptr);
    ASSERT_NE(halOtherState, nullptr);

    LeakAnalyzer::GetInstance().MarkExitUnfreedLeaks();

    // 无step打点：全部兜底，但同一内存只计NPU层一次
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 2u);
    EXPECT_TRUE(npuState->isLeak);
    EXPECT_FALSE(halSameState->isLeak);  // 同地址HAL彻底跳过
    EXPECT_TRUE(halOtherState->isLeak);  // 独立地址HAL正常标记
    EXPECT_EQ(records.at(halOtherState->allocationId).poolType, PoolType::HAL);
}

// NPU/HAL分析开关门控：--steps、DEFERRED、仅ALLOC、未开启分析类型
TEST_F(LeakAnalyzerTest, analysis_enable_gates_cover_all_conditions)
{
    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();

    // 默认配置（未开启泄漏/OOM分析）：两者均关闭
    ConfigManager::Instance().SetConfig(Config{});
    EXPECT_FALSE(analyzer.IsNpuAnalysisEnable());
    EXPECT_FALSE(analyzer.IsHalAnalysisEnable());

    // 正常配置：两者均开启
    SetLeakConfig();
    EXPECT_TRUE(analyzer.IsNpuAnalysisEnable());
    EXPECT_TRUE(analyzer.IsHalAnalysisEnable());

    // --steps开启：两者均关闭
    ConfigManager::Instance().SetConfig(MakeLeakConfig(2, CollectMode::IMMEDIATE, false));
    EXPECT_FALSE(analyzer.IsNpuAnalysisEnable());
    EXPECT_FALSE(analyzer.IsHalAnalysisEnable());

    // DEFERRED采集模式：仅HAL分析关闭
    ConfigManager::Instance().SetConfig(MakeLeakConfig(0, CollectMode::DEFERRED, false));
    EXPECT_TRUE(analyzer.IsNpuAnalysisEnable());
    EXPECT_FALSE(analyzer.IsHalAnalysisEnable());

    // 仅采集ALLOC、未采集FREE：两者均关闭
    // SetConfig会经SetEventDefaultConfig自动补齐FREE位，走公开接口无法得到“仅ALLOC”状态，
    // 此处直接写config_绕过默认化
    ConfigManager::Instance().config_ = MakeLeakConfig(0, CollectMode::IMMEDIATE, true);
    EXPECT_FALSE(analyzer.IsNpuAnalysisEnable());
    EXPECT_FALSE(analyzer.IsHalAnalysisEnable());
}

// shadow事件在分发前返回：HAL FREE影子事件不会登记client，非影子事件会登记
TEST_F(LeakAnalyzerTest, shadow_event_returns_before_dispatch)
{
    auto shadowFree = CreateHalFree(0x7A00, 0, 21, 1, 1000);
    shadowFree->isShadowEvent = true;
    std::shared_ptr<EventBase> shadowBase = shadowFree;
    LeakAnalyzer::GetInstance().EventHandle(shadowBase, nullptr);
    EXPECT_TRUE(LeakAnalyzer::GetInstance().halClients_.empty());
    EXPECT_TRUE(LeakAnalyzer::GetInstance().leakRecords_.empty());

    // 对照：非影子HAL FREE会登记client（进程集合语义）
    auto normalFree = CreateHalFree(0x7A00, 0, 21, 2, 2000);
    std::shared_ptr<EventBase> normalBase = normalFree;
    LeakAnalyzer::GetInstance().EventHandle(normalBase, nullptr);
    EXPECT_EQ(LeakAnalyzer::GetInstance().halClients_.count(21), 1u);
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{21, 0, 0x7A00});
}

// HAL MALLOC携带未知moduleId时仍登记client；同client后续事件不重复登记
TEST_F(LeakAnalyzerTest, hal_malloc_with_unknown_module_is_recorded)
{
    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();
    auto halMalloc = CreateHalMalloc(0x7C00, 0, 41, 1, 1000);
    halMalloc->moduleId = -1;  // 不在MODULE_HASH_TABLE中
    std::shared_ptr<EventBase> firstBase = halMalloc;
    analyzer.EventHandle(firstBase, nullptr);
    ASSERT_EQ(analyzer.halClients_.count(41), 1u);

    // 同client第二个HAL事件：插入失败分支（不重复登记）
    auto halMalloc2 = CreateHalMalloc(0x7D00, 0, 41, 2, 2000);
    halMalloc2->moduleId = 0;  // 已知module，不触发告警
    std::shared_ptr<EventBase> secondBase = halMalloc2;
    analyzer.EventHandle(secondBase, nullptr);
    EXPECT_EQ(analyzer.halClients_.count(41), 1u);

    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{41, 0, 0x7C00});
}

// 幽灵state判定：要求state中恰好存在一条与当前事件同id的FREE事件
TEST_F(LeakAnalyzerTest, is_ghost_state_requires_single_matching_free_event)
{
    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();
    auto freeEvent = CreateNpuFree(0x7F00, 0, 71, 10, 1000);
    std::shared_ptr<EventBase> base = freeEvent;

    // state为空
    EXPECT_FALSE(analyzer.IsGhostState(nullptr, base));

    // 单条同id FREE：成立
    MemoryState ghost;
    ghost.events.push_back(freeEvent);
    EXPECT_TRUE(analyzer.IsGhostState(&ghost, base));

    // 事件id不一致：不成立
    auto otherEvent = CreateNpuFree(0x7F00, 0, 71, 11, 2000);
    std::shared_ptr<EventBase> otherBase = otherEvent;
    EXPECT_FALSE(analyzer.IsGhostState(&ghost, otherBase));

    // 多条事件：不成立
    MemoryState multi;
    multi.events.push_back(freeEvent);
    multi.events.push_back(otherEvent);
    EXPECT_FALSE(analyzer.IsGhostState(&multi, base));

    // 单条同id但事件类型不是FREE：不成立
    MemoryState mallocState;
    auto mallocEvent = CreateNpuMalloc(0x7F00, 0, 71, 10, 1000);
    mallocState.events.push_back(mallocEvent);
    std::shared_ptr<EventBase> mallocBase = mallocEvent;
    EXPECT_FALSE(analyzer.IsGhostState(&mallocState, mallocBase));
}

// 退出报告：明细按allocStep升序、每设备组尾SUMMARY输出总大小/总块数
// （记录种入leakRecords_，key为allocationId）
TEST_F(LeakAnalyzerTest, report_leak_prints_sorted_detail_and_summary)
{
    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();
    LeakRecord atbRec;
    atbRec.ptr = 0x1200;
    atbRec.poolType = PoolType::ATB;
    atbRec.device = 0;
    atbRec.leakSize = static_cast<uint64_t>(2 * BYTE_TO_MB);
    atbRec.kernelIndex = 3;
    atbRec.allocStep = 2;
    atbRec.detectedStep = 4;
    analyzer.leakRecords_[1001] = atbRec;
    LeakRecord ptaRec;
    ptaRec.ptr = 0x1100;
    ptaRec.poolType = PoolType::PTA_CACHING;
    ptaRec.device = 0;
    ptaRec.leakSize = static_cast<uint64_t>(BYTE_TO_MB);
    ptaRec.kernelIndex = 9;
    ptaRec.allocStep = 5;
    ptaRec.source = LeakSource::EXIT_UNFREED;
    analyzer.leakRecords_[1002] = ptaRec;
    LeakRecord halRec;
    halRec.ptr = 0x1300;
    halRec.poolType = PoolType::HAL;
    halRec.device = 1;
    halRec.leakSize = static_cast<uint64_t>(BYTE_TO_MB);
    halRec.kernelIndex = 1;
    halRec.allocStep = 1;
    halRec.source = LeakSource::EXIT_UNFREED;
    analyzer.leakRecords_[1003] = halRec;

    testing::internal::CaptureStdout();
    analyzer.ReportLeak();
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("====== ERROR: Detected memory leaks on device 0 ======"), std::string::npos);
    EXPECT_NE(output.find("====== ERROR: Detected memory leaks on device 1 ======"), std::string::npos);
    // 明细行带判定来源与首次命中step（累计语义下"曾经疑似泄漏"与"退出未释放"可区分）
    EXPECT_NE(output.find("Direct ATB memory pool leak of 2.000000 Mb(s) at 0x1200 in kernel_3 at step 2 "
                          "(suspected: alive at consecutive step ends, first detected at step 4)."),
              std::string::npos);
    EXPECT_NE(output.find("Direct Pytorch Caching memory pool leak of 1.000000 Mb(s) at 0x1100 in kernel_9 at "
                          "step 5 (exit unfreed)."),
              std::string::npos);
    EXPECT_NE(output.find("Direct HAL memory pool leak of 1.000000 Mb(s) at 0x1300 in kernel_1 at step 1 "
                          "(exit unfreed)."),
              std::string::npos);
    // 明细按step升序（step2的ATB行在step5的PTA行之前）
    size_t atbPos = output.find("at step 2 (");
    size_t ptaPos = output.find("at step 5 (");
    ASSERT_NE(atbPos, std::string::npos);
    ASSERT_NE(ptaPos, std::string::npos);
    EXPECT_LT(atbPos, ptaPos);
    // 多device输出顺序稳定：device 0段先于device 1段
    size_t dev0Pos = output.find("on device 0");
    size_t dev1Pos = output.find("on device 1");
    ASSERT_NE(dev0Pos, std::string::npos);
    ASSERT_NE(dev1Pos, std::string::npos);
    EXPECT_LT(dev0Pos, dev1Pos);
    EXPECT_NE(output.find("====== SUMMARY: 3 Mb(s) leaked in 2 allocation(s) ======"), std::string::npos);
    EXPECT_NE(output.find("====== SUMMARY: 1 Mb(s) leaked in 1 allocation(s) ======"), std::string::npos);
}

// CheckHalLeak：无client记录与有存活块/无存活块client混合场景均不抛异常，且不改变存活块表
TEST_F(LeakAnalyzerTest, check_hal_leak_covers_empty_and_live_block_paths)
{
    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();
    // 无client记录：告警后返回
    EXPECT_NO_THROW(analyzer.CheckHalLeak());

    // client 31：存在未释放HAL块；client 32：仅登记、无存活块
    auto liveMalloc = CreateHalMalloc(0x7B00, 0, 31, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(liveMalloc), nullptr);
    analyzer.halClients_.insert(31);
    analyzer.halClients_.insert(32);

    EXPECT_NO_THROW(analyzer.CheckHalLeak());
    // 检查不改变存活块表：HAL块仍可查询到
    LiveBlockFilter filter;
    filter.poolTypes = {PoolType::HAL};
    filter.pid = 31;
    auto blocks = MemoryStateManager::GetInstance().QueryLiveBlocks(filter);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].addr, 0x7B00u);
}

// GetMemoryPoolName：已映射池返回精确名，未映射池返回空
TEST_F(LeakAnalyzerTest, undefined_pool_type_returns_empty_name)
{
    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();
    EXPECT_EQ(analyzer.GetMemoryPoolName(PoolType::PTA_CACHING), "Pytorch Caching memory pool");
    EXPECT_EQ(analyzer.GetMemoryPoolName(PoolType::ATB), "ATB memory pool");
    EXPECT_TRUE(analyzer.GetMemoryPoolName(PoolType::INVALID).empty());
    EXPECT_TRUE(analyzer.GetMemoryPoolName(PoolType::HOST).empty());
}

// 析构报告：NPU记录逐device输出；同地址HAL块经判定②去重（D7）不重复计入退出统计，
// 仅经CheckHalLeak按client告警
TEST_F(LeakAnalyzerTest, destructor_reports_npu_and_hal_leaks)
{
    // HAL存活块：析构中CheckHalLeak按client查询到该块并逐块告警
    auto halMalloc = CreateHalMalloc(0x8100, 0, 91, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halMalloc), nullptr);

    testing::internal::CaptureStdout();
    {
        LeakAnalyzer probe;
        // 两台device各一条泄漏记录：验证析构逐device输出明细与SUMMARY（unordered_map遍历覆盖多条）
        // 记录pid与HAL块一致：析构中MarkExitUnfreedLeaks对同(pid,device,addr)的HAL块按D7彻底
        // 跳过，退出统计不被HAL层重复计入
        LeakRecord dev0Rec;
        dev0Rec.ptr = 0x8100;
        dev0Rec.poolType = PoolType::PTA_CACHING;
        dev0Rec.pid = 91;
        dev0Rec.device = 0;
        dev0Rec.leakSize = static_cast<uint64_t>(BYTE_TO_MB);
        dev0Rec.kernelIndex = 0;
        dev0Rec.allocStep = 2;
        dev0Rec.detectedStep = 4;
        probe.leakRecords_[1001] = dev0Rec;
        LeakRecord dev1Rec;
        dev1Rec.ptr = 0x8200;
        dev1Rec.poolType = PoolType::ATB;
        dev1Rec.pid = 91;
        dev1Rec.device = 1;
        dev1Rec.leakSize = static_cast<uint64_t>(2 * BYTE_TO_MB);
        dev1Rec.kernelIndex = 0;
        dev1Rec.allocStep = 3;
        dev1Rec.detectedStep = 6;
        probe.leakRecords_[1002] = dev1Rec;
        probe.halClients_.insert(91);
    }  // 析构：UnSubscribe→MarkExitUnfreedLeaks→逐device ReportLeak→CheckHalLeak→FlushProcExitResiduals
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("====== ERROR: Detected memory leaks on device 0 ======"), std::string::npos);
    EXPECT_NE(output.find("====== ERROR: Detected memory leaks on device 1 ======"), std::string::npos);
    EXPECT_NE(output.find("Direct Pytorch Caching memory pool leak of 1.000000 Mb(s) at 0x8100 in kernel_0 at "
                          "step 2 (suspected: alive at consecutive step ends, first detected at step 4)."),
              std::string::npos);
    EXPECT_NE(output.find("Direct ATB memory pool leak of 2.000000 Mb(s) at 0x8200 in kernel_0 at step 3 "
                          "(suspected: alive at consecutive step ends, first detected at step 6)."),
              std::string::npos);
    EXPECT_NE(output.find("====== SUMMARY: 1 Mb(s) leaked in 1 allocation(s) ======"), std::string::npos);
    EXPECT_NE(output.find("====== SUMMARY: 2 Mb(s) leaked in 1 allocation(s) ======"), std::string::npos);

    // 显式落盘链路：probe析构中FlushProcExitResiduals已把存活块（含上述HAL块）经PROC_EXIT路由
    // 落盘并删除状态，leak:true的落盘不依赖~MemoryStateManager的静态析构顺序；
    // 重复调用幂等（状态已删，空操作不崩溃）
    EXPECT_TRUE(MemoryStateManager::GetInstance().GetAllStateKeys().empty());
    MemoryStateManager::GetInstance().FlushProcExitResiduals();
    EXPECT_TRUE(MemoryStateManager::GetInstance().GetAllStateKeys().empty());

    // 局部实例析构已UnSubscribe同一SubscriberId，恢复单例订阅
    LeakAnalyzer::GetInstance().Subscribe();
}

// 多device交叉打点（M1回归）：mstx的stepId为进程内全局自增（EventReport::SetStepInfo），
// 多卡场景下各device的stepId序列不连续（dev0: 1,3,5…；dev1: 2,4,6…）。判定①按"该device
// 相邻两次结束事件+锚点stepId值>=2"配对，不做算术相邻——旧口径下多卡场景判定①对所有块静默失效
TEST_F(LeakAnalyzerTest, multi_device_interleaved_steps_flag_per_device)
{
    // dev0与dev1交叉打点，stepId交错（模拟全局自增计数器）
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);  // dev0 step1
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 1, 1050, 2);  // dev1 step2
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 3);      // dev0 E1
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 1, 1250, 4);      // dev1 E2
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 2000, 5);  // dev0 step3（dev0无step2）
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    // dev0 step3内分配块：dev0的step序列为1,3,5
    auto npuEvent = CreateNpuMalloc(0x9100, 0, 1, 6, 2100);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 2500, 7);      // dev0 E3
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);
    // 配对(E1,E3)：锚点stepId=1为首个step结束 → 排除，此时尚未命中
    EXPECT_TRUE(LeakAnalyzer::GetInstance().leakRecords_.empty());

    std::shared_ptr<EventBase> start5 = CreateMstxStart(5, 500, 0, 3000, 8);  // dev0 step5
    LeakAnalyzer::GetInstance().EventHandle(start5, nullptr);
    std::shared_ptr<EventBase> end5 = CreateMstxEnd(5, 500, 0, 3500, 9);      // dev0 E5
    LeakAnalyzer::GetInstance().EventHandle(end5, nullptr);
    // 配对(E3,E5)：锚点stepId=3>=2（3→5不连续，但为dev0相邻两次结束事件）→ 命中
    EXPECT_TRUE(state->isLeak);
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 1u);
    auto it = records.find(state->allocationId);
    ASSERT_NE(it, records.end());
    EXPECT_EQ(it->second.source, LeakSource::STEP_SUSPECTED);
    EXPECT_EQ(it->second.detectedStep, 5u);
    EXPECT_EQ(it->second.allocStep, 3u);  // dev0边界归属：start3后分配

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x9100});
}

// 间隙块（W2回归）：step1结束~step2开始之间分配的块，按"首个step结束点之后"口径应判入；
// 旧的开始边界归属（allocStep=1<2）会误杀此块
TEST_F(LeakAnalyzerTest, gap_allocated_block_between_steps_counted)
{
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 2);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);
    // 间隙块：ts=1500在E1(1200)与start2(2000)之间
    auto npuEvent = CreateNpuMalloc(0x9200, 0, 1, 3, 1500);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 4);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 2500, 5);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);
    // 配对(E1,E2)锚点stepId=1 → 排除
    EXPECT_TRUE(LeakAnalyzer::GetInstance().leakRecords_.empty());

    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3000, 6);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 3500, 7);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);
    // 配对(E2,E3)：块在两次结束都存活，且分配于首个step结束点(1200)之后 → 命中
    EXPECT_TRUE(state->isLeak);
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 1u);
    auto it = records.find(state->allocationId);
    ASSERT_NE(it, records.end());
    EXPECT_EQ(it->second.source, LeakSource::STEP_SUSPECTED);
    EXPECT_EQ(it->second.detectedStep, 3u);
    // 边界归属仍是step1（记录展示用），过滤按结束点口径放行——两者有意不同
    EXPECT_EQ(it->second.allocStep, 1u);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x9200});
}

// 地址复用防漏报（M6回归）：NPU块命中判定①后释放，其地址被HAL复用——判定②的NPU地址
// 对照须排除已freed的历史记录，HAL块作为真泄漏候选被标记；旧逻辑对全部历史记录去重会漏报
TEST_F(LeakAnalyzerTest, freed_npu_record_not_dedup_hal_reuse)
{
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 2);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);
    auto npuEvent = CreateNpuMalloc(0x9300, 0, 1, 3, 1300);
    MemoryState* npuState = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(npuState, nullptr);
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 4);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 2500, 5);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3000, 6);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 3500, 7);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);
    // 配对(E2,E3)命中：NPU块打标+记录
    EXPECT_TRUE(npuState->isLeak);
    ASSERT_EQ(LeakAnalyzer::GetInstance().leakRecords_.size(), 1u);

    // NPU块释放：FREE事件将累计记录置freed
    auto npuFree = CreateNpuFree(0x9300, 0, 1, 8, 3600);
    MemoryState* freeState = MemoryStateManager::GetInstance().AddEvent(npuFree);
    ASSERT_NE(freeState, nullptr);
    std::shared_ptr<EventBase> freeBase = npuFree;
    LeakAnalyzer::GetInstance().EventHandle(freeBase, freeState);
    auto npuIt = LeakAnalyzer::GetInstance().leakRecords_.find(npuState->allocationId);
    ASSERT_NE(npuIt, LeakAnalyzer::GetInstance().leakRecords_.end());
    EXPECT_TRUE(npuIt->second.freed);

    // HAL块复用同一地址0x9300（同pid/device）
    auto halMalloc = CreateHalMalloc(0x9300, 0, 1, 9, 3700);
    MemoryState* halState = MemoryStateManager::GetInstance().AddEvent(halMalloc);
    ASSERT_NE(halState, nullptr);

    // 判定②（白盒直调，不走probe析构）：npuAddrs排除freed的NPU记录后不再误去重
    LeakAnalyzer::GetInstance().MarkExitUnfreedLeaks();
    EXPECT_TRUE(halState->isLeak);
    const auto& records = LeakAnalyzer::GetInstance().leakRecords_;
    ASSERT_EQ(records.size(), 2u);
    bool hasHalExitRecord = false;
    for (const auto& pair : records)
    {
        if (pair.second.poolType == PoolType::HAL && pair.second.ptr == 0x9300u &&
            pair.second.source == LeakSource::EXIT_UNFREED)
        {
            hasHalExitRecord = true;
        }
    }
    EXPECT_TRUE(hasHalExitRecord);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x9300});
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 0, 0x9300});
}

// oom-only配置（W1回归）：IsNpu/IsHal分析仍开（OOM查询依赖），但泄漏打标/报告须关——
// 新增IsLeakAnalysisEnable只认可LEAKS_ANALYSIS，判定①/②与ReportLeak均不产生泄漏输出
TEST_F(LeakAnalyzerTest, oom_only_config_does_not_mark_or_report_leaks)
{
    Config oomConfig = Config{};
    BitField<decltype(oomConfig.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::OOM_ANALYSIS));
    oomConfig.analysisType = analysisBit.getValue();
    BitField<decltype(oomConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    oomConfig.eventType = eventBit.getValue();
    oomConfig.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    ConfigManager::Instance().SetConfig(oomConfig);

    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();
    EXPECT_TRUE(analyzer.IsNpuAnalysisEnable());
    EXPECT_TRUE(analyzer.IsHalAnalysisEnable());
    EXPECT_FALSE(analyzer.IsLeakAnalysisEnable());

    // step驱动+存活块：判定①被IsLeakAnalysisEnable拦截，不产生记录/打标
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 1000, 1);
    analyzer.EventHandle(start2, nullptr);
    auto npuEvent = CreateNpuMalloc(0x9600, 0, 1, 2, 1100);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 1500, 3);
    analyzer.EventHandle(end2, nullptr);
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 2000, 4);
    analyzer.EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 2500, 5);
    analyzer.EventHandle(end3, nullptr);
    EXPECT_TRUE(analyzer.leakRecords_.empty());
    EXPECT_FALSE(state->isLeak);

    // 判定②：不产生标记/记录
    analyzer.MarkExitUnfreedLeaks();
    EXPECT_TRUE(analyzer.leakRecords_.empty());
    EXPECT_FALSE(state->isLeak);

    // ReportLeak无输出（门禁拦截）
    testing::internal::CaptureStdout();
    analyzer.ReportLeak();
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(output.empty());

    // OOM查询不受影响（IsNpuAnalysisEnable仍为true，QueryUnfreedRecords可用）
    auto oomRecords = analyzer.QueryUnfreedRecords(0, 1);
    EXPECT_FALSE(oomRecords.empty());

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x9600});
}

// 累计语义（D2-A回归）：判定①命中后又被释放的块仍保留在退出汇总——"曾经疑似泄漏"也是
// 定位信号，与基线leakMemSums_口径一致；明细行标注判定来源与首次命中step以便区分
TEST_F(LeakAnalyzerTest, report_keeps_cumulative_step_flagged_record_after_free)
{
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    LeakAnalyzer::GetInstance().EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 1200, 2);
    LeakAnalyzer::GetInstance().EventHandle(end1, nullptr);
    auto npuEvent = CreateNpuMalloc(0x9400, 0, 1, 3, 1300, BYTE_TO_MB);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(npuEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 4);
    LeakAnalyzer::GetInstance().EventHandle(start2, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 2500, 5);
    LeakAnalyzer::GetInstance().EventHandle(end2, nullptr);
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3000, 6);
    LeakAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 3500, 7);
    LeakAnalyzer::GetInstance().EventHandle(end3, nullptr);
    // 配对(E2,E3)命中：detectedStep=3
    EXPECT_TRUE(state->isLeak);
    ASSERT_EQ(LeakAnalyzer::GetInstance().leakRecords_.size(), 1u);

    // 块释放：记录保留（累计告警语义），仅置freed标志
    auto npuFree = CreateNpuFree(0x9400, 0, 1, 8, 3600);
    MemoryState* freeState = MemoryStateManager::GetInstance().AddEvent(npuFree);
    ASSERT_NE(freeState, nullptr);
    std::shared_ptr<EventBase> freeBase = npuFree;
    LeakAnalyzer::GetInstance().EventHandle(freeBase, freeState);

    testing::internal::CaptureStdout();
    LeakAnalyzer::GetInstance().ReportLeak();
    std::string output = testing::internal::GetCapturedStdout();
    // 已释放的命中块仍在退出汇总，明细带来源①与首次命中step
    EXPECT_NE(output.find("Direct Pytorch Caching memory pool leak of 1.000000 Mb(s) at 0x9400 in kernel_7 at "
                          "step 1 (suspected: alive at consecutive step ends, first detected at step 3)."),
              std::string::npos);
    EXPECT_NE(output.find("====== SUMMARY: 1 Mb(s) leaked in 1 allocation(s) ======"), std::string::npos);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING,
                                                  MemoryStateKey{1, 0, 0x9400});
}

// 明细打印上限（D4回归）：无step打点场景判定②可能全量命中数万条，每设备明细至多
// LEAK_REPORT_MAX_DETAIL_COUNT条，超出折叠提示改用dump文件leak:true筛查看全量；
// SUMMARY统计始终完整
TEST_F(LeakAnalyzerTest, report_folds_details_beyond_limit)
{
    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();
    // 60条记录：上限50条明细 + 折叠10条
    for (uint64_t i = 0; i < 60; ++i)
    {
        LeakRecord rec;
        rec.ptr = 0x2000 + i;
        rec.poolType = PoolType::PTA_CACHING;
        rec.device = 0;
        rec.leakSize = static_cast<uint64_t>(BYTE_TO_MB);
        rec.kernelIndex = 0;
        rec.allocStep = i + 1;
        rec.source = LeakSource::EXIT_UNFREED;
        analyzer.leakRecords_[2000 + i] = rec;
    }

    testing::internal::CaptureStdout();
    analyzer.ReportLeak();
    std::string output = testing::internal::GetCapturedStdout();

    // 明细恰好打印上限条数
    size_t detailCount = 0;
    size_t pos = 0;
    while ((pos = output.find("Direct ", pos)) != std::string::npos)
    {
        ++detailCount;
        pos += 7;
    }
    EXPECT_EQ(detailCount, static_cast<size_t>(LEAK_REPORT_MAX_DETAIL_COUNT));
    // 折叠行：10条未打印，提示改用dump文件leak:true筛查看
    EXPECT_NE(output.find("... 10 more leak(s) not printed, filter leak:true in dump file to see all details."),
              std::string::npos);
    // SUMMARY统计完整（60条全计入）
    EXPECT_NE(output.find("====== SUMMARY: 60 Mb(s) leaked in 60 allocation(s) ======"), std::string::npos);
}

// 端到端落盘链路（M9）：判定②打标 → FlushProcExitResiduals经CLEAN_UP(PROC_EXIT)路由 →
// Dump订阅者DumpMemoryState写出MALLOC行（attr携带leak:true），随后状态被删除。
// 该链路是leak:true语义的最终交付路径，不依赖~MemoryStateManager的静态析构顺序
TEST_F(LeakAnalyzerTest, exit_mark_flush_writes_leak_true_to_csv)
{
    Dump& dumper = Dump::GetInstance();
    // 清理其他用例遗留的旧handler（绑定失效目录，不清会导致本用例写入落空）
    dumper.handlerMap_.clear();
    dumper.sharedEventLists_.clear();

    // CSV输出配置（与DumpTest::SetUp同构，输出目录隔离避免污染其他用例）
    const std::string outputDir = "./testmsmemscope_leak_e2e";
    Config config = Config{};
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    config.analysisType = analysisBit.getValue();
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    BitField<decltype(config.dumpEventType)> dumpBit;
    dumpBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    dumpBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.dumpEventType = dumpBit.getValue();
    config.dataFormat = static_cast<uint8_t>(DataFormat::CSV);
    config.enableCStack = false;
    config.enablePyStack = false;
    config.stepList.stepCount = 0;
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    strncpy_s(config.outputDir, sizeof(config.outputDir), outputDir.c_str(), outputDir.size());
    ConfigManager::Instance().SetConfig(config);
    Utility::FileCreateManager::GetInstance(outputDir).SetProjectDir(outputDir);

    // 无step打点 → 判定②全量兜底：存活HAL块打标
    auto halMalloc = CreateHalMalloc(0x9500, 0, 55, 1, 1000);
    MemoryState* halState = MemoryStateManager::GetInstance().AddEvent(halMalloc);
    ASSERT_NE(halState, nullptr);
    LeakAnalyzer::GetInstance().MarkExitUnfreedLeaks();
    EXPECT_TRUE(halState->isLeak);

    // 显式冲刷：残留块经PROC_EXIT路由落盘并删状态
    MemoryStateManager::GetInstance().FlushProcExitResiduals();
    EXPECT_TRUE(MemoryStateManager::GetInstance().GetAllStateKeys().empty());
    // CsvHandler经FILE*缓冲写入，须再触发Dump的Fflush把缓冲同步到磁盘文件
    dumper.FflushEventToFile();

    // 读回CSV（可能按日期跨多文件，全部拼接）
    std::string content;
    std::string dumpDir = outputDir + "/device_0/dump";
    DIR* dir = opendir(dumpDir.c_str());
    ASSERT_NE(dir, nullptr);
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        std::string name = entry->d_name;
        if (name.rfind("memscope_dump_", 0) != 0)
        {
            continue;
        }
        std::ifstream file(dumpDir + "/" + name);
        if (file.is_open())
        {
            content += std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }
    }
    closedir(dir);
    ASSERT_FALSE(content.empty());
    // 0x9500的MALLOC行attr含leak:true：判定②标记经冲刷真实落盘
    EXPECT_NE(content.find(Uint64ToHexString(0x9500)), std::string::npos);
    EXPECT_NE(content.find("leak:true"), std::string::npos);

    // 恢复现场：清handler、清产物目录、还原配置
    dumper.handlerMap_.clear();
    dumper.sharedEventLists_.clear();
    Utility::FileCreateManager::GetInstance(outputDir).SetProjectDir("");
    dir = opendir(dumpDir.c_str());
    if (dir != nullptr)
    {
        while ((entry = readdir(dir)) != nullptr)
        {
            std::string name = entry->d_name;
            if (name == "." || name == "..")
            {
                continue;
            }
            std::remove((dumpDir + "/" + name).c_str());
        }
        closedir(dir);
    }
    rmdir(dumpDir.c_str());
    rmdir((outputDir + "/device_0").c_str());
    rmdir(outputDir.c_str());
    SetLeakConfig();
}
