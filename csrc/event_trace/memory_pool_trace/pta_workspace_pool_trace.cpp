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
#include <string>
#include "utils.h"
#include "event_report.h"
#include "call_stack.h"
#include "describe_trace.h"
#include "pta_workspace_pool_trace.h"

namespace MemScope {
PTAWorkspacePoolTrace::PTAWorkspacePoolTrace()
{
    ptaWorkspaceDomain_ = new mstxDomainRegistration_st { };
}

PTAWorkspacePoolTrace::~PTAWorkspacePoolTrace()
{
    delete ptaWorkspaceDomain_;
    ptaWorkspaceDomain_ = nullptr;
}

mstxMemHeapHandle_t PTAWorkspacePoolTrace::Allocate(mstxDomainHandle_t domain, mstxMemHeapDesc_t const *desc)
{
    if (domain == nullptr || desc == nullptr || domain != ptaWorkspaceDomain_) {
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(mutex_);

    const mstxMemVirtualRangeDesc_t *rangeDesc =
        reinterpret_cast<const mstxMemVirtualRangeDesc_t *>(desc->typeSpecificDesc);
    int64_t memSize = rangeDesc->size;
    int devId = rangeDesc->deviceId;
    memUsageMp_[devId].totalReserved = memSize;
    heapHandleMp_[rangeDesc->ptr] = *rangeDesc;

    return nullptr;
}

void PTAWorkspacePoolTrace::Deallocate(mstxDomainHandle_t domain, mstxMemHeapHandle_t heap)
{
    if (domain == nullptr || heap == nullptr || domain != ptaWorkspaceDomain_) {
        return;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    void *ptr = reinterpret_cast<void *>(heap);
    if (heapHandleMp_.count(ptr)) {
        auto desc = heapHandleMp_[ptr];
        memUsageMp_[desc.deviceId].totalReserved =
            Utility::GetSubResult(memUsageMp_[desc.deviceId].totalReserved, desc.size);
    }
}

void PTAWorkspacePoolTrace::Reallocate(mstxDomainHandle_t domain, mstxMemRegionsRegisterBatch_t const *desc)
{
    if (domain == nullptr || desc == nullptr || domain != ptaWorkspaceDomain_) {
        return;
    }
    std::lock_guard<std::mutex> guard(mutex_);

    const mstxMemVirtualRangeDesc_t *rangeDescArray =
        reinterpret_cast<const mstxMemVirtualRangeDesc_t *>(desc->regionDescArray);

    CallStackString stack;
    Utility::GetCallstack(stack);

    for (size_t i = 0; i < desc->regionCount; i++) {
        uint32_t devId = rangeDescArray[i].deviceId;
        MemoryUsage& usage = memUsageMp_[devId];
        usage.dataType = 0;
        usage.deviceIndex = devId;
        usage.ptr = reinterpret_cast<int64_t>(rangeDescArray[i].ptr);
        usage.allocSize = rangeDescArray[i].size;
        usage.totalAllocated = usage.allocSize;
        regionHandleMp_[rangeDescArray[i].ptr] = rangeDescArray[i];
        std::string owner = DescribeTrace::GetInstance().GetDescribe();

        if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportMemPoolRecord(
            EventSubType::PTA_WORKSPACE, usage, owner, std::move(stack))) {
            LOG_ERROR("Report PTA Workspace Data Failed");
        }
    }
}

void PTAWorkspacePoolTrace::Release(mstxDomainHandle_t domain, mstxMemRegionsUnregisterBatch_t const *desc)
{
    if (domain == nullptr || desc == nullptr || domain != ptaWorkspaceDomain_) {
        return;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    
    CallStackString stack;
    Utility::GetCallstack(stack);

    for (size_t i = 0; i < desc->refCount; i++) {
        if (!regionHandleMp_.count(desc->refArray[i].pointer)) {
            continue;
        }
        mstxMemVirtualRangeDesc_t rangeDesc = regionHandleMp_[desc->refArray[i].pointer];
        MemoryUsage& usage = memUsageMp_[rangeDesc.deviceId];
        usage.dataType = 1;
        usage.deviceIndex = rangeDesc.deviceId;
        usage.ptr = reinterpret_cast<int64_t>(rangeDesc.ptr);
        usage.totalAllocated = Utility::GetSubResult(usage.totalAllocated, rangeDesc.size);
        usage.allocSize = rangeDesc.size;

        if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportMemPoolRecord(
            EventSubType::PTA_WORKSPACE, usage, "", std::move(stack))) {
            LOG_ERROR("Report PTA Workspace Data Failed");
        }
    }
}

mstxDomainHandle_t PTAWorkspacePoolTrace::CreateDomain(const std::string &domainName)
{
    if (domainName == "ptaWorkspace") {
        return ptaWorkspaceDomain_;
    }
    return nullptr;
}
}