// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
#include <gtest/gtest.h>
#include <gtest/internal/gtest-port.h>
#include <string>
#include <memory>
#include "py_step_manager.h"
#include "record_info.h"
#include "config_info.h"

using namespace MemScope;

// 预期能够正常接收来自python接口的step信息
TEST(PyStepManagerTest, do_msmemscope_step_expect_success) {
    ClientId clientId = 0;

    std::shared_ptr<SystemEvent> eventStep1 = std::make_shared<SystemEvent>();
    eventStep1->eventType = EventBaseType::SYSTEM;
    eventStep1->eventSubType = EventSubType::STEP;
    eventStep1->name = "1";

    std::shared_ptr<SystemEvent> eventStep2 = std::make_shared<SystemEvent>();
    eventStep2->eventType = EventBaseType::SYSTEM;
    eventStep2->eventSubType = EventSubType::STEP;
    eventStep2->name = "2";

    std::shared_ptr<SystemEvent> eventStep3 = std::make_shared<SystemEvent>();
    eventStep3->eventType = EventBaseType::SYSTEM;
    eventStep3->eventSubType = EventSubType::STEP;
    eventStep3->name = "3";

    // 分别模拟三次step信息进入
    PyStepManager::Instance().RecordPyStep(clientId, eventStep1);
    PyStepManager::Instance().RecordPyStep(clientId, eventStep2);
    PyStepManager::Instance().RecordPyStep(clientId, eventStep3);
}

// 预期能够正常注册和注销观察者模块
TEST(PyStepManagerTest, do_analyzer_register_and_unregister_expect_success) {
    PyStepManager::Instance().Subscribe(PyStepEventSubscriber::STEP_INNER_ANALYZER, nullptr);
    PyStepManager::Instance().UnSubscribe(PyStepEventSubscriber::STEP_INNER_ANALYZER);
}

// 预期能够正常分发信息给各位观察者
TEST(PyStepManagerTest, do_analyzer_notify_expect_success) {
    PyStepManager::Instance().Subscribe(PyStepEventSubscriber::STEP_INNER_ANALYZER, nullptr);

    ClientId clientId = 0;
    std::shared_ptr<SystemEvent> eventStep1 = std::make_shared<SystemEvent>();
    eventStep1->eventType = EventBaseType::SYSTEM;
    eventStep1->eventSubType = EventSubType::STEP;
    eventStep1->name = "1";

    PyStepManager::Instance().RecordPyStep(clientId, eventStep1);
    PyStepManager::Instance().UnSubscribe(PyStepEventSubscriber::STEP_INNER_ANALYZER);
}
