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
#include <unordered_map>
#include <memory.h>
#define private public
#include "event_trace/event_report.h"
#undef private
#include "bit_field.h"
#include "kernel_hooks/runtime_hooks.h"
#include "kernel_hooks/acl_hooks.h"
#include "vallina_symbol.h"

using namespace testing;
using namespace MemScope;
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
