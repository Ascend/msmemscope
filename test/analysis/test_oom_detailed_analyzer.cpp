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
#define private public
#include "oom_detailed_analyzer.h"
#include "hal_analyzer.h"
#include "stepinner_analyzer.h"
#undef private
#include "bit_field.h"
#include "config_info.h"
#include "event.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#include "record_info.h"
#include "utility/utils.h"

using namespace MemScope;

static Config MakeOOMConfig()
{
    Config cfg;
    BitField<decltype(cfg.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::OOM_ANALYSIS));
    cfg.analysisType = analysisBit.getValue();

    BitField<decltype(cfg.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    cfg.eventType = eventBit.getValue();

    cfg.stepList.stepCount = 0;
    cfg.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    cfg.oomTopK = 10;
    return cfg;
}

static uint32_t GetCurrentClientId()
{
    return static_cast<uint32_t>(Utility::GetPid());
}

class OOMDetailedAnalyzerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Config cfg = MakeOOMConfig();
        ConfigManager::Instance().SetConfig(cfg);
        OOMDetailedAnalyzer::GetInstance(cfg);
    }

    void TearDown() override
    {
        ConfigManager::Instance().SetConfig(Config{});
        HalAnalyzer::GetInstance().memtables_.clear();
        StepInnerAnalyzer::GetInstance().npuMemUsages_.clear();
        StepInnerAnalyzer::GetInstance().leakMemSums_.clear();
        StepInnerAnalyzer::GetInstance().stepInfoTables_.clear();
        StepInnerAnalyzer::GetInstance().skipSteps_ = 1;
    }

    void InjectHalRecord(uint64_t addr, int64_t size, uint64_t timestamp, int32_t deviceId,
                         const std::string& cStack = "", const std::string& pyStack = "")
    {
        std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
        event->eventType = EventBaseType::MALLOC;
        event->poolType = PoolType::HAL;
        event->eventSubType = EventSubType::HAL;
        event->device = deviceId;
        event->addr = addr;
        event->size = size;
        event->timestamp = timestamp;
        event->cCallStack = cStack;
        event->pyCallStack = pyStack;

        std::shared_ptr<EventBase> baseEvent = event;
        HalAnalyzer::GetInstance().EventHandle(baseEvent, nullptr);
    }

    void InjectNpuRecord(uint64_t addr, int64_t size, uint64_t timestamp, int32_t deviceId,
                         PoolType poolType = PoolType::PTA_CACHING,
                         const std::string& cStack = "", const std::string& pyStack = "")
    {
        std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
        event->eventType = EventBaseType::MALLOC;
        event->poolType = poolType;
        event->eventSubType = static_cast<EventSubType>(poolType);
        event->device = deviceId;
        event->addr = addr;
        event->size = size;
        event->used = size;
        event->total = size;
        event->timestamp = timestamp;
        event->kernelIndex = 0;
        event->cCallStack = cStack;
        event->pyCallStack = pyStack;

        std::shared_ptr<EventBase> baseEvent = event;
        StepInnerAnalyzer::GetInstance().EventHandle(baseEvent, nullptr);
    }
};

// ==================== IsEnabled ====================

TEST_F(OOMDetailedAnalyzerTest, IsEnabled_ReturnsTrue_WhenOOMFlagSet)
{
    Config cfg = MakeOOMConfig();
    BitField<decltype(cfg.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::OOM_ANALYSIS));
    cfg.analysisType = analysisBit.getValue();

    OOMDetailedAnalyzer::GetInstance(cfg).config_.analysisType = cfg.analysisType;
    ASSERT_TRUE(OOMDetailedAnalyzer::GetInstance(cfg).IsEnabled());
}

TEST_F(OOMDetailedAnalyzerTest, IsEnabled_ReturnsFalse_WhenNoOOMFlag)
{
    Config cfg = MakeOOMConfig();
    cfg.analysisType = 0;

    OOMDetailedAnalyzer::GetInstance(cfg).config_.analysisType = cfg.analysisType;
    ASSERT_FALSE(OOMDetailedAnalyzer::GetInstance(cfg).IsEnabled());
}

// ==================== ShouldDumpDetails ====================

TEST_F(OOMDetailedAnalyzerTest, ShouldDumpDetails_FirstCallReturnsTrue)
{
    OOMDetailedAnalyzer::GetInstance(MakeOOMConfig()).lastDetailDumpTimestamp_ = 0;
    ASSERT_TRUE(OOMDetailedAnalyzer::GetInstance(MakeOOMConfig()).ShouldDumpDetails());
}

TEST_F(OOMDetailedAnalyzerTest, ShouldDumpDetails_SecondCallReturnsFalse)
{
    auto& analyzer = OOMDetailedAnalyzer::GetInstance(MakeOOMConfig());
    analyzer.lastDetailDumpTimestamp_ = 0;

    ASSERT_TRUE(analyzer.ShouldDumpDetails());
    ASSERT_FALSE(analyzer.ShouldDumpDetails());
}

TEST_F(OOMDetailedAnalyzerTest, ShouldDumpDetails_AfterIntervalReturnsTrue)
{
    auto& analyzer = OOMDetailedAnalyzer::GetInstance(MakeOOMConfig());

    // 第一次调用成功
    analyzer.lastDetailDumpTimestamp_ = 0;
    ASSERT_TRUE(analyzer.ShouldDumpDetails());

    // 模拟时间回退到 3s 前（超过 2s 防抖窗口）
    uint64_t now = Utility::GetTimeNanoseconds();
    uint64_t kThreeSeconds = 3000000000ULL;
    analyzer.lastDetailDumpTimestamp_ = now - kThreeSeconds;

    ASSERT_TRUE(analyzer.ShouldDumpDetails());
}

// ==================== QueryRecentAllocs ====================

TEST_F(OOMDetailedAnalyzerTest, QueryRecentAllocs_SortedByTimestampDesc)
{
    const int32_t deviceId = 0;
    const uint32_t clientId = GetCurrentClientId();
    Config cfg = MakeOOMConfig();
    cfg.oomTopK = 10;

    // 注入 3 条 NPU 记录：timestamp 分别为 100, 300, 200
    InjectNpuRecord(0x1000, 1024, 100, deviceId);
    InjectNpuRecord(0x2000, 2048, 300, deviceId);
    InjectNpuRecord(0x3000, 4096, 200, deviceId);

    // 注入 2 条 HAL 记录：timestamp 分别为 150, 50
    InjectHalRecord(0xA000, 512, 150, deviceId);
    InjectHalRecord(0xB000, 256, 50, deviceId);

    auto records = OOMDetailedAnalyzer::GetInstance(cfg).QueryRecentAllocs(deviceId, clientId);

    ASSERT_EQ(records.size(), 5u);

    // 验证按 timestamp 降序排列
    for (size_t i = 0; i < records.size() - 1; i++)
    {
        ASSERT_GE(records[i].allocTimestamp, records[i + 1].allocTimestamp)
            << "records not sorted by timestamp desc at index " << i;
    }
}

// ==================== QueryTopAllocs ====================

TEST_F(OOMDetailedAnalyzerTest, QueryTopAllocs_SortedBySizeDesc)
{
    const int32_t deviceId = 0;
    const uint32_t clientId = GetCurrentClientId();
    Config cfg = MakeOOMConfig();
    cfg.oomTopK = 10;

    // 注入 2 条 NPU 记录：size 1024, 4096
    InjectNpuRecord(0x1000, 1024, 100, deviceId);
    InjectNpuRecord(0x2000, 4096, 200, deviceId);

    // 注入 2 条 HAL 记录：size 512, 2048
    InjectHalRecord(0xA000, 512, 150, deviceId);
    InjectHalRecord(0xB000, 2048, 250, deviceId);

    auto records = OOMDetailedAnalyzer::GetInstance(cfg).QueryTopAllocs(deviceId, clientId);

    ASSERT_EQ(records.size(), 4u);

    // 验证按 memSize 降序排列
    for (size_t i = 0; i < records.size() - 1; i++)
    {
        ASSERT_GE(records[i].memSize, records[i + 1].memSize)
            << "records not sorted by size desc at index " << i;
    }
}

// ==================== TopK 控制 ====================

TEST_F(OOMDetailedAnalyzerTest, QueryRecentAllocs_RespectsTopK)
{
    const int32_t deviceId = 0;
    const uint32_t clientId = GetCurrentClientId();

    auto& analyzer = OOMDetailedAnalyzer::GetInstance(MakeOOMConfig());
    analyzer.config_.oomTopK = 2;

    // 注入 5 条记录（3 NPU + 2 HAL），但 K=2
    InjectNpuRecord(0x1000, 100, 500, deviceId);
    InjectNpuRecord(0x2000, 100, 400, deviceId);
    InjectNpuRecord(0x3000, 100, 300, deviceId);
    InjectHalRecord(0xA000, 100, 200, deviceId);
    InjectHalRecord(0xB000, 100, 100, deviceId);

    auto records = analyzer.QueryRecentAllocs(deviceId, clientId);

    ASSERT_EQ(records.size(), 2u);
    // 最新的 2 条：timestamp 500 和 400
    ASSERT_EQ(records[0].allocTimestamp, 500u);
    ASSERT_EQ(records[1].allocTimestamp, 400u);
}

TEST_F(OOMDetailedAnalyzerTest, QueryTopAllocs_TakesAllWhenLessThanK)
{
    const int32_t deviceId = 0;
    const uint32_t clientId = GetCurrentClientId();
    Config cfg = MakeOOMConfig();
    cfg.oomTopK = 10;

    // 只注入 2 条
    InjectNpuRecord(0x1000, 1024, 100, deviceId);
    InjectHalRecord(0xA000, 512, 200, deviceId);

    auto records = OOMDetailedAnalyzer::GetInstance(cfg).QueryTopAllocs(deviceId, clientId);

    ASSERT_EQ(records.size(), 2u);
}

TEST_F(OOMDetailedAnalyzerTest, QueryRecentAllocs_EmptyWhenNoRecords)
{
    const int32_t deviceId = 0;
    const uint32_t clientId = GetCurrentClientId();
    Config cfg = MakeOOMConfig();

    auto records = OOMDetailedAnalyzer::GetInstance(cfg).QueryRecentAllocs(deviceId, clientId);

    ASSERT_TRUE(records.empty());
}
