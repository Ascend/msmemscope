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
#include "inefficient_analyzer.h"
#include <string>
#include "event_dispatcher.h"

// 假设以下类和结构体已定义
namespace MemScope {
// 测试用例
class InefficientAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        analyzer = &InefficientAnalyzer::GetInstance();
    }

    InefficientAnalyzer* analyzer;
};

TEST_F(InefficientAnalyzerTest, TestOPLaunchStartEnd)
{
    uint64_t pid = 1234;

    // ATB_START
    auto eventStart = std::make_shared<EventBase>();
    eventStart->eventType = EventBaseType::OP_LAUNCH;
    eventStart->eventSubType = EventSubType::ATB_START;
    eventStart->pid = pid;
    MemoryState state;
    analyzer->EventHandle(eventStart, &state);

    // ATB_END
    auto eventEnd = std::make_shared<EventBase>();
    eventEnd->eventType = EventBaseType::OP_LAUNCH;
    eventEnd->eventSubType = EventSubType::ATB_END;
    eventEnd->pid = pid;
    analyzer->EventHandle(eventEnd, &state);
}

TEST_F(InefficientAnalyzerTest, TestMALLOCEventHandling)
{
    uint64_t pid = 1234;

    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::ATB;
    event->pid = pid;
    MemoryState state;
    analyzer->EventHandle(event, &state);
}

TEST_F(InefficientAnalyzerTest, TestACCESSEventTemporaryIdleness)
{
    uint64_t pid = 1234;

    auto mallocEvent = std::make_shared<EventBase>();
    mallocEvent->eventType = EventBaseType::MALLOC;
    mallocEvent->eventSubType = EventSubType::ATB;
    mallocEvent->pid = pid;
    auto event1 = std::make_shared<EventBase>();
    event1->eventType = EventBaseType::ACCESS;
    event1->eventSubType = EventSubType::ATB;
    event1->pid = pid;
    auto event2 = std::make_shared<EventBase>();
    event2->eventType = EventBaseType::ACCESS;
    event2->eventSubType = EventSubType::ATB;
    event2->pid = pid;
    MemoryState state;
    auto memMallocEvent = std::dynamic_pointer_cast<MemoryEvent>(mallocEvent);
    auto memEvent1 = std::dynamic_pointer_cast<MemoryEvent>(event1);
    auto memEvent2 = std::dynamic_pointer_cast<MemoryEvent>(event2);
    state.events.push_back(memMallocEvent);
    state.events.push_back(memEvent1);
    state.events.push_back(memEvent2);
    state.apiId.push_back(0);
    state.apiId.push_back(1);
    state.apiId.push_back(200);

    analyzer->EventHandle(event2, &state);
}

// 影子事件应被跳过
TEST_F(InefficientAnalyzerTest, TestShadowEventIsSkipped)
{
    auto shadowEvent = std::make_shared<MemoryEvent>();
    shadowEvent->eventType = EventBaseType::MALLOC;
    shadowEvent->poolType = PoolType::PTA_CACHING;
    shadowEvent->eventSubType = EventSubType::PTA_CACHING;
    shadowEvent->pid = 12345;
    shadowEvent->addr = 0x9999;
    shadowEvent->size = 512;
    shadowEvent->isShadowEvent = true;

    std::shared_ptr<EventBase> eventBase = shadowEvent;
    MemoryState state;

    // Should return immediately without modifying state or crashing
    EXPECT_NO_THROW(analyzer->EventHandle(eventBase, &state));
    // State should remain unmodified
    EXPECT_TRUE(state.events.empty());
}

} // namespace