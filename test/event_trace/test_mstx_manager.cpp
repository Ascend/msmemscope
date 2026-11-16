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
#include "event_trace/mstx_hooks/mstx_manager.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "event_trace/event_report.h"
#include "event_trace/kernel_hooks/acl_hooks.h"
#include "event_trace/op_handler.h"
#include "event_trace/trace_manager/event_trace_manager.h"

using namespace MemScope;

// dlsym桩由test_kernel_hooks.cpp提供，这里注入aclrtStreamGetIdImpl以验证stream id回填
extern std::unordered_map<std::string, void *> g_funcMocks;
extern bool g_isDlsymNullptr;

namespace {
constexpr char STREAM_ID_SYMBOL[] = "aclrtStreamGetIdImpl";
constexpr int32_t UT_STREAM_ID = 77;

int32_t g_streamIdMockCalls = 0;

aclError MockAclrtStreamGetId(aclrtStream stream, int32_t *streamId)
{
    (void)stream;
    ++g_streamIdMockCalls;
    if (streamId != nullptr) {
        *streamId = UT_STREAM_ID;
    }
    return ACL_SUCCESS;
}

// 注入/恢复dlsym桩，避免影响其他用例
class StreamIdMockGuard {
public:
    StreamIdMockGuard()
        : existed_(g_funcMocks.count(STREAM_ID_SYMBOL) != 0),
          oldValue_(existed_ ? g_funcMocks[STREAM_ID_SYMBOL] : nullptr),
          oldDlsymNull_(g_isDlsymNullptr)
    {
        g_isDlsymNullptr = false;
        g_funcMocks[STREAM_ID_SYMBOL] = reinterpret_cast<void *>(&MockAclrtStreamGetId);
    }
    ~StreamIdMockGuard()
    {
        if (existed_) {
            g_funcMocks[STREAM_ID_SYMBOL] = oldValue_;
        } else {
            g_funcMocks.erase(STREAM_ID_SYMBOL);
        }
        g_isDlsymNullptr = oldDlsymNull_;
    }
    StreamIdMockGuard(const StreamIdMockGuard &) = delete;
    StreamIdMockGuard &operator=(const StreamIdMockGuard &) = delete;

private:
    bool existed_;
    void *oldValue_;
    bool oldDlsymNull_;
};

// 保存/恢复追踪与sanitizer使能状态，避免用例间相互影响
class MstxStateGuard {
public:
    MstxStateGuard()
        : tracing_(EventTraceManager::Instance().IsTracingEnabled()),
          sanitizerEnabled_(SanitizerOpHandler::IsEnabled())
    {
    }
    ~MstxStateGuard()
    {
        EventTraceManager::Instance().SetTraceStatus(
            tracing_ ? EventTraceStatus::IN_TRACING : EventTraceStatus::NOT_IN_TRACING);
        SanitizerOpHandler::SetEnabled(sanitizerEnabled_);
    }
    MstxStateGuard(const MstxStateGuard &) = delete;
    MstxStateGuard &operator=(const MstxStateGuard &) = delete;

private:
    bool tracing_;
    bool sanitizerEnabled_;
};
}  // namespace

TEST(MstxManagerTest, ReportMarkATest) {
    const char* msg = "Test Message A";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream);
}

TEST(MstxManagerTest, ReportMarkATest_Nullptr_Msg_Strcpy_Failed) {
    const char* msg = nullptr;
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream);
}

TEST(MstxManagerTest, ReportRangeStartTest) {
    const char* msg = "Test Message A";
    aclrtStream stream = nullptr;
    std::uint64_t rangeId = MstxManager::GetInstance().ReportRangeStart(msg, stream);
    EXPECT_GT(rangeId, 0);
}

TEST(MstxManagerTest, ReportRangeStartTest_Nullptr_Msg_Strcpy_Failed) {
    const char* msg = nullptr;
    aclrtStream stream = nullptr;
    std::uint64_t rangeId = MstxManager::GetInstance().ReportRangeStart(msg, stream);
    EXPECT_GT(rangeId, 0);
}

TEST(MstxManagerTest, ReportRangeEndTest) {
    MstxManager::GetInstance().ReportRangeEnd(1);
}

TEST(MstxManagerTest, GetRangeIdTest) {
    const char* msg = "GetRangeIdTest";
    aclrtStream stream = nullptr;
    uint64_t firstId = MstxManager::GetInstance().ReportRangeStart(msg, stream);
    uint64_t secondId = MstxManager::GetInstance().ReportRangeStart(msg, stream);
    EXPECT_EQ(secondId, firstId +1);
}

TEST(MstxManagerTest, ReportHeapRegisterTest_Get_Nullptr)
{
    mstxDomainHandle_t domain = nullptr;
    mstxMemHeapDesc_t const *desc = nullptr;
    auto ret = MstxManager::GetInstance().ReportHeapRegister(domain, desc);
    ASSERT_EQ(ret, nullptr);
}

TEST(MstxManagerTest, ReportRegionsRegisterTest_Get_Nullptr)
{
    mstxDomainHandle_t domain = nullptr;
    mstxMemRegionsRegisterBatch_t const *desc = nullptr;
    MstxManager::GetInstance().ReportRegionsRegister(domain, desc);
}

TEST(MstxManagerTest, ReportRegionsUnregisterTest_Get_Nullptr)
{
    mstxDomainHandle_t domain = nullptr;
    mstxMemRegionsUnregisterBatch_t const *desc = nullptr;
    MstxManager::GetInstance().ReportRegionsUnregister(domain, desc);
}

// aten前缀消息：追踪态下应取stream id并交给AtenManager处理
TEST(MstxManagerExtraTest, report_mark_a_aten_branch_uses_stream_id)
{
    MstxStateGuard stateGuard;
    StreamIdMockGuard guard;
    g_streamIdMockCalls = 0;
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);

    aclrtStream stream = reinterpret_cast<aclrtStream>(0x2000);
    MstxManager::GetInstance().ReportMarkA("memscope-aten-b:ut_op", stream);

    // GetStreamID仅在成功取到符号后固化函数指针，注入的桩必然被调用一次
    EXPECT_EQ(g_streamIdMockCalls, 1);
}

// 非追踪态下普通打点直接返回，不取stream id
TEST(MstxManagerExtraTest, report_mark_a_not_tracing_skips_stream_id)
{
    MstxStateGuard stateGuard;
    StreamIdMockGuard guard;
    g_streamIdMockCalls = 0;
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);

    aclrtStream stream = reinterpret_cast<aclrtStream>(0x3000);
    MstxManager::GetInstance().ReportMarkA("ut_normal_mark", stream);

    EXPECT_EQ(g_streamIdMockCalls, 0);
}
