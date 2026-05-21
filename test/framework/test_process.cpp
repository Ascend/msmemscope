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
#include <vector>
#include "securec.h"
#define private public
#include "process.h"
#undef private
#include "analysis/mstx_analyzer.h"
#include "client_parser.h"
#include "bit_field.h"
using namespace MemScope;

// 大部分单例config在此初始化，config相关修改请修改此处。
void setConfig(Config &config)
{
    BitField<decltype(config.eventType)> eventBit;
    BitField<decltype(config.levelType)> levelBit;
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    analysisBit.setBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS));
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    config.analysisType = analysisBit.getValue();
    config.eventType = eventBit.getValue();
    config.levelType = levelBit.getValue();
    config.enableCStack = true;
    config.enablePyStack = true;
    config.stepList.stepCount = 0;
    config.dataFormat = 0;
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);
}

TEST(Process, process_setpreloadenv_without_atb_expect_success)
{
    unsetenv("ATB_HOME_PATH");
    setenv("LD_PRELOAD_PATH", "/lib64/", 1);
    Config config;
    Utility::FileCreateManager::GetInstance("testmsmemscope");
    Process process(config);
    process.SetPreloadEnv();
    char *env = getenv("LD_PRELOAD");
    std::string hooksSo = "libleaks_ascend_hal_hook.so:"
                          "libascend_mstx_hook.so:libascend_kernel_hook.so";
    EXPECT_EQ(std::string(env), hooksSo);
    setenv("LD_PRELOAD", "test.so", 1);
    process.SetPreloadEnv();
    env = getenv("LD_PRELOAD");
    EXPECT_EQ(std::string(env), hooksSo + ":test.so");
    unsetenv("LD_PRELOAD");
}

TEST(Process, process_setpreloadenv_with_atb_abi_0_expect_success)
{
    setenv("ATB_HOME_PATH", "/usr/local/Ascend/nnal/atb/latest/atb/cxx_abi_0", 1);
    setenv("LD_PRELOAD_PATH", "/lib64/", 1);
    Config config;
    Utility::FileCreateManager::GetInstance("testmsmemscope");
    Process process(config);
    process.SetPreloadEnv();
    char *env = getenv("LD_PRELOAD");
    std::string hooksSo = "libleaks_ascend_hal_hook.so:"
                          "libascend_mstx_hook.so:libascend_kernel_hook.so:libatb_abi_0_hook.so";
    EXPECT_EQ(std::string(env), hooksSo);
    setenv("LD_PRELOAD", "test.so", 1);
    process.SetPreloadEnv();
    env = getenv("LD_PRELOAD");
    EXPECT_EQ(std::string(env), hooksSo + ":test.so");
    unsetenv("LD_PRELOAD");
}

TEST(Process, process_setpreloadenv_with_atb_abi_1_expect_success)
{
    setenv("ATB_HOME_PATH", "/usr/local/Ascend/nnal/atb/latest/atb/cxx_abi_1", 1);
    setenv("LD_PRELOAD_PATH", "/lib64/", 1);
    Config config;
    Utility::FileCreateManager::GetInstance("testmsmemscope");
    Process process(config);
    process.SetPreloadEnv();
    char *env = getenv("LD_PRELOAD");
    std::string hooksSo = "libleaks_ascend_hal_hook.so:"
                          "libascend_mstx_hook.so:libascend_kernel_hook.so:libatb_abi_1_hook.so";
    EXPECT_EQ(std::string(env), hooksSo);
    setenv("LD_PRELOAD", "test.so", 1);
    process.SetPreloadEnv();
    env = getenv("LD_PRELOAD");
    EXPECT_EQ(std::string(env), hooksSo + ":test.so");
    unsetenv("LD_PRELOAD");
}

TEST(Process, do_record_handler_except_success)
{
    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::MALLOC;
    event1->eventSubType = EventSubType::PTA_CACHING;
    event1->id = 1;
    event1->device = 0;
    event1->size = 512;
    event1->addr = 12345;
    event1->used = 512;
    
    std::shared_ptr<MstxEvent> event2 = std::make_shared<MstxEvent>();
    event2->eventType = EventBaseType::MSTX;
    event2->eventSubType = EventSubType::MSTX_RANGE_START;
    event2->rangeId = 0;
    event2->stepId = 1;
    event2->streamId = 123;

    std::shared_ptr<KernelLaunchEvent> event3 = std::make_shared<KernelLaunchEvent>();
    event3->eventType = EventBaseType::KERNEL_LAUNCH;
    event3->eventSubType = EventSubType::KERNEL_LAUNCH;

    std::shared_ptr<SystemEvent> event4 = std::make_shared<SystemEvent>();
    event4->eventType = EventBaseType::SYSTEM;
    event4->eventSubType = EventSubType::ACL_INIT;

    std::shared_ptr<OpLaunchEvent> event5 = std::make_shared<OpLaunchEvent>();
    event5->eventType = EventBaseType::OP_LAUNCH;
    event5->eventSubType = EventSubType::ATEN_START;
    event5->name = "X";

    std::shared_ptr<MemoryEvent> event6 = std::make_shared<MemoryEvent>();
    event6->eventType = EventBaseType::MALLOC;
    event6->eventSubType = EventSubType::PTA_WORKSPACE;
    event6->id = 1;
    event6->device = 0;
    event6->size = 512;
    event6->addr = 12347;
    event6->used = 512;

    Config config;
    setConfig(config);
    Process::GetInstance(config).SendEvent(event1);
    Process::GetInstance(config).SendEvent(event2);
    Process::GetInstance(config).SendEvent(event3);
    Process::GetInstance(config).SendEvent(event4);
    Process::GetInstance(config).SendEvent(event5);
    Process::GetInstance(config).SendEvent(event6);
}