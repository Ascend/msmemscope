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
using namespace MemScope;

TEST(MstxManagerTest, ReportMarkATest) {
    const char* msg = "Test Message A";
    aclrtStream stream = nullptr;
    MstxManager::GetInstance().ReportMarkA(msg, stream);
}

TEST(MstxManagerTest, ReportMarkATest_Nullptr_Msg_Strcpy_Failed) {
    const char* msg = "";
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
    const char* msg = "";
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