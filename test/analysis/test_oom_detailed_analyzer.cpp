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

#include "event.h"
#include "memory_state_manager.h"
#include "oom_detailed_analyzer.h"
#include "record_info.h"

using namespace MemScope;

namespace
{
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
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
    }

    void TearDown() override
    {
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
    }
};

TEST_F(OOMDetailedAnalyzerTest, query_merges_npu_and_hal_records_sorted)
{
    // GetInstance为带参单例（static仅初始化一次），全测试共用同一oomTopK
    Config config{};
    config.oomTopK = 10;
    OOMDetailedAnalyzer& analyzer = OOMDetailedAnalyzer::GetInstance(config);

    // NPU块（device=0, pid=1）
    auto npuEvent = CreateMalloc(PoolType::PTA_CACHING, 0x1000, 0, 1, 1, 1000, 2048);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(npuEvent), nullptr);
    // HAL块（pid=1）
    auto halEvent = CreateMalloc(PoolType::HAL, 0x2000, 0, 1, 2, 2000, 4096);
    ASSERT_NE(MemoryStateManager::GetInstance().AddEvent(halEvent), nullptr);
    // 其他pid的块不参与
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
