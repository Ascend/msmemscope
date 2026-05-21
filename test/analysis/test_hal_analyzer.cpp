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
#include "event.h"
#include "hal_analyzer.h"
#include "record_info.h"
#include "config_info.h"
#include "bit_field.h"

using namespace MemScope;

TEST(HalAnalyzerTest, do_hal_record_except_memscope) {
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    ClientId clientId = 0;
    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::MALLOC;
    event1->flag = 2377900603261207558;
    event1->id = 1;
    event1->space = MemOpSpace::DEVICE;
    event1->eventSubType = EventSubType::HAL;
    event1->addr = 0x7958;
    event1->size = 1024;
    event1->timestamp = 1234567;

    std::shared_ptr<MemoryEvent> event2 = std::make_shared<MemoryEvent>();
    event2->eventType = EventBaseType::MALLOC;
    event2->flag = 18374686480754951175;
    event2->id = 2;
    event2->space = MemOpSpace::INVALID;
    event2->eventSubType = EventSubType::HAL;
    event2->addr = 0x7957;
    event2->size = 512;
    event2->timestamp = 1234568;
 
    std::shared_ptr<MemoryEvent> event3 = std::make_shared<MemoryEvent>();
    event3->eventType = EventBaseType::MALLOC;
    event3->flag = 504403158275081222;
    event3->id = 3;
    event3->space = MemOpSpace::DEVICE;
    event3->eventSubType = EventSubType::HAL;
    event3->addr = 0x7960;
    event3->size = 1024;
    event3->timestamp = 1234557;
 
    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event1));
    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event2));
    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event3));
}

TEST(HalAnalyzerTest, do_record_except_no_memscope) {
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    ClientId clientId = 0;

    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::MALLOC;
    event1->flag = 2377900603261207558;
    event1->id = 1;
    event1->space = MemOpSpace::DEVICE;
    event1->eventSubType = EventSubType::HAL;
    event1->addr = 0x7958;
    event1->size = 1024;
    event1->timestamp = 1234567;

    std::shared_ptr<MemoryEvent> event2 = std::make_shared<MemoryEvent>();
    event2->eventType = EventBaseType::FREE;
    event2->flag = 18374686480754951175;
    event2->id = 2;
    event2->space = MemOpSpace::INVALID;
    event2->eventSubType = EventSubType::HAL;
    event2->addr = 0x7958;
    event2->size = 0;

    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event1));
    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event2));
}

TEST(HalAnalyzerTest, do_record_excpet_double_free) {
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    ClientId clientId = 0;

    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::MALLOC;
    event1->flag = 2377900603261207558;
    event1->id = 1;
    event1->space = MemOpSpace::DEVICE;
    event1->eventSubType = EventSubType::HAL;
    event1->addr = 0x7958;
    event1->size = 1024;
    event1->timestamp = 1234567;

    std::shared_ptr<MemoryEvent> event2 = std::make_shared<MemoryEvent>();
    event2->eventType = EventBaseType::FREE;
    event2->flag = 18374686480754951175;
    event2->id = 2;
    event2->space = MemOpSpace::INVALID;
    event2->eventSubType = EventSubType::HAL;
    event2->addr = 0x7958;
    event2->size = 0;

    std::shared_ptr<MemoryEvent> event3 = std::make_shared<MemoryEvent>();
    event3->eventType = EventBaseType::FREE;
    event3->flag = 18374686480754951175;
    event3->id = 3;
    event3->space = MemOpSpace::INVALID;
    event3->eventSubType = EventSubType::HAL;
    event3->addr = 0x7958;
    event3->size = 0;

    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event1));
    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event2));
    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event3));
}

TEST(HalAnalyzerTest, do_record_except_double_malloc) {
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    ClientId clientId = 0;

    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::MALLOC;
    event1->flag = 2377900603261207558;
    event1->id = 1;
    event1->space = MemOpSpace::DEVICE;
    event1->eventSubType = EventSubType::HAL;
    event1->addr = 0x7958;
    event1->size = 1024;
    event1->timestamp = 1234567;

    std::shared_ptr<MemoryEvent> event2 = std::make_shared<MemoryEvent>();
    event2->eventType = EventBaseType::FREE;
    event2->flag = 2377900603261207558;
    event2->id = 2;
    event2->space = MemOpSpace::INVALID;
    event2->eventSubType = EventSubType::HAL;
    event2->addr = 0x7958;
    event2->size = 0;

    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event1));
    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event2));
}

TEST(HalAnalyzerTest, do_record_except_free_null) {
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    ClientId clientId = 0;

    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::FREE;
    event1->id = 2;
    event1->space = MemOpSpace::INVALID;
    event1->eventSubType = EventSubType::HAL;
    event1->addr = 0x7958;
    event1->size = 0;

    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event1));
}

TEST(HalAnalyzerTest, do_record_fail) {
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();
    ClientId clientId = 0;

    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::FREE;
    event1->id = 1;
    event1->space = MemOpSpace::INVALID;
    event1->eventSubType = EventSubType::HAL;
    event1->addr = 0x7958;
    event1->size = 0;

    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event1));
}

TEST(HalAnalyzerTest, do_memory_record_nulltable) {
    Config analysisConfig;
    BitField<decltype(analysisConfig.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    analysisConfig.eventType = eventBit.getValue();

    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::FREE;
    event1->id = 123;
    event1->space = MemOpSpace::INVALID;
    event1->eventSubType = EventSubType::HAL;
    event1->addr = 0x7958;
    event1->size = 0;
    ClientId clientId = 0;

    EXPECT_TRUE(HalAnalyzer::GetInstance(analysisConfig).Record(clientId, event1));
}