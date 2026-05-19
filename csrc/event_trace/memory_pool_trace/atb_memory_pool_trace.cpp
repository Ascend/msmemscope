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
#include "atb_memory_pool_trace.h"

namespace MemScope {
ATBMemoryPoolTrace::ATBMemoryPoolTrace()
{
    atbDomain_ = new mstxDomainRegistration_st { };
}

ATBMemoryPoolTrace::~ATBMemoryPoolTrace()
{
    delete atbDomain_;
    atbDomain_ = nullptr;
}

mstxMemHeapHandle_t ATBMemoryPoolTrace::Allocate(mstxDomainHandle_t domain, mstxMemHeapDesc_t const *desc)
{
    if (domain == nullptr || desc == nullptr || domain != atbDomain_) {
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(mutex_);

    const mstxMemVirtualRangeDesc_t *rangeDesc =
        reinterpret_cast<const mstxMemVirtualRangeDesc_t *>(desc->typeSpecificDesc);
    int64_t memPoolSize = rangeDesc->size;
    
    if (memUsageMp_.count(rangeDesc->deviceId)) {
        memUsageMp_[rangeDesc->deviceId].totalReserved =
            Utility::GetAddResult(memUsageMp_[rangeDesc->deviceId].totalReserved, memPoolSize);
    } else {
        auto memoryUsage = MemoryUsage { };
        memoryUsage.totalReserved = memPoolSize;
        memUsageMp_.insert({rangeDesc->deviceId, memoryUsage});
    }

    heapHandleMp_[rangeDesc->ptr] = *rangeDesc;

    return const_cast<mstxMemHeapHandle_t>(reinterpret_cast<const mstxMemHeap_t*>(rangeDesc->ptr));
}

void ATBMemoryPoolTrace::Deallocate(mstxDomainHandle_t domain, mstxMemHeapHandle_t heap)
{
    if (domain == nullptr || heap == nullptr || domain != atbDomain_) {
        return;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    auto handle = reinterpret_cast<void *>(heap);
    if (heapHandleMp_.count(handle)) {
        mstxMemVirtualRangeDesc_t &rangeDesc = heapHandleMp_[handle];
        if (memUsageMp_.count(rangeDesc.deviceId)) {
            memUsageMp_[rangeDesc.deviceId].totalReserved =
                Utility::GetSubResult(memUsageMp_[rangeDesc.deviceId].totalReserved, rangeDesc.size);
        }
        heapHandleMp_.erase(handle);
    }
    return;
}

void ATBMemoryPoolTrace::Reallocate(mstxDomainHandle_t domain, mstxMemRegionsRegisterBatch_t const *desc)
{
    if (domain == nullptr || desc == nullptr || domain != atbDomain_) {
        return;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    CallStackString stack;
    Utility::GetCallstack(stack);
    const mstxMemVirtualRangeDesc_t *rangeDescArray =
        reinterpret_cast<const mstxMemVirtualRangeDesc_t *>(desc->regionDescArray);

    for (size_t i = 0; i < desc->regionCount; i++) {
        uint32_t devId = rangeDescArray[i].deviceId;

        if (!memUsageMp_.count(devId)) {
            continue;
        }
        MemoryUsage& usage = memUsageMp_[devId];
        usage.dataType = 0;
        usage.deviceIndex = devId;
        usage.ptr = reinterpret_cast<int64_t>(rangeDescArray[i].ptr);
        usage.allocSize = rangeDescArray[i].size;
        usage.totalAllocated =
            Utility::GetAddResult(usage.totalAllocated, usage.allocSize);
        auto handle = new mstxMemRegion_t {};
        desc->regionHandleArrayOut[i] = handle;
        regionHandleMp_[handle] = rangeDescArray[i];
        std::string owner = DescribeTrace::GetInstance().GetDescribe();

        if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportMemPoolRecord(
            EventSubType::ATB, usage, owner, std::move(stack))) {
            LOG_ERROR("Report ATB Data Failed");
        }
    }
}

void ATBMemoryPoolTrace::Release(mstxDomainHandle_t domain, mstxMemRegionsUnregisterBatch_t const *desc)
{
    if (domain == nullptr || desc == nullptr || domain != atbDomain_) {
        return;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    CallStackString stack;
    Utility::GetCallstack(stack);
    for (size_t i = 0; i < desc->refCount; i++) {
        auto handle = desc->refArray[i].handle;
        auto iter = regionHandleMp_.find(handle);
        if (iter == regionHandleMp_.end()) {
            continue;
        }
        mstxMemVirtualRangeDesc_t rangeDesc = iter->second;
        MemoryUsage& usage = memUsageMp_[rangeDesc.deviceId];
        usage.dataType = 1;
        usage.deviceIndex = rangeDesc.deviceId;
        usage.ptr = reinterpret_cast<int64_t>(rangeDesc.ptr);
        usage.totalAllocated = Utility::GetSubResult(usage.totalAllocated, rangeDesc.size);
        usage.allocSize = rangeDesc.size;

        regionHandleMp_.erase(handle);
        delete handle;
        handle = nullptr;

        if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportMemPoolRecord(
            EventSubType::ATB, usage, "", std::move(stack))) {
            LOG_ERROR("Report ATB Data Failed");
        }
    }
}

mstxDomainHandle_t ATBMemoryPoolTrace::CreateDomain(const std::string &domainName)
{
    if (domainName == "atb") {
        return atbDomain_;
    }
    return nullptr;
}
}