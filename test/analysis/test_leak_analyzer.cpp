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
#include <iostream>
#include <string>
#include <vector>

#include "bit_field.h"
#include "config_info.h"
#include "event.h"
// ConfigManager::config_为私有成员：测试需绕过SetConfig的默认化补齐（ALLOC会自动补FREE）
// 才能构造“仅采集ALLOC”的开关门控状态，故与event_trace_manager.h一并白盒引入
#define private public
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

// ==================== 增强覆盖用例（开关门控/异常事件/报告输出） ====================
namespace
{
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
}  // namespace

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
    EXPECT_TRUE(LeakAnalyzer::GetInstance().leakMemSums_.empty());

    // 对照：非影子HAL FREE会登记client（进程集合语义）
    auto normalFree = CreateHalFree(0x7A00, 0, 21, 2, 2000);
    std::shared_ptr<EventBase> normalBase = normalFree;
    LeakAnalyzer::GetInstance().EventHandle(normalBase, nullptr);
    EXPECT_EQ(LeakAnalyzer::GetInstance().halClients_.count(21), 1u);
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{21, 0, 0x7A00});
}

// HAL MALLOC：首次登记client；未知moduleId走告警分支，重复client不重复登记
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

// 幽灵state判定：仅“单条且id相同的FREE事件”成立
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

// ReportLeak：按step升序输出每块明细与汇总
TEST_F(LeakAnalyzerTest, report_leak_prints_sorted_detail_and_summary)
{
    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();
    auto& table = analyzer.leakMemSums_[0];
    auto& atbInfo = table[LeakMemKey{0x1200, PoolType::ATB, 2}];
    atbInfo.leakSize = static_cast<int64_t>(BYTE_TO_MB) * 2;
    atbInfo.kernelIndex = 3;
    auto& ptaInfo = table[LeakMemKey{0x1100, PoolType::PTA_CACHING, 5}];
    ptaInfo.leakSize = static_cast<int64_t>(BYTE_TO_MB);
    ptaInfo.kernelIndex = 9;

    testing::internal::CaptureStdout();
    analyzer.ReportLeak(0);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("====== ERROR: Detected memory leaks on device 0 ======"), std::string::npos);
    EXPECT_NE(output.find("Direct ATB memory pool leak of 2.000000 Mb(s) at 0x1200 in kernel_3 at step 2."),
              std::string::npos);
    EXPECT_NE(output.find("Direct Pytorch Caching memory pool leak of 1.000000 Mb(s) at 0x1100 in kernel_9 at step 5."),
              std::string::npos);
    // 明细按step升序（step2的ATB行在step5的PTA行之前）
    size_t atbPos = output.find("at step 2.");
    size_t ptaPos = output.find("at step 5.");
    ASSERT_NE(atbPos, std::string::npos);
    ASSERT_NE(ptaPos, std::string::npos);
    EXPECT_LT(atbPos, ptaPos);
    EXPECT_NE(output.find("====== SUMMARY: 3 Mb(s) leaked in 2 allocation(s) ======"), std::string::npos);
}

// CheckHalLeak：无登记client直接返回；存活块逐块告警；无块client走no leak分支
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

// 未定义池类型：返回空串（HAL等无展示名类型同样返回空）
TEST_F(LeakAnalyzerTest, undefined_pool_type_returns_empty_name)
{
    LeakAnalyzer& analyzer = LeakAnalyzer::GetInstance();
    EXPECT_EQ(analyzer.GetMemoryPoolName(PoolType::PTA_CACHING), "Pytorch Caching memory pool");
    EXPECT_EQ(analyzer.GetMemoryPoolName(PoolType::ATB), "ATB memory pool");
    EXPECT_TRUE(analyzer.GetMemoryPoolName(PoolType::INVALID).empty());
    EXPECT_TRUE(analyzer.GetMemoryPoolName(PoolType::HOST).empty());
}

// 析构兜底：NPU泄漏表非空时逐device输出汇总，HAL client存在存活块时执行泄漏检查
TEST_F(LeakAnalyzerTest, destructor_reports_npu_and_hal_leaks)
{
    // HAL存活块：析构中CheckHalLeak按client查询到该块并逐块告警
    auto halMalloc = CreateHalMalloc(0x8100, 0, 91, 1, 1000);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halMalloc), nullptr);

    testing::internal::CaptureStdout();
    {
        LeakAnalyzer probe;
        // 两台device各一条泄漏记录：验证析构逐device调用ReportLeak（unordered_map遍历覆盖多条）
        probe.leakMemSums_[0][LeakMemKey{0x8100, PoolType::PTA_CACHING, 2}].leakSize =
            static_cast<int64_t>(BYTE_TO_MB);
        probe.leakMemSums_[1][LeakMemKey{0x8200, PoolType::ATB, 3}].leakSize =
            static_cast<int64_t>(2 * BYTE_TO_MB);
        probe.halClients_.insert(91);
    }  // 析构：IsNpuAnalysisEnable为真→逐device ReportLeak；IsHalAnalysisEnable为真→CheckHalLeak
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("====== ERROR: Detected memory leaks on device 0 ======"), std::string::npos);
    EXPECT_NE(output.find("====== ERROR: Detected memory leaks on device 1 ======"), std::string::npos);
    EXPECT_NE(output.find("Direct Pytorch Caching memory pool leak of 1.000000 Mb(s) at 0x8100 in kernel_0 at step 2."),
              std::string::npos);
    EXPECT_NE(output.find("Direct ATB memory pool leak of 2.000000 Mb(s) at 0x8200 in kernel_0 at step 3."),
              std::string::npos);
    EXPECT_NE(output.find("====== SUMMARY: 1 Mb(s) leaked in 1 allocation(s) ======"), std::string::npos);
    EXPECT_NE(output.find("====== SUMMARY: 2 Mb(s) leaked in 1 allocation(s) ======"), std::string::npos);

    // 局部实例析构已UnSubscribe同一SubscriberId，恢复单例订阅
    LeakAnalyzer::GetInstance().Subscribe();
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{91, 0, 0x8100});
}
