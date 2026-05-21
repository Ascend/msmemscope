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
#include <memory>
#include "mstx_analyzer.h"
#include "record_info.h"
#include "config_info.h"

using namespace MemScope;

TEST(MstxAnalyzerTest, do_mstx_record_expect_success) {
    ClientId clientId = 0;

    std::shared_ptr<MstxEvent> eventStart = std::make_shared<MstxEvent>();
    eventStart->eventType = EventBaseType::MSTX;
    eventStart->eventSubType = EventSubType::MSTX_RANGE_START;
    eventStart->stepId = 1;
    eventStart->streamId = 123;

    std::shared_ptr<MstxEvent> eventEnd = std::make_shared<MstxEvent>();
    eventEnd->eventType = EventBaseType::MSTX;
    eventEnd->eventSubType = EventSubType::MSTX_RANGE_END;
    eventEnd->stepId = 1;
    eventEnd->streamId = 123;

    std::shared_ptr<MstxEvent> eventMark = std::make_shared<MstxEvent>();
    eventMark->eventType = EventBaseType::MSTX;
    eventMark->eventSubType = EventSubType::MSTX_MARK;
    eventMark->stepId = 0;
    eventMark->streamId = 123;

    EXPECT_TRUE(MstxAnalyzer::Instance().RecordMstx(clientId, eventStart));
    EXPECT_TRUE(MstxAnalyzer::Instance().RecordMstx(clientId, eventEnd));
    EXPECT_TRUE(MstxAnalyzer::Instance().RecordMstx(clientId, eventMark));
}

TEST(MstxAnalyzerTest, do_analyzer_register_and_unregister_expect_success) {
    MstxAnalyzer::Instance().Subscribe(MstxEventSubscriber::STEP_INNER_ANALYZER, nullptr);
    MstxAnalyzer::Instance().UnSubscribe(MstxEventSubscriber::STEP_INNER_ANALYZER);
}

TEST(MstxAnalyzerTest, do_analyzer_notify_expect_success) {
    MstxAnalyzer::Instance().Subscribe(MstxEventSubscriber::STEP_INNER_ANALYZER, nullptr);

    ClientId clientId = 0;
    std::shared_ptr<MstxEvent> eventStart = std::make_shared<MstxEvent>();
    eventStart->eventType = EventBaseType::MSTX;
    eventStart->eventSubType = EventSubType::MSTX_RANGE_START;
    eventStart->stepId = 1;
    eventStart->streamId = 123;

    EXPECT_TRUE(MstxAnalyzer::Instance().RecordMstx(clientId, eventStart));
    MstxAnalyzer::Instance().UnSubscribe(MstxEventSubscriber::STEP_INNER_ANALYZER);
}