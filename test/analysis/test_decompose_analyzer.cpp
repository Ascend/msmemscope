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
#include "decompose_analyzer.h"
#include <string>
#include "event_dispatcher.h"

namespace MemScope {

class DecomposeAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        analyzer = &DecomposeAnalyzer::GetInstance();
    }

    DecomposeAnalyzer* analyzer;
};

TEST_F(DecomposeAnalyzerTest, TestInitOwner_CANN)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;
    memoryEvent->describeOwner = "hal";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "CANN@UNKNOWN");
    EXPECT_EQ(state->userDefinedOwner, "hal");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_CANN_HCCL)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;
    memoryEvent->describeOwner = "hal";
    memoryEvent->moduleId = 3;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "CANN@HCCL");
    EXPECT_EQ(state->userDefinedOwner, "hal");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_PTA_CACHING)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::PTA_CACHING;
    memoryEvent->describeOwner = "pta_caching";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "PTA");
    EXPECT_EQ(state->userDefinedOwner, "pta_caching");
    delete state;
}


TEST_F(DecomposeAnalyzerTest, TestInitOwner_PTA_WORKSPACE)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::PTA_WORKSPACE;
    memoryEvent->describeOwner = "pta_workspace";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "PTA_WORKSPACE");
    EXPECT_EQ(state->userDefinedOwner, "pta_workspace");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_MINDSPORE)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::MINDSPORE;
    memoryEvent->describeOwner = "mindspore";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "MINDSPORE");
    EXPECT_EQ(state->userDefinedOwner, "mindspore");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_ATB)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::ATB;
    memoryEvent->describeOwner = "atb";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "ATB");
    EXPECT_EQ(state->userDefinedOwner, "atb");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_UnknownSubType)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType =  static_cast<EventBaseType>(999);  // 无效值
    memoryEvent->eventSubType = static_cast<EventSubType>(999);
    memoryEvent->describeOwner = "unknown";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "");
    EXPECT_EQ(state->userDefinedOwner, "");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwnerByAtenAccess_OpsAten_UnknownSubType)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::ACCESS;
    memoryEvent->eventSubType = static_cast<EventSubType>(999);
    memoryEvent->describeOwner = "aten_owner";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();
    state->memscopeDefinedOwner = "PTA";

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "PTA");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwnerByAtenAccess_UnknownDefinedOwner)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::ACCESS;
    memoryEvent->eventSubType = EventSubType::ATEN_READ;
    memoryEvent->describeOwner = "aten_owner";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwnerByAtenAccess_OpsAten)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::ACCESS;
    memoryEvent->eventSubType = EventSubType::ATEN_READ;
    memoryEvent->describeOwner = "aten_owner";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();
    state->memscopeDefinedOwner = "PTA";

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "PTA@ops@aten");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwner_DescribeOwner)
{
    auto memoryEvent = std::make_shared<MemoryOwnerEvent>();
    memoryEvent->eventType = EventBaseType::MEMORY_OWNER;
    memoryEvent->eventSubType = EventSubType::DESCRIBE_OWNER;
    memoryEvent->owner = "@user_defined";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->userDefinedOwner, "@user_defined");

    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwner_DescribeOwner_TorchOptimizer)
{
    auto memoryEvent = std::make_shared<MemoryOwnerEvent>();
    memoryEvent->eventType = EventBaseType::MEMORY_OWNER;
    memoryEvent->eventSubType = EventSubType::TORCH_OPTIMIZER_STEP_OWNER;
    memoryEvent->owner = "@gradient";

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();
    state->memscopeDefinedOwner = "PTA";

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->memscopeDefinedOwner, "PTA@gradient");

    delete state;
}

// 影子事件应被跳过，不修改state
TEST_F(DecomposeAnalyzerTest, TestShadowEventIsSkipped)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::PTA_CACHING;
    memoryEvent->describeOwner = "should_be_ignored";
    memoryEvent->isShadowEvent = true;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    // 影子事件被跳过，memscopeDefinedOwner不应被设置
    EXPECT_TRUE(state->memscopeDefinedOwner.empty());
    EXPECT_TRUE(state->userDefinedOwner.empty());

    delete state;
}

} // namespace MemScope
