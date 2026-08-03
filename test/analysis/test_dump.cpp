// Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

#include <gtest/gtest.h>

#define private public
#include "dump.h"
#undef private

#include <string>
#include "bit_field.h"
#include "event_dispatcher.h"
#include "event_trace/trace_manager/event_trace_manager.h"

namespace MemScope {
class DumpTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        dumper = &Dump::GetInstance();
    }
    Dump* dumper;
};

TEST_F(DumpTest, GetInstance_Singleton)
{
    // 判断是否返回同一个实例（只会初始化一次）,后续调用直接返回该实例的引用
    Dump* instance1 = &Dump::GetInstance();
    Dump* instance2 = &Dump::GetInstance();
    EXPECT_EQ(instance1, instance2);
}

TEST_F(DumpTest, EventHandle_Interface)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    dumper->EventHandle(eventBase, state);
}

TEST_F(DumpTest, WritePublicEventToFile_Interface)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    // 测试接口
    dumper->EventHandle(eventBase, state);
    dumper->WritePublicEventToFile();
}

TEST_F(DumpTest, FflushEventToFile_Interface)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    // 测试接口
    dumper->EventHandle(eventBase, state);
    dumper->FflushEventToFile();
}

// ==================== RFC: Precise Event Filtering Tests ====================

TEST_F(DumpTest, ShouldDumpEvent_only_launch_enabled)
{
    // 通过全局配置设置 dumpEventType = LAUNCH_EVENT (bit 2)
    Config config = Config{};
    BitField<decltype(config.dumpEventType)> dumpBit;
    dumpBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.dumpEventType = dumpBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    const Config& globalConfig = GetConfig();

    // MALLOC / FREE 不应落盘
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::MALLOC, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::FREE, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::ACCESS, globalConfig));

    // OP_LAUNCH / KERNEL_LAUNCH 应落盘（映射到 LAUNCH_EVENT）
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::OP_LAUNCH, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::KERNEL_LAUNCH, globalConfig));

    // 不可控事件始终落盘
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MSTX, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::SYSTEM, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::SNAPSHOT, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::CLEAN_UP, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MEMORY_OWNER, globalConfig));
}

TEST_F(DumpTest, ShouldDumpEvent_all_disabled)
{
    // 通过全局配置设置 dumpEventType = 0，不落盘任何用户可控事件
    Config config = Config{};
    config.dumpEventType = 0;
    ConfigManager::Instance().SetConfig(config);
    const Config& globalConfig = GetConfig();

    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::MALLOC, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::FREE, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::OP_LAUNCH, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::KERNEL_LAUNCH, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::ACCESS, globalConfig));

    // 不可控事件仍然落盘
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MSTX, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::SYSTEM, globalConfig));
}

TEST_F(DumpTest, ShouldDumpEvent_all_enabled)
{
    // 通过全局配置设置 dumpEventType = 所有用户可控事件
    Config config = Config{};
    BitField<decltype(config.dumpEventType)> dumpBit;
    dumpBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    dumpBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    dumpBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    dumpBit.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    config.dumpEventType = dumpBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    const Config& globalConfig = GetConfig();

    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MALLOC, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::FREE, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::OP_LAUNCH, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::KERNEL_LAUNCH, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::ACCESS, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MSTX, globalConfig));
}

}