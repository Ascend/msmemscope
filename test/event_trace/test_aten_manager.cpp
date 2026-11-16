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
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "securec.h"

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

namespace
{
// 用例自归一：保存并注入配置/追踪状态/观察者单例字段，用例结束逐项恢复，
// 避免对跨用例单例（AtenManager/MemoryWatch/TensorMonitor/EventTraceManager）做绝对断言
class AtenManagerWatchTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        // 先在环境配置下确保MemoryWatch单例完成构造：其构造函数会读取watch首尾目标，
        // 若延迟到注入配置之后才首次构造，保存/恢复的将是本用例的注入值并污染后续用例
        auto& watch = MemoryWatch::GetInstance();
        savedFirstWatchTarget_ = watch.firstWatchTarget_;
        savedLastWatchTarget_ = watch.lastWatchTarget_;
        savedWatchOutputId_ = watch.outputId_;
        savedWatchedTargetName_ = watch.watchedTargetName_;
        savedFirstWatchTargetCnt_ = watch.firstWatchTargetCnt_;
        savedTargetNameCnt_ = watch.targetNameCnt_;

        auto& configManager = ConfigManager::Instance();
        savedConfig_ = configManager.config_;
        Config config = configManager.GetConfig();
        config.watchConfig.isWatched = true;
        config.watchConfig.fullContent = false;
        config.watchConfig.outputId = UINT32_MAX;
        ASSERT_EQ(strncpy_s(config.watchConfig.start, sizeof(config.watchConfig.start), "opBegin",
                            sizeof(config.watchConfig.start) - 1), EOK);
        ASSERT_EQ(strncpy_s(config.watchConfig.end, sizeof(config.watchConfig.end), "opEnd",
                            sizeof(config.watchConfig.end) - 1), EOK);
        config.enablePyStack = true;
        config.pyStackDepth = 2;
        BitField<decltype(config.levelType)> levelBit;
        levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
        config.levelType = levelBit.getValue();
        config.eventType = 0x0F;
        config.collectCpu = false;
        configManager.config_ = config;

        auto& traceManager = EventTraceManager::Instance();
        savedStatus_ = traceManager.status_;
        savedDestroyed_ = traceManager.destroyed_.load();
        traceManager.status_ = EventTraceStatus::IN_TRACING;
        traceManager.destroyed_.store(false);
        traceManager.InitJudgeFuncTable();

        auto& manager = AtenManager::GetInstance();
        savedIsWatchEnable_ = manager.isWatchEnable_;
        savedIsFirstWatchOpSet_ = manager.isfirstWatchOpSet_;
        savedFirstWatchOp_ = manager.firstWatchOp_;
        savedLastWatchOp_ = manager.lastWatchOp_;
        savedOutputTensors_ = manager.outputTensors_;
        manager.isWatchEnable_ = false;
        manager.isfirstWatchOpSet_ = false;
        manager.firstWatchOp_ = "opBegin";
        manager.lastWatchOp_ = "opEnd";
        manager.outputTensors_.clear();

        // MemoryWatch的watch目标改为不匹配值，使被调用链路不产生落盘副作用
        watch.firstWatchTarget_ = "__unmatched_first__";
        watch.lastWatchTarget_ = "__unmatched_last__";
        watch.outputId_ = UINT32_MAX;
        watch.watchedTargetName_.clear();
        watch.firstWatchTargetCnt_ = 1;
        watch.targetNameCnt_.clear();

        TensorMonitor::GetInstance().ClearCmdWatchTensor();
    }

    void TearDown() override
    {
        auto& configManager = ConfigManager::Instance();
        configManager.config_ = savedConfig_;

        auto& traceManager = EventTraceManager::Instance();
        traceManager.status_ = savedStatus_;
        traceManager.destroyed_.store(savedDestroyed_);
        traceManager.InitJudgeFuncTable();

        auto& manager = AtenManager::GetInstance();
        manager.isWatchEnable_ = savedIsWatchEnable_;
        manager.isfirstWatchOpSet_ = savedIsFirstWatchOpSet_;
        manager.firstWatchOp_ = savedFirstWatchOp_;
        manager.lastWatchOp_ = savedLastWatchOp_;
        manager.outputTensors_ = savedOutputTensors_;

        auto& watch = MemoryWatch::GetInstance();
        watch.firstWatchTarget_ = savedFirstWatchTarget_;
        watch.lastWatchTarget_ = savedLastWatchTarget_;
        watch.outputId_ = savedWatchOutputId_;
        watch.watchedTargetName_ = savedWatchedTargetName_;
        watch.firstWatchTargetCnt_ = savedFirstWatchTargetCnt_;
        watch.targetNameCnt_ = savedTargetNameCnt_;

        TensorMonitor::GetInstance().ClearCmdWatchTensor();
    }

    Config savedConfig_;
    EventTraceStatus savedStatus_ = EventTraceStatus::NOT_IN_TRACING;
    bool savedDestroyed_ = false;

    bool savedIsWatchEnable_ = false;
    bool savedIsFirstWatchOpSet_ = false;
    std::string savedFirstWatchOp_;
    std::string savedLastWatchOp_;
    std::vector<MonitoredTensor> savedOutputTensors_;

    std::string savedFirstWatchTarget_;
    std::string savedLastWatchTarget_;
    uint32_t savedWatchOutputId_ = UINT32_MAX;
    std::string savedWatchedTargetName_;
    uint32_t savedFirstWatchTargetCnt_ = 1;
    std::unordered_map<std::string, uint64_t> savedTargetNameCnt_;
};

// 构造函数：watch使能（配置打开或TensorMonitor已在监控）时置isWatchEnable_，并捕获首尾watch算子名
TEST_F(AtenManagerWatchTest, constructor_enables_watch_from_config_or_monitoring)
{
    auto& configManager = ConfigManager::Instance();
    configManager.config_.watchConfig.isWatched = true;

    AtenManager localManager;
    EXPECT_TRUE(localManager.isWatchEnable_);
    EXPECT_EQ(localManager.firstWatchOp_, "opBegin");
    EXPECT_EQ(localManager.lastWatchOp_, "opEnd");
    EXPECT_FALSE(localManager.isfirstWatchOpSet_);

    // 配置关闭watch但TensorMonitor已在监控时，同样使能
    configManager.config_.watchConfig.isWatched = false;
    MonitoredTensor tensor = {};
    tensor.data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000));
    tensor.dataSize = 16;
    std::vector<MonitoredTensor> tensors = {tensor};
    TensorMonitor::GetInstance().AddWatchTensor(tensors, 0);
    ASSERT_TRUE(TensorMonitor::GetInstance().IsInMonitoring());

    AtenManager monitoringManager;
    EXPECT_TRUE(monitoringManager.isWatchEnable_);
    TensorMonitor::GetInstance().ClearCmdWatchTensor();
}

// ProcessMsg：aten开始消息走ReportAtenLaunch的begin分支，使能watch并登记begin算子，不置首算子标记
TEST_F(AtenManagerWatchTest, process_msg_begin_enters_launch_begin_branch)
{
    auto& manager = AtenManager::GetInstance();
    manager.isWatchEnable_ = false;
    manager.isfirstWatchOpSet_ = false;
    manager.outputTensors_.clear();
    MemoryWatch::GetInstance().targetNameCnt_.clear();

    manager.ProcessMsg("b:name=opBegin;ptr=4096;tensor_size=16;", 0);

    EXPECT_TRUE(manager.isWatchEnable_);
    EXPECT_FALSE(manager.isfirstWatchOpSet_);
    // begin分支确实进入OpExcuteBegin并登记了算子名（而非仅改动使能标志）
    EXPECT_EQ(MemoryWatch::GetInstance().targetNameCnt_["opBegin"], 1u);
    EXPECT_TRUE(manager.outputTensors_.empty());
}

// ProcessMsg：结束消息先置首算子标记，命中尾算子时清空输出tensor并复位标记
TEST_F(AtenManagerWatchTest, process_msg_end_sets_then_clears_first_flag)
{
    auto& manager = AtenManager::GetInstance();
    manager.isfirstWatchOpSet_ = false;
    manager.outputTensors_.clear();
    MonitoredTensor pending = {};
    pending.data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x2000));
    pending.dataSize = 32;
    manager.outputTensors_.push_back(pending);

    manager.ProcessMsg("e:name=opBegin;", 0);
    EXPECT_TRUE(manager.isfirstWatchOpSet_);
    // 首watch算子只置标记，输出tensor保留给后续尾算子处理
    EXPECT_EQ(manager.outputTensors_.size(), 1u);

    manager.ProcessMsg("e:name=opEnd;", 0);
    EXPECT_FALSE(manager.isfirstWatchOpSet_);
    EXPECT_TRUE(manager.outputTensors_.empty());
}

// ReportAtenAccess：UNKNOWN访问类型且is_output为True时把tensor记入输出列表，字段解析齐全
TEST_F(AtenManagerWatchTest, process_msg_access_unknown_collects_output_tensor)
{
    ConfigManager::Instance().config_.enablePyStack = false;
    auto& manager = AtenManager::GetInstance();
    manager.isfirstWatchOpSet_ = false;
    manager.outputTensors_.clear();

    manager.ProcessMsg("ac:ptr=4096;dtype=float32;shape=1,2;tensor_size=512;name=opBegin;"
                       "is_write=False;is_read=False;is_output=True;device=0",
                       0);

    ASSERT_EQ(manager.outputTensors_.size(), 1u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(manager.outputTensors_[0].data),
              static_cast<std::uintptr_t>(4096));
    EXPECT_EQ(manager.outputTensors_[0].dataSize, 512u);
    // access消息不参与首/尾算子标记
    EXPECT_FALSE(manager.isfirstWatchOpSet_);
}

// ReportAtenAccess：WRITE类型；addr/size非法时StrToUint64失败，tensor以0值记录
TEST_F(AtenManagerWatchTest, process_msg_access_write_with_invalid_addr_and_size)
{
    ConfigManager::Instance().config_.enablePyStack = false;
    auto& manager = AtenManager::GetInstance();
    manager.isfirstWatchOpSet_ = false;
    manager.outputTensors_.clear();

    manager.ProcessMsg("ac:ptr=bad_addr;tensor_size=bad_size;name=opBegin;"
                       "is_write=True;is_read=False;is_output=True;",
                       0);

    ASSERT_EQ(manager.outputTensors_.size(), 1u);
    // 解析失败保留默认值0，证明确实走了StrToUint64失败分支
    EXPECT_EQ(manager.outputTensors_[0].data, nullptr);
    EXPECT_EQ(manager.outputTensors_[0].dataSize, 0u);
}

// ReportAtenAccess：READ类型；输出tensor记录发生在采集判据之前，非采集态仍记录但不继续上报
TEST_F(AtenManagerWatchTest, process_msg_access_read_and_not_tracing_gate)
{
    ConfigManager::Instance().config_.enablePyStack = false;
    auto& manager = AtenManager::GetInstance();
    manager.isfirstWatchOpSet_ = false;
    manager.outputTensors_.clear();

    manager.ProcessMsg("ac:ptr=8192;tensor_size=64;name=other;"
                       "is_write=False;is_read=True;is_output=False;",
                       0);
    EXPECT_TRUE(manager.outputTensors_.empty());

    auto& traceManager = EventTraceManager::Instance();
    traceManager.status_ = EventTraceStatus::NOT_IN_TRACING;
    manager.ProcessMsg("ac:ptr=8192;tensor_size=64;name=opBegin;"
                       "is_write=False;is_read=True;is_output=True;",
                       0);
    // 判据在tensor收集之后：非采集态也先记录输出tensor，随后才因ACCESS不需要采集而返回
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::ACCESS));
    ASSERT_EQ(manager.outputTensors_.size(), 1u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(manager.outputTensors_[0].data),
              static_cast<std::uintptr_t>(8192));
}

// ExtractTensorInfo：消息末尾没有';'/'}'分隔符时，值取到消息结尾
TEST_F(AtenManagerWatchTest, extract_tensor_info_value_at_message_end)
{
    auto& manager = AtenManager::GetInstance();
    std::string value = "keep";
    EXPECT_TRUE(manager.ExtractTensorInfo("ptr=4096", "ptr=", value));
    EXPECT_EQ(value, "4096");

    value = "keep";
    EXPECT_FALSE(manager.ExtractTensorInfo("no_such_field=1", "ptr=", value));
    EXPECT_EQ(value, "keep");
}
}  // namespace
