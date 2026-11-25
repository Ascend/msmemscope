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

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "event_trace/kernel_hooks/kernel_event_trace.h"
#include "event_trace/kernel_hooks/runtime_prof_api.h"
#include "event_trace/kernel_hooks/stars_common.h"

using namespace MemScope;

// dlsym桩由test_kernel_hooks.cpp提供，这里通过函数表注入回调注册/命令下发符号
extern std::unordered_map<std::string, void *> g_funcMocks;
extern bool g_isDlsymNullptr;

namespace MemScope {
// 仅在本.cpp内定义，头文件未声明
int32_t ApiReporterCallbackImpl(uint32_t agingFlag, const MsprofApi *const data);
int32_t AdditionalInfoCallbackImpl(uint32_t agingFlag, const void *data, uint32_t len);
}  // namespace MemScope

namespace {
constexpr char REGISTER_CALLBACK_SYMBOL[] = "MsprofRegisterProfileCallback";
constexpr char SET_PROF_COMMAND_SYMBOL[] = "profSetProfCommand";
constexpr char ID2STR_SYMBOL[] = "MsprofId2Str";

struct RegisterCall {
    int32_t type;
    void *func;
    uint32_t size;
};

std::vector<RegisterCall> g_registerCalls;
int32_t g_registerRet = 0;
int32_t g_setCommandRet = 0;
int32_t g_setCommandCalls = 0;
CommandHandle g_lastCommand{};

int32_t MockMsprofRegisterProfileCallback(int32_t type, void *func, uint32_t size)
{
    g_registerCalls.push_back(RegisterCall{type, func, size});
    return g_registerRet;
}

int32_t MockProfSetProfCommand(void *data, uint32_t len)
{
    ++g_setCommandCalls;
    if (data != nullptr && len == sizeof(CommandHandle)) {
        g_lastCommand = *reinterpret_cast<const CommandHandle *>(data);
    }
    return g_setCommandRet;
}

char g_id2StrResult[64] = "kernel_reverse";

// MsprofId2Str桩：仅对hash=7且结果非空时返回名字，其余返回nullptr
char *MockMsprofId2Str(uint64_t hashId)
{
    if (hashId == 7U && g_id2StrResult[0] != '\0') {
        return g_id2StrResult;
    }
    return nullptr;
}

// 注入/恢复dlsym桩，避免影响其他用例
class ProfApiMockGuard {
public:
    ProfApiMockGuard()
        : hadRegister_(g_funcMocks.count(REGISTER_CALLBACK_SYMBOL) != 0),
          oldRegister_(hadRegister_ ? g_funcMocks[REGISTER_CALLBACK_SYMBOL] : nullptr),
          hadCommand_(g_funcMocks.count(SET_PROF_COMMAND_SYMBOL) != 0),
          oldCommand_(hadCommand_ ? g_funcMocks[SET_PROF_COMMAND_SYMBOL] : nullptr),
          hadId2Str_(g_funcMocks.count(ID2STR_SYMBOL) != 0),
          oldId2Str_(hadId2Str_ ? g_funcMocks[ID2STR_SYMBOL] : nullptr),
          oldDlsymNull_(g_isDlsymNullptr)
    {
        g_isDlsymNullptr = false;
        ResetKernelNameReverseLookupForTest();
    }
    ~ProfApiMockGuard()
    {
        Restore(REGISTER_CALLBACK_SYMBOL, hadRegister_, oldRegister_);
        Restore(SET_PROF_COMMAND_SYMBOL, hadCommand_, oldCommand_);
        Restore(ID2STR_SYMBOL, hadId2Str_, oldId2Str_);
        ResetKernelNameReverseLookupForTest();
        g_isDlsymNullptr = oldDlsymNull_;
    }
    ProfApiMockGuard(const ProfApiMockGuard &) = delete;
    ProfApiMockGuard &operator=(const ProfApiMockGuard &) = delete;

private:
    static void Restore(const char *symbol, bool existed, void *value)
    {
        if (existed) {
            g_funcMocks[symbol] = value;
        } else {
            g_funcMocks.erase(symbol);
        }
    }

    bool hadRegister_;
    void *oldRegister_;
    bool hadCommand_;
    void *oldCommand_;
    bool hadId2Str_;
    void *oldId2Str_;
    bool oldDlsymNull_;
};
}  // namespace

// 成功注册时按既定顺序注册4类回调，且均以sizeof(void*)为入参长度
TEST(RuntimeProfApiTest, register_callback_registers_all_four_callback_types)
{
    ProfApiMockGuard guard;
    g_funcMocks[REGISTER_CALLBACK_SYMBOL] = reinterpret_cast<void *>(&MockMsprofRegisterProfileCallback);
    g_registerCalls.clear();
    g_registerRet = 0;

    RegisterRtProfileCallback();

    ASSERT_EQ(g_registerCalls.size(), 4U);
    EXPECT_EQ(g_registerCalls[0].type, static_cast<int32_t>(PROFILE_REPORT_GET_HASH_ID_C_CALLBACK));
    EXPECT_EQ(g_registerCalls[0].func, reinterpret_cast<void *>(GetHashIdCallBackImply));
    EXPECT_EQ(g_registerCalls[1].type, static_cast<int32_t>(PROFILE_REPORT_COMPACT_CALLBACK));
    EXPECT_EQ(g_registerCalls[1].func, reinterpret_cast<void *>(CompactInfoReporterCallbackImpl));
    EXPECT_EQ(g_registerCalls[2].type, static_cast<int32_t>(PROFILE_REPORT_API_C_CALLBACK));
    EXPECT_EQ(g_registerCalls[2].func, reinterpret_cast<void *>(ApiReporterCallbackImpl));
    EXPECT_EQ(g_registerCalls[3].type, static_cast<int32_t>(PROFILE_REPORT_ADDITIONAL_CALLBACK));
    EXPECT_EQ(g_registerCalls[3].func, reinterpret_cast<void *>(AdditionalInfoCallbackImpl));
    for (const auto &call : g_registerCalls) {
        EXPECT_EQ(call.size, sizeof(void *));
    }
}

// 单类回调注册失败仅打印错误，不影响其余类型继续注册
TEST(RuntimeProfApiTest, register_callback_continues_when_one_register_fails)
{
    ProfApiMockGuard guard;
    g_funcMocks[REGISTER_CALLBACK_SYMBOL] = reinterpret_cast<void *>(&MockMsprofRegisterProfileCallback);
    g_registerCalls.clear();
    g_registerRet = -1;

    RegisterRtProfileCallback();

    EXPECT_EQ(g_registerCalls.size(), 4U);
    g_registerRet = 0;
}

// 注册符号缺失时告警返回，不调用任何注册
TEST(RuntimeProfApiTest, register_callback_missing_symbol_does_not_register)
{
    ProfApiMockGuard guard;
    g_funcMocks.erase(REGISTER_CALLBACK_SYMBOL);
    g_registerCalls.clear();

    RegisterRtProfileCallback();

    EXPECT_TRUE(g_registerCalls.empty());
}

// compact回调：level/type匹配时更新stream扩容状态
TEST(RuntimeProfApiTest, compact_callback_updates_stream_expand_status)
{
    MsprofCompactInfo info{};
    info.level = MSPROF_REPORT_RUNTIME_LEVEL;
    info.type = MSPROF_STREAM_EXPAND_SPEC_TYPE;
    info.data.streamExpandInfo.expandStatus = 1;
    EXPECT_EQ(CompactInfoReporterCallbackImpl(0, &info, sizeof(info)), PROFAPI_ERROR_NONE);
    // 扩容态生效：stream id第16位置位时取task id低15位
    EXPECT_EQ(StarsCommon::GetStreamId(0x8001, 0x0042), 0x0042);

    info.data.streamExpandInfo.expandStatus = 0;
    EXPECT_EQ(CompactInfoReporterCallbackImpl(0, &info, sizeof(info)), PROFAPI_ERROR_NONE);
    // 非扩容态：按原有规则取stream id低11位余数
    EXPECT_EQ(StarsCommon::GetStreamId(0x8001, 0x0042), 0x0001);

    // 数据为空或长度不匹配：直接返回错误，状态保持不变
    EXPECT_EQ(CompactInfoReporterCallbackImpl(0, &info, sizeof(info) - 1), PROFAPI_ERROR);
    EXPECT_EQ(CompactInfoReporterCallbackImpl(0, nullptr, sizeof(info)), PROFAPI_ERROR);
}

// hash回调：显式len与NUL结尾字符串应得到同一hashId，且名字写入映射表
TEST(RuntimeProfApiTest, get_hash_id_callback_accepts_zero_and_explicit_len)
{
    uint64_t hashByNul = GetHashIdCallBackImply("len_check_kernel", 0);
    uint64_t hashByLen = GetHashIdCallBackImply("len_check_kernel", std::strlen("len_check_kernel"));
    EXPECT_NE(hashByNul, 0U);
    EXPECT_EQ(hashByNul, hashByLen);
    EXPECT_EQ(RuntimeKernelLinker::GetInstance().GetHashInfo(hashByNul), "len_check_kernel");

    // 空指针：返回0且不写入映射表
    EXPECT_EQ(GetHashIdCallBackImply(nullptr, 0), 0U);
}

// MsprofId2Str反查：符号存在时取回名字，未命中/空串返回空
TEST(RuntimeProfApiTest, get_kernel_name_by_hash_id_uses_reverse_lookup_symbol)
{
    ProfApiMockGuard guard;
    g_funcMocks[ID2STR_SYMBOL] = reinterpret_cast<void *>(&MockMsprofId2Str);
    std::memcpy(g_id2StrResult, "kernel_reverse", sizeof("kernel_reverse"));

    EXPECT_EQ(GetKernelNameByHashId(7U), "kernel_reverse");
    // 反查未命中（桩返回nullptr）
    EXPECT_EQ(GetKernelNameByHashId(8U), "");
    // 符号只解析一次：擦除桩后仍走已缓存的函数指针，避免每次下发重复dlsym
    g_funcMocks.erase(ID2STR_SYMBOL);
    EXPECT_EQ(GetKernelNameByHashId(7U), "kernel_reverse");
    // 反查返回空串按未命中处理
    g_id2StrResult[0] = '\0';
    EXPECT_EQ(GetKernelNameByHashId(7U), "");
}

// MsprofId2Str符号缺失（老版本CANN不导出）：返回空串，不崩溃
TEST(RuntimeProfApiTest, get_kernel_name_by_hash_id_missing_symbol_returns_empty)
{
    ProfApiMockGuard guard;
    g_funcMocks.erase(ID2STR_SYMBOL);

    EXPECT_EQ(GetKernelNameByHashId(7U), "");
}

// SetProfCommand：按devId组装启动命令并下发
TEST(RuntimeProfApiTest, set_prof_command_builds_expected_command)
{
    ProfApiMockGuard guard;
    g_funcMocks[SET_PROF_COMMAND_SYMBOL] = reinterpret_cast<void *>(&MockProfSetProfCommand);
    g_setCommandCalls = 0;
    g_setCommandRet = 0;

    SetProfCommand(3U);

    ASSERT_EQ(g_setCommandCalls, 1);
    EXPECT_EQ(g_lastCommand.devNums, 1U);
    EXPECT_EQ(g_lastCommand.devIdList[0], 3U);
    EXPECT_EQ(g_lastCommand.profSwitch, PROF_TASK_TIME | PROF_RUNTIME_TRACE);
    EXPECT_EQ(g_lastCommand.profSwitchHi, 0U);
    EXPECT_EQ(g_lastCommand.modelId, PROF_INVALID_MODE_ID);
    EXPECT_EQ(g_lastCommand.type, static_cast<uint32_t>(PROF_COMMANDHANDLE_TYPE_START));
}

// SetProfCommand：下发失败与符号缺失均打印错误后返回
TEST(RuntimeProfApiTest, set_prof_command_failure_and_missing_symbol)
{
    ProfApiMockGuard guard;
    g_funcMocks[SET_PROF_COMMAND_SYMBOL] = reinterpret_cast<void *>(&MockProfSetProfCommand);
    g_setCommandCalls = 0;
    g_setCommandRet = 1;

    SetProfCommand(0U);
    EXPECT_EQ(g_setCommandCalls, 1);
    EXPECT_EQ(g_lastCommand.devNums, 1U);

    // 符号缺失：告警返回，不再调用下方命令函数
    g_funcMocks.erase(SET_PROF_COMMAND_SYMBOL);
    SetProfCommand(0U);
    EXPECT_EQ(g_setCommandCalls, 1);
}
