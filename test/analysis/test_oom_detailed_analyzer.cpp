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

#include "bit_field.h"
#include "config_info.h"
#include "event.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#define private public
#include "oom_detailed_analyzer.h"
#undef private
#include "memory_state_manager.h"
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

namespace
{
// 构造MALLOC事件并直接注入MemoryStateManager（重构后OOM查询统一走
// LeakAnalyzer::QueryUnfreedRecords → MemoryStateManager::QueryLiveBlocks，
// 不再经过HalAnalyzer/StepInnerAnalyzer）
std::shared_ptr<MemoryEvent> CreateMalloc(PoolType poolType, uint64_t addr, int32_t device, uint64_t pid,
                                          uint64_t id, uint64_t timestamp, uint64_t size)
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
    return event;
}
}  // namespace

class OOMDetailedAnalyzerTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        Config cfg = MakeOOMConfig();
        ConfigManager::Instance().SetConfig(cfg);
        // 带参单例（static仅初始化一次），SetUp中以默认oomTopK=10初始化，供后续用例复用
        OOMDetailedAnalyzer::GetInstance(cfg);
        // 清理单例跨用例残留state
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
    }

    void TearDown() override
    {
        ConfigManager::Instance().SetConfig(Config{});
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
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
    // NPU按device过滤、HAL按clientId(pid)过滤，注入块pid统一为1
    const uint32_t clientId = 1;

    // 注入 3 条 NPU 记录：timestamp 分别为 100, 300, 200
    auto npuEvent1 = CreateMalloc(PoolType::PTA_CACHING, 0x1000, deviceId, clientId, 1, 100, 1024);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent1), nullptr);
    auto npuEvent2 = CreateMalloc(PoolType::PTA_CACHING, 0x2000, deviceId, clientId, 2, 300, 2048);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent2), nullptr);
    auto npuEvent3 = CreateMalloc(PoolType::PTA_CACHING, 0x3000, deviceId, clientId, 3, 200, 4096);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent3), nullptr);

    // 注入 2 条 HAL 记录：timestamp 分别为 150, 50
    auto halEvent1 = CreateMalloc(PoolType::HAL, 0xA000, deviceId, clientId, 4, 150, 512);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent1), nullptr);
    auto halEvent2 = CreateMalloc(PoolType::HAL, 0xB000, deviceId, clientId, 5, 50, 256);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent2), nullptr);

    auto records = OOMDetailedAnalyzer::GetInstance(MakeOOMConfig()).QueryRecentAllocs(deviceId, clientId);

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
    const uint32_t clientId = 1;

    // 注入 2 条 NPU 记录：size 1024, 4096
    auto npuEvent1 = CreateMalloc(PoolType::PTA_CACHING, 0x1000, deviceId, clientId, 1, 100, 1024);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent1), nullptr);
    auto npuEvent2 = CreateMalloc(PoolType::PTA_CACHING, 0x2000, deviceId, clientId, 2, 200, 4096);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent2), nullptr);

    // 注入 2 条 HAL 记录：size 512, 2048
    auto halEvent1 = CreateMalloc(PoolType::HAL, 0xA000, deviceId, clientId, 3, 150, 512);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent1), nullptr);
    auto halEvent2 = CreateMalloc(PoolType::HAL, 0xB000, deviceId, clientId, 4, 250, 2048);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent2), nullptr);

    auto records = OOMDetailedAnalyzer::GetInstance(MakeOOMConfig()).QueryTopAllocs(deviceId, clientId);

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
    const uint32_t clientId = 1;

    auto& analyzer = OOMDetailedAnalyzer::GetInstance(MakeOOMConfig());
    analyzer.config_.oomTopK = 2;

    // 注入 5 条记录（3 NPU + 2 HAL），但 K=2
    auto npuEvent1 = CreateMalloc(PoolType::PTA_CACHING, 0x1000, deviceId, clientId, 1, 500, 100);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent1), nullptr);
    auto npuEvent2 = CreateMalloc(PoolType::PTA_CACHING, 0x2000, deviceId, clientId, 2, 400, 100);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent2), nullptr);
    auto npuEvent3 = CreateMalloc(PoolType::PTA_CACHING, 0x3000, deviceId, clientId, 3, 300, 100);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent3), nullptr);
    auto halEvent1 = CreateMalloc(PoolType::HAL, 0xA000, deviceId, clientId, 4, 200, 100);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent1), nullptr);
    auto halEvent2 = CreateMalloc(PoolType::HAL, 0xB000, deviceId, clientId, 5, 100, 100);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent2), nullptr);

    auto records = analyzer.QueryRecentAllocs(deviceId, clientId);

    ASSERT_EQ(records.size(), 2u);
    // 最新的 2 条：timestamp 500 和 400
    ASSERT_EQ(records[0].allocTimestamp, 500u);
    ASSERT_EQ(records[1].allocTimestamp, 400u);
}

TEST_F(OOMDetailedAnalyzerTest, QueryTopAllocs_TakesAllWhenLessThanK)
{
    const int32_t deviceId = 0;
    const uint32_t clientId = 1;

    auto& analyzer = OOMDetailedAnalyzer::GetInstance(MakeOOMConfig());
    analyzer.config_.oomTopK = 10;

    // 只注入 2 条
    auto npuEvent1 = CreateMalloc(PoolType::PTA_CACHING, 0x1000, deviceId, clientId, 1, 100, 1024);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent1), nullptr);
    auto halEvent1 = CreateMalloc(PoolType::HAL, 0xA000, deviceId, clientId, 2, 200, 512);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent1), nullptr);

    auto records = analyzer.QueryTopAllocs(deviceId, clientId);

    ASSERT_EQ(records.size(), 2u);
}

TEST_F(OOMDetailedAnalyzerTest, QueryRecentAllocs_EmptyWhenNoRecords)
{
    const int32_t deviceId = 0;
    const uint32_t clientId = 1;

    auto records = OOMDetailedAnalyzer::GetInstance(MakeOOMConfig()).QueryRecentAllocs(deviceId, clientId);

    ASSERT_TRUE(records.empty());
}

// ==================== NPU与HAL合并查询 ====================

TEST_F(OOMDetailedAnalyzerTest, QueryMergesNpuAndHalRecords_Sorted)
{
    // GetInstance为带参单例（static仅初始化一次），全测试共用同一oomTopK（SetUp中=10）
    OOMDetailedAnalyzer& analyzer = OOMDetailedAnalyzer::GetInstance(MakeOOMConfig());

    // NPU块（device=0, pid=1）
    auto npuEvent = CreateMalloc(PoolType::PTA_CACHING, 0x1000, 0, 1, 1, 1000, 2048);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent), nullptr);
    // HAL块（pid=1）
    auto halEvent = CreateMalloc(PoolType::HAL, 0x2000, 0, 1, 2, 2000, 4096);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent), nullptr);
    // 其他pid的块不参与（HAL按clientId过滤）
    auto otherPidEvent = CreateMalloc(PoolType::HAL, 0x3000, 0, 2, 3, 3000, 8192);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(otherPidEvent), nullptr);

    // 合并NPU(device过滤)+HAL(pid过滤)
    auto topAllocs = analyzer.QueryTopAllocs(0, 1);
    ASSERT_EQ(topAllocs.size(), 2u);
    EXPECT_EQ(topAllocs[0].ptr, 0x2000);  // 按size降序：4096 > 2048
    EXPECT_EQ(topAllocs[1].ptr, 0x1000);

    auto recentAllocs = analyzer.QueryRecentAllocs(0, 1);
    ASSERT_EQ(recentAllocs.size(), 2u);
    EXPECT_EQ(recentAllocs[0].ptr, 0x2000);  // 按时间降序：2000 > 1000
    EXPECT_EQ(recentAllocs[1].ptr, 0x1000);

    MemoryStateManager::GetInstance().DeteleState(PoolType::PTA_CACHING, MemoryStateKey{1, 0, 0x1000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{1, 0, 0x2000});
    MemoryStateManager::GetInstance().DeteleState(PoolType::HAL, MemoryStateKey{2, 0, 0x3000});
}
