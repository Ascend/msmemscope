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
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include "event_trace/event_report.h"
#include "event_trace/kernel_hooks/acl_hooks.h"
#include "event_trace/kernel_hooks/driver_prof_api.h"
#include "event_trace/kernel_hooks/kernel_event_trace.h"
#include "event_trace/kernel_hooks/runtime_prof_api.h"
#include "event_trace/kernel_hooks/stars_common.h"
#include "ascend_hal.h"
#include "bit_field.h"
#include "config_info.h"
#include "file.h"
#include "trace_manager/event_trace_manager.h"
#include "utils.h"

using namespace MemScope;

TEST(TestKernelExcuteTrace, TestKernelExcuteTraceNormal)
{
    uint32_t deviceId = 0;
    uint64_t sysCnt = 0;
    uint16_t streamId = 1;
    uint16_t taskId = 2;
    StartDriverKernelInfoTrace(static_cast<int32_t>(deviceId));
    RegisterRtProfileCallback();

    uint32_t agingFlag = 1;
    int value = 42;
    const void* data = static_cast<const void*>(&value);
    uint32_t length = sizeof(struct MsprofCompactInfo);
    CompactInfoReporterCallbackImpl(agingFlag, data, length);

    GetRealTimeFromSysCnt(deviceId, sysCnt);

    // aclnn下发
    auto taskKey = std::make_tuple(static_cast<uint16_t>(deviceId), streamId, taskId);

    std::string hashInfo = "add";
    uint64_t hashId = GetHashIdCallBackImply(hashInfo.c_str(), hashInfo.size());
    RuntimeKernelLinker::GetInstance().RuntimeTaskInfoLaunch(taskKey, hashId);

    std::string name = "add";
    uint64_t startTime = 123;
    uint64_t endTime = 1234;
    // kernel下发
    RuntimeKernelLinker::GetInstance().KernelLaunch();

    // kernel执行
    KernelEventTrace::GetInstance().KernelStartExcute(taskKey, startTime);
    KernelEventTrace::GetInstance().KernelEndExcute(taskKey, endTime);
    EndDriverKernelInfoTrace();
}

TEST(TestKernelExcuteTrace, TestCompactInfoReporterCallbackImpl)
{
    uint32_t agingFlag = 1;
    const void* data = nullptr;
    uint32_t length = sizeof(struct MsprofCompactInfo);
    CompactInfoReporterCallbackImpl(agingFlag, data, length);

    MsprofCompactInfo value;
    value.level = MSPROF_REPORT_RUNTIME_LEVEL;
    value.type = RT_PROFILE_TYPE_TASK_TRACK;
    const void* data1 = static_cast<const void*>(&value);
    CompactInfoReporterCallbackImpl(agingFlag, data1, length);
}

TEST(TestKernelExcuteTrace, TestGetHashIdCallBackImply)
{
    const char* hashInfo  = nullptr;
    uint32_t length = sizeof(struct MsprofCompactInfo);
    GetHashIdCallBackImply(hashInfo, length);
}

TEST(TestKernelExcuteTrace, TestGetStreamIdFuncAbnormalInput)
{
    uint16_t streamId = STREAM_JUDGE_BIT12_OPERATOR;
    uint16_t taskId = 1;
    StarsCommon::GetStreamId(streamId, taskId);
    streamId = STREAM_JUDGE_BIT13_OPERATOR;
    StarsCommon::GetStreamId(streamId, taskId);
}

TEST(TestKernelExcuteTrace, TestGetTaskIdFuncAbnormalInput)
{
    uint16_t streamId = STREAM_JUDGE_BIT12_OPERATOR;
    uint16_t taskId = 1;
    StarsCommon::GetTaskId(streamId, taskId);
    streamId = STREAM_JUDGE_BIT13_OPERATOR;
    StarsCommon::GetTaskId(streamId, taskId);
}

// ============================================================================
// 驱动接口桩与隔离子进程用例
// kernel_event_trace.cpp 依赖的驱动符号（aclrtGetSocName/halGetDeviceInfo/
// profSetProfCommand/prof_drv_start/prof_stop/prof_channel_read）都由函数内 static
// 或 std::call_once 缓存，且 SoC 检测结果、设备时间信息均为进程级单次初始化；
// 读取驱动SOC日志的后台线程也只在 StartKernelEventTrace 之后存在。
// 为使“A2/A3”、“A5/A6”两种解析路径都获得干净的首次初始化时机，用例通过
// fork+exec 拉起本测试二进制，并以 --gtest_filter 只运行目标子用例；
// 符号劫持复用 test_kernel_hooks.cpp 中定义的 dlopen/dlsym 桩。
// ============================================================================
extern std::unordered_map<std::string, void *> g_funcMocks;  // NOLINT
extern bool g_isDlsymNullptr;
bool RunMemScopeTestChild(const char *gtestFilter);

namespace
{
std::atomic<int> g_readCalls{0};
std::atomic<int> g_profStartCalls{0};
std::atomic<int> g_profStopCalls{0};
std::atomic<int> g_profCommandCalls{0};
std::atomic<int> g_lastDevId{-1};
std::atomic<int> g_lastChannelId{-1};
bool g_childSocA5 = false;

// 设备侧上报的一条task记录的原始字段：
// 非扩容状态下 streamId=1、taskId=2、sqeType=0 解析为 (streamId=1, taskId=2)；
// A5/A6结构直接取 taskId 且 streamId 恒为0，故两族命中同一个 (device,1,2)/(device,0,2) 关联键
constexpr uint16_t SOC_LOG_RAW_STREAM_ID = 1;
constexpr uint16_t SOC_LOG_RAW_TASK_ID = 2;
constexpr uint32_t SOC_LOG_BEGIN_SYS_CNT = 3000;
constexpr uint32_t SOC_LOG_END_SYS_CNT = 4000;

size_t SocLogSize()
{
    return g_childSocA5 ? sizeof(StarsSocLogA5A6) : sizeof(StarsSocLog);
}

void FillSocLog(char *buffer, uint32_t funcType, uint32_t sysCnt)
{
    if (g_childSocA5)
    {
        StarsSocLogA5A6 log{};
        log.funcType = funcType;
        log.taskId = SOC_LOG_RAW_TASK_ID;
        log.timestamp = sysCnt;
        std::memcpy(buffer, &log, sizeof(log));
        return;
    }
    StarsSocLog log{};
    log.funcType = funcType;
    log.sqeType = 0;
    log.streamId = SOC_LOG_RAW_STREAM_ID;
    log.taskId = SOC_LOG_RAW_TASK_ID;
    log.sysCntL = sysCnt;
    log.sysCntH = 0;
    std::memcpy(buffer, &log, sizeof(log));
}

// 驱动侧真函数桩：SoC名决定 GetDeviceSocType 的解析结果
const char *MockGetSocName()
{
    return g_childSocA5 ? "Ascend950PR" : "Ascend910B";
}

// 受限视图（设置ASCEND_RT_VISIBLE_DEVICES）下GetDeviceId按env做范围校验：aclrtGetDevice
// 返回的是0基可见序号而非物理卡号，TransDeviceId以序号为key查visibleDeviceMap；
// 桩固定返回序号0，保证成功路径不受运行环境影响
int32_t MockDeviceIdForRestrictedView()
{
    return 0;
}

// 运行时接口桩：设备号取env首个可见编号
aclError MockAclrtGetDevice(int32_t *deviceId)
{
    if (deviceId != nullptr)
    {
        *deviceId = MockDeviceIdForRestrictedView();
    }
    return ACL_SUCCESS;
}

// 驱动信息桩：提供振荡器频率与系统计数，供 InitDevTimeInfo 计算设备时间基准
drvError_t MockHalGetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t *value)
{
    if (moduleType != DRV_MODULE_TYPE_SYSTEM || value == nullptr)
    {
        return DRV_ERROR_NOT_SUPPORT;
    }
    if (infoType == DRV_INFO_TYPE_DEV_OSC_FREQUE)
    {
        *value = 50;
        return DRV_ERROR_NONE;
    }
    if (infoType == DRV_INFO_TYPE_SYS_COUNT)
    {
        *value = 1000;
        return DRV_ERROR_NONE;
    }
    return DRV_ERROR_NOT_SUPPORT;
}

int32_t MockProfSetProfCommand(void *command, uint32_t commandSize)
{
    ++g_profCommandCalls;
    return 0;
}

int MockProfDrvStart(unsigned int devId, unsigned int channelId, ProfStartParaT *para)
{
    ++g_profStartCalls;
    return 0;
}

int MockProfStop(unsigned int devId, unsigned int channelId)
{
    ++g_profStopCalls;
    return 0;
}

// 驱动日志读取桩：按调用次序返回“无数据”、“一整条BEGIN”、“一整条END+半条残留”、
// “非法长度”，随后持续无数据，用于覆盖读取线程的各分支
int MockProfChannelRead(unsigned int devId, unsigned int channelId, char *buffer, unsigned int bufferSize)
{
    const int callIndex = g_readCalls.fetch_add(1);
    g_lastDevId.store(static_cast<int>(devId));
    g_lastChannelId.store(static_cast<int>(channelId));
    if (buffer == nullptr)
    {
        return -1;
    }
    if (callIndex == 1)
    {
        FillSocLog(buffer, STARS_FUNC_TYPE_BEGIN, SOC_LOG_BEGIN_SYS_CNT);
        return static_cast<int>(SocLogSize());
    }
    if (callIndex == 2)
    {
        FillSocLog(buffer, STARS_FUNC_TYPE_END, SOC_LOG_END_SYS_CNT);
        std::memset(buffer + SocLogSize(), 0, SocLogSize() / 2);
        return static_cast<int>(SocLogSize() + SocLogSize() / 2);
    }
    if (callIndex == 3)
    {
        // 返回长度等于剩余可用空间，触发“驱动返回非法长度”分支
        return static_cast<int>(bufferSize);
    }
    return -1;
}

void InstallKernelDriverMocks()
{
    g_funcMocks["aclrtGetSocName"] = reinterpret_cast<void *>(&MockGetSocName);
    g_funcMocks["aclrtGetDeviceImpl"] = reinterpret_cast<void *>(&MockAclrtGetDevice);
    g_funcMocks["halGetDeviceInfo"] = reinterpret_cast<void *>(&MockHalGetDeviceInfo);
    g_funcMocks["profSetProfCommand"] = reinterpret_cast<void *>(&MockProfSetProfCommand);
    g_funcMocks["prof_drv_start"] = reinterpret_cast<void *>(&MockProfDrvStart);
    g_funcMocks["prof_stop"] = reinterpret_cast<void *>(&MockProfStop);
    g_funcMocks["prof_channel_read"] = reinterpret_cast<void *>(&MockProfChannelRead);
}

Config MakeKernelTraceConfig(uint8_t collectMode)
{
    Config config{};
    BitField<decltype(config.eventType)> eventBits(config.eventType);
    eventBits.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBits.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBits.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.eventType = eventBits.getValue();
    BitField<decltype(config.levelType)> levelBits(config.levelType);
    levelBits.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    levelBits.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    config.levelType = levelBits.getValue();
    // 打开watch开关，覆盖 RuntimeTaskInfoLaunch 中 MemoryWatch 开始监控的分支
    config.watchConfig.isWatched = true;
    config.collectMode = collectMode;
    // 子进程内SetConfig会把配置落盘：相对outputDir避免空路径派生根目录
    // "/msmemscope_<pid>_<date>_ascend"（非root CI刷错误日志/root CI产生残留目录）
    std::snprintf(config.outputDir, sizeof(config.outputDir), "%s", "./memscope_ut_kernel");
    return config;
}

void RunKernelSocLogFlow(bool isA5)
{
    g_childSocA5 = isA5;
    g_isDlsymNullptr = false;
    InstallKernelDriverMocks();
    Utility::FileCreateManager::GetInstance("./memscope_ut_kernel").SetProjectDir("./memscope_ut_kernel");

    RuntimeKernelLinker &linker = RuntimeKernelLinker::GetInstance();
    KernelEventTrace &trace = KernelEventTrace::GetInstance();

    // 先解析本进程设备号，并以同一设备号登记关联键：StartKernelEventTrace 与驱动SOC
    // 日志使用该设备号，避免受限视图下设备号置换造成键不一致
    int32_t deviceId = GD_INVALID_NUM;
    ASSERT_TRUE(GetDeviceInfo::Instance().GetDeviceId(deviceId));
    ASSERT_NE(deviceId, GD_INVALID_NUM);
    const uint16_t keyDev = static_cast<uint16_t>(deviceId);
    const uint16_t otherDev = (keyDev == 0) ? static_cast<uint16_t>(1) : static_cast<uint16_t>(0);
    TaskKey key = std::make_tuple(keyDev, static_cast<uint16_t>(1), static_cast<uint16_t>(2));

    // 当前线程尚未下发任何kernel：找不到线程记录，直接返回
    linker.RuntimeTaskInfoLaunch(key, 2026);
    // hashId为0：提前返回
    linker.RuntimeTaskInfoLaunch(key, 0);

    // 未采集状态：KernelLaunch/KernelStartExcute/KernelEndExcute 的采集使能早退分支
    ConfigManager::Instance().SetConfig(MakeKernelTraceConfig(static_cast<uint8_t>(CollectMode::DEFERRED)));
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    AclnnKernelMapInfo offInfo{0, key, "kernel_off"};
    trace.KernelLaunch(offInfo);
    linker.KernelLaunch();
    linker.SetHashInfo(1001, "off_add");
    linker.RuntimeTaskInfoLaunch(key, 1001);
    trace.KernelStartExcute(key, 1);
    trace.KernelEndExcute(key, 2);  // END 命中并删除关联记录

    // 切换为采集态
    ConfigManager::Instance().SetConfig(MakeKernelTraceConfig(static_cast<uint8_t>(CollectMode::IMMEDIATE)));
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);

    AclnnKernelMapInfo launchInfo{0, key, "kernel_add"};
    trace.KernelLaunch(launchInfo);  // 采集态下的kernel下发上报

    linker.SetHashInfo(2026, "kernel_add");
    EXPECT_EQ(linker.GetHashInfo(2026), "kernel_add");
    linker.SetHashInfo(2026, "kernel_add_dup");  // 已存在的hash不覆盖
    EXPECT_EQ(linker.GetHashInfo(2026), "kernel_add");
    EXPECT_EQ(linker.GetHashInfo(4040), "");

    linker.KernelLaunch();
    linker.RuntimeTaskInfoLaunch(key, 2026);
    const std::string expectName =
        std::to_string(keyDev) + "_" + std::to_string(Utility::GetTid()) + "/kernel_add";
    EXPECT_EQ(linker.GetLastKernelName(Utility::GetTid()), expectName);

    TaskKey probeKey = key;
    EXPECT_EQ(linker.GetKernelName(probeKey, RecordSubType::KERNEL_START), "kernel_add");
    trace.KernelStartExcute(key, 123);  // 采集态命中：上报kernel开始执行
    trace.KernelEndExcute(key, 456);    // 采集态命中并删除关联记录
    EXPECT_EQ(linker.GetKernelName(probeKey, RecordSubType::KERNEL_START), "");

    // 关联记录已被END删除：空记录时提前返回 / 未命中时提前返回
    linker.RuntimeTaskInfoLaunch(key, 2026);
    trace.KernelStartExcute(key, 789);

    // 重新登记一条记录：用于覆盖“键不匹配跳过”的分支，并作为SOC日志的关联目标
    linker.KernelLaunch();
    linker.RuntimeTaskInfoLaunch(key, 2026);

    if (isA5)
    {
        // A5/A6只比较device与taskId：streamId不一致也能命中并回填，device不一致则跳过
        TaskKey streamProbe = std::make_tuple(keyDev, static_cast<uint16_t>(77), static_cast<uint16_t>(2));
        EXPECT_EQ(linker.GetKernelName(streamProbe, RecordSubType::KERNEL_START), "kernel_add");
        EXPECT_EQ(std::get<1>(streamProbe), static_cast<uint16_t>(1));
        TaskKey deviceProbe = std::make_tuple(otherDev, static_cast<uint16_t>(0), static_cast<uint16_t>(2));
        EXPECT_EQ(linker.GetKernelName(deviceProbe, RecordSubType::KERNEL_START), "");
        TaskKey endProbe = std::make_tuple(keyDev, static_cast<uint16_t>(55), static_cast<uint16_t>(2));
        EXPECT_EQ(linker.GetKernelName(endProbe, RecordSubType::KERNEL_END), "kernel_add");
        // END已删除记录，重新登记供驱动SOC日志线程使用
        linker.KernelLaunch();
        linker.RuntimeTaskInfoLaunch(key, 2026);
    }
    else
    {
        // A2/A3按完整键比较：streamId不一致时跳过
        TaskKey mismatchProbe = std::make_tuple(keyDev, static_cast<uint16_t>(1), static_cast<uint16_t>(3));
        EXPECT_EQ(linker.GetKernelName(mismatchProbe, RecordSubType::KERNEL_START), "");
    }

    // 启动驱动信息采集与SOC日志读取线程：读取桩按脚本产出BEGIN/END后，关联记录被END删除
    trace.StartKernelEventTrace();
    EXPECT_GE(g_profStartCalls.load(), 1);
    EXPECT_EQ(g_profCommandCalls.load(), 1);
    // InitDevTimeInfo 已用驱动桩填好频率与起始系统计数，换算结果非0
    EXPECT_NE(GetRealTimeFromSysCnt(0, 2000), 0);

    for (int i = 0; i < 400 && g_readCalls.load() < 4; ++i)
    {
        usleep(5000);
    }
    EXPECT_GE(g_readCalls.load(), 4);
    EXPECT_EQ(g_lastDevId.load(), static_cast<int>(deviceId));
    EXPECT_EQ(g_lastChannelId.load(), static_cast<int>(PROF_CHANNEL_STARS_SOC_LOG));

    TaskKey erasedProbe = key;
    EXPECT_EQ(linker.GetKernelName(erasedProbe, RecordSubType::KERNEL_START), "");

    trace.EndKernelEventTrace();
    EXPECT_GE(g_profStopCalls.load(), 1);
}
}  // namespace

// 子进程用例：A2/A3 平台的SOC日志解析与关联（完整键比较）
TEST(KernelExcuteChild, SocA2A3)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    RunKernelSocLogFlow(false);
}

// 子进程用例：A5/A6 平台的SOC日志解析与关联（仅比较device/taskId）
TEST(KernelExcuteChild, SocA5A6)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    RunKernelSocLogFlow(true);
}

// 父进程驱动用例：拉起隔离子进程并断言其正常退出（退出码非0说明子用例失败）
TEST(KernelExcuteTraceIsolation, SocA2A3ChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("KernelExcuteChild.SocA2A3"));
}

TEST(KernelExcuteTraceIsolation, SocA5A6ChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("KernelExcuteChild.SocA5A6"));
}