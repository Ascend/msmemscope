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
#include "event_trace/trace_manager/event_trace_manager.h"
#include "event_trace/event_report.h"
#undef private
#include "event_trace/vallina_symbol.h"
#include "securec.h"
#include "bit_field.h"

using namespace MemScope;

class EventReportTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("./testmsmemscope");
    }

    void TearDown() override
    {
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("");
        rmdir("./testmsmemscope");
    }
};

TEST_F(EventReportTest, EventReportInstanceTest)
{
    EventReport& instance1 = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    EventReport& instance2 = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(EventReportTest, ReportHalMallocTestDEVICE)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);

    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
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
    uint64_t testAddr = 0x12345678;
    uint64_t testSize = 1024;
    unsigned long long testFlag = 2377900603261207558;
    MemOpSpace space = MemOpSpace::DEVICE;

    CallStackString callStack;
    EXPECT_TRUE(instance.ReportHalMalloc(testAddr, testSize, 1, std::move(callStack)));
}

TEST_F(EventReportTest, ReportAddrInfoTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t testAddr = 0x12345678;
    std::vector<std::pair<OwnerLevel, std::string>> labels = {{OwnerLevel::USER_DEFINED_1, "owner"}};
    EXPECT_TRUE(instance.ReportAddrInfo(EventSubType::DESCRIBE_OWNER, testAddr, labels));
}

TEST_F(EventReportTest, ReportPyStepTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    EXPECT_TRUE(instance.ReportPyStepRecord());
}

TEST_F(EventReportTest, ReportTorchNpuMallocTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    auto memoryusage1 = MemoryUsage {};
    memoryusage1.deviceIndex = 0;
    memoryusage1.dataType = 0;
    memoryusage1.ptr = 12345;
    memoryusage1.allocSize = 512;
    memoryusage1.totalAllocated = 512;
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    CallStackString stack;
    EXPECT_TRUE(instance.ReportMemPoolRecord(EventSubType::PTA_CACHING, memoryusage1, std::move(stack)));
}

TEST_F(EventReportTest, ReportTorchNpuFreeTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    auto memoryusage1 = MemoryUsage {};
    memoryusage1.deviceIndex = 3;
    memoryusage1.dataType = 1;
    memoryusage1.ptr = 12345;
    memoryusage1.allocSize = 512;
    memoryusage1.totalAllocated = 512;
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    CallStackString stack;
    EXPECT_TRUE(instance.ReportMemPoolRecord(EventSubType::PTA_CACHING, memoryusage1, std::move(stack)));
}

TEST_F(EventReportTest, ReportTorchNpuConditionTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    auto memoryusage1 = MemoryUsage {};
    memoryusage1.deviceIndex = 3;
    memoryusage1.dataType = 1;
    memoryusage1.ptr = 12345;
    memoryusage1.allocSize = 512;
    memoryusage1.totalAllocated = 512;
    CallStackString stack;

    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    EXPECT_TRUE(instance.ReportMemPoolRecord(EventSubType::PTA_CACHING, memoryusage1, std::move(stack)));
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    EXPECT_TRUE(instance.ReportMemPoolRecord(EventSubType::PTA_CACHING, memoryusage1, std::move(stack)));
}

TEST_F(EventReportTest, ReportHalMallocTestHost)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;

    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.eventType = eventBit.getValue();
    config.enableCStack = true;
    config.enablePyStack = true;
    ConfigManager::Instance().SetConfig(config);
    uint64_t testAddr = 0x12345678;
    uint64_t testSize = 1024;
    unsigned long long testFlag = 504403158274934784;
    MemOpSpace space = MemOpSpace::HOST;
    CallStackString callStack;
    EXPECT_TRUE(instance.ReportHalMalloc(testAddr, testSize, 1, std::move(callStack)));
}

TEST_F(EventReportTest, ReportHalFreeTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.eventType = eventBit.getValue();
    config.enableCStack = true;
    config.enablePyStack = true;
    ConfigManager::Instance().SetConfig(config);
    uint64_t testAddr = 0x12345678;
    CallStackString callStack;
    EXPECT_TRUE(instance.ReportHalFree(testAddr, std::move(callStack)));
}

TEST_F(EventReportTest, ReportHostRegisterTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;

    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.eventType = eventBit.getValue();
    config.enableCStack = true;
    config.enablePyStack = true;
    config.collectCpu = true;
    ConfigManager::Instance().SetConfig(config);
    uint64_t testAddr = 0x12345678;
    uint64_t testSize = 1024;
    CallStackString callStack;
    EXPECT_TRUE(instance.ReportHostRegister(testAddr, testSize, std::move(callStack)));
}

TEST_F(EventReportTest, ReportHostUnregisterTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;

    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.eventType = eventBit.getValue();
    config.enableCStack = true;
    config.enablePyStack = true;
    config.collectCpu = true;
    ConfigManager::Instance().SetConfig(config);
    uint64_t testAddr = 0x12345678;
    uint64_t testSize = 1024;
    CallStackString callStack;
    EXPECT_TRUE(instance.ReportHostUnregister(testAddr, std::move(callStack)));
}

TEST_F(EventReportTest, ReportMarkTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    std::string msg("mark");
    EXPECT_TRUE(instance.ReportMark(MarkType::MARK_A, msg, 0, 0));
}

TEST_F(EventReportTest, ReportKernelLaunchTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);

    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    BitField<decltype(config.eventType)> eventBit;
    BitField<decltype(config.levelType)> levelBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    config.eventType = eventBit.getValue();
    ConfigManager::Instance().SetConfig(config);

    uint16_t devId = 1;
    uint16_t streamId = 1;
    uint16_t taskId = 1;
    auto taskKey = std::make_tuple(devId, streamId, taskId);
    AclnnKernelMapInfo kernelLaunchInfo {};
    kernelLaunchInfo.taskKey = taskKey;
    kernelLaunchInfo.timestamp = 123;
    kernelLaunchInfo.kernelName = "add";
    EXPECT_TRUE(instance.ReportKernelLaunch(kernelLaunchInfo));

    int16_t devIdNeg = -1;
    auto taskKeyNeg = std::make_tuple(devIdNeg, streamId, taskId);
    AclnnKernelMapInfo kernelLaunchInfoNeg {};
    kernelLaunchInfoNeg.taskKey = taskKeyNeg;
    kernelLaunchInfoNeg.timestamp = 123;
    kernelLaunchInfoNeg.kernelName = "add";
    EXPECT_TRUE(instance.ReportKernelLaunch(kernelLaunchInfoNeg));
}

TEST_F(EventReportTest, ReportAclItfTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);

    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);

    EXPECT_TRUE(instance.ReportAclItf(RecordSubType::INIT));
}

TEST_F(EventReportTest, ReportAtbOpExecuteTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    char name[255] = "operation";
    char attr[255] = "path:model_struct";
    EXPECT_TRUE(instance.ReportAtbOpExecute(name, sizeof(name), attr, sizeof(attr), RecordSubType::ATB_START));
}

TEST_F(EventReportTest, ReportAtbKernelTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    char name[255] = "operation";
    char attr[255] = "path:model_struct";
    EXPECT_TRUE(instance.ReportAtbKernel(name, sizeof(name), attr, sizeof(attr), RecordSubType::KERNEL_START));
}

TEST_F(EventReportTest, ReportAtbMemAccessTest)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    char name[255] = "operation";
    char attr[255] = "path:model_struct";
    uint64_t addr = 123;
    uint64_t size = 1;
    EXPECT_TRUE(instance.ReportAtbAccessMemory(name, sizeof(name), attr, sizeof(attr), addr, size,
                AccessType::UNKNOWN));
}

TEST_F(EventReportTest, ReportAtenLaunchTestExpectSuccess)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    std::string name("op");
    std::string stack;
    EXPECT_TRUE(instance.ReportAtenLaunch(name, true, std::move(stack)));
}

TEST_F(EventReportTest, ReportAtenAccessTestExpectSuccess)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    std::string stack;
    EXPECT_TRUE(instance.ReportAtenAccess("op", "", AccessType::UNKNOWN, 123456, 512, std::move(stack)));
}

TEST_F(EventReportTest, ReportAtenLaunchTestExpextSuccess)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    std::string stack;
    EXPECT_TRUE(instance.ReportAtenLaunch("op", true, std::move(stack)));
}

TEST_F(EventReportTest, ReportKernelExcuteTestExpextSuccess)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);
    CallStackString stack;
    std::string name = "test";
    uint64_t time = 1234;
    RecordSubType type = RecordSubType::KERNEL_START;
    TaskKey key;
    EXPECT_TRUE(instance.ReportKernelExcute(key, name, time, type));
}

struct TestLoader {
    static void *Load(void)
    {
        void *ptr = nullptr;
        return ptr;
    }
};

TEST_F(EventReportTest, VallinaSymbolTest)
{
    using va = VallinaSymbol<TestLoader>;
    char const *symbol;
    va::Instance().Get(symbol);
}

void ResetEventReportStepInfo()
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    instance.stepInfo_.currentStepId = 0;
    instance.stepInfo_.inStepRange = false;
    instance.stepInfo_.stepMarkRangeIdList.clear();
    return;
}

TEST_F(EventReportTest, TestReportSkipStepsNormal)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    config.collectCpu = true;
    config.stepList.stepCount = 3;
    config.stepList.stepIdList[0] = 1;
    config.stepList.stepIdList[1] = 2;
    config.stepList.stepIdList[2] = 6;
    ConfigManager::Instance().SetConfig(config);

    instance.SetStepInfo(MarkType::RANGE_START_A, "step start", 8);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), false);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), false);

    instance.SetStepInfo(MarkType::RANGE_END, "step end", 8);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), true);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), true);

    instance.SetStepInfo(MarkType::RANGE_START_A, "step start", 9);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), false);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), false);

    instance.SetStepInfo(MarkType::RANGE_END, "step end", 9);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), true);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), true);

    instance.SetStepInfo(MarkType::RANGE_START_A, "step start", 10);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), true);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), true);

    instance.SetStepInfo(MarkType::RANGE_END, "step end", 10);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), true);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), true);

    ResetEventReportStepInfo();
}

TEST_F(EventReportTest, TestReportSkipStepsWithNoMstx)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    config.collectCpu = true;
    config.stepList.stepCount = 3;
    config.stepList.stepIdList[0] = 1;
    config.stepList.stepIdList[1] = 2;
    config.stepList.stepIdList[2] = 6;
    ConfigManager::Instance().SetConfig(config);

    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), true);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), true);

    ResetEventReportStepInfo();
}

TEST_F(EventReportTest, TestReportSkipStepsWithOtherMessageMstx)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    config.collectCpu = true;
    config.stepList.stepCount = 3;
    config.stepList.stepIdList[0] = 1;
    config.stepList.stepIdList[1] = 2;
    config.stepList.stepIdList[2] = 6;
    ConfigManager::Instance().SetConfig(config);
    instance.SetStepInfo(MarkType::RANGE_START_A, "report host memory info start", 8);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), true);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), true);

    ResetEventReportStepInfo();
}

TEST_F(EventReportTest, TestReportSkipStepsWithMstxEndMismatch)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    config.collectCpu = true;
    config.stepList.stepCount = 3;
    config.stepList.stepIdList[0] = 1;
    config.stepList.stepIdList[1] = 2;
    config.stepList.stepIdList[2] = 6;
    ConfigManager::Instance().SetConfig(config);

    instance.SetStepInfo(MarkType::RANGE_START_A, "step start", 8);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), false);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), false);
    config.collectCpu = false;
    ConfigManager::Instance().SetConfig(config);
    EXPECT_EQ(instance.IsNeedSkip(DEVICE_ID_CPU), true);

    instance.SetStepInfo(MarkType::RANGE_END, "step end", 9);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), false);

    ResetEventReportStepInfo();
}

TEST_F(EventReportTest, TestReportSkipStepsWithOnlyMstxEnd)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    config.stepList.stepCount = 3;
    config.stepList.stepIdList[0] = 1;
    config.stepList.stepIdList[1] = 2;
    config.stepList.stepIdList[2] = 6;
    ConfigManager::Instance().SetConfig(config);

    instance.SetStepInfo(MarkType::RANGE_END, "step end", 9);
    EXPECT_EQ(instance.IsNeedSkip(GD_INVALID_NUM), true);

    ResetEventReportStepInfo();
}

TEST_F(EventReportTest, ReportTestWithNoReceiveServerInfo)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t testAddr = 0x12345678;
    uint64_t testSize = 1024;
    unsigned long long flag = 0x1234;
    CallStackString callStack;
    Config config = MemScope::GetConfig();
    config.collectAllNpu = true;
    ConfigManager::Instance().SetConfig(config);

    EXPECT_TRUE(instance.ReportHalMalloc(testAddr, testSize, flag, std::move(callStack)));
    EXPECT_TRUE(instance.ReportHalFree(testAddr, std::move(callStack)));

    uint16_t devId = 1;
    uint16_t streamId = 1;
    uint16_t taskId = 1;
    auto taskKey = std::make_tuple(devId, streamId, taskId);
    AclnnKernelMapInfo kernelLaunchInfo {};
    kernelLaunchInfo.taskKey = taskKey;
    kernelLaunchInfo.timestamp = 123;
    kernelLaunchInfo.kernelName = "add";
    EXPECT_TRUE(instance.ReportKernelLaunch(kernelLaunchInfo));

    RecordSubType subtype = {};
    EXPECT_TRUE(instance.ReportAclItf(subtype));

    MemoryUsage info;
    CallStackString stack;
    EXPECT_TRUE(instance.ReportMemPoolRecord(EventSubType::PTA_CACHING, info, std::move(stack)));
    std::string msg("mark");
    EXPECT_TRUE(instance.ReportMark(MarkType::MARK_A, msg, 0, 0));
}

constexpr uint64_t MEM_VIRT_BIT = 10;
constexpr uint64_t MEM_SVM_VAL = 0x0;
constexpr uint64_t MEM_DEV_VAL = 0x1;
constexpr uint64_t MEM_HOST_VAL = 0x2;
constexpr uint64_t MEM_DVPP_VAL = 0x3;
constexpr uint64_t MEM_INVALID_VAL = 0x4;

TEST_F(EventReportTest, GetMemOpSpaceIfINvalid)
{
    unsigned long long flag = MEM_INVALID_VAL << MEM_VIRT_BIT;
    MemScope::MemOpSpace result = MemScope::GetMemOpSpace(flag);
    EXPECT_EQ(result, MemScope::MemOpSpace::INVALID);
}

TEST_F(EventReportTest, GetMemOpSpaceIfSVM)
{
    unsigned long long flag = MEM_SVM_VAL << MEM_VIRT_BIT;
    MemScope::MemOpSpace result = MemScope::GetMemOpSpace(flag);
    EXPECT_EQ(result, MemScope::MemOpSpace::SVM);
}

TEST_F(EventReportTest, GetMemOpSpaceIfDevice)
{
    unsigned long long flag = MEM_DEV_VAL << MEM_VIRT_BIT;
    MemScope::MemOpSpace result = MemScope::GetMemOpSpace(flag);
    EXPECT_EQ(result, MemScope::MemOpSpace::DEVICE);
}

TEST_F(EventReportTest, GetMemOpSpaceIfHost)
{
    unsigned long long flag = MEM_HOST_VAL << MEM_VIRT_BIT;
    MemScope::MemOpSpace result = MemScope::GetMemOpSpace(flag);
    EXPECT_EQ(result, MemScope::MemOpSpace::HOST);
}

TEST_F(EventReportTest, GetMemOpSpaceIfDvpp)
{
    unsigned long long flag = MEM_DVPP_VAL << MEM_VIRT_BIT;
    MemScope::MemOpSpace result = MemScope::GetMemOpSpace(flag);
    EXPECT_EQ(result, MemScope::MemOpSpace::DVPP);
}

TEST_F(EventReportTest, GetMemOpSpaceIfOverType)
{
    unsigned long long flag = 0x5 << MEM_VIRT_BIT;
    MemScope::MemOpSpace result = MemScope::GetMemOpSpace(flag);
    EXPECT_EQ(result, MemScope::MemOpSpace::INVALID);
}

TEST_F(EventReportTest, GetMemOpSpaceIfOverType1)
{
    unsigned long long flag = 0xF << MEM_VIRT_BIT;
    MemScope::MemOpSpace result = MemScope::GetMemOpSpace(flag);
    EXPECT_EQ(result, MemScope::MemOpSpace::INVALID);
}

TEST_F(EventReportTest, GetMemOpSpaceExpectSuccess)
{
    unsigned long long flag = 0b00100000000000;
    ASSERT_EQ(GetMemOpSpace(flag), MemOpSpace::HOST);
    flag = 0b00110000000000;
    ASSERT_EQ(GetMemOpSpace(flag), MemOpSpace::DVPP);
    flag = 0b11110000000000;
    ASSERT_EQ(GetMemOpSpace(flag), MemOpSpace::INVALID);
}
// ============================================================================
// 退出闭窗(进程退出时host泄漏窗口自动闭窗触发完整分析):
// 主测试进程无钩子装配(BindHostMemHook经桩dlsym解析必失败),svcHostMem_注入
// mock svc表驱动CloseHostMemWindowAtExit的行为契约;等待循环的排空信号分支
// (get_status符号不可得→立即返回)在无钩子环境天然覆盖。钩子侧闭窗→排空→
// STAGE_END链路由子进程套件UT-H2覆盖
// ============================================================================
namespace {
// mock svc表:记录set_enabled调用序列(get_stats/dump_live_blocks桩实现)
std::vector<int> g_exitCloseSetEnabledArgs;
void MockExitCloseSetEnabled(int enabled) { g_exitCloseSetEnabledArgs.push_back(enabled); }
void MockExitCloseGetStats(MsmemscopeHostmemStats* stats)
{
    stats->liveBlockCount = 0;
}
void MockExitCloseDumpLiveBlocks(void (*emit)(void*, uint64_t, uint64_t, uint64_t, uint64_t), void* ctx)
{
    (void)emit;
    (void)ctx;
}
const MsmemscopeHostmemSvc g_exitCloseMockSvc = {MockExitCloseSetEnabled, MockExitCloseGetStats,
                                                 MockExitCloseDumpLiveBlocks};
}  // namespace

// 窗口开着(mock svc在位+闩锁未置)→置stop闩锁+set_enabled(0);重复调用幂等
TEST_F(EventReportTest, ExitCloseHostMemWindow)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    const MsmemscopeHostmemSvc* savedSvc = instance.svcHostMem_;
    const bool savedLatch = instance.hostMemStopLatch_.load();
    g_exitCloseSetEnabledArgs.clear();
    instance.svcHostMem_ = &g_exitCloseMockSvc;
    instance.hostMemStopLatch_.store(false);

    instance.CloseHostMemWindowAtExit();
    EXPECT_TRUE(instance.hostMemStopLatch_.load()) << "stop latch must be set (window closed, no reopen)";
    ASSERT_FALSE(g_exitCloseSetEnabledArgs.empty());
    EXPECT_EQ(g_exitCloseSetEnabledArgs.back(), 0) << "intersection semantics must close the window";

    // 幂等:重复调用行为一致不崩溃
    instance.CloseHostMemWindowAtExit();
    EXPECT_EQ(g_exitCloseSetEnabledArgs.back(), 0);

    instance.svcHostMem_ = savedSvc;
    instance.hostMemStopLatch_.store(savedLatch);
}

// 未bind/已析构早退:无窗口可言,不得触达svc表,闩锁不动
TEST_F(EventReportTest, ExitCloseHostMemWindowSkips)
{
    EventReport& instance = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    const MsmemscopeHostmemSvc* savedSvc = instance.svcHostMem_;
    const bool savedLatch = instance.hostMemStopLatch_.load();
    g_exitCloseSetEnabledArgs.clear();

    // 未bind
    instance.svcHostMem_ = nullptr;
    instance.hostMemStopLatch_.store(false);
    instance.CloseHostMemWindowAtExit();
    EXPECT_TRUE(g_exitCloseSetEnabledArgs.empty()) << "unbound must not touch svc";
    EXPECT_FALSE(instance.hostMemStopLatch_.load()) << "unbound must leave latch untouched";

    // 已析构(svcHostMem_有效也不得触达)
    instance.svcHostMem_ = &g_exitCloseMockSvc;
    instance.destroyed_.store(true);
    instance.CloseHostMemWindowAtExit();
    EXPECT_TRUE(g_exitCloseSetEnabledArgs.empty()) << "destroyed must not touch svc";
    instance.destroyed_.store(false);  // 恢复单例状态

    instance.svcHostMem_ = savedSvc;
    instance.hostMemStopLatch_.store(savedLatch);
}
