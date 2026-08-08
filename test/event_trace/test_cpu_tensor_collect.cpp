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
#define private public
#include "event_trace/event_report.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#undef private
#include "bit_field.h"
#include "framework/record_info.h"

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

        // Reset step info to avoid cross-test interference
        EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
        instance.stepInfo_.currentStepId = 0;
        instance.stepInfo_.inStepRange = false;
        instance.stepInfo_.stepMarkRangeIdList.clear();
        instance.hostPtrs_.clear();
    }
};

TEST_F(CpuTensorCollectTest, ReportCpuTensorMallocSuccess)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xABCD0000;
    uint64_t size = 1024;
    std::string stack = "test_stack";

    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, true, std::move(stack)));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);
}

TEST_F(CpuTensorCollectTest, ReportCpuTensorMallocDedup)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xABCD0001;
    uint64_t size = 2048;

    // First MALLOC succeeds
    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, true, ""));
    // Second MALLOC of same addr returns false (same-channel or cross-channel dedup)
    EXPECT_FALSE(instance.ReportCpuTensor(addr, size, true, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);
}

TEST_F(CpuTensorCollectTest, ReportCpuTensorFreeSuccess)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xABCD0002;
    uint64_t size = 4096;

    // MALLOC first
    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, true, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);
    // FREE succeeds for known addr
    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, false, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 0);
}

TEST_F(CpuTensorCollectTest, ReportCpuTensorFreeUnknownAddr)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xDEAD0000;

    // FREE of unknown addr returns false (orphan FREE prevention)
    EXPECT_FALSE(instance.ReportCpuTensor(addr, 1024, false, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 0);
}

TEST_F(CpuTensorCollectTest, ReportCpuTensorMallocFreeLifecycle)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xBEEF0000;
    uint64_t size = 8192;

    // Full lifecycle: MALLOC -> FREE -> MALLOC (new addr reuse ok after free)
    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, true, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);

    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, false, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 0);

    // Same addr can be re-reported after free
    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, true, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);
}

TEST_F(CpuTensorCollectTest, ReportCpuTensorCrossChannelDedup)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xCAFE0000;

    // Simulate HAL channel having already reported this addr (cross-channel dedup)
    instance.halHostPtrs_.insert(addr);
    instance.hostPtrs_.insert(addr);  // Other channel (HAL) already registered

    // Python channel MALLOC should return false (first-come-first-served)
    EXPECT_FALSE(instance.ReportCpuTensor(addr, 1024, true, ""));
    // hostPtrs_ should remain size 1 (no duplicate insert)
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);
}

TEST_F(CpuTensorCollectTest, ReportCpuTensorSkipWhenCollectCpuDisabled)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);

    // Disable collectCpu
    Config config = MemScope::GetConfig();
    config.collectCpu = false;
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);

    uint64_t addr = 0xFACE0000;
    // Should return false (skipped) when collectCpu is disabled
    EXPECT_FALSE(instance.ReportCpuTensor(addr, 1024, true, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 0);

    // Restore config for subsequent tests
    config.collectCpu = true;
    ConfigManager::Instance().SetConfig(config);
}

TEST_F(CpuTensorCollectTest, ReportCpuTensorEventFields)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xFEED0000;
    uint64_t size = 16384;

    // MALLOC: verify event is accepted
    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, true, "malloc_stack"));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);

    // FREE: verify event is accepted
    EXPECT_TRUE(instance.ReportCpuTensor(addr, size, false, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 0);
}

TEST_F(CpuTensorCollectTest, ReportHostRegisterDedupWithCpuTensor)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t addr = 0xCAFE1000;
    uint64_t size = 4096;
    CallStackString stack;

    // HAL channel reports host register first (takes priority)
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    config.collectCpu = true;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    ConfigManager::Instance().SetConfig(config);

    EXPECT_TRUE(instance.ReportHostRegister(addr, size, std::move(stack)));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);
    EXPECT_EQ(instance.halHostPtrs_.count(addr), 1);

    // Python channel MALLOC of same addr should be rejected
    EXPECT_FALSE(instance.ReportCpuTensor(addr, size, true, ""));
    EXPECT_EQ(instance.hostPtrs_.count(addr), 1);
}
