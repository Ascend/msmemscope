/* -------------------------------------------------------------------------
 * This file is part of the MindStudio project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 *
 * MindStudio is licensed under Mulan PSL v2.
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
#define private public
#include "event_trace/event_report.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#undef private
#include "bit_field.h"

using namespace MemScope;

class CpuTensorCollectTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Utility::FileCreateManager::GetInstance("./testCpuTensor").SetProjectDir("./testCpuTensor");
        Config config = MemScope::GetConfig();
        config.collectAllNpu = true;
        config.collectCpu = true;
        BitField<decltype(config.eventType)> eventBit;
        eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
        eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
        config.eventType = eventBit.getValue();
        ConfigManager::Instance().SetConfig(config);
        EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);
    }

    void TearDown() override
    {
        Utility::FileCreateManager::GetInstance("./testCpuTensor").SetProjectDir("");
        rmdir("./testCpuTensor");
        EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
        instance.stepInfo_.currentStepId = 0;
        instance.stepInfo_.inStepRange = false;
        instance.stepInfo_.stepMarkRangeIdList.clear();
        instance.hostPtrs_.clear();
    }
};

TEST_F(CpuTensorCollectTest, MallocFree)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xABCD0000;
    uint64_t size = 1024;

    // MALLOC succeeds
    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, true, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);
    // FREE succeeds
    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, false, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 0);
}

TEST_F(CpuTensorCollectTest, MallocDedup)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xABCD0001;

    EXPECT_TRUE(instance.ReportCpuTensor(addr, 1024, true, ""));
    // duplicate MALLOC rejected (same-channel or cross-channel dedup)
    EXPECT_FALSE(instance.ReportCpuTensor(addr, 1024, true, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);
}

TEST_F(CpuTensorCollectTest, FreeUnknownAddr)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    EXPECT_FALSE(instance.ReportCpuTensor(0xDEAD, 1024, false, ""));
}

TEST_F(CpuTensorCollectTest, SkipWhenCpuDisabled)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectCpu = false;
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);

    EXPECT_FALSE(instance.ReportCpuTensor(0xFACE, 1024, true, ""));
    EXPECT_EQ(instance.hostPtrs_.count(0xFACE), 0);

    config.collectCpu = true;
    ConfigManager::Instance().SetConfig(config);
}
