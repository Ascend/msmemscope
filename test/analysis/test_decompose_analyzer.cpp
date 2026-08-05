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
#include <string>
#include "decompose_analyzer.h"
#include "describe_trace.h"
#include "event_dispatcher.h"
#include "memory_state_manager.h"

namespace MemScope {

// 清空当前线程 DescribeTrace 标签状态(线程局部单例跨用例残留隔离):
// 系统标签栈可能同标签计数嵌套, 循环撤销至该级别为空
static void ClearDescribeState()
{
    DescribeTrace& trace = DescribeTrace::GetInstance();
    std::vector<std::string> labels = trace.GetDescribe();
    for (uint8_t level = 0; level < static_cast<uint8_t>(OwnerLevel::OWNER_LEVEL_NUM); ++level)
    {
        if (labels[level].empty())
        {
            continue;
        }
        if (level <= static_cast<uint8_t>(OwnerLevel::DETAIL_2))
        {
            while (!trace.GetDescribe()[level].empty())
            {
                trace.EraseDescribe(static_cast<OwnerLevel>(level), trace.GetDescribe()[level]);
            }
        }
        else
        {
            trace.EraseUserDescribe(labels[level]);
        }
    }
}

class DecomposeAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        analyzer = &DecomposeAnalyzer::GetInstance();
        ClearDescribeState();
    }

    void TearDown() override { ClearDescribeState(); }

    DecomposeAnalyzer* analyzer;
};

TEST_F(DecomposeAnalyzerTest, TestInitOwner_CANN)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "CANN@UNKNOWN");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_CANN_HCCL)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;
    memoryEvent->moduleId = 3;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "CANN@HCCL");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_PTA_CACHING)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::PTA_CACHING;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "PTA");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_PTA_WORKSPACE)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::PTA_WORKSPACE;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "PTA_WORKSPACE");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_MINDSPORE)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::MINDSPORE;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "MINDSPORE");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_ATB)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::ATB;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "ATB");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_UnknownSubType)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = static_cast<EventBaseType>(999);  // 无效值
    memoryEvent->eventSubType = static_cast<EventSubType>(999);

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_TRUE(state->owner.GetOwnerStr().empty());
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestInitOwner_WithDescribeLabels)
{
    // 事件不再携带 owner: InitOwner 分析时直接从 DescribeTrace 读取当前线程分级标签
    DescribeTrace& trace = DescribeTrace::GetInstance();
    trace.AddDescribe(OwnerLevel::COMPONENT, "ut_fsdp2");
    trace.AddDescribe(OwnerLevel::PROCESS, "ut_activation");

    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::PTA_CACHING;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "PTA@ut_fsdp2@ut_activation");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwnerByAtenAccess_UnknownSubType)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::ACCESS;
    memoryEvent->eventSubType = static_cast<EventSubType>(999);

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_TRUE(state->owner.GetOwnerStr().empty());
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwnerByAtenAccess_WeakMark)
{
    // ATEN 访问为弱标记: 细化分类2(DETAIL_2)为空时写入
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::ACCESS;
    memoryEvent->eventSubType = EventSubType::ATEN_READ;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "ops");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwnerByAtenAccess_NotOverwrite)
{
    // ATEN 访问不覆盖已有细化标签(DETAIL_2 非空时跳过)
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::ACCESS;
    memoryEvent->eventSubType = EventSubType::ATEN_READ;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();
    state->owner.AddLabel(OwnerLevel::DETAIL_2, "ut_existing");

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "ut_existing");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwner_DescribeOwner)
{
    // 地址直标: 携带分级标签列表, 逐级更新块 owner
    auto memoryEvent = std::make_shared<MemoryOwnerEvent>();
    memoryEvent->eventType = EventBaseType::MEMORY_OWNER;
    memoryEvent->eventSubType = EventSubType::DESCRIBE_OWNER;
    memoryEvent->ownerLabels = {{OwnerLevel::COMPONENT, "user_defined"}};

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "user_defined");
    delete state;
}

TEST_F(DecomposeAnalyzerTest, TestUpdateOwner_AddressLabelOverwrites)
{
    // 地址直标: 同级别重复时以地址标签为准(AddLabel 覆盖语义)
    auto memoryEvent = std::make_shared<MemoryOwnerEvent>();
    memoryEvent->eventType = EventBaseType::MEMORY_OWNER;
    memoryEvent->eventSubType = EventSubType::DESCRIBE_OWNER;
    memoryEvent->ownerLabels = {{OwnerLevel::DETAIL_1, "gradient"}};

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();
    state->owner.AddLabel(OwnerLevel::FRAMEWORK, "PTA");
    state->owner.AddLabel(OwnerLevel::DETAIL_1, "ut_range_mark");

    analyzer->EventHandle(eventBase, state);

    EXPECT_EQ(state->owner.GetOwnerStr(), "PTA@gradient");
    delete state;
}

// 影子事件应被跳过，不修改state
TEST_F(DecomposeAnalyzerTest, TestShadowEventIsSkipped)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::PTA_CACHING;
    memoryEvent->isShadowEvent = true;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    analyzer->EventHandle(eventBase, state);

    // 影子事件被跳过, owner 不应被设置
    EXPECT_TRUE(state->owner.GetOwnerStr().empty());
    delete state;
}

} // namespace MemScope
