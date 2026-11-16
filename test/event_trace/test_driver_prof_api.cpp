/* -------------------------------------------------------------------------
 * This file is part of the MindStudio project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 *
 * MindStudio is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
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
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

#include "event_trace/ascend_hal.h"
#include "event_trace/event_report.h"
#include "event_trace/kernel_hooks/driver_prof_api.h"
#include "event_trace/kernel_hooks/runtime_prof_api.h"

// 全局符号桩表与 dlsym 空指针开关定义在 test/event_trace/test_kernel_hooks.cpp
extern std::unordered_map<std::string, void *> g_funcMocks;
extern bool g_isDlsymNullptr;

namespace MemScope
{
// driver_prof_api.cpp 内部的全局时间基准，用例直接校验其取值
extern DevTimeInfo g_devTimeInfo;

namespace
{
// 驱动的可切换桩状态：一次进程内 g_socDetectFlag 只会真正探测一次 SoC 类型，
// 因此默认按 950(A5) 返回，保证探测路径被走到
struct DriverProfMockState
{
    int socNameCase = 950;  // 950/960/其他(返回 nullptr)
    bool oscQueryFail = false;
    int64_t oscFreq = 50;
    int64_t versionChipId = 5;  // PlatformType::CHIP_910B
    int64_t sysCnt = 1000;
    int profStartRet = 0;
    int profStopRet = 0;
    int startCalls = 0;
    int stopCalls = 0;
    unsigned int lastStartDevId = 0;
    unsigned int lastStartChannel = 0;
    unsigned int lastStartSamplePeriod = 0;
    unsigned int lastStartUserDataSize = 0;
    void *lastStartUserData = nullptr;
    unsigned int lastStopDevId = 0;
    unsigned int lastStopChannel = 0;
};

DriverProfMockState g_driverProfMock;

const char *MockAclrtGetSocName()
{
    if (g_driverProfMock.socNameCase == 950)
    {
        return "Ascend950PR";
    }
    if (g_driverProfMock.socNameCase == 960)
    {
        return "Ascend960";
    }
    return nullptr;
}

tagDrvError MockHalGetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t *value)
{
    (void)deviceId;
    if (moduleType != DRV_MODULE_TYPE_SYSTEM || value == nullptr)
    {
        return DRV_ERROR_NOT_SUPPORT;
    }
    switch (infoType)
    {
        case DRV_INFO_TYPE_VERSION:
            *value = g_driverProfMock.versionChipId << 8;
            return DRV_ERROR_NONE;
        case DRV_INFO_TYPE_DEV_OSC_FREQUE:
            if (g_driverProfMock.oscQueryFail)
            {
                return DRV_ERROR_NOT_SUPPORT;
            }
            *value = g_driverProfMock.oscFreq;
            return DRV_ERROR_NONE;
        case DRV_INFO_TYPE_SYS_COUNT:
            *value = g_driverProfMock.sysCnt;
            return DRV_ERROR_NONE;
        default:
            return DRV_ERROR_NOT_SUPPORT;
    }
}

int MockProfDrvStart(unsigned int devId, unsigned int channel, struct ProfStartPara *para)
{
    ++g_driverProfMock.startCalls;
    g_driverProfMock.lastStartDevId = devId;
    g_driverProfMock.lastStartChannel = channel;
    if (para != nullptr)
    {
        g_driverProfMock.lastStartSamplePeriod = para->samplePeriod;
        g_driverProfMock.lastStartUserDataSize = para->userDataSize;
        g_driverProfMock.lastStartUserData = para->userData;
    }
    return g_driverProfMock.profStartRet;
}

int MockProfStop(unsigned int devId, unsigned int channel)
{
    ++g_driverProfMock.stopCalls;
    g_driverProfMock.lastStopDevId = devId;
    g_driverProfMock.lastStopChannel = channel;
    return g_driverProfMock.profStopRet;
}

// 在所有用例之前注入驱动符号桩，保证 GetSymbol 首次取值（内部 static 缓存）即命中桩函数
class DriverProfMockEnvironment : public ::testing::Environment
{
   public:
    void SetUp() override
    {
        g_isDlsymNullptr = false;
        g_funcMocks["aclrtGetSocName"] = reinterpret_cast<void *>(&MockAclrtGetSocName);
        g_funcMocks["halGetDeviceInfo"] = reinterpret_cast<void *>(&MockHalGetDeviceInfo);
        g_funcMocks["prof_drv_start"] = reinterpret_cast<void *>(&MockProfDrvStart);
        g_funcMocks["prof_stop"] = reinterpret_cast<void *>(&MockProfStop);
        // 提前构造 GetDeviceInfo 单例：其析构注册必须早于用例中 atexit(EndDriverKernelInfoTrace)，
        // 保证进程退出时 atexit 回调先于单例析构执行，避免回调访问已析构的成员
        (void)GetDeviceInfo::Instance();
    }
};

::testing::Environment *const g_driverProfMockEnv =
    ::testing::AddGlobalTestEnvironment(new DriverProfMockEnvironment());

constexpr const char *DRIVER_PROF_CHILD_ENV = "MSMEMSCOPE_DRIVER_PROF_CHILD";

bool InDriverProfChild()
{
    return getenv(DRIVER_PROF_CHILD_ENV) != nullptr;
}

// fork+exec 当前测试二进制，仅运行指定用例，为一次性 once_flag / 函数内 static 符号缓存提供独立进程
bool RunDriverProfTestChild(const char *gtestFilter)
{
    char exePath[4096] = {0};
    const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0)
    {
        return false;
    }
    exePath[len] = '\0';
    const pid_t pid = fork();
    if (pid < 0)
    {
        return false;
    }
    if (pid == 0)
    {
        setenv(DRIVER_PROF_CHILD_ENV, "1", 1);
        const std::string filterArg = std::string("--gtest_filter=") + gtestFilter;
        execl(exePath, exePath, filterArg.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    for (int i = 0; i < 200; ++i)
    {
        const pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid)
        {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return false;
}
}  // namespace

// SoC 类型探测：名称含 950 时识别为 A5（进程内只探测一次，由全局环境保证桩先于其他用例生效）
TEST(DriverProfApiTest, get_device_soc_type_detects_a5)
{
    g_isDlsymNullptr = false;
    EXPECT_EQ(GetDeviceSocType(), DeviceSocType::SOC_A5);
}

// StartDriverKernelInfoTrace：驱动接口启动成功/失败均走到返回值处理，A5 使用完整配置大小
TEST(DriverProfApiTest, start_driver_kernel_info_trace_success_and_failure)
{
    g_isDlsymNullptr = false;
    g_driverProfMock.oscQueryFail = false;
    g_driverProfMock.oscFreq = 50;
    g_driverProfMock.profStartRet = 0;

    int before = g_driverProfMock.startCalls;
    StartDriverKernelInfoTrace(0);

    EXPECT_EQ(g_driverProfMock.startCalls, before + 1);
    EXPECT_EQ(g_driverProfMock.lastStartDevId, 0u);
    EXPECT_EQ(g_driverProfMock.lastStartChannel, static_cast<unsigned int>(PROF_CHANNEL_STARS_SOC_LOG));
    EXPECT_EQ(g_driverProfMock.lastStartSamplePeriod, 20u);
    EXPECT_NE(g_driverProfMock.lastStartUserData, nullptr);
    // A5/A6 分支下发完整 StarsSocLogConfigT，不裁剪 tag/block_shrink_flag 字段
    EXPECT_EQ(g_driverProfMock.lastStartUserDataSize,
              static_cast<unsigned int>(sizeof(StarsSocLogConfigT)));

    g_driverProfMock.profStartRet = -1;
    before = g_driverProfMock.startCalls;
    StartDriverKernelInfoTrace(0);
    EXPECT_EQ(g_driverProfMock.startCalls, before + 1);

    g_driverProfMock.profStartRet = 0;
}

// GetDevFreq：驱动频率查询失败时按版本号解析芯片类型，覆盖合法芯片号与越界芯片号两条路径
TEST(DriverProfApiTest, get_chip_type_from_version_info)
{
    g_isDlsymNullptr = false;
    g_driverProfMock.oscQueryFail = true;

    // chipId=5 对应 CHIP_910B，命中 FREQ_MAP 的默认频率
    g_driverProfMock.versionChipId = 5;
    StartDriverKernelInfoTrace(0);
    EXPECT_EQ(g_devTimeInfo.freq, 50u);

    // chipId=9 超出 PlatformType 范围，返回 END_TYPE 后回退默认频率
    g_driverProfMock.versionChipId = 9;
    StartDriverKernelInfoTrace(0);
    EXPECT_EQ(g_devTimeInfo.freq, 50u);

    g_driverProfMock.oscQueryFail = false;
    g_driverProfMock.versionChipId = 5;
}

// GetRealTimeFromSysCnt：频率为 0 时告警并返回 0；频率有效时按基准点换算
TEST(DriverProfApiTest, get_real_time_from_sys_cnt_zero_and_positive_freq)
{
    g_isDlsymNullptr = false;
    g_driverProfMock.oscQueryFail = false;
    g_driverProfMock.sysCnt = 1000;

    g_driverProfMock.oscFreq = 0;
    StartDriverKernelInfoTrace(0);
    EXPECT_EQ(g_devTimeInfo.freq, 0u);
    EXPECT_EQ(GetRealTimeFromSysCnt(0, 2000), 0u);

    g_driverProfMock.oscFreq = 100;
    StartDriverKernelInfoTrace(0);
    ASSERT_EQ(g_devTimeInfo.freq, 100u);
    // sysCnt 取基准点：换算结果等于 startRealTime，验证公式基准项
    EXPECT_EQ(GetRealTimeFromSysCnt(0, g_devTimeInfo.startSysCnt), g_devTimeInfo.startRealTime);
}

// EndDriverKernelInfoTrace：prof_stop 成功与失败均覆盖
TEST(DriverProfApiTest, end_driver_kernel_info_trace_success_and_failure)
{
    g_isDlsymNullptr = false;
    g_driverProfMock.profStopRet = 0;
    int before = g_driverProfMock.stopCalls;
    EndDriverKernelInfoTrace();
    EXPECT_EQ(g_driverProfMock.stopCalls, before + 1);
    EXPECT_EQ(g_driverProfMock.lastStopChannel, static_cast<unsigned int>(PROF_CHANNEL_STARS_SOC_LOG));

    g_driverProfMock.profStopRet = -1;
    before = g_driverProfMock.stopCalls;
    EndDriverKernelInfoTrace();
    EXPECT_EQ(g_driverProfMock.stopCalls, before + 1);

    g_driverProfMock.profStopRet = 0;
}

// 子进程用例：SoC 名称为空时保留默认 A2/A3；960 名称识别为 A6。
// once_flag 使同一进程只能探测一次，故用独立子进程分别覆盖两条分支
TEST(DriverProfApiTest, child_soc_name_branches)
{
    EXPECT_TRUE(RunDriverProfTestChild("DriverProfApiChild.SocNameNull"));
    EXPECT_TRUE(RunDriverProfTestChild("DriverProfApiChild.SocName960"));
}

// 子进程用例：驱动符号全部解析失败时，各接口走空桩回退分支（A2/A3 配置大小裁剪）
TEST(DriverProfApiTest, child_null_driver_symbols_fallbacks)
{
    EXPECT_TRUE(RunDriverProfTestChild("DriverProfApiChild.NullDriverSymbols"));
}

// 仅在 RunDriverProfTestChild 拉起的子进程中执行
TEST(DriverProfApiChild, SocNameNull)
{
    if (!InDriverProfChild())
    {
        GTEST_SKIP() << "仅在RunDriverProfTestChild拉起的子进程中执行";
    }
    g_isDlsymNullptr = false;
    g_driverProfMock.socNameCase = 0;
    EXPECT_EQ(GetDeviceSocType(), DeviceSocType::SOC_A2_A3);
}

TEST(DriverProfApiChild, SocName960)
{
    if (!InDriverProfChild())
    {
        GTEST_SKIP() << "仅在RunDriverProfTestChild拉起的子进程中执行";
    }
    g_isDlsymNullptr = false;
    g_driverProfMock.socNameCase = 960;
    EXPECT_EQ(GetDeviceSocType(), DeviceSocType::SOC_A6);
}

// 符号解析为空的回退路径：halGetDeviceInfo / prof_drv_start / prof_stop 均取到 nullptr，
// 且 SoC 探测保持默认 A2/A3，触发 userDataSize 裁剪分支
TEST(DriverProfApiChild, NullDriverSymbols)
{
    if (!InDriverProfChild())
    {
        GTEST_SKIP() << "仅在RunDriverProfTestChild拉起的子进程中执行";
    }
    g_isDlsymNullptr = true;
    g_driverProfMock.oscQueryFail = false;

    StartDriverKernelInfoTrace(0);
    // 频率查询失败后回退默认值 50，说明走了空桩回退而不是真实驱动
    EXPECT_EQ(g_devTimeInfo.freq, 50u);
    EXPECT_EQ(g_driverProfMock.startCalls, 0);

    EndDriverKernelInfoTrace();
    EXPECT_EQ(g_driverProfMock.stopCalls, 0);
}

}  // namespace MemScope
