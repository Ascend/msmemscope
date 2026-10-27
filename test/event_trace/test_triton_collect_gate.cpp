/*
 * -------------------------------------------------------------------------
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

#define private public
#include "event_trace/trace_manager/event_trace_manager.h"
#undef private

#include "bit_field.h"
#include "config_info.h"

using namespace MemScope;

namespace {

Config MakeBaseConfig()
{
    Config config = GetConfig();
    BitField<decltype(config.eventType)> eventBit;
    BitField<decltype(config.levelType)> levelBit;
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    eventBit.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.levelType = levelBit.getValue();
    config.eventType = eventBit.getValue();
    return config;
}

}  // namespace

class TritonCollectGateTest : public ::testing::Test {
protected:
    Config savedConfig_{};
    EventTraceStatus savedStatus_{EventTraceStatus::NOT_IN_TRACING};
    bool savedAcl_{false};

    void SetUp() override
    {
        savedConfig_ = GetConfig();
        auto& mgr = EventTraceManager::Instance();
        savedStatus_ = mgr.status_;
        savedAcl_ = mgr.aclInit_.load();
    }

    void TearDown() override
    {
        ConfigManager::Instance().SetConfig(savedConfig_);
        auto& mgr = EventTraceManager::Instance();
        mgr.status_ = savedStatus_;
        mgr.aclInit_.store(savedAcl_);
    }

    void ApplyAllTrue()
    {
        ConfigManager::Instance().SetConfig(MakeBaseConfig());
        auto& mgr = EventTraceManager::Instance();
        mgr.status_ = EventTraceStatus::IN_TRACING;
        mgr.aclInit_.store(true);
    }
};

TEST_F(TritonCollectGateTest, AllConditionsTrue_Enables)
{
    ApplyAllTrue();
    EXPECT_TRUE(EventTraceManager::Instance().ShouldEnableTritonCollect());
}

TEST_F(TritonCollectGateTest, NotInTracing_Disables)
{
    ApplyAllTrue();
    EventTraceManager::Instance().status_ = EventTraceStatus::NOT_IN_TRACING;
    EXPECT_FALSE(EventTraceManager::Instance().ShouldEnableTritonCollect());
}

TEST_F(TritonCollectGateTest, MissingOpLevel_Disables)
{
    ApplyAllTrue();
    Config config = MakeBaseConfig();
    BitField<decltype(config.levelType)> levelBit;
    // LEVEL_KERNEL only — no LEVEL_OP
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    config.levelType = levelBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    EXPECT_FALSE(EventTraceManager::Instance().ShouldEnableTritonCollect());
}

TEST_F(TritonCollectGateTest, MissingAccessButHasLaunch_Enables)
{
    // Default-like: alloc,free,launch (no access) still needs OP_LAUNCH / watch path.
    ApplyAllTrue();
    Config config = MakeBaseConfig();
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    // no ACCESS_EVENT
    config.eventType = eventBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    EXPECT_TRUE(EventTraceManager::Instance().ShouldEnableTritonCollect());
}

TEST_F(TritonCollectGateTest, MissingAccessAndLaunch_Disables)
{
    // --events=alloc,free --level=op: no launch/access → do not wrap.
    ApplyAllTrue();
    Config config = MakeBaseConfig();
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    EXPECT_FALSE(EventTraceManager::Instance().ShouldEnableTritonCollect());
}

TEST_F(TritonCollectGateTest, AccessOnly_Enables)
{
    ApplyAllTrue();
    Config config = MakeBaseConfig();
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    config.eventType = eventBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    EXPECT_TRUE(EventTraceManager::Instance().ShouldEnableTritonCollect());
}

TEST_F(TritonCollectGateTest, AclNotInit_Disables)
{
    ApplyAllTrue();
    EventTraceManager::Instance().aclInit_.store(false);
    EXPECT_FALSE(EventTraceManager::Instance().ShouldEnableTritonCollect());
}
