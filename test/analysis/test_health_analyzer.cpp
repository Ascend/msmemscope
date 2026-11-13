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

#include "bit_field.h"
#include "config_info.h"
#include "event.h"
// ConfigManager::config_为私有成员：测试需绕过SetConfig的默认化补齐（ALLOC会自动补FREE）
// 才能构造“仅采集ALLOC”的开关门控状态，故与event_trace_manager.h一并白盒引入
#define private public
#include "event_trace/trace_manager/event_trace_manager.h"
#include "health_analyzer.h"
#undef private

using namespace MemScope;

namespace
{
void SetHealthConfig()
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
    ConfigManager::Instance().SetConfig(config);
}

std::shared_ptr<MemoryEvent> CreateNpuMalloc(uint64_t addr, int32_t device, uint64_t pid, uint64_t id,
                                             uint64_t timestamp, int64_t size, int64_t used, int64_t total)
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
    event->size = size;
    event->used = used;
    event->total = total;
    return event;
}

std::shared_ptr<MemoryEvent> CreateNpuFree(uint64_t addr, int32_t device, uint64_t pid, uint64_t id,
                                           uint64_t timestamp, int64_t used, int64_t total)
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
    event->used = used;
    event->total = total;
    return event;
}

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

class HealthAnalyzerTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        SetHealthConfig();
        HealthAnalyzer::GetInstance();
        // 清理单例跨用例残留状态
        HealthAnalyzer::GetInstance().poolStatusTables_.clear();
        HealthAnalyzer::GetInstance().stepStartAllocated_.clear();
        HealthAnalyzer::GetInstance().stepTracker_.deviceStates_.clear();
        HealthAnalyzer::GetInstance().stepTracker_.stepSource_ = StepSource::NONE;
    }

    void TearDown() override
    {
        ConfigManager::Instance().SetConfig(Config{});
        HealthAnalyzer::GetInstance().poolStatusTables_.clear();
        HealthAnalyzer::GetInstance().stepStartAllocated_.clear();
        HealthAnalyzer::GetInstance().stepTracker_.deviceStates_.clear();
        HealthAnalyzer::GetInstance().stepTracker_.stepSource_ = StepSource::NONE;
    }
};

TEST_F(HealthAnalyzerTest, mem_events_update_pool_status)
{
    auto mallocEvent = CreateNpuMalloc(0x1000, 0, 1, 1, 1000, 512, 512, 1024);
    std::shared_ptr<EventBase> baseEvent1 = mallocEvent;
    HealthAnalyzer::GetInstance().EventHandle(baseEvent1, nullptr);

    auto freeEvent = CreateNpuFree(0x1000, 0, 1, 2, 2000, 0, 1024);
    std::shared_ptr<EventBase> baseEvent2 = freeEvent;
    HealthAnalyzer::GetInstance().EventHandle(baseEvent2, nullptr);
    // 事件处理无异常即为通过（池状态在分析器内部维护，无块生命周期表）
}

TEST_F(HealthAnalyzerTest, non_npu_pools_are_ignored)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HAL;
    event->poolType = PoolType::HAL;
    event->addr = 0x2000;
    event->device = 0;
    event->pid = 1;
    event->id = 1;
    std::shared_ptr<EventBase> baseEvent = event;
    HealthAnalyzer::GetInstance().EventHandle(baseEvent, nullptr);
    // HAL池事件不进入NPU统计路径，无异常即为通过
}

TEST_F(HealthAnalyzerTest, step_start_end_log_and_gap_check)
{
    // step1 start（当前step=1，allocated尚未稳定不更新max/min）
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 1000, 1);
    HealthAnalyzer::GetInstance().EventHandle(start1, nullptr);

    // step1期间的内存事件（update被skipSteps门控）
    auto mallocEvent = CreateNpuMalloc(0x3000, 0, 1, 2, 1100, 100, 100, 200);
    std::shared_ptr<EventBase> baseMalloc = mallocEvent;
    HealthAnalyzer::GetInstance().EventHandle(baseMalloc, nullptr);

    // step2 start/end：CheckGap门控（duringStep=2 > skipSteps_=1，正常结算）
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 3);
    HealthAnalyzer::GetInstance().EventHandle(start2, nullptr);
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 4);
    HealthAnalyzer::GetInstance().EventHandle(end2, nullptr);

    // step3 start/end：第二次Gap结算
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3000, 5);
    HealthAnalyzer::GetInstance().EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 6);
    HealthAnalyzer::GetInstance().EventHandle(end3, nullptr);
    // 无异常即为通过（日志输出由LOG宏处理）
}

TEST_F(HealthAnalyzerTest, py_step_events_drive_end_then_start)
{
    // msmemscope.step(1)：step1开始
    auto pyStep1 = std::make_shared<EventBase>();
    pyStep1->eventType = EventBaseType::SYSTEM;
    pyStep1->eventSubType = EventSubType::STEP;
    pyStep1->device = 0;
    pyStep1->timestamp = 1000;
    pyStep1->id = 1;
    pyStep1->name = "1";
    std::shared_ptr<EventBase> baseStep1 = pyStep1;
    HealthAnalyzer::GetInstance().EventHandle(baseStep1, nullptr);

    // msmemscope.step(2)：step1结束（End日志+CheckGap）、step2开始
    auto pyStep2 = std::make_shared<EventBase>();
    pyStep2->eventType = EventBaseType::SYSTEM;
    pyStep2->eventSubType = EventSubType::STEP;
    pyStep2->device = 0;
    pyStep2->timestamp = 2000;
    pyStep2->id = 2;
    pyStep2->name = "2";
    std::shared_ptr<EventBase> baseStep2 = pyStep2;
    HealthAnalyzer::GetInstance().EventHandle(baseStep2, nullptr);
    // 无异常即为通过（stepId==1时End日志后early-return，不触发Gap）
}

// ==================== 增强覆盖用例（开关门控/Gap分支/报告输出） ====================
namespace
{
// 允许指定stepCount与事件采集位的最小配置
void SetHealthConfigWithGates(uint8_t stepCount, bool allocOnly)
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
    if (allocOnly)
    {
        // SetConfig会经SetEventDefaultConfig/SetAnalysisDefaultConfig自动补齐FREE位，
        // 走公开接口无法得到“仅ALLOC”状态；此处直接写config_绕过默认化
        ConfigManager::Instance().config_ = config;
        return;
    }
    ConfigManager::Instance().SetConfig(config);
}

// 构造带Gap统计值的池状态
MemoryPoolStatus MakePoolStatus(double maxRatio, uint64_t maxStep, int64_t maxMinAlloc, double minRatio,
                                uint64_t minStep, int64_t minMinAlloc)
{
    MemoryPoolStatus status{};
    status.maxGapInfo.gapStepId = maxStep;
    status.maxGapInfo.minMaxAllocRatio = maxRatio;
    status.maxGapInfo.minAllocMemory = maxMinAlloc;
    status.minGapInfo.gapStepId = minStep;
    status.minGapInfo.minMaxAllocRatio = minRatio;
    status.minGapInfo.minAllocMemory = minMinAlloc;
    return status;
}
}  // namespace

// 分析开关门控：--steps开启、仅采集ALLOC、分析类型未开启时都不做统计
TEST_F(HealthAnalyzerTest, analysis_gates_disable_statistics)
{
    // --steps开启（stepCount != 0）
    SetHealthConfigWithGates(3, false);
    auto stepEvent = CreateNpuMalloc(0x1000, 0, 1, 1, 1000, 512, 512, 1024);
    std::shared_ptr<EventBase> baseStep = stepEvent;
    HealthAnalyzer::GetInstance().EventHandle(baseStep, nullptr);
    EXPECT_TRUE(HealthAnalyzer::GetInstance().poolStatusTables_.empty());

    // 仅采集ALLOC、未采集FREE
    SetHealthConfigWithGates(0, true);
    auto allocOnly = CreateNpuMalloc(0x2000, 0, 1, 2, 2000, 256, 256, 512);
    std::shared_ptr<EventBase> baseAlloc = allocOnly;
    HealthAnalyzer::GetInstance().EventHandle(baseAlloc, nullptr);
    EXPECT_TRUE(HealthAnalyzer::GetInstance().poolStatusTables_.empty());

    // 分析类型未开启
    Config noAnalysis{};
    ConfigManager::Instance().SetConfig(noAnalysis);
    auto noAnalysisEvent = CreateNpuMalloc(0x3000, 0, 1, 3, 3000, 128, 128, 256);
    std::shared_ptr<EventBase> baseNoAnalysis = noAnalysisEvent;
    HealthAnalyzer::GetInstance().EventHandle(baseNoAnalysis, nullptr);
    EXPECT_TRUE(HealthAnalyzer::GetInstance().poolStatusTables_.empty());
}

// shadow事件在开关判定前直接跳过：开关开启也不会建表
TEST_F(HealthAnalyzerTest, shadow_event_is_skipped_before_analysis)
{
    auto shadowEvent = std::make_shared<MemoryEvent>();
    shadowEvent->eventType = EventBaseType::MALLOC;
    shadowEvent->eventSubType = EventSubType::PTA_CACHING;
    shadowEvent->poolType = PoolType::PTA_CACHING;
    shadowEvent->device = 0;
    shadowEvent->pid = 1;
    shadowEvent->addr = 0x4000;
    shadowEvent->id = 1;
    shadowEvent->used = 128;
    shadowEvent->total = 256;
    shadowEvent->isShadowEvent = true;
    std::shared_ptr<EventBase> base = shadowEvent;
    HealthAnalyzer::GetInstance().EventHandle(base, nullptr);
    EXPECT_TRUE(HealthAnalyzer::GetInstance().poolStatusTables_.empty());
}

// step1的End仅打日志不判泄漏；step未稳定时CheckGap直接返回；未记录device安全返回
TEST_F(HealthAnalyzerTest, step1_end_and_unstable_step_gap_are_skipped)
{
    HealthAnalyzer& analyzer = HealthAnalyzer::GetInstance();
    // step0（当前step=0）的内存事件：UpdateAllocated被skipSteps门控，不更新max/min
    auto initEvent = CreateNpuMalloc(0x5000, 0, 1, 1, 1000, 128, 100, 200);
    std::shared_ptr<EventBase> baseInit = initEvent;
    analyzer.EventHandle(baseInit, nullptr);
    ASSERT_EQ(analyzer.poolStatusTables_.count(0), 1u);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].totalAllocated, 100);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].stepMaxAllocated, 0);

    // step1 start/end：End在stepId==1时提前返回，Gap不结算
    std::shared_ptr<EventBase> start1 = CreateMstxStart(1, 100, 0, 2000, 2);
    analyzer.EventHandle(start1, nullptr);
    std::shared_ptr<EventBase> end1 = CreateMstxEnd(1, 100, 0, 3000, 3);
    analyzer.EventHandle(end1, nullptr);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].maxGapInfo.minMaxAllocRatio, 0.0);

    // 当前step=1（未超过skipSteps_）：直接调用CheckGap也不结算
    analyzer.CheckGap(0);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].maxGapInfo.minMaxAllocRatio, 0.0);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].stepMaxAllocated, 0);

    // 未记录device：CheckGap直接返回且不新增表项
    analyzer.CheckGap(9);
    EXPECT_EQ(analyzer.poolStatusTables_.count(9), 0u);
}

// 完整step序列驱动UpdateAllocated首值/最大/最小分支与CheckGap首算/最大/最小分支
TEST_F(HealthAnalyzerTest, update_allocated_and_gap_sequences)
{
    HealthAnalyzer& analyzer = HealthAnalyzer::GetInstance();
    // 建立device0的池表（当前step=0，占用100）
    auto initEvent = CreateNpuMalloc(0x6000, 0, 1, 1, 1000, 128, 100, 200);
    std::shared_ptr<EventBase> baseInit = initEvent;
    analyzer.EventHandle(baseInit, nullptr);
    ASSERT_EQ(analyzer.poolStatusTables_.count(0), 1u);

    // step2开始：记录起始占用100，之后currentStep=2允许统计
    std::shared_ptr<EventBase> start2 = CreateMstxStart(2, 200, 0, 2000, 2);
    analyzer.EventHandle(start2, nullptr);

    // step2内：首事件500初始化max/min；1000刷新最大；300刷新最小
    std::shared_ptr<EventBase> event501 = CreateNpuMalloc(0x6100, 0, 1, 3, 2100, 128, 500, 600);
    analyzer.EventHandle(event501, nullptr);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].stepMaxAllocated, 500);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].stepMinAllocated, 500);
    std::shared_ptr<EventBase> event1000 = CreateNpuMalloc(0x6200, 0, 1, 4, 2200, 128, 1000, 1100);
    analyzer.EventHandle(event1000, nullptr);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].stepMaxAllocated, 1000);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].stepMinAllocated, 500);
    std::shared_ptr<EventBase> event300 = CreateNpuMalloc(0x6300, 0, 1, 5, 2300, 128, 300, 400);
    analyzer.EventHandle(event300, nullptr);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].stepMaxAllocated, 1000);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].stepMinAllocated, 300);
    EXPECT_EQ(analyzer.poolStatusTables_[0][PoolType::PTA_CACHING].totalAllocated, 300);

    // step2结束：首次Gap结算，ratio=300/1000，随后min/max复位
    std::shared_ptr<EventBase> end2 = CreateMstxEnd(2, 200, 0, 3000, 6);
    analyzer.EventHandle(end2, nullptr);
    auto& status = analyzer.poolStatusTables_[0][PoolType::PTA_CACHING];
    EXPECT_EQ(status.maxGapInfo.gapStepId, 2u);
    EXPECT_DOUBLE_EQ(status.maxGapInfo.minMaxAllocRatio, 0.3);
    EXPECT_EQ(status.maxGapInfo.minAllocMemory, 300);
    EXPECT_EQ(status.minGapInfo.gapStepId, 2u);
    EXPECT_DOUBLE_EQ(status.minGapInfo.minMaxAllocRatio, 0.3);
    EXPECT_EQ(status.minGapInfo.minAllocMemory, 300);
    EXPECT_EQ(status.stepMaxAllocated, 0);
    EXPECT_EQ(status.stepMinAllocated, 0);

    // step3：1000/800 → gap 0.8 刷新最大比值，最小比值保持0.3
    std::shared_ptr<EventBase> start3 = CreateMstxStart(3, 300, 0, 3000, 7);
    analyzer.EventHandle(start3, nullptr);
    std::shared_ptr<EventBase> step3Max = CreateNpuMalloc(0x6400, 0, 1, 8, 3100, 128, 1000, 1100);
    analyzer.EventHandle(step3Max, nullptr);
    std::shared_ptr<EventBase> step3Min = CreateNpuMalloc(0x6500, 0, 1, 9, 3200, 128, 800, 900);
    analyzer.EventHandle(step3Min, nullptr);
    std::shared_ptr<EventBase> end3 = CreateMstxEnd(3, 300, 0, 4000, 10);
    analyzer.EventHandle(end3, nullptr);
    EXPECT_EQ(status.maxGapInfo.gapStepId, 3u);
    EXPECT_DOUBLE_EQ(status.maxGapInfo.minMaxAllocRatio, 0.8);
    EXPECT_EQ(status.maxGapInfo.minAllocMemory, 800);
    EXPECT_EQ(status.minGapInfo.gapStepId, 2u);
    EXPECT_DOUBLE_EQ(status.minGapInfo.minMaxAllocRatio, 0.3);

    // step4：1000/200 → gap 0.2 刷新最小比值，最大比值保持0.8
    std::shared_ptr<EventBase> start4 = CreateMstxStart(4, 400, 0, 4000, 11);
    analyzer.EventHandle(start4, nullptr);
    std::shared_ptr<EventBase> step4Max = CreateNpuMalloc(0x6600, 0, 1, 12, 4100, 128, 1000, 1100);
    analyzer.EventHandle(step4Max, nullptr);
    std::shared_ptr<EventBase> step4Min = CreateNpuMalloc(0x6700, 0, 1, 13, 4200, 128, 200, 300);
    analyzer.EventHandle(step4Min, nullptr);
    std::shared_ptr<EventBase> end4 = CreateMstxEnd(4, 400, 0, 5000, 14);
    analyzer.EventHandle(end4, nullptr);
    EXPECT_EQ(status.minGapInfo.gapStepId, 4u);
    EXPECT_DOUBLE_EQ(status.minGapInfo.minMaxAllocRatio, 0.2);
    EXPECT_EQ(status.minGapInfo.minAllocMemory, 200);
    EXPECT_DOUBLE_EQ(status.maxGapInfo.minMaxAllocRatio, 0.8);

    // step5：本step无内存事件 → stepMax为0，告警后跳过，不改变已有Gap
    std::shared_ptr<EventBase> start5 = CreateMstxStart(5, 500, 0, 5000, 15);
    analyzer.EventHandle(start5, nullptr);
    std::shared_ptr<EventBase> end5 = CreateMstxEnd(5, 500, 0, 6000, 16);
    analyzer.EventHandle(end5, nullptr);
    EXPECT_EQ(status.maxGapInfo.gapStepId, 3u);
    EXPECT_EQ(status.minGapInfo.gapStepId, 4u);
    EXPECT_EQ(status.stepMaxAllocated, 0);
    EXPECT_EQ(status.stepMinAllocated, 0);
}

// ReportGap：未记录device无输出；已记录device打印表头与Min/Max Gap行
TEST_F(HealthAnalyzerTest, report_gap_prints_summary_for_known_device)
{
    HealthAnalyzer& analyzer = HealthAnalyzer::GetInstance();
    testing::internal::CaptureStdout();
    analyzer.ReportGap(9);
    std::string emptyOutput = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(emptyOutput.empty());

    analyzer.poolStatusTables_[0][PoolType::PTA_CACHING] = MakePoolStatus(0.75, 3, 768, 0.25, 2, 256);
    testing::internal::CaptureStdout();
    analyzer.ReportGap(0);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("Pytorch Caching memory pool Gap Analysis of Device 0"), std::string::npos);
    EXPECT_NE(output.find("MinAlloc/MaxAlloc(%)"), std::string::npos);
    EXPECT_NE(output.find("MinGap"), std::string::npos);
    EXPECT_NE(output.find("MaxGap"), std::string::npos);
    EXPECT_NE(output.find("25.0000"), std::string::npos);
    EXPECT_NE(output.find("75.0000"), std::string::npos);
}

// 析构时逐device输出Gap报告；分析关闭时析构直接返回（局部实例作用域结束触发）
TEST_F(HealthAnalyzerTest, destructor_reports_gap_and_respects_analysis_gate)
{
    MemoryPoolStatus status = MakePoolStatus(0.6, 3, 600, 0.4, 2, 400);
    testing::internal::CaptureStdout();
    {
        HealthAnalyzer probe;
        probe.poolStatusTables_[0][PoolType::PTA_CACHING] = status;
    }  // 析构：IsAnalysisEnable为真 → 对device0调用ReportGap
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("Gap Analysis of Device 0"), std::string::npos);
    EXPECT_NE(output.find("MinGap"), std::string::npos);
    EXPECT_NE(output.find("MaxGap"), std::string::npos);

    // 分析关闭时析构不输出Gap报告
    Config disabledConfig{};
    ConfigManager::Instance().SetConfig(disabledConfig);
    testing::internal::CaptureStdout();
    {
        HealthAnalyzer probe;
        probe.poolStatusTables_[0][PoolType::PTA_CACHING] = status;
    }  // 析构：IsAnalysisEnable为假 → 直接返回
    std::string disabledOutput = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(disabledOutput.empty());

    // 局部实例析构已UnSubscribe同一SubscriberId，恢复单例订阅
    HealthAnalyzer::GetInstance().Subscribe();
}
