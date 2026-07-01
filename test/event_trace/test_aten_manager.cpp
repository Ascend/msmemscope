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
#define private public
#include "event_trace/event_report.h"
#include "event_trace/mstx_hooks/aten_manager.h"
#undef private
#include "bit_field.h"
#include "event_trace/mstx_hooks/mstx_manager.h"
#include "event_trace/memory_watch/tensor_monitor.h"

using namespace MemScope;

TEST(AtenManagerTest, ReportAtenStartLaunchTest) {
    Config config = GetConfig();
    BitField<decltype(config.eventType)> eventBit;
    BitField<decltype(config.levelType)> levelBit;
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    config.eventType = eventBit.getValue();
    config.levelType = levelBit.getValue();
    config.enableCStack = true;
    config.enablePyStack = true;
    ConfigManager::Instance().SetConfig(config);
    const char* msg = "memscope-aten-b: {func.__module__}.{func.__name__}";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream, MemScopeCommType::MEMORY_DEBUG);
}

TEST(AtenManagerTest, ReportAtenStart_nullname_Test) {
    const char* msg = "memscope-aten-b:";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream, MemScopeCommType::MEMORY_DEBUG);
}

TEST(AtenManagerTest, ReportAtenEndLaunchTest) {
    const char* msg = "memscope-aten-e: {func.__module__}.{func.__name__}";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream, MemScopeCommType::MEMORY_DEBUG);
}

TEST(AtenManagerTest, ReportAtenUnknownAccessTest) {
    const char* msg = "memscope-aten-ac:ptr=1000;is_write=False;is_read=False;is_output=False;"\
        "name={func.__module__}.{func.__name__};shape={value.shape};dtype={value.dtype};tensor_size=500;device=0";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream, MemScopeCommType::MEMORY_DEBUG);
}

TEST(AtenManagerTest, ReportAtenWriteAccessTest) {
    const char* msg = "memscope-aten-ac:ptr=1000;is_write=True;is_read=False;is_output=False;"\
        "name={func.__module__}.{func.__name__};shape={value.shape};dtype={value.dtype};tensor_size=500;device=0";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream, MemScopeCommType::MEMORY_DEBUG);
}

TEST(AtenManagerTest, ReportAtenReadAccessTest) {
    const char* msg = "memscope-aten-ac:ptr=1000;is_write=False;is_read=True;is_output=False;"\
        "name={func.__module__}.{func.__name__};shape={value.shape};dtype={value.dtype};tensor_size=500;device=0";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream, MemScopeCommType::MEMORY_DEBUG);
}

TEST(AtenManagerTest, ReportAtenReadAccess_nullname_Test) {
    const char* msg = "memscope-aten-ac:ptr=1000;is_write=False;is_read=True;is_output=False;"\
        "name=;shape={value.shape};dtype={value.dtype};tensor_size=500;device=0";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream, MemScopeCommType::MEMORY_DEBUG);
}

TEST(AtenManagerTest, ExtractTensorInfoTest) {
    const char* msg = "memscope-aten-ac:ptr=1000";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream, MemScopeCommType::MEMORY_DEBUG);
}

TEST(AtenManagerTest, ExtractTensorInfoSuccessTest)
{
    const char* msg = "ac:ptr=1000;is_write=False;is_read=True;is_output=True;"\
        "name=test;shape=value.shape;dtype=value.dtype;tensor_size=500;device=0";
    std::string key = "ptr=";
    std::string value = "";
    auto ret = AtenManager::GetInstance().ExtractTensorInfo(msg, key, value);
    ASSERT_TRUE(ret);
    EXPECT_EQ(value, "1000");

    key = "is_write=";
    ret = AtenManager::GetInstance().ExtractTensorInfo(msg, key, value);
    ASSERT_TRUE(ret);
    EXPECT_EQ(value, "False");

    key = "is_read=";
    ret = AtenManager::GetInstance().ExtractTensorInfo(msg, key, value);
    ASSERT_TRUE(ret);
    EXPECT_EQ(value, "True");

    key = "is_output=";
    ret = AtenManager::GetInstance().ExtractTensorInfo(msg, key, value);
    ASSERT_TRUE(ret);
    EXPECT_EQ(value, "True");

    key = "name=";
    ret = AtenManager::GetInstance().ExtractTensorInfo(msg, key, value);
    ASSERT_TRUE(ret);
    EXPECT_EQ(value, "test");

    key = "shape=";
    ret = AtenManager::GetInstance().ExtractTensorInfo(msg, key, value);
    ASSERT_TRUE(ret);
    EXPECT_EQ(value, "value.shape");

    key = "dtype=";
    ret = AtenManager::GetInstance().ExtractTensorInfo(msg, key, value);
    ASSERT_TRUE(ret);
    EXPECT_EQ(value, "value.dtype");

    key = "tensor_size=";
    ret = AtenManager::GetInstance().ExtractTensorInfo(msg, key, value);
    ASSERT_TRUE(ret);
    EXPECT_EQ(value, "500");
}

TEST(AtenManagerTest, ExtractTensorInfoFailedTest)
{
    const char* msg = "ac:test";
    std::string key = "ptr=";
    std::string value = "";
    auto ret = AtenManager::GetInstance().ExtractTensorInfo(msg, key, value);
    ASSERT_FALSE(ret);
    EXPECT_EQ(value, "");
}

TEST(AtenManagerTest, IsFirstWatchedOpTest)
{
    AtenManager::GetInstance().firstWatchOp_ = "test";
    auto ret = AtenManager::GetInstance().IsFirstWatchedOp("test");
    ASSERT_TRUE(ret);

    AtenManager::GetInstance().firstWatchOp_ = "test1";
    ret = AtenManager::GetInstance().IsFirstWatchedOp("test2");
    ASSERT_FALSE(ret);
}

TEST(AtenManagerTest, IsLastWatchedOpTest)
{
    AtenManager::GetInstance().lastWatchOp_ = "test";
    auto ret = AtenManager::GetInstance().IsLastWatchedOp("test");
    ASSERT_TRUE(ret);

    AtenManager::GetInstance().lastWatchOp_ = "test1";
    ret = AtenManager::GetInstance().IsLastWatchedOp("test2");
    ASSERT_FALSE(ret);
}
