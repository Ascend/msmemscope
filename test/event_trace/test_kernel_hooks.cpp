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
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <memory.h>
#define private public
#include "event_trace/event_report.h"
#undef private
#include "bit_field.h"
#include "config_info.h"
#include "kernel_hooks/kernel_event_trace.h"
#include "kernel_hooks/runtime_hooks.h"
#include "kernel_hooks/acl_hooks.h"
#include "trace_manager/event_trace_manager.h"
#include "utils.h"
#include "vallina_symbol.h"

using namespace testing;
using namespace MemScope;

#define EXPECT_RESERVED_EXIT(call) \
    EXPECT_EXIT(std::_Exit((call) == RT_ERROR_RESERVED ? EXIT_SUCCESS : EXIT_FAILURE), \
        ExitedWithCode(EXIT_SUCCESS), ".*")

RTS_API rtError_t MockRtKernelLaunch(
    const void *stubFunc, uint32_t blockDim, void *args, uint32_t argsSize, rtSmDesc_t *smDesc, rtStream_t stm)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockRtKernelLaunchWithHandleV2(void *hdl, const uint64_t tilingKey, uint32_t blockDim,
    rtArgsEx_t *argsInfo, rtSmDesc_t *smDesc, rtStream_t stm, const rtTaskCfgInfo_t *cfgInfo)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockRtKernelLaunchWithFlagV2(const void *stubFunc, uint32_t blockDim, rtArgsEx_t *argsInfo,
    rtSmDesc_t *smDesc, rtStream_t stm, uint32_t flags, const rtTaskCfgInfo_t *cfgInfo)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockRtAicpuKernelLaunchExWithArgs(const uint32_t kernelType, const char* const opName,
    const uint32_t blockDim, const RtAicpuArgsExT *argsInfo, RtSmDescT * const smDesc, const RtStreamT stm,
    const uint32_t flags)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockRtLaunchKernelByFuncHandle(rtFuncHandle funcHandle, uint32_t blockDim,
    rtLaunchArgsHandle argsHandle, RtStreamT stm)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockRtLaunchKernelByFuncHandleV2(rtFuncHandle funcHandle, uint32_t blockDim,
    rtLaunchArgsHandle argsHandle, RtStreamT stm, const RtTaskCfgInfoT *cfgInfo)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockRtGetStreamId(rtStream_t stm, int32_t *streamId)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return RT_ERROR_NONE;
}

ACL_FUNC_VISIBILITY aclError MockAclInit(const char *configPath)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return ACL_SUCCESS;
}

ACL_FUNC_VISIBILITY aclError MockAclFinalize()
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return ACL_SUCCESS;
}

int MockSqlite3_Open(const char* filename, sqlite3** db)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    *db = reinterpret_cast<sqlite3*>(0x1234);
    return SQLITE_OK;
}

int MockSqlite3_Close(sqlite3* db)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return SQLITE_OK;
}

int MockSqlite3_BusyTimeout(sqlite3* db, int ms)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return SQLITE_OK;
}

int MockSqlite3_Exec(sqlite3* db, const char* sql,
    int (*callback)(void*, int, char**, char**), void* arg, char** errmsg)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return SQLITE_OK;
}

int MockSqlite3_PrepareV2(sqlite3* db, const char* sql, int nByte, sqlite3_stmt** ppStmt, const char** pzTail)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    *ppStmt = reinterpret_cast<sqlite3_stmt*>(0x1234);
    return SQLITE_OK;
}

int MockSqlite3_Step(sqlite3_stmt* pStmt)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return SQLITE_DONE;
}

int MockSqlite3_Finalize(sqlite3_stmt* pStmt)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return SQLITE_OK;
}

int MockSqlite3_BindText(sqlite3_stmt* pStmt, int index, const char* value, int n, void(*)(void*))
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return SQLITE_OK;
}

int MockSqlite3_BindInt(sqlite3_stmt* pStmt, int index, int value)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return SQLITE_OK;
}

int MockSqlite3_BindInt64(sqlite3_stmt* pStmt, int index, sqlite3_int64 value)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return SQLITE_OK;
}

const char* MockSqlite3_Errmsg(sqlite3* db)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return nullptr;
}

int MockSqlite3_Reset(sqlite3_stmt* pStmt)
{
    std::cout << "Stub Func: " << __func__ << std::endl;
    return SQLITE_OK;
}

std::unordered_map<std::string, void *> g_funcMocks{
    {"rtKernelLaunch", reinterpret_cast<void *>(&MockRtKernelLaunch)},
    {"rtKernelLaunchWithHandleV2", reinterpret_cast<void *>(&MockRtKernelLaunchWithHandleV2)},
    {"rtKernelLaunchWithFlagV2", reinterpret_cast<void *>(&MockRtKernelLaunchWithFlagV2)},
    {"rtAicpuKernelLaunchExWithArgs", reinterpret_cast<void *>(&MockRtAicpuKernelLaunchExWithArgs)},
    {"rtLaunchKernelByFuncHandle", reinterpret_cast<void *>(&MockRtLaunchKernelByFuncHandle)},
    {"rtLaunchKernelByFuncHandleV2", reinterpret_cast<void *>(&MockRtLaunchKernelByFuncHandleV2)},
    {"aclInit", reinterpret_cast<void *>(&MockAclInit)},
    {"aclFinalize", reinterpret_cast<void *>(&MockAclFinalize)},
    {"sqlite3_open", reinterpret_cast<void *>(&MockSqlite3_Open)},
    {"sqlite3_close", reinterpret_cast<void *>(&MockSqlite3_Close)},
    {"sqlite3_busy_timeout", reinterpret_cast<void *>(&MockSqlite3_BusyTimeout)},
    {"sqlite3_exec", reinterpret_cast<void *>(&MockSqlite3_Exec)},
    {"sqlite3_prepare_v2", reinterpret_cast<void *>(&MockSqlite3_PrepareV2)},
    {"sqlite3_step", reinterpret_cast<void *>(&MockSqlite3_Step)},
    {"sqlite3_finalize", reinterpret_cast<void *>(&MockSqlite3_Finalize)},
    {"sqlite3_bind_text", reinterpret_cast<void *>(&MockSqlite3_BindText)},
    {"sqlite3_bind_int", reinterpret_cast<void *>(&MockSqlite3_BindInt)},
    {"sqlite3_bind_int64", reinterpret_cast<void *>(&MockSqlite3_BindInt64)},
    {"sqlite3_errmsg", reinterpret_cast<void *>(&MockSqlite3_Errmsg)},
    {"sqlite3_reset", reinterpret_cast<void *>(&MockSqlite3_Reset)},
};

bool g_isDlsymNullptr = false;

extern "C" {
void *dlopen(const char *filename, int flag)
{
    return reinterpret_cast<void *>(0x1234);
}

void *dlsym(void *handle, const char *symbol)
{
    if (g_isDlsymNullptr) {
        return nullptr;
    }
    return g_funcMocks.count(std::string(symbol)) ? g_funcMocks[std::string(symbol)] : nullptr;
}
}

TEST(AclHooks, do_aclInit_expect_success)
{
    g_isDlsymNullptr = false;
    const char *configPath = "test";
    aclInit(configPath);
    EXPECT_EQ(aclInit(configPath), ACL_SUCCESS);
}

TEST(AclHooks, do_aclFinalize_expect_success)
{
    g_isDlsymNullptr = false;
    EXPECT_EQ(aclFinalize(), ACL_SUCCESS);
}

TEST(AclHooks, do_aclInit_expect_error)
{
    g_isDlsymNullptr = true;
    const char *configPath = "test";
    EXPECT_EQ(aclInit(configPath), ACL_ERROR_INTERNAL_ERROR);
}

TEST(AclHooks, do_aclFinalize_expect_error)
{
    g_isDlsymNullptr = true;
    EXPECT_EQ(aclFinalize(), ACL_ERROR_INTERNAL_ERROR);
}

TEST(RuntimeHooks, do_rtKernelLaunch_expect_success)
{
    g_isDlsymNullptr = false;
    const void *stubFunc = nullptr;
    uint32_t blockDim = 1;
    void *args = nullptr;
    uint32_t argsSize = 1;
    rtSmDesc_t *smDesc = nullptr;
    rtStream_t stm = nullptr;
    EXPECT_EQ(rtKernelLaunch(stubFunc, blockDim, args, argsSize, smDesc, stm), RT_ERROR_NONE);
}

TEST(RuntimeHooks, do_rtKernelLaunchWithHandleV2_expect_success)
{
    g_isDlsymNullptr = false;
    void *hdl = nullptr;
    const uint64_t tilingKey = 1;
    uint32_t blockDim = 1;
    rtArgsEx_t *argsInfo = nullptr;
    rtSmDesc_t *smDesc = nullptr;
    rtStream_t stm = nullptr;
    const rtTaskCfgInfo_t *cfgInfo = nullptr;
    EXPECT_EQ(rtKernelLaunchWithHandleV2(hdl, tilingKey, blockDim, argsInfo, smDesc, stm, cfgInfo), RT_ERROR_NONE);
}

TEST(RuntimeHooks, do_rtKernelLaunchWithFlagV2_expect_success)
{
    g_isDlsymNullptr = false;
    const void *stubFunc = nullptr;
    uint32_t blockDim = 1;
    rtArgsEx_t *argsInfo = nullptr;
    rtSmDesc_t *smDesc = nullptr;
    rtStream_t stm = nullptr;
    uint32_t flags = 1;
    const rtTaskCfgInfo_t *cfgInfo = nullptr;
    EXPECT_EQ(rtKernelLaunchWithFlagV2(stubFunc, blockDim, argsInfo, smDesc, stm, flags, cfgInfo), RT_ERROR_NONE);
}

TEST(RuntimeHooks, do_rtAicpuKernelLaunchExWithArgs_expect_success)
{
    g_isDlsymNullptr = false;
    uint32_t kernelType = 0;
    const char* const opName = "add";
    uint32_t blockDim = 1;
    const RtAicpuArgsExT *argsInfo = nullptr;
    RtSmDescT * const smDesc = nullptr;
    const RtStreamT stm = nullptr;
    uint32_t flags = 1;
    const rtTaskCfgInfo_t *cfgInfo = nullptr;
    EXPECT_EQ(rtAicpuKernelLaunchExWithArgs(kernelType, opName, blockDim, argsInfo, smDesc, stm, flags), RT_ERROR_NONE);
}

TEST(RuntimeHooks, do_rtLaunchKernelByFuncHandle_expect_success)
{
    g_isDlsymNullptr = false;
    rtFuncHandle funcHandle = nullptr;
    rtLaunchArgsHandle argsHandle = nullptr;
    uint32_t blockDim = 1;
    const RtStreamT stm = nullptr;
    EXPECT_EQ(rtLaunchKernelByFuncHandle(funcHandle, blockDim, argsHandle, stm), RT_ERROR_NONE);
}

TEST(RuntimeHooks, do_rtLaunchKernelByFuncHandleV2_expect_success)
{
    g_isDlsymNullptr = false;
    rtFuncHandle funcHandle = nullptr;
    rtLaunchArgsHandle argsHandle = nullptr;
    uint32_t blockDim = 1;
    const RtStreamT stm = nullptr;
    RtTaskCfgInfoT *cfgInfo = nullptr;
    EXPECT_EQ(rtLaunchKernelByFuncHandleV2(funcHandle, blockDim, argsHandle, stm, cfgInfo), RT_ERROR_NONE);
}

// NOTE: The _expect_error tests for each runtime hook function are intentionally omitted.
// Each runtime hook function (e.g. rtKernelLaunch) caches the result of VallinaSymbol::Get()
// in a function-local static variable on first call. Since the success tests (_expect_success)
// are defined first and run first, the static variable is already initialized to a non-null
// mock function pointer by the time any error test would run. The error path (vallina == nullptr
// → return RT_ERROR_RESERVED) can never be exercised after the mock has been cached.
// Instead, we validate the underlying mechanism below.
TEST(RuntimeHooks, do_vallina_get_nullptr_when_dlsym_fails)
{
    g_isDlsymNullptr = true;
    // VallinaSymbol's handle_ is non-null (dlopen is mocked to return 0x1234),
    // so Get() will call dlsym(handle_, symbol), which returns nullptr when g_isDlsymNullptr is true.
    void *ptr = VallinaSymbol<RuntimeLibLoader>::Instance().Get("rtKernelLaunch");
    EXPECT_EQ(ptr, nullptr);

}

// ============================================================================
// 子进程隔离用例：runtime_hooks 的符号解析结果缓存在函数内 static 中，同一进程
// 内无法既验证“解析成功”又验证“解析失败”。以下用例 fork+exec 当前测试二进制，
// 以 --gtest_filter 只运行目标用例，使每个子进程拥有独立的首次解析时机。
// 子进程由 gtest 正常返回退出，gcov 计数可正常落盘；父进程断言子进程退出码。
// ============================================================================
bool RunMemScopeTestChild(const char* gtestFilter)
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
        setenv("MSMEMSCOPE_UT_CHILD", "1", 1);
        const std::string filterArg = std::string("--gtest_filter=") + gtestFilter;
        execl(exePath, exePath, filterArg.c_str(), static_cast<char*>(nullptr));
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

// aclrt 钩子在 g_isInAclrtFunc 窗口内回调 rt 钩子的次数（验证委托分支）
std::atomic<int> g_rtDelegatedCalls{0};
std::atomic<int> g_aclrtImplCalls{0};
std::atomic<int> g_aclrtCfgCalls{0};
std::atomic<int> g_aclrtV2Calls{0};
std::atomic<int> g_aclrtHostArgsCalls{0};

RTS_API rtError_t MockDelegatedRtKernelLaunch(const void*, uint32_t, void*, uint32_t, rtSmDesc_t*, rtStream_t)
{
    ++g_rtDelegatedCalls;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockDelegatedRtKernelLaunchWithHandleV2(void*, const uint64_t, uint32_t, rtArgsEx_t*, rtSmDesc_t*,
                                                          rtStream_t, const rtTaskCfgInfo_t*)
{
    ++g_rtDelegatedCalls;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockDelegatedRtKernelLaunchWithFlagV2(const void*, uint32_t, rtArgsEx_t*, rtSmDesc_t*, rtStream_t,
                                                        uint32_t, const rtTaskCfgInfo_t*)
{
    ++g_rtDelegatedCalls;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockDelegatedRtAicpuKernelLaunchExWithArgs(const uint32_t, const char* const, const uint32_t,
                                                             const RtAicpuArgsExT*, RtSmDescT* const, const RtStreamT,
                                                             const uint32_t)
{
    ++g_rtDelegatedCalls;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockDelegatedRtLaunchKernelByFuncHandle(rtFuncHandle, uint32_t, rtLaunchArgsHandle, RtStreamT)
{
    ++g_rtDelegatedCalls;
    return RT_ERROR_NONE;
}

RTS_API rtError_t MockDelegatedRtLaunchKernelByFuncHandleV2(rtFuncHandle, uint32_t, rtLaunchArgsHandle, RtStreamT,
                                                            const RtTaskCfgInfoT*)
{
    ++g_rtDelegatedCalls;
    return RT_ERROR_NONE;
}

// aclrt 钩子的桩：在钩子置位的 g_isInAclrtFunc 窗口内回调对应 rt 钩子，
// 既验证返回值透传，也覆盖 rt 钩子的“已在 aclrt 调用内”委托分支。
aclError MockAclrtLaunchKernelImpl(aclrtFuncHandle, uint32_t, const void*, size_t, aclrtStream)
{
    ++g_aclrtImplCalls;
    const rtError_t r1 = rtKernelLaunch(nullptr, 1, nullptr, 0, nullptr, nullptr);
    const rtError_t r2 = rtKernelLaunchWithFlagV2(nullptr, 1, nullptr, nullptr, nullptr, 0, nullptr);
    const rtError_t r3 = rtAicpuKernelLaunchExWithArgs(0, "add", 1, nullptr, nullptr, nullptr, 0);
    return (r1 == RT_ERROR_NONE && r2 == RT_ERROR_NONE && r3 == RT_ERROR_NONE) ? ACL_SUCCESS : ACL_ERROR_RT_FAILURE;
}

aclError MockAclrtLaunchKernelWithConfig(aclrtFuncHandle, uint32_t, aclrtStream, aclrtLaunchKernelCfg*,
                                         aclrtArgsHandle, void*)
{
    ++g_aclrtCfgCalls;
    return rtKernelLaunchWithHandleV2(nullptr, 0, 1, nullptr, nullptr, nullptr, nullptr) == RT_ERROR_NONE
               ? ACL_SUCCESS
               : ACL_ERROR_RT_FAILURE;
}

aclError MockAclrtLaunchKernelV2Impl(aclrtFuncHandle, uint32_t, const void*, size_t, aclrtLaunchKernelCfg*,
                                     aclrtStream)
{
    ++g_aclrtV2Calls;
    return rtLaunchKernelByFuncHandle(nullptr, 1, nullptr, nullptr) == RT_ERROR_NONE ? ACL_SUCCESS
                                                                                     : ACL_ERROR_RT_FAILURE;
}

aclError MockAclrtLaunchKernelWithHostArgs(aclrtFuncHandle, uint32_t, aclrtStream, aclrtLaunchKernelCfg*, void*,
                                           size_t, aclrtPlaceHolderInfo*, size_t)
{
    ++g_aclrtHostArgsCalls;
    return rtLaunchKernelByFuncHandleV2(nullptr, 1, nullptr, nullptr, nullptr) == RT_ERROR_NONE
               ? ACL_SUCCESS
               : ACL_ERROR_RT_FAILURE;
}

// 子进程用例：符号全部解析失败时，各钩子返回“保留错误”且不再访问驱动
TEST(RuntimeHooksChild, NullVallina)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    g_isDlsymNullptr = true;
    EXPECT_EQ(rtKernelLaunch(nullptr, 1, nullptr, 0, nullptr, nullptr), RT_ERROR_RESERVED);
    EXPECT_EQ(rtKernelLaunchWithHandleV2(nullptr, 0, 1, nullptr, nullptr, nullptr, nullptr), RT_ERROR_RESERVED);
    EXPECT_EQ(rtKernelLaunchWithFlagV2(nullptr, 1, nullptr, nullptr, nullptr, 0, nullptr), RT_ERROR_RESERVED);
    EXPECT_EQ(rtAicpuKernelLaunchExWithArgs(0, "add", 1, nullptr, nullptr, nullptr, 0), RT_ERROR_RESERVED);
    EXPECT_EQ(rtLaunchKernelByFuncHandle(nullptr, 1, nullptr, nullptr), RT_ERROR_RESERVED);
    EXPECT_EQ(rtLaunchKernelByFuncHandleV2(nullptr, 1, nullptr, nullptr, nullptr), RT_ERROR_RESERVED);
    EXPECT_EQ(aclrtLaunchKernelImpl(nullptr, 1, nullptr, 0, nullptr), ACL_ERROR_RT_FAILURE);
    EXPECT_EQ(aclrtLaunchKernelWithConfig(nullptr, 1, nullptr, nullptr, nullptr, nullptr), ACL_ERROR_RT_FAILURE);
    EXPECT_EQ(aclrtLaunchKernelV2Impl(nullptr, 1, nullptr, 0, nullptr, nullptr), ACL_ERROR_RT_FAILURE);
    EXPECT_EQ(aclrtLaunchKernelWithHostArgs(nullptr, 1, nullptr, nullptr, nullptr, 0, nullptr, 0),
              ACL_ERROR_RT_FAILURE);
}

// 子进程用例：符号解析成功时覆盖委托分支（g_isInAclrtFunc 置位窗口）与
// KernelWatchEnd 的 watch 配置分支
TEST(RuntimeHooksChild, DelegateAndWatch)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    g_isDlsymNullptr = false;
    g_funcMocks["rtKernelLaunch"] = reinterpret_cast<void*>(&MockDelegatedRtKernelLaunch);
    g_funcMocks["rtKernelLaunchWithHandleV2"] = reinterpret_cast<void*>(&MockDelegatedRtKernelLaunchWithHandleV2);
    g_funcMocks["rtKernelLaunchWithFlagV2"] = reinterpret_cast<void*>(&MockDelegatedRtKernelLaunchWithFlagV2);
    g_funcMocks["rtAicpuKernelLaunchExWithArgs"] =
        reinterpret_cast<void*>(&MockDelegatedRtAicpuKernelLaunchExWithArgs);
    g_funcMocks["rtLaunchKernelByFuncHandle"] = reinterpret_cast<void*>(&MockDelegatedRtLaunchKernelByFuncHandle);
    g_funcMocks["rtLaunchKernelByFuncHandleV2"] =
        reinterpret_cast<void*>(&MockDelegatedRtLaunchKernelByFuncHandleV2);
    g_funcMocks["aclrtLaunchKernelImpl"] = reinterpret_cast<void*>(&MockAclrtLaunchKernelImpl);
    g_funcMocks["aclrtLaunchKernelWithConfig"] = reinterpret_cast<void*>(&MockAclrtLaunchKernelWithConfig);
    g_funcMocks["aclrtLaunchKernelV2Impl"] = reinterpret_cast<void*>(&MockAclrtLaunchKernelV2Impl);
    g_funcMocks["aclrtLaunchKernelWithHostArgs"] = reinterpret_cast<void*>(&MockAclrtLaunchKernelWithHostArgs);

    Config config{};
    BitField<decltype(config.levelType)> levelBits(config.levelType);
    levelBits.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    levelBits.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    config.levelType = levelBits.getValue();
    config.watchConfig.isWatched = true;  // 触发 KernelWatchEnd 的 watch 分支
    ConfigManager::Instance().SetConfig(config);

    RuntimeKernelLinker& linker = RuntimeKernelLinker::GetInstance();
    linker.SetHashInfo(2026, "kernel_add");
    linker.KernelLaunch();
    auto key = std::make_tuple(static_cast<uint16_t>(0), static_cast<uint16_t>(1), static_cast<uint16_t>(2));
    linker.RuntimeTaskInfoLaunch(key, 2026);
    const std::string expectName = "0_" + std::to_string(Utility::GetTid()) + "/kernel_add";
    EXPECT_EQ(linker.GetLastKernelName(Utility::GetTid()), expectName);

    // 4个aclrt钩子内部共委托6个rt钩子（其中impl桩连续回调3个），返回值透传为ACL_SUCCESS
    EXPECT_EQ(aclrtLaunchKernelImpl(nullptr, 1, nullptr, 0, nullptr), ACL_SUCCESS);
    EXPECT_EQ(aclrtLaunchKernelWithConfig(nullptr, 1, nullptr, nullptr, nullptr, nullptr), ACL_SUCCESS);
    EXPECT_EQ(aclrtLaunchKernelV2Impl(nullptr, 1, nullptr, 0, nullptr, nullptr), ACL_SUCCESS);
    EXPECT_EQ(aclrtLaunchKernelWithHostArgs(nullptr, 1, nullptr, nullptr, nullptr, 0, nullptr, 0), ACL_SUCCESS);
    EXPECT_EQ(g_aclrtImplCalls.load(), 1);
    EXPECT_EQ(g_aclrtCfgCalls.load(), 1);
    EXPECT_EQ(g_aclrtV2Calls.load(), 1);
    EXPECT_EQ(g_aclrtHostArgsCalls.load(), 1);
    EXPECT_EQ(g_rtDelegatedCalls.load(), 6);

    // 非委托路径：g_isInAclrtFunc 为 false，执行 StartKernelEventTrace + KernelLaunch + KernelWatchEnd
    EXPECT_EQ(rtKernelLaunch(nullptr, 1, nullptr, 0, nullptr, nullptr), RT_ERROR_NONE);
    EXPECT_EQ(linker.GetLastKernelName(Utility::GetTid()), expectName);
}

// 父进程驱动用例：拉起隔离子进程并断言其正常退出（退出码非0说明子用例失败）
TEST(RuntimeHooksIsolation, NullVallinaChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("RuntimeHooksChild.NullVallina"));
}

TEST(RuntimeHooksIsolation, DelegateAndWatchChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("RuntimeHooksChild.DelegateAndWatch"));
}
