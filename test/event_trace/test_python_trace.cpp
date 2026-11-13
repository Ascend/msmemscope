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
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include "config_info.h"
#include "record_info.h"
#include "cpython.h"
#include "file.h"
#include "file_write_manager.h"
#include "utils.h"
#include "data_handler.h"
#include "event_report.h"
#include "trace_manager/event_trace_manager.h"
// 用例需要直接检查帧栈/忽略标记/处理器缓存，并直接调用私有 DumpTraceEvent
// 覆盖“非法设备号直接返回”等仅内部可达的分支
#define private public
#include "python_trace.h"
#undef private

using namespace MemScope;

class PythonTraceTest : public ::testing::Test {
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

TEST_F(PythonTraceTest, RecordPyCallTest)
{
    PythonTrace::GetInstance().RecordPyCall("123", "123", 0);
    PythonTrace::GetInstance().RecordPyCall("123:__torch_dispatch__", "123", 0);
}

TEST_F(PythonTraceTest, RecordCCallTest)
{
    PythonTrace::GetInstance().RecordCCall("123", "123");
    PythonTrace::GetInstance().RecordReturn("123:__torch_dispatch__", "123");
}

TEST_F(PythonTraceTest, RecordReturnTest)
{
    PythonTrace::GetInstance().Start();
    PythonTrace::GetInstance().RecordReturn("123", "123");
    PythonTrace::GetInstance().RecordPyCall("123", "123", 0);
    PythonTrace::GetInstance().RecordReturn("123", "123");

    std::string hash;
    std::string info;
    uint64_t timestamp = 0;
    PyTraceType what = PyTraceType::PYCALL;
    callback(hash, info, what, timestamp);
    what = PyTraceType::PYRETURN;
    callback(hash, info, what, timestamp);
    what = PyTraceType::CCALL;
    callback(hash, info, what, timestamp);
    what = PyTraceType::CRETURN;
    callback(hash, info, what, timestamp);
    PythonTrace::GetInstance().Stop();
}

// ============================================================================
// 帧栈/忽略标记检查用例：IsIgnoreRecordFunc 不依赖设备与解释器，可在主进程直接校验
// ============================================================================
TEST_F(PythonTraceTest, IsIgnoreRecordFunc)
{
    PythonTrace& trace = PythonTrace::GetInstance();
    // record_function 自身产生的三类事件都应被忽略
    EXPECT_TRUE(trace.IsIgnoreRecordFunc("msmemscope/record_function.py:1"));
    EXPECT_TRUE(trace.IsIgnoreRecordFunc("record_start of _msmemscope._record_function"));
    EXPECT_TRUE(trace.IsIgnoreRecordFunc("xx_record_end of _msmemscope._record_function_yy"));
    // 普通用户函数不应被忽略
    EXPECT_FALSE(trace.IsIgnoreRecordFunc("tensor.py:__torch_dispatch__"));
    EXPECT_FALSE(trace.IsIgnoreRecordFunc(""));
}

// ============================================================================
// 隔离子进程用例
// python_trace.cpp 的设备号解析（GetDeviceInfo）以函数内 static 缓存 dlsym 结果，
// 同一进程内一旦以“无驱动符号”完成首次解析，成功路径便无法进入；
// Start/Stop 还依赖 Python 解释器，而解释器初始化状态在共享测试进程中不可控。
// 因此成功路径改由 fork+exec 拉起本测试二进制的子进程执行，--gtest_filter 保证
// 子进程只跑目标用例；符号劫持复用 test_kernel_hooks.cpp 中定义的 dlopen/dlsym 桩。
// ============================================================================
extern std::unordered_map<std::string, void *> g_funcMocks;
extern bool g_isDlsymNullptr;
bool RunMemScopeTestChild(const char *gtestFilter);

namespace
{
enum class DeviceMockMode
{
    VALID = 0,        // 成功返回设备0
    INVALID_DEVICE,   // 返回 GD_INVALID_NUM，覆盖“设备号非法”判定
    ERROR,            // 返回错误码，覆盖“GetDeviceId 失败”判定
};

DeviceMockMode g_deviceMockMode = DeviceMockMode::VALID;

// 受限视图（设置ASCEND_RT_VISIBLE_DEVICES）下GetDeviceId按env做范围校验：aclrtGetDevice
// 返回的是0基可见序号而非物理卡号，TransDeviceId以序号为key查visibleDeviceMap；
// 桩固定返回序号0，保证成功路径不受运行环境影响
int32_t MockDeviceIdForRestrictedView()
{
    return 0;
}

aclError MockPythonTraceGetDevice(int32_t *deviceId)
{
    if (deviceId == nullptr)
    {
        return ACL_ERROR_INTERNAL_ERROR;
    }
    switch (g_deviceMockMode)
    {
        case DeviceMockMode::VALID:
            *deviceId = MockDeviceIdForRestrictedView();
            return ACL_SUCCESS;
        case DeviceMockMode::INVALID_DEVICE:
            *deviceId = GD_INVALID_NUM;
            return ACL_SUCCESS;
        default:
            return ACL_ERROR_INTERNAL_ERROR;
    }
}

void InstallPythonTraceDeviceMock()
{
    g_isDlsymNullptr = false;
    g_funcMocks["aclrtGetDeviceImpl"] = reinterpret_cast<void *>(&MockPythonTraceGetDevice);
}

void PinPythonTraceProjectDir(const char *dir)
{
    Utility::FileCreateManager::GetInstance(dir).SetProjectDir(dir);
}

Config MakePythonTraceConfig(const char *outputDir, DataFormat format)
{
    Config config{};
    std::snprintf(config.outputDir, sizeof(config.outputDir), "%s", outputDir);
    config.dataFormat = static_cast<uint8_t>(format);
    return config;
}

bool DirectoryExists(const std::string &path)
{
    return access(path.c_str(), F_OK) == 0;
}

// 子进程用例：CSV 落盘分支 + 帧栈/throw 标记/record_function 栈与 callback 全分支
void RunPythonTraceCsvFlow()
{
    InstallPythonTraceDeviceMock();
    g_deviceMockMode = DeviceMockMode::VALID;

    const char *dir = "./memscope_pytrace_ut_csv";
    PinPythonTraceProjectDir(dir);
    ConfigManager::Instance().SetConfig(MakePythonTraceConfig(dir, DataFormat::CSV));
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);

    PythonTrace &trace = PythonTrace::GetInstance();
    const uint64_t tid = Utility::GetTid();

    int32_t devId = GD_INVALID_NUM;
    ASSERT_TRUE(GetDeviceInfo::Instance().GetDeviceId(devId));
    ASSERT_NE(devId, GD_INVALID_NUM);

    // 解释器未初始化：Stop 在解释器检查处直接返回，活动标志不被清除
    trace.active_.store(true);
    trace.Stop();
    EXPECT_TRUE(trace.IsTraceActive());
    trace.active_.store(false);

    // 设备号获取失败：RecordPyCall/RecordCCall 直接返回；RecordFuncPyCall 仅告警仍入栈
    g_deviceMockMode = DeviceMockMode::ERROR;
    trace.RecordPyCall("mod.py:fail", "info", 0);
    trace.RecordCCall("mod.py:fail", "info");
    trace.RecordFuncPyCall("mod.py:fail", "info", 0);
    EXPECT_TRUE(trace.frameStack_[tid].empty());
    ASSERT_EQ(trace.frameStackRecordFunc_[tid].size(), 1u);
    EXPECT_EQ(trace.frameStackRecordFunc_[tid].top()->device, GD_INVALID_NUM);
    trace.frameStackRecordFunc_[tid].pop();

    // 设备号非法（GD_INVALID_NUM）：RecordPyCall/RecordCCall 同样早退
    g_deviceMockMode = DeviceMockMode::INVALID_DEVICE;
    trace.RecordPyCall("mod.py:invalid", "info", 0);
    trace.RecordCCall("mod.py:invalid", "info");
    EXPECT_TRUE(trace.frameStack_[tid].empty());

    // RecordPyCall 成功路径：timestamp=0 走时间戳兜底，非0 时原样使用
    g_deviceMockMode = DeviceMockMode::VALID;
    trace.RecordPyCall("mod.py:pycall", "py_info", 0);
    ASSERT_EQ(trace.frameStack_[tid].size(), 1u);
    EXPECT_NE(trace.frameStack_[tid].top()->startTs, 0u);
    EXPECT_EQ(trace.frameStack_[tid].top()->device, devId);
    EXPECT_EQ(trace.frameStack_[tid].top()->tid, tid);
    trace.RecordPyCall("mod.py:pycall_ts", "py_info", 123456789ULL);
    ASSERT_EQ(trace.frameStack_[tid].size(), 2u);
    EXPECT_EQ(trace.frameStack_[tid].top()->startTs, 123456789ULL);

    // 不匹配返回且 throw 标记为 false：构造合成事件并落盘，帧栈不变
    trace.RecordReturn("mod.py:other", "other_info");
    EXPECT_EQ(trace.frameStack_[tid].size(), 2u);
    EXPECT_FALSE(trace.throw_[tid]);

    // 不匹配返回的设备号获取失败与非法设备号分支
    g_deviceMockMode = DeviceMockMode::ERROR;
    trace.RecordReturn("mod.py:other", "other_info");
    EXPECT_EQ(trace.frameStack_[tid].size(), 2u);
    g_deviceMockMode = DeviceMockMode::INVALID_DEVICE;
    trace.RecordReturn("mod.py:other", "other_info");
    EXPECT_EQ(trace.frameStack_[tid].size(), 2u);
    g_deviceMockMode = DeviceMockMode::VALID;

    // 匹配返回：置回 throw、填 endTs、落盘并出栈（CSV 分支）
    trace.RecordReturn("mod.py:pycall_ts", "py_info");
    EXPECT_EQ(trace.frameStack_[tid].size(), 1u);
    trace.RecordReturn("mod.py:pycall", "py_info");
    EXPECT_TRUE(trace.frameStack_[tid].empty());
    EXPECT_EQ(trace.handlerMap_.count(devId), 1u);

    // 空栈返回：直接返回，不产生处理器
    trace.RecordReturn("mod.py:empty", "info");
    EXPECT_TRUE(trace.frameStack_[tid].empty());

    // IsIgnore 命中：置 throw 标记，后续调用在 throw 检查处早退
    trace.RecordPyCall("torch.py:__torch_dispatch__", "dispatch_info", 0);
    ASSERT_EQ(trace.frameStack_[tid].size(), 1u);
    EXPECT_TRUE(trace.throw_[tid]);
    trace.RecordPyCall("torch.py:after", "info", 0);
    trace.RecordCCall("torch.py:after", "info");
    EXPECT_EQ(trace.frameStack_[tid].size(), 1u);
    // throw 为 true 时的不匹配返回：两个分支都不进入，栈与标记保持
    trace.RecordReturn("torch.py:other", "info");
    EXPECT_EQ(trace.frameStack_[tid].size(), 1u);
    EXPECT_TRUE(trace.throw_[tid]);
    // 匹配返回：清 throw 并出栈
    trace.RecordReturn("torch.py:__torch_dispatch__", "dispatch_info");
    EXPECT_TRUE(trace.frameStack_[tid].empty());
    EXPECT_FALSE(trace.throw_[tid]);

    // RecordCCall 成功路径
    trace.RecordCCall("mod.py:ccall", "c_info");
    ASSERT_EQ(trace.frameStack_[tid].size(), 1u);
    EXPECT_NE(trace.frameStack_[tid].top()->startTs, 0u);
    trace.RecordReturn("mod.py:ccall", "c_info");
    EXPECT_TRUE(trace.frameStack_[tid].empty());

    // DumpTraceEvent 非法设备号：直接返回，不新增处理器
    std::shared_ptr<TraceEvent> invalidEvent =
        std::make_shared<TraceEvent>(0, 0, tid, Utility::GetPid(), GD_INVALID_NUM, "info", "hash");
    const size_t handlerCount = trace.handlerMap_.size();
    trace.DumpTraceEvent(invalidEvent);
    EXPECT_EQ(trace.handlerMap_.size(), handlerCount);

    // record_function 栈：匹配出栈落盘、不匹配仅告警不出栈、空栈直接返回
    ASSERT_EQ(trace.frameStackRecordFunc_[tid].size(), 0u);
    trace.RecordFuncPyCall("rf.py:call", "rf_info", 0);
    trace.RecordFuncPyCall("rf.py:call_ts", "rf_info", 42);
    ASSERT_EQ(trace.frameStackRecordFunc_[tid].size(), 2u);
    EXPECT_EQ(trace.frameStackRecordFunc_[tid].top()->startTs, 42u);
    trace.RecordFuncReturn("rf.py:other", "rf_info");
    EXPECT_EQ(trace.frameStackRecordFunc_[tid].size(), 2u);
    trace.RecordFuncReturn("rf.py:call_ts", "rf_info");
    EXPECT_EQ(trace.frameStackRecordFunc_[tid].size(), 1u);
    trace.RecordFuncReturn("rf.py:call", "rf_info");
    EXPECT_TRUE(trace.frameStackRecordFunc_[tid].empty());
    trace.RecordFuncReturn("rf.py:empty", "rf_info");
    EXPECT_TRUE(trace.frameStackRecordFunc_[tid].empty());

    // callback：采集态下四种事件类型、忽略过滤与默认分支
    callback("mod.py:cb", "cb_info", PyTraceType::PYCALL, 0);
    ASSERT_EQ(trace.frameStack_[tid].size(), 1u);
    callback("mod.py:cb", "cb_info", PyTraceType::PYRETURN, 0);
    EXPECT_TRUE(trace.frameStack_[tid].empty());
    callback("mod.py:cb_c", "cb_info", PyTraceType::CCALL, 0);
    ASSERT_EQ(trace.frameStack_[tid].size(), 1u);
    callback("mod.py:cb_c", "cb_info", PyTraceType::CRETURN, 0);
    EXPECT_TRUE(trace.frameStack_[tid].empty());
    callback("mod.py:cb", "cb_info", static_cast<PyTraceType>(255), 0);
    callback("msmemscope/record_function.py:1", "cb_info", PyTraceType::PYCALL, 0);
    EXPECT_TRUE(trace.frameStack_[tid].empty());

    // 非采集态：callback 首行早退
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    callback("mod.py:off", "cb_info", PyTraceType::PYCALL, 0);
    EXPECT_TRUE(trace.frameStack_[tid].empty());

    // CSV 处理器已按设备号创建，落盘目录存在
    EXPECT_EQ(trace.handlerMap_.count(devId), 1u);
    const std::string deviceDir = std::string(dir) + "/device_" + std::to_string(devId);
    EXPECT_TRUE(DirectoryExists(deviceDir));
}

// 子进程用例：DB 落盘分支（DumpTraceEvent 中按 dataFormat 走文件锁）
void RunPythonTraceDbFlow()
{
    InstallPythonTraceDeviceMock();
    g_deviceMockMode = DeviceMockMode::VALID;

    const char *dir = "./memscope_pytrace_ut_db";
    PinPythonTraceProjectDir(dir);
    ConfigManager::Instance().SetConfig(MakePythonTraceConfig(dir, DataFormat::DB));
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);

    PythonTrace &trace = PythonTrace::GetInstance();
    const uint64_t tid = Utility::GetTid();

    int32_t devId = GD_INVALID_NUM;
    ASSERT_TRUE(GetDeviceInfo::Instance().GetDeviceId(devId));
    ASSERT_NE(devId, GD_INVALID_NUM);

    trace.RecordPyCall("mod.py:db", "db_info", 0);
    ASSERT_EQ(trace.frameStack_[tid].size(), 1u);
    // 匹配返回触发 DumpTraceEvent 的 DB 分支
    trace.RecordReturn("mod.py:db", "db_info");
    EXPECT_TRUE(trace.frameStack_[tid].empty());

    ASSERT_EQ(trace.handlerMap_.count(devId), 1u);
    EXPECT_TRUE(trace.handlerMap_[devId] != nullptr);
    const std::string deviceDir = std::string(dir) + "/device_" + std::to_string(devId);
    EXPECT_TRUE(DirectoryExists(deviceDir));
}

// 子进程用例：解释器初始化后的 Start/Stop 成功路径与 CAS 重复启动分支
void RunPythonTraceStartStopFlow()
{
    InstallPythonTraceDeviceMock();
    g_deviceMockMode = DeviceMockMode::VALID;

    const char *dir = "./memscope_pytrace_ut_start";
    PinPythonTraceProjectDir(dir);
    ConfigManager::Instance().SetConfig(MakePythonTraceConfig(dir, DataFormat::CSV));
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);

    Py_Initialize();
    ASSERT_NE(Py_IsInitialized(), 0);

    PythonTrace &trace = PythonTrace::GetInstance();
    const uint64_t tid = Utility::GetTid();
    const bool versionSupported = !(Utility::GetPyVersion() < Utility::Version("3.9"));

    trace.Start();
    // 版本低于3.9时 Start 在版本检查处返回，不会激活
    EXPECT_EQ(trace.IsTraceActive(), versionSupported);
    // 重复 Start：CAS 失败告警分支，状态仍为激活
    trace.Start();
    EXPECT_EQ(trace.IsTraceActive(), versionSupported);

    if (!versionSupported)
    {
        trace.Stop();
        EXPECT_FALSE(trace.IsTraceActive());
        return;
    }

    int32_t devId = GD_INVALID_NUM;
    ASSERT_TRUE(GetDeviceInfo::Instance().GetDeviceId(devId));
    ASSERT_NE(devId, GD_INVALID_NUM);

    // 压入一个未返回的帧，Stop 负责注销回调、残留帧落盘并清空帧栈
    trace.RecordPyCall("mod.py:pending", "pending_info", 0);
    ASSERT_EQ(trace.frameStack_[tid].size(), 1u);
    trace.Stop();
    EXPECT_FALSE(trace.IsTraceActive());
    EXPECT_TRUE(trace.frameStack_[tid].empty());
    EXPECT_EQ(trace.handlerMap_.count(devId), 1u);

    // 未启动时再次 Stop：告警分支
    trace.Stop();
    EXPECT_FALSE(trace.IsTraceActive());
}
}  // namespace

TEST(PythonTraceChild, CsvAndStackBranches)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    RunPythonTraceCsvFlow();
}

TEST(PythonTraceChild, DbDumpBranch)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    RunPythonTraceDbFlow();
}

TEST(PythonTraceChild, StartStopWithInterpreter)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    RunPythonTraceStartStopFlow();
}

// 父进程驱动用例：拉起隔离子进程并断言其正常退出（退出码非0说明子用例失败）
TEST(PythonTraceIsolation, CsvChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("PythonTraceChild.CsvAndStackBranches"));
}

TEST(PythonTraceIsolation, DbChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("PythonTraceChild.DbDumpBranch"));
}

TEST(PythonTraceIsolation, StartStopChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("PythonTraceChild.StartStopWithInterpreter"));
}
