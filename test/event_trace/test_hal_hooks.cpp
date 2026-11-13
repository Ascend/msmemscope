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
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include "event_trace/hal_hooks/hal_hooks.h"
#include "event_trace/event_report.h"
#include "bit_field.h"
#include "config_info.h"
#include "file.h"
#include "trace_manager/event_trace_manager.h"

TEST(HalHooksTest, GetMallocMemTypeTest) {
    unsigned long long flag = 2377900603261207558;
    MemScope::MemOpSpace result = MemScope::GetMemOpSpace(flag);
    EXPECT_EQ(result, MemScope::MemOpSpace::DEVICE);
}


TEST(HalHooksTest, HalMemAllocTest) {
    void* ptr = nullptr;               // 指针初始化为空指针
    unsigned long long size = 1024;    // 分配的大小
    unsigned long long flag = 0x1234;  // 测试 flag

    drvError_t result = halMemAlloc(&ptr, size, flag);
    EXPECT_EQ(result, DRV_ERROR_NOT_SUPPORT);

    halMemFree(ptr);
}

// 3. 测试 halMemFree 函数
TEST(HalHooksTest, HalMemFreeTest) {
    void* ptr = malloc(1024); // 手动分配内存，以便测试 halMemFree

    drvError_t result = halMemFree(ptr);
    EXPECT_EQ(result, DRV_ERROR_NOT_SUPPORT);
}

// 测试新的驱动接口
TEST(HalHooksTest, HalMemCreate)
{
    drv_mem_handle_t* ptr = nullptr;
    drv_mem_prop *prop = nullptr;
    unsigned long long size = 1024;
    unsigned long long flag = 0;
 
    drvError_t result = halMemCreate(&ptr, size, prop, flag);
    EXPECT_EQ(result, DRV_ERROR_RESERVED);
}
 
TEST(HalHooksTest, HalMemRelease)
{
    drv_mem_handle_t* ptr = nullptr;

    drvError_t result = halMemRelease(ptr);
    EXPECT_EQ(result, DRV_ERROR_RESERVED);
}

// ============================================================================
// 驱动符号桩与隔离子进程用例
// hal 钩子把 ResolveDriverSymbol 的结果缓存在函数内 static 中：同一进程内一旦以
// “无驱动符号”完成首次解析（上面的既有用例即如此），成功/采集分支便无法再进入。
// 因此采集分支改由 fork+exec 拉起本测试二进制的子进程执行，--gtest_filter 保证
// 子进程只跑目标用例，从而获得独立的首次解析时机；符号劫持复用
// test_kernel_hooks.cpp 中定义的 dlsym/dlopen 桩与函数表。
// ============================================================================
using namespace MemScope;

extern std::unordered_map<std::string, void *> g_funcMocks;
extern bool g_isDlsymNullptr;
bool RunMemScopeTestChild(const char *gtestFilter);

namespace
{
// 与 hal_hooks.cpp 内部宏 ACL_HOST_REG_PINNED 一致（该宏仅定义在实现文件内）
constexpr unsigned long long HAL_HOST_REG_PINNED_FLAG = 0X10000000UL;

enum class HalDriverMode
{
    NORMAL = 0,  // 驱动成功
    OOM,         // 驱动返回内存不足
    ERROR,       // 驱动返回其他错误
};

HalDriverMode g_halDriverMode = HalDriverMode::NORMAL;
std::atomic<int> g_halAllocCalls{0};
std::atomic<int> g_halFreeCalls{0};
std::atomic<int> g_halCreateCalls{0};
std::atomic<int> g_halReleaseCalls{0};
std::atomic<int> g_halHostRegisterCalls{0};
std::atomic<int> g_halHostUnregisterCalls{0};

// 驱动侧真函数桩：hal钩子以 <函数名>Inner 优先解析
drvError_t MockHalMemAllocInner(void **pp, unsigned long long size, unsigned long long flag)
{
    ++g_halAllocCalls;
    if (g_halDriverMode == HalDriverMode::OOM)
    {
        return DRV_ERROR_OUT_OF_MEMORY;
    }
    if (g_halDriverMode == HalDriverMode::ERROR)
    {
        return DRV_ERROR_RESERVED;
    }
    if (pp != nullptr)
    {
        *pp = reinterpret_cast<void *>(0x1000);
    }
    return DRV_ERROR_NONE;
}

drvError_t MockHalMemFreeInner(void *pp)
{
    ++g_halFreeCalls;
    if (g_halDriverMode == HalDriverMode::OOM)
    {
        return DRV_ERROR_OUT_OF_MEMORY;
    }
    if (g_halDriverMode == HalDriverMode::ERROR)
    {
        return DRV_ERROR_RESERVED;
    }
    return DRV_ERROR_NONE;
}

drvError_t MockHalMemCreateInner(drv_mem_handle_t **handle, size_t size, const struct drv_mem_prop *prop,
                                 uint64_t flag)
{
    ++g_halCreateCalls;
    if (g_halDriverMode == HalDriverMode::OOM)
    {
        return DRV_ERROR_OUT_OF_MEMORY;
    }
    if (g_halDriverMode == HalDriverMode::ERROR)
    {
        return DRV_ERROR_RESERVED;
    }
    if (handle != nullptr)
    {
        *handle = reinterpret_cast<drv_mem_handle_t *>(0x2000);
    }
    return DRV_ERROR_NONE;
}

drvError_t MockHalMemReleaseInner(drv_mem_handle_t *handle)
{
    ++g_halReleaseCalls;
    if (g_halDriverMode == HalDriverMode::OOM)
    {
        return DRV_ERROR_OUT_OF_MEMORY;
    }
    if (g_halDriverMode == HalDriverMode::ERROR)
    {
        return DRV_ERROR_RESERVED;
    }
    return DRV_ERROR_NONE;
}

// aclrtHostRegisterV2/Unregister 以 RTLD_NEXT 原名解析，桩的返回类型与钩子内
// 函数指针强转的目标类型一致（drvError_t），错误值取 aclError 的数值
drvError_t MockHalHostRegisterV2(void *ptr, uint64_t size, uint32_t flag)
{
    ++g_halHostRegisterCalls;
    if (g_halDriverMode == HalDriverMode::ERROR)
    {
        return static_cast<drvError_t>(ACL_ERROR_INTERNAL_ERROR);
    }
    return DRV_ERROR_NONE;
}

drvError_t MockHalHostUnregister(void *ptr)
{
    ++g_halHostUnregisterCalls;
    if (g_halDriverMode == HalDriverMode::ERROR)
    {
        return static_cast<drvError_t>(ACL_ERROR_INTERNAL_ERROR);
    }
    return DRV_ERROR_NONE;
}

// 受限视图（设置ASCEND_RT_VISIBLE_DEVICES）下GetDeviceId按env做范围校验：aclrtGetDevice
// 返回的是0基可见序号而非物理卡号，TransDeviceId以序号为key查visibleDeviceMap；
// 桩固定返回序号0，保证成功路径不受运行环境影响
int32_t MockDeviceIdForRestrictedView()
{
    return 0;
}

// GetDeviceInfo 使用的运行时接口桩：成功返回env首个可见设备号
aclError MockAclrtGetDeviceImpl(int32_t *deviceId)
{
    if (deviceId != nullptr)
    {
        *deviceId = MockDeviceIdForRestrictedView();
    }
    return ACL_SUCCESS;
}

void InstallHalDriverMocks()
{
    g_funcMocks["halMemAllocInner"] = reinterpret_cast<void *>(&MockHalMemAllocInner);
    g_funcMocks["halMemFreeInner"] = reinterpret_cast<void *>(&MockHalMemFreeInner);
    g_funcMocks["halMemCreateInner"] = reinterpret_cast<void *>(&MockHalMemCreateInner);
    g_funcMocks["halMemReleaseInner"] = reinterpret_cast<void *>(&MockHalMemReleaseInner);
    g_funcMocks["aclrtHostRegisterV2"] = reinterpret_cast<void *>(&MockHalHostRegisterV2);
    g_funcMocks["aclrtHostUnregister"] = reinterpret_cast<void *>(&MockHalHostUnregister);
}

Config MakeMemTraceConfig(bool withOomAnalysis)
{
    Config config{};
    BitField<decltype(config.eventType)> eventBits;
    eventBits.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBits.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBits.getValue();
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    // 子进程内SetConfig会把配置落盘：必须给相对outputDir，否则空路径派生
    // "/msmemscope_<pid>_<date>_ascend"（根目录）并在非root CI刷错误日志
    std::snprintf(config.outputDir, sizeof(config.outputDir), "%s", "./memscope_ut_hal");
    if (withOomAnalysis)
    {
        BitField<decltype(config.analysisType)> analysisBits;
        analysisBits.setBit(static_cast<size_t>(AnalysisType::OOM_ANALYSIS));
        config.analysisType = analysisBits.getValue();
    }
    return config;
}
}  // namespace

// 主进程用例：驱动符号缺失时 host 注册/注销钩子返回内部错误。
// 这两个符号在主测试进程内没有桩，且不存在其他用例提前解析，故结果确定。
TEST(HalHooksTest, HostRegisterNullDriver)
{
    void *ptr = reinterpret_cast<void *>(0x4000);
    EXPECT_EQ(aclrtHostRegisterV2(ptr, 1024, HAL_HOST_REG_PINNED_FLAG), ACL_ERROR_INTERNAL_ERROR);
    EXPECT_EQ(aclrtHostUnregister(ptr), ACL_ERROR_INTERNAL_ERROR);
}

// 子进程用例：正常/影子/跳过/抑制四种采集模式以及驱动OOM、普通错误全路径
TEST(HalHooksChild, FullCoverage)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    g_isDlsymNullptr = false;
    g_halDriverMode = HalDriverMode::NORMAL;
    InstallHalDriverMocks();
    Utility::FileCreateManager::GetInstance("./memscope_ut_hal").SetProjectDir("./memscope_ut_hal");

    // NORMAL：alloc/free事件需求成立且处于采集态，上报完整调用栈
    Config config = MakeMemTraceConfig(false);
    ConfigManager::Instance().SetConfig(config);
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);

    void *ptr = nullptr;
    int before = g_halAllocCalls.load();
    EXPECT_EQ(halMemAlloc(&ptr, 1024, 0x1234ULL), DRV_ERROR_NONE);
    EXPECT_EQ(g_halAllocCalls.load(), before + 1);  // 确认执行到桩而非空指针
    EXPECT_EQ(ptr, reinterpret_cast<void *>(0x1000));

    before = g_halFreeCalls.load();
    EXPECT_EQ(halMemFree(ptr), DRV_ERROR_NONE);
    EXPECT_EQ(g_halFreeCalls.load(), before + 1);

    drv_mem_prop prop{};
    prop.side = 0;
    prop.devid = 0;
    drv_mem_handle_t *handle = nullptr;
    before = g_halCreateCalls.load();
    EXPECT_EQ(halMemCreate(&handle, 2048, &prop, 0), DRV_ERROR_NONE);
    EXPECT_EQ(g_halCreateCalls.load(), before + 1);
    EXPECT_EQ(handle, reinterpret_cast<drv_mem_handle_t *>(0x2000));

    before = g_halReleaseCalls.load();
    EXPECT_EQ(halMemRelease(handle), DRV_ERROR_NONE);
    EXPECT_EQ(g_halReleaseCalls.load(), before + 1);
    // 采集态但属性指针为空：判空后提前返回
    EXPECT_EQ(halMemCreate(&handle, 2048, nullptr, 0), DRV_ERROR_NONE);

    void *hostPtr = reinterpret_cast<void *>(0x3000);
    before = g_halHostRegisterCalls.load();
    EXPECT_EQ(aclrtHostRegisterV2(hostPtr, 4096, HAL_HOST_REG_PINNED_FLAG), ACL_SUCCESS);
    EXPECT_EQ(g_halHostRegisterCalls.load(), before + 1);
    // 非PINNED标记不算显存事件，直接返回
    EXPECT_EQ(aclrtHostRegisterV2(hostPtr, 4096, 0), ACL_SUCCESS);

    before = g_halHostUnregisterCalls.load();
    EXPECT_EQ(aclrtHostUnregister(hostPtr), ACL_SUCCESS);
    EXPECT_EQ(g_halHostUnregisterCalls.load(), before + 1);

    // SHADOW：事件需求保留alloc/free但退出采集态，只上报addr/size
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    EXPECT_EQ(halMemAlloc(&ptr, 1024, 0x1234ULL), DRV_ERROR_NONE);
    EXPECT_EQ(halMemFree(ptr), DRV_ERROR_NONE);
    // halMemCreate/halMemRelease在非采集态经IsNeedTrace短路，不进入DetermineTraceMode
    EXPECT_EQ(halMemCreate(&handle, 2048, &prop, 0), DRV_ERROR_NONE);
    EXPECT_EQ(halMemRelease(handle), DRV_ERROR_NONE);
    EXPECT_EQ(aclrtHostRegisterV2(hostPtr, 4096, HAL_HOST_REG_PINNED_FLAG), ACL_SUCCESS);
    EXPECT_EQ(aclrtHostUnregister(hostPtr), ACL_SUCCESS);

    // SKIP：无alloc/free事件需求
    Config skipConfig{};
    std::snprintf(skipConfig.outputDir, sizeof(skipConfig.outputDir), "%s", "./memscope_ut_hal");
    ConfigManager::Instance().SetConfig(skipConfig);
    EXPECT_EQ(halMemAlloc(&ptr, 1024, 0x1234ULL), DRV_ERROR_NONE);
    EXPECT_EQ(halMemFree(ptr), DRV_ERROR_NONE);
    EXPECT_EQ(halMemCreate(&handle, 2048, &prop, 0), DRV_ERROR_NONE);
    EXPECT_EQ(halMemRelease(handle), DRV_ERROR_NONE);
    EXPECT_EQ(aclrtHostRegisterV2(hostPtr, 4096, HAL_HOST_REG_PINNED_FLAG), ACL_SUCCESS);
    EXPECT_EQ(aclrtHostUnregister(hostPtr), ACL_SUCCESS);

    // 抑制窗口内：事件需求成立但DetermineTraceMode返回SKIP
    ConfigManager::Instance().SetConfig(config);
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);
    {
        EventReportSuppressor suppressor;
        EXPECT_EQ(halMemAlloc(&ptr, 1024, 0x1234ULL), DRV_ERROR_NONE);
        EXPECT_EQ(halMemFree(ptr), DRV_ERROR_NONE);
        EXPECT_EQ(halMemCreate(&handle, 2048, &prop, 0), DRV_ERROR_NONE);
        EXPECT_EQ(halMemRelease(handle), DRV_ERROR_NONE);
        EXPECT_EQ(aclrtHostRegisterV2(hostPtr, 4096, HAL_HOST_REG_PINNED_FLAG), ACL_SUCCESS);
        EXPECT_EQ(aclrtHostUnregister(hostPtr), ACL_SUCCESS);

        // 抑制窗口内的OOM：HandleOOM首行直接返回
        g_halDriverMode = HalDriverMode::OOM;
        EXPECT_EQ(halMemAlloc(&ptr, 1024, 0x1234ULL), DRV_ERROR_OUT_OF_MEMORY);
        g_halDriverMode = HalDriverMode::NORMAL;
    }

    // OOM但未要求采集：HandleOOM在事件需求检查处早退
    ConfigManager::Instance().SetConfig(skipConfig);
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);
    g_halDriverMode = HalDriverMode::OOM;
    EXPECT_EQ(halMemAlloc(&ptr, 1024, 0x1234ULL), DRV_ERROR_OUT_OF_MEMORY);
    EXPECT_EQ(halMemCreate(&handle, 2048, &prop, 0), DRV_ERROR_OUT_OF_MEMORY);

    // OOM且采集开启，但未启用OOM详细分析：上报栈后跳过详细分析
    ConfigManager::Instance().SetConfig(config);
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);
    EXPECT_EQ(halMemAlloc(&ptr, 1024, 0x1234ULL), DRV_ERROR_OUT_OF_MEMORY);
    EXPECT_EQ(halMemCreate(&handle, 2048, &prop, 0), DRV_ERROR_OUT_OF_MEMORY);
    g_halDriverMode = HalDriverMode::NORMAL;

    // 驱动返回其他错误：错误透传与LOG_ERROR分支
    g_halDriverMode = HalDriverMode::ERROR;
    EXPECT_EQ(halMemAlloc(&ptr, 1024, 0x1234ULL), DRV_ERROR_RESERVED);
    EXPECT_EQ(halMemFree(ptr), DRV_ERROR_RESERVED);
    EXPECT_EQ(halMemCreate(&handle, 2048, &prop, 0), DRV_ERROR_RESERVED);
    EXPECT_EQ(halMemRelease(handle), DRV_ERROR_RESERVED);
    EXPECT_EQ(aclrtHostRegisterV2(hostPtr, 4096, HAL_HOST_REG_PINNED_FLAG), ACL_ERROR_INTERNAL_ERROR);
    EXPECT_EQ(aclrtHostUnregister(hostPtr), ACL_ERROR_INTERNAL_ERROR);
    g_halDriverMode = HalDriverMode::NORMAL;
}

// 子进程用例：启用OOM详细分析且设备号可获取，走完trigger上报+详情查询链路
TEST(HalHooksChild, OomDeepDetail)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    g_isDlsymNullptr = false;
    g_halDriverMode = HalDriverMode::NORMAL;
    InstallHalDriverMocks();
    g_funcMocks["aclrtGetDeviceImpl"] = reinterpret_cast<void *>(&MockAclrtGetDeviceImpl);
    Utility::FileCreateManager::GetInstance("./memscope_ut_hal").SetProjectDir("./memscope_ut_hal");

    ConfigManager::Instance().SetConfig(MakeMemTraceConfig(true));
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);

    int32_t devId = GD_INVALID_NUM;
    ASSERT_TRUE(GetDeviceInfo::Instance().GetDeviceId(devId));
    // 受限视图下设备号可能被置换为物理卡号，此处只校验“取得有效设备号”，不绑定具体取值
    EXPECT_NE(devId, GD_INVALID_NUM);

    g_halDriverMode = HalDriverMode::OOM;
    void *ptr = nullptr;
    // 首次OOM：完整详情
    EXPECT_EQ(halMemAlloc(&ptr, 4096, 0x1234ULL), DRV_ERROR_OUT_OF_MEMORY);
    drv_mem_prop prop{};
    drv_mem_handle_t *handle = nullptr;
    // 2s内再次OOM：只上报trigger，不重复dump详情
    EXPECT_EQ(halMemCreate(&handle, 4096, &prop, 0), DRV_ERROR_OUT_OF_MEMORY);
    g_halDriverMode = HalDriverMode::NORMAL;
}

// 子进程用例：启用OOM详细分析但设备号获取失败，HandleOOM只告警后返回
TEST(HalHooksChild, OomWithoutDevice)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    g_isDlsymNullptr = false;
    g_halDriverMode = HalDriverMode::NORMAL;
    InstallHalDriverMocks();
    g_funcMocks.erase("aclrtGetDeviceImpl");  // 运行时接口缺失

    ConfigManager::Instance().SetConfig(MakeMemTraceConfig(true));
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);

    int32_t devId = GD_INVALID_NUM;
    EXPECT_FALSE(GetDeviceInfo::Instance().GetDeviceId(devId));

    g_halDriverMode = HalDriverMode::OOM;
    void *ptr = nullptr;
    EXPECT_EQ(halMemAlloc(&ptr, 4096, 0x1234ULL), DRV_ERROR_OUT_OF_MEMORY);
    g_halDriverMode = HalDriverMode::NORMAL;
}

// 父进程驱动用例：拉起隔离子进程并断言其正常退出（退出码非0说明子用例失败）
TEST(HalHooksIsolation, FullCoverageChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("HalHooksChild.FullCoverage"));
}

TEST(HalHooksIsolation, OomDeepDetailChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("HalHooksChild.OomDeepDetail"));
}

TEST(HalHooksIsolation, OomWithoutDeviceChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("HalHooksChild.OomWithoutDevice"));
}