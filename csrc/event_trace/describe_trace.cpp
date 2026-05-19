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
#include "describe_trace.h"
#include <cstring>
#include <algorithm>
#include "securec.h"
#include "call_stack.h"
#include "event_report.h"
#include "record_info.h"
#include "log.h"
#include "ustring.h"

namespace MemScope {

std::string DescribeTrace::GetDescribe()
{
    std::string res;
    auto tid = Utility::GetTid();
    for (auto s : describe_[tid]) {
        res += "@" + s;
    }
    return res;
}

bool DescribeTrace::IsRepeat(uint64_t threadId, std::string owner)
{
    Utility::ToSafeString(owner);
    for (auto s : describe_[threadId]) {
        if (s == owner) {
            return true;
        }
    }
    return false;
}

void DescribeTrace::DescribeAddr(uint64_t addr, std::string owner)
{
    EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAddrInfo(EventSubType::DESCRIBE_OWNER, addr, owner);
    return;
}

void DescribeTrace::AddDescribe(std::string owner)
{
    auto tid = Utility::GetTid();
    Utility::ToSafeString(owner);
    if (IsRepeat(tid, owner)) {
        LOG_ERROR("Cannot add duplicate tags %s", owner.c_str());
        return;
    }
    if (describe_[tid].size() >= maxSize) {
        LOG_ERROR("The current thread label exceeds %u", static_cast<uint32_t>(maxSize));
        return;
    }
    describe_[tid].emplace_back(owner);
}

void DescribeTrace::EraseDescribe(std::string owner)
{
    auto tid = Utility::GetTid();
    Utility::ToSafeString(owner);
    size_t i;
    size_t siz = describe_[tid].size();
    for (i = 0; i < siz; i++) {
        if (owner == describe_[tid][i]) {
            describe_[tid].erase(describe_[tid].begin() + i);
            break;
        }
    }
    if (i == siz) {
        LOG_ERROR("Tag %s not found", owner.c_str());
    }
}

}