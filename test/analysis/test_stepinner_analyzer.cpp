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
#include "securec.h"
#define private public
#include "stepinner_analyzer.h"
#include "file.h"
#undef private
#include "bit_field.h"
#include "mstx_analyzer.h"
#include "py_step_manager.h"
#include "record_info.h"
#include "config_info.h"

using namespace MemScope;

Config stepInnerConfig;

class StepInnerAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        stepInnerConfig = Config{};
        Utility::FileCreateManager::GetInstance("testmsmemscope");
    }
 
    void TearDown() override
    {
        stepInnerConfig = Config{};
        StepInnerAnalyzer::GetInstance(stepInnerConfig).config_ = Config{};
        StepInnerAnalyzer::GetInstance(stepInnerConfig).skipSteps_ = 1;
        StepInnerAnalyzer::GetInstance(stepInnerConfig).leakMemSums_.clear();
        StepInnerAnalyzer::GetInstance(stepInnerConfig).stepInfoTables_.clear();
        StepInnerAnalyzer::GetInstance(stepInnerConfig).npuMemUsages_.clear();
        Utility::FileCreateManager::GetInstance("testmsmemscope");
    }
};

std::shared_ptr<MstxEvent> CreateMstxEvent(MarkType type, const char* message, uint64_t stepId, uint64_t rangeId,
                                           uint64_t streamId)
{
    std::shared_ptr<MstxEvent> event = std::make_shared<MstxEvent>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = static_cast<EventSubType>(
        static_cast<int32_t>(EventSubType::MSTX_MARK) + static_cast<int32_t>(type));
    event->device = 0;
    event->stepId = stepId;
    event->rangeId = rangeId;
    event->streamId = streamId;
    event->name = message;
    return event;
}

TEST(StepInnerAnalyzerTest, do_npu_free_record_expect_sucess) {
    // 先初始化注册
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    analysisConfig.stepList.stepCount = 0;
    StepInnerAnalyzer::GetInstance(analysisConfig).config_.stepList.stepCount = 0;
    static StepInnerAnalyzer analyzer(analysisConfig);
    ClientId clientId = 0;

    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::MALLOC;
    event1->eventSubType = EventSubType::PTA_CACHING;
    event1->id = 1;
    event1->device = 0;
    event1->size = 512;
    event1->addr = 12345;
    event1->used = 512;

    std::shared_ptr<MemoryEvent> event2 = std::make_shared<MemoryEvent>();
    event2->eventType = EventBaseType::FREE;
    event2->eventSubType = EventSubType::PTA_CACHING;
    event2->id = 2;
    event2->device = 0;
    event2->size = 512;
    event2->addr = 12345;
    event2->used = 0;

    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, event1));
    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, event2));
}

TEST(StepInnerAnalyzerTest, do_reveive_mstxmsg_expect_memscope_warning)
{
    ClientId clientId = 0;
    std::shared_ptr<MstxEvent> eventStart1 = std::make_shared<MstxEvent>();
    eventStart1->eventType = EventBaseType::MSTX;
    eventStart1->eventSubType = EventSubType::MSTX_RANGE_START;
    eventStart1->name = "step start";
    eventStart1->device = 0;
    eventStart1->stepId = 2;
    eventStart1->streamId = 123;

    // 经过第二个step，但仍然未释放
    std::shared_ptr<MstxEvent> eventStart2 = std::make_shared<MstxEvent>();
    eventStart2->eventType = EventBaseType::MSTX;
    eventStart2->eventSubType = EventSubType::MSTX_RANGE_START;
    eventStart2->name = "step start";
    eventStart2->device = 0;
    eventStart2->stepId = 3;
    eventStart2->streamId = 123;

    std::shared_ptr<MstxEvent> eventEnd = std::make_shared<MstxEvent>();
    eventEnd->eventType = EventBaseType::MSTX;
    eventEnd->eventSubType = EventSubType::MSTX_RANGE_END;
    eventEnd->device = 0;
    eventEnd->stepId = 3;
    eventEnd->streamId = 123;

    // 经过两个step的内存
    std::shared_ptr<MemoryEvent> eventMem = std::make_shared<MemoryEvent>();
    eventMem->eventType = EventBaseType::MALLOC;
    eventMem->eventSubType = EventSubType::PTA_CACHING;
    eventMem->id = 1;
    eventMem->device = 0;
    eventMem->size = 512;
    eventMem->addr = 12345;
    eventMem->used = 512;

    // 先初始化注册
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    analysisConfig.stepList.stepCount = 0;
    StepInnerAnalyzer::GetInstance(analysisConfig).config_.stepList.stepCount = 0;
    static StepInnerAnalyzer analyzer(analysisConfig);
    // 重置StepInnerAnalyzer的step信息接收方式
    StepInnerAnalyzer::GetInstance(analysisConfig).crtStepSource_.store(
        StepSource::None, std::memory_order_release);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(eventStart1);
    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, eventMem));
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(eventStart2);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(eventEnd);
}

TEST(StepInnerAnalyzerTest, do_reveive_stepmsg_expect_memscope_warning)
{
    ClientId clientId = 0;
    std::shared_ptr<SystemEvent> eventStep1 = std::make_shared<SystemEvent>();
    eventStep1->device = 0;
    eventStep1->name = "2";

    // 经过第二个step，但仍然未释放
    std::shared_ptr<SystemEvent> eventStep2 = std::make_shared<SystemEvent>();
    eventStep2->device = 0;
    eventStep2->name = "3";

    // 经过两个step的内存
    std::shared_ptr<MemoryEvent> eventMem = std::make_shared<MemoryEvent>();
    eventMem->eventType = EventBaseType::MALLOC;
    eventMem->eventSubType = EventSubType::PTA_CACHING;
    eventMem->id = 1;
    eventMem->device = 0;
    eventMem->size = 512;
    eventMem->addr = 12345;
    eventMem->used = 512;

    // 先初始化注册
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    analysisConfig.stepList.stepCount = 0;
    // 重置StepInnerAnalyzer的step信息接收方式
    StepInnerAnalyzer::GetInstance(analysisConfig).crtStepSource_.store(
        StepSource::None, std::memory_order_release);
    StepInnerAnalyzer::GetInstance(analysisConfig).config_.stepList.stepCount = 0;
    static StepInnerAnalyzer analyzer(analysisConfig);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveStepMsg(eventStep1);
    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, eventMem));
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveStepMsg(eventStep2);
}

TEST(StepInnerAnalyzerTest, do_npu_malloc_record_expect_sucess) {
    // 先初始化注册
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    analysisConfig.stepList.stepCount = 0;
    StepInnerAnalyzer::GetInstance(analysisConfig).config_.stepList.stepCount = 0;
    static StepInnerAnalyzer analyzer(analysisConfig);
    ClientId clientId = 0;

    std::shared_ptr<MemoryEvent> eventMem = std::make_shared<MemoryEvent>();
    eventMem->eventType = EventBaseType::MALLOC;
    eventMem->eventSubType = EventSubType::PTA_CACHING;
    eventMem->id = 1;
    eventMem->device = 0;
    eventMem->size = 512;
    eventMem->addr = 12345;
    eventMem->used = 512;
    eventMem->total = 512;

    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, eventMem));
}

TEST(StepInnerAnalyzerTest, do_npu_malloc_record_expect_double_malloc) {
    // 先初始化注册
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    analysisConfig.stepList.stepCount = 0;
    StepInnerAnalyzer::GetInstance(analysisConfig).config_.stepList.stepCount = 0;
    static StepInnerAnalyzer analyzer(analysisConfig);
    ClientId clientId = 0;

    std::shared_ptr<MemoryEvent> eventMem = std::make_shared<MemoryEvent>();
    eventMem->eventType = EventBaseType::MALLOC;
    eventMem->eventSubType = EventSubType::PTA_CACHING;
    eventMem->id = 1;
    eventMem->device = 0;
    eventMem->size = 512;
    eventMem->addr = 12345;

    // 地址重复的申请
    std::shared_ptr<MemoryEvent> eventMem2 = std::make_shared<MemoryEvent>();
    eventMem2->eventType = EventBaseType::MALLOC;
    eventMem2->eventSubType = EventSubType::PTA_CACHING;
    eventMem2->id = 1;
    eventMem2->device = 0;
    eventMem2->size = 512;
    eventMem2->addr = 12345;

    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, eventMem));
    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, eventMem2));
}


TEST(StepInnerAnalyzerTest, do_npu_free_record_expect_free_error) {
    // 先初始化注册
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    analysisConfig.stepList.stepCount = 0;
    StepInnerAnalyzer::GetInstance(analysisConfig).config_.stepList.stepCount = 0;
    static StepInnerAnalyzer analyzer(analysisConfig);
    ClientId clientId = 0;

    std::shared_ptr<MemoryEvent> eventMem = std::make_shared<MemoryEvent>();
    eventMem->eventType = EventBaseType::FREE;
    eventMem->eventSubType = EventSubType::PTA_CACHING;
    eventMem->id = 1;
    eventMem->device = 0;
    eventMem->size = 512;
    eventMem->addr = 12345;
    eventMem->used = 0;
    eventMem->total = 1024;

    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, eventMem));
}

TEST(StepInnerAnalyzerTest, do_reveive_mstxmsg_expect_torch_memscope) {
    ClientId clientId = 0;
    MemScope::DeviceId deviceId = 0;
    // 先初始化注册
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    analysisConfig.stepList.stepCount = 0;
    StepInnerAnalyzer::GetInstance(analysisConfig).config_.stepList.stepCount = 0;
    static StepInnerAnalyzer analyzer(analysisConfig);

    // 第一个step会跳过
    auto mstxRecordStartFirstBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 1, 1, 123);
    auto mstxRecordEndFirstBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 1, 1, 123);

    // 第二个step发生泄漏
    auto mstxRecordStartSecondBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 2, 2, 123);
    auto mstxRecordEndSecondBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 2, 2, 123);

    // 构造第三个step时仍未释放的情景
    auto mstxRecordStartThirdBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 3, 3, 123);
    auto mstxRecordEndThirdBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 3, 3, 123);

    // 构造第四个step时仍未释放的情景，超过duration限制
    auto mstxRecordStartFourthBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 4, 4, 123);
    auto mstxRecordEndFourthBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 4, 4, 123);

    // step前后allocated内存不一致
    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::PTA_CACHING;
    event->id = 1;
    event->device = 0;
    event->size = 512;
    event->addr = 92345;
    event->used = 512;

    // 重置StepInnerAnalyzer的step信息接收方式
    StepInnerAnalyzer::GetInstance(analysisConfig).crtStepSource_.store(
        StepSource::None, std::memory_order_release);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartFirstBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndFirstBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartSecondBuffer);
    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, event));
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndSecondBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartThirdBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndThirdBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartFourthBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndFourthBuffer);
}


TEST(StepInnerAnalyzerTest, do_reveive_mstxmsg_expect_mindspore_memscope) {
    ClientId clientId = 0;
    MemScope::DeviceId deviceId = 0;
    // 先初始化注册
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    analysisConfig.stepList.stepCount = 0;
    StepInnerAnalyzer::GetInstance(analysisConfig).config_.stepList.stepCount = 0;
    static StepInnerAnalyzer analyzer(analysisConfig);

    auto mstxRecordStartFirstBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 1, 1, 123);
    auto mstxRecordEndFirstBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 1, 1, 123);
    auto mstxRecordStartSecondBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 2, 2, 123);
    auto mstxRecordEndSecondBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 2, 2, 123);
    auto mstxRecordStartThirdBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 3, 3, 123);
    auto mstxRecordEndThirdBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 3, 3, 123);
    auto mstxRecordStartFourthBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 4, 4, 123);
    auto mstxRecordEndFourthBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 4, 4, 123);

    // step前后allocated内存不一致
    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::MINDSPORE;
    event->id = 1;
    event->device = 0;
    event->size = 512;
    event->addr = 92345;
    event->used = 512;

    // 重置StepInnerAnalyzer的step信息接收方式
    StepInnerAnalyzer::GetInstance(analysisConfig).crtStepSource_.store(
        StepSource::None, std::memory_order_release);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartFirstBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndFirstBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartSecondBuffer);
    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, event));
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndSecondBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartThirdBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndThirdBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartFourthBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndFourthBuffer);
}

TEST(StepInnerAnalyzerTest, do_reveive_mstxmsg_expect_atb_memscope) {
    ClientId clientId = 0;
    MemScope::DeviceId deviceId = 0;
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    analysisConfig.stepList.stepCount = 0;
    StepInnerAnalyzer::GetInstance(analysisConfig).config_.stepList.stepCount = 0;
    static StepInnerAnalyzer analyzer(analysisConfig);

    auto mstxRecordStartFirstBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 1, 1, 123);
    auto mstxRecordEndFirstBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 1, 1, 123);
    auto mstxRecordStartSecondBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 2, 2, 123);
    auto mstxRecordEndSecondBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 2, 2, 123);
    auto mstxRecordStartThirdBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 3, 3, 123);
    auto mstxRecordEndThirdBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 3, 3, 123);
    auto mstxRecordStartFourthBuffer = CreateMstxEvent(MarkType::RANGE_START_A, "step start", 4, 4, 123);
    auto mstxRecordEndFourthBuffer = CreateMstxEvent(MarkType::RANGE_END, "", 4, 4, 123);

    // step前后allocated内存不一致
    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::ATB;
    event->id = 1;
    event->device = 0;
    event->size = 512;
    event->addr = 92345;
    event->used = 512;

    // 重置StepInnerAnalyzer的step信息接收方式
    StepInnerAnalyzer::GetInstance(analysisConfig).crtStepSource_.store(
        StepSource::None, std::memory_order_release);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartFirstBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndFirstBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartSecondBuffer);
    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(analysisConfig).Record(clientId, event));
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndSecondBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartThirdBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndThirdBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordStartFourthBuffer);
    StepInnerAnalyzer::GetInstance(analysisConfig).ReceiveMstxMsg(mstxRecordEndFourthBuffer);
}

TEST(StepInnerAnalyzerTest, do_input_exist_deviceid_CreateTables_return_true)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage{};
    stepInner.npuMemUsages_.insert({1, npumemusage});
    auto ret = stepInner.CreateTables(1);
    ASSERT_TRUE(ret);
}

TEST(StepInnerAnalyzerTest, do_input_not_exist_deviceid_CreateTables_return_true)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    auto ret = stepInner.CreateTables(1);
    ASSERT_TRUE(ret);
}

TEST(StepInnerAnalyzerTest, do_input_exist_deviceid_CreateStepInfoTables_return_true)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    StepInfoTable stepInfoTable{};
    stepInner.stepInfoTables_.insert({1, stepInfoTable});
    auto ret = stepInner.CreateStepInfoTables(1);
    ASSERT_TRUE(ret);
}

TEST(StepInnerAnalyzerTest, do_input_not_exist_deviceid_CreateStepInfoTables_return_true)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    auto ret = stepInner.CreateStepInfoTables(1);
    ASSERT_TRUE(ret);
}

TEST(StepInnerAnalyzerTest, do_input_exist_deviceid_CreateLeakSumTables_return_true)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    LeakSumsTable memscopeumstable{};
    stepInner.leakMemSums_.insert({1, memscopeumstable});
    auto ret = stepInner.CreateLeakSumTables(1);
    ASSERT_TRUE(ret);
}

TEST(StepInnerAnalyzerTest, do_input_not_exist_deviceid_CreateLeakSumTables_return_true)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    auto ret = stepInner.CreateLeakSumTables(1);
    ASSERT_TRUE(ret);
}

TEST(StepInnerAnalyzerTest, do_input_steps_command_disable_analysis)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 2;
    StepInnerAnalyzer stepInner{config};
    auto ret = stepInner.IsStepInnerAnalysisEnable();
    ASSERT_FALSE(ret);
}

TEST(StepInnerAnalyzerTest, do_not_input_steps_command_enable_analysis)
{
    Config config;
    config.stepList.stepCount = 0;
    BitField<decltype(config.eventType)> eventBit;
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.analysisType = analysisBit.getValue();
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    auto ret = stepInner.IsStepInnerAnalysisEnable();
    ASSERT_TRUE(ret);
}

TEST(StepInnerAnalyzerTest, do_stepId_below_1_SkipCheck_return_true)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    NpuMemInfo npuMemInfo{};
    npuMemInfo.stepId = 0;
    auto ret = stepInner.SkipCheck(npuMemInfo);
    ASSERT_TRUE(ret);
}

TEST(StepInnerAnalyzerTest, do_stepId_up_1_SkipCheck_return_true)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    NpuMemInfo npuMemInfo{};
    npuMemInfo.stepId = 3;
    auto ret = stepInner.SkipCheck(npuMemInfo);
    ASSERT_FALSE(ret);
}

TEST(StepInnerAnalyzerTest, do_updateallocated_step_0_update_0)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 2;
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    npumemusage.duringStep = 0;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated = 0;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated = 0;
    stepInner.npuMemUsages_.insert({0, npumemusage});
    stepInner.UpdateAllocated(0, PoolType::PTA_CACHING, 100);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated, 0);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated, 0);
}

TEST(StepInnerAnalyzerTest, do_updateallocated_step_2_update_allocated)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 2;
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    npumemusage.duringStep = 2;

    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated = 20;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated = 20;
    stepInner.npuMemUsages_.insert({0, npumemusage});
    stepInner.UpdateAllocated(0, PoolType::PTA_CACHING, 100);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated, 20);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated, 20);
}

TEST(StepInnerAnalyzerTest, do_checkgap_minmaxallocratio_equal_0_expect_reset_allocated)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 2;
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    npumemusage.duringStep = 2;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated = 100;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated = 20;
    stepInner.npuMemUsages_.insert({0, npumemusage});
    stepInner.CheckGap(0);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated, 0);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated, 0);
}

TEST(StepInnerAnalyzerTest, do_checkgap_minmaxallocratio_expect_true_allocated)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 2;
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    GapInfo gapinfo;
    gapinfo.minMaxAllocRatio = 0.1;
    npumemusage.duringStep = 2;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated = 100;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated = 20;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].maxGapInfo = gapinfo;
    stepInner.npuMemUsages_.insert({0, npumemusage});
    stepInner.CheckGap(0);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated, 0);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated, 0);
}

TEST(StepInnerAnalyzerRecordFuncTest, Recordtest)
{
    MemScope::Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.enableCompare = false;
    config.inputCorrectPaths = true;
    config.outputCorrectPaths = false;
    config.stepList.stepCount = 0;
    ClientId clientId = 1;

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    EXPECT_TRUE(MemScope::StepInnerAnalyzer::GetInstance(config).Record(clientId, event));
}

TEST(StepInnerAnalyzerRecordFuncTest, recordMallocSuccess) {
    // 先初始化注册
    MemScope::Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.enableCompare = false;
    config.inputCorrectPaths = true;
    config.outputCorrectPaths = false;
    config.stepList.stepCount = 0;
    ClientId clientId = 1;

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::PTA_CACHING;
    event->id = 1;
    event->device = 0;
    event->size = 512;
    event->addr = 12345;
    event->used = 512;

    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(config).Record(clientId, event));
}

TEST(StepInnerAnalyzerRecordFuncTest, recordFreeSuccess) {
    // 先初始化注册
    MemScope::Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.enableCompare = false;
    config.inputCorrectPaths = true;
    config.outputCorrectPaths = false;
    config.stepList.stepCount = 0;
    ClientId clientId = 1;

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = EventSubType::PTA_CACHING;
    event->id = 1;
    event->device = 0;
    event->size = 512;
    event->addr = 12345;
    event->used = 512;

    EXPECT_TRUE(StepInnerAnalyzer::GetInstance(config).Record(clientId, event));
}

TEST(StepInnerAnalyzerReceiveMstxMsgFuncTest, ReceiveMstxMsgIfRangeStartA) {
    // 先初始化注册
    MemScope::Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.enableCompare = false;
    config.inputCorrectPaths = true;
    config.outputCorrectPaths = false;
    config.stepList.stepCount = 0;
    ClientId clientId = 1;

    std::shared_ptr<MstxEvent> event = std::make_shared<MstxEvent>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = EventSubType::MSTX_RANGE_START;
    event->device = 0;
    event->stepId = 1;
    event->streamId = 123;
    event->name = "step start";

    // 重置StepInnerAnalyzer的step信息接收方式
    StepInnerAnalyzer::GetInstance(config).crtStepSource_.store(
        StepSource::None, std::memory_order_release);
    StepInnerAnalyzer::GetInstance(config).ReceiveMstxMsg(event);
}

TEST(StepInnerAnalyzerReceiveMstxMsgFuncTest, ReceiveMstxMsgIfRangeEnd) {
    // 先初始化注册
    MemScope::Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.enableCompare = false;
    config.inputCorrectPaths = true;
    config.outputCorrectPaths = false;
    config.stepList.stepCount = 0;
    ClientId clientId = 1;

    std::shared_ptr<MstxEvent> event = std::make_shared<MstxEvent>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = EventSubType::MSTX_RANGE_START;
    event->device = 0;
    event->stepId = 1;
    event->streamId = 123;
    event->name = "step end";

    // 重置StepInnerAnalyzer的step信息接收方式
    StepInnerAnalyzer::GetInstance(config).crtStepSource_.store(
        StepSource::None, std::memory_order_release);
    StepInnerAnalyzer::GetInstance(config).ReceiveMstxMsg(event);
}

TEST(StepInnerAnalyzerUpdateAllocatedFuncTest, UpdateAllocatedUpdateMaxTest)
{
    Config config;
    config.stepList.stepCount = 0;
    BitField<decltype(config.eventType)> eventBit;
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.analysisType = analysisBit.getValue();
    config.eventType = eventBit.getValue();
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    npumemusage.duringStep = 2;

    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated = 20;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated = 20;
    stepInner.npuMemUsages_.insert({0, npumemusage});
    stepInner.UpdateAllocated(0, PoolType::PTA_CACHING, 100);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated, 100);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated, 20);
}

TEST(StepInnerAnalyzerUpdateAllocatedFuncTest, UpdateAllocatedInitTest)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.analysisType = analysisBit.getValue();
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 0;
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    npumemusage.duringStep = 2;

    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated = 0;
    npumemusage.poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated = 0;
    stepInner.npuMemUsages_.insert({0, npumemusage});
    stepInner.UpdateAllocated(0, PoolType::PTA_CACHING, 100);
    stepInner.UpdateAllocated(0, PoolType::PTA_CACHING, 200);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMaxAllocated, 200);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::PTA_CACHING].stepMinAllocated, 100);
}

TEST(StepInnerAnalyzerUpdateAllocatedFuncTest, UpdateAllocatedreturnTest)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 0;
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    npumemusage.duringStep = 0;

    npumemusage.poolStatusTable[PoolType::MINDSPORE].stepMaxAllocated = 0;
    npumemusage.poolStatusTable[PoolType::MINDSPORE].stepMinAllocated = 0;
    stepInner.npuMemUsages_.insert({0, npumemusage});
    stepInner.UpdateAllocated(0, PoolType::MINDSPORE, 100);
    stepInner.UpdateAllocated(0, PoolType::MINDSPORE, 200);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::MINDSPORE].stepMaxAllocated, 0);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolStatusTable[PoolType::MINDSPORE].stepMinAllocated, 0);
}

TEST(StepInnerAnalyzerAddDurationTest, AddDurationTest)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 0;
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    npumemusage.duringStep = 0;

    MemScope::NpuMemInfo memInfo;
    memInfo.duration = 1;
    npumemusage.poolOpTable.insert({MemScope::NpuMemKey(0, PoolType::PTA_CACHING), memInfo});
    stepInner.npuMemUsages_.insert({0, npumemusage});
    stepInner.AddDuration(0);
    ASSERT_EQ(stepInner.npuMemUsages_[0].poolOpTable[MemScope::NpuMemKey(0, PoolType::PTA_CACHING)].duration, 2);
}

TEST(StepInnerAnalyzerAddDurationTest, AddDurationReturnTest)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 0;
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    npumemusage.duringStep = 0;

    MemScope::NpuMemInfo memInfo;
    memInfo.duration = 1;
    npumemusage.poolOpTable.insert({MemScope::NpuMemKey(0, PoolType::PTA_CACHING), memInfo});
    stepInner.npuMemUsages_.insert({1, npumemusage});
    stepInner.AddDuration(0); // 不存在的deviceID，提前返回
    ASSERT_EQ(stepInner.npuMemUsages_[1].poolOpTable[MemScope::NpuMemKey(0, PoolType::PTA_CACHING)].duration, 1);
}

TEST(StepInnerAnalyzerSetStepIdFuncTest, SetStepIdTest)
{
    Config config;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.stepList.stepCount = 0;
    StepInnerAnalyzer stepInner{config};
    NpuMemUsage npumemusage;
    npumemusage.duringStep = 1;

    stepInner.npuMemUsages_.insert({1, npumemusage});
    stepInner.SetStepId(1, 2);
    ASSERT_EQ(stepInner.npuMemUsages_[1].duringStep, 2);
}