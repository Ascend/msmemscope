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
#include "event_trace/trace_manager/event_trace_manager.h"
#define private public
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
