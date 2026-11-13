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
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

// aclInit_ / deviceReady_ 为私有成员：SetDeviceReadyStatus(false) 是空操作，
// aclInit_(true) 也没有公开复位接口，白盒直接读写原子量以隔离用例间状态
#define private public
#include "event_trace/trace_manager/event_trace_manager.h"
#undef private
#include "event_trace/kernel_hooks/acl_hooks.h"
#include "event_trace/kernel_hooks/stars_common.h"

using namespace MemScope;

// dlsym桩由test_kernel_hooks.cpp提供，这里注入acl接口桩验证状态置位分支
extern std::unordered_map<std::string, void *> g_funcMocks;
extern bool g_isDlsymNullptr;

namespace {
constexpr char ACL_INIT_SYMBOL[] = "aclInit";
constexpr char ACL_FINALIZE_SYMBOL[] = "aclFinalize";
constexpr char ACL_SET_DEVICE_SYMBOL[] = "aclrtSetDevice";

int32_t g_aclInitCalls = 0;
int32_t g_aclInitRet = ACL_SUCCESS;
int32_t g_aclFinalizeCalls = 0;
int32_t g_aclFinalizeRet = ACL_SUCCESS;
int32_t g_setDeviceCalls = 0;
int32_t g_setDeviceRet = ACL_SUCCESS;

aclError MockAclInit(const char *configPath)
{
    (void)configPath;
    ++g_aclInitCalls;
    return g_aclInitRet;
}

aclError MockAclFinalize()
{
    ++g_aclFinalizeCalls;
    return g_aclFinalizeRet;
}

aclError MockAclrtSetDevice(int32_t deviceId)
{
    (void)deviceId;
    ++g_setDeviceCalls;
    return g_setDeviceRet;
}

// 注入/恢复dlsym桩，避免影响其他用例
class AclHookMockGuard {
public:
    AclHookMockGuard()
        : hadInit_(g_funcMocks.count(ACL_INIT_SYMBOL) != 0),
          oldInit_(hadInit_ ? g_funcMocks[ACL_INIT_SYMBOL] : nullptr),
          hadFinalize_(g_funcMocks.count(ACL_FINALIZE_SYMBOL) != 0),
          oldFinalize_(hadFinalize_ ? g_funcMocks[ACL_FINALIZE_SYMBOL] : nullptr),
          hadSetDevice_(g_funcMocks.count(ACL_SET_DEVICE_SYMBOL) != 0),
          oldSetDevice_(hadSetDevice_ ? g_funcMocks[ACL_SET_DEVICE_SYMBOL] : nullptr),
          oldDlsymNull_(g_isDlsymNullptr)
    {
        g_isDlsymNullptr = false;
        g_funcMocks[ACL_INIT_SYMBOL] = reinterpret_cast<void *>(&MockAclInit);
        g_funcMocks[ACL_FINALIZE_SYMBOL] = reinterpret_cast<void *>(&MockAclFinalize);
        g_funcMocks[ACL_SET_DEVICE_SYMBOL] = reinterpret_cast<void *>(&MockAclrtSetDevice);
    }
    ~AclHookMockGuard()
    {
        Restore(ACL_INIT_SYMBOL, hadInit_, oldInit_);
        Restore(ACL_FINALIZE_SYMBOL, hadFinalize_, oldFinalize_);
        Restore(ACL_SET_DEVICE_SYMBOL, hadSetDevice_, oldSetDevice_);
        g_isDlsymNullptr = oldDlsymNull_;
    }
    AclHookMockGuard(const AclHookMockGuard &) = delete;
    AclHookMockGuard &operator=(const AclHookMockGuard &) = delete;

private:
    static void Restore(const char *symbol, bool existed, void *value)
    {
        if (existed) {
            g_funcMocks[symbol] = value;
        } else {
            g_funcMocks.erase(symbol);
        }
    }

    bool hadInit_;
    void *oldInit_;
    bool hadFinalize_;
    void *oldFinalize_;
    bool hadSetDevice_;
    void *oldSetDevice_;
    bool oldDlsymNull_;
};

// 保存/恢复追踪状态，避免用例间相互影响
class TraceStatusGuard {
public:
    TraceStatusGuard() : tracing_(EventTraceManager::Instance().IsTracingEnabled()) {}
    ~TraceStatusGuard()
    {
        EventTraceManager::Instance().SetTraceStatus(tracing_ ? EventTraceStatus::IN_TRACING
                                                              : EventTraceStatus::NOT_IN_TRACING);
    }
    TraceStatusGuard(const TraceStatusGuard &) = delete;
    TraceStatusGuard &operator=(const TraceStatusGuard &) = delete;

private:
    bool tracing_;
};

// 保存/恢复一次性状态位：aclInit_ 被hook置位后无公开复位接口，
// deviceReady_ 是单向闩锁（false不生效），只能白盒读写
class OneShotStateGuard {
public:
    OneShotStateGuard()
        : aclInit_(EventTraceManager::Instance().aclInit_.load()),
          deviceReady_(EventTraceManager::Instance().deviceReady_.load())
    {
        EventTraceManager::Instance().aclInit_.store(false);
        EventTraceManager::Instance().deviceReady_.store(false);
    }
    ~OneShotStateGuard()
    {
        EventTraceManager::Instance().aclInit_.store(aclInit_);
        EventTraceManager::Instance().deviceReady_.store(deviceReady_);
    }
    OneShotStateGuard(const OneShotStateGuard &) = delete;
    OneShotStateGuard &operator=(const OneShotStateGuard &) = delete;

private:
    bool aclInit_;
    bool deviceReady_;
};
}  // namespace

// 非追踪态：aclInit不产生上报，直接返回运行时结果
TEST(AclHooksExtraTest, acl_init_not_tracing_returns_runtime_result)
{
    AclHookMockGuard guard;
    TraceStatusGuard statusGuard;
    OneShotStateGuard stateGuard;
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    g_aclInitCalls = 0;
    g_aclInitRet = ACL_ERROR_INTERNAL_ERROR;

    EXPECT_EQ(aclInit("ut_config"), ACL_ERROR_INTERNAL_ERROR);
    EXPECT_EQ(g_aclInitCalls, 1);
    EXPECT_FALSE(EventTraceManager::Instance().IsTracingEnabled());
}

// 非追踪态：aclFinalize同样直接返回运行时结果
TEST(AclHooksExtraTest, acl_finalize_not_tracing_returns_runtime_result)
{
    AclHookMockGuard guard;
    TraceStatusGuard statusGuard;
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    g_aclFinalizeCalls = 0;
    g_aclFinalizeRet = ACL_ERROR_INTERNAL_ERROR;

    EXPECT_EQ(aclFinalize(), ACL_ERROR_INTERNAL_ERROR);
    EXPECT_EQ(g_aclFinalizeCalls, 1);
}

// aclrtSetDevice成功：设备就绪状态置位
TEST(AclHooksExtraTest, acl_set_device_success_marks_device_ready)
{
    AclHookMockGuard guard;
    OneShotStateGuard stateGuard;
    g_setDeviceCalls = 0;
    g_setDeviceRet = ACL_SUCCESS;

    EXPECT_EQ(aclrtSetDevice(2), ACL_SUCCESS);
    EXPECT_EQ(g_setDeviceCalls, 1);
    EXPECT_TRUE(EventTraceManager::Instance().IsDeviceReady());
}

// aclrtSetDevice失败：不置位设备就绪状态，原样返回错误码
TEST(AclHooksExtraTest, acl_set_device_failure_keeps_ready_state)
{
    AclHookMockGuard guard;
    OneShotStateGuard stateGuard;
    g_setDeviceCalls = 0;
    g_setDeviceRet = ACL_ERROR_INTERNAL_ERROR;

    EXPECT_EQ(aclrtSetDevice(0), ACL_ERROR_INTERNAL_ERROR);
    EXPECT_EQ(g_setDeviceCalls, 1);
    EXPECT_FALSE(EventTraceManager::Instance().IsDeviceReady());
}

// 符号缺失：aclrtSetDevice告警并返回内部错误，不调用运行时
TEST(AclHooksExtraTest, acl_set_device_missing_symbol_returns_internal_error)
{
    AclHookMockGuard guard;
    g_funcMocks.erase(ACL_SET_DEVICE_SYMBOL);
    g_isDlsymNullptr = false;
    g_setDeviceCalls = 0;

    EXPECT_EQ(aclrtSetDevice(0), ACL_ERROR_INTERNAL_ERROR);
    EXPECT_EQ(g_setDeviceCalls, 0);
}

// 扩容态下stream/task id解析规则
TEST(StarsCommonExtraTest, expand_status_stream_and_task_id_rules)
{
    StarsCommon::SetStreamExpandStatus(1);

    // 占位/事件记录SQE：stream取低15位，task原样返回
    EXPECT_EQ(StarsCommon::GetStreamId(0x1234, 0x5678, PLACE_HOLDER_SQE), 0x1234);
    EXPECT_EQ(StarsCommon::GetStreamId(0x1234, 0x5678, EVENT_RECORD_SQE), 0x1234);
    EXPECT_EQ(StarsCommon::GetTaskId(0x1234, 0x5678, PLACE_HOLDER_SQE), 0x5678);
    EXPECT_EQ(StarsCommon::GetTaskId(0x1234, 0x5678, EVENT_RECORD_SQE), 0x5678);

    // stream第16位置位：stream取taskId低15位，task取taskId第16位与stream低15位组合
    EXPECT_EQ(StarsCommon::GetStreamId(0x8001, 0x0042, 0), 0x0042);
    EXPECT_EQ(StarsCommon::GetTaskId(0x8001, 0x0042, 0), 0x0001);
    EXPECT_EQ(StarsCommon::GetTaskId(0x8001, 0x8042, 0), 0x8001);

    // stream第16位未置位：stream取自身低15位，task原样返回
    EXPECT_EQ(StarsCommon::GetStreamId(0x0001, 0x0042, 0), 0x0001);
    EXPECT_EQ(StarsCommon::GetTaskId(0x0001, 0x0042, 0), 0x0042);

    // 恢复默认非扩容态
    StarsCommon::SetStreamExpandStatus(0);
}
