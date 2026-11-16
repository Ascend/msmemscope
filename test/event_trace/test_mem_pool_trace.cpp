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
#include <sstream>
#define private public
#include "event_trace/memory_pool_trace/memory_pool_trace_manager.h"
#include "event_trace/mstx_hooks/mstx_inject.h"
#include "event_trace/memory_pool_trace/atb_memory_pool_trace.h"
#include "event_trace/memory_pool_trace/mindspore_memory_pool_trace.h"
#include "event_trace/memory_pool_trace/pta_caching_pool_trace.h"
#include "event_trace/memory_pool_trace/pta_workspace_pool_trace.h"

#undef private
#include <gtest/gtest.h>

using namespace MemScope;

TEST(MemPoolTraceTest, MemPoolTraceTestATBHeapRegisterAndRegionRegister)
{
    auto domainHandle = MstxDomainCreateAFunc("atb");
    EXPECT_EQ(domainHandle, ATBMemoryPoolTrace::GetInstance().atbDomain_);

    mstxMemHeapDesc_t heapDesc;
    uint32_t deviceId = 1;
    void const* ptrHeap = reinterpret_cast<void const*>(123);
    mstxMemVirtualRangeDesc_t memRangeDesc{deviceId, ptrHeap, 500};
    heapDesc.typeSpecificDesc = reinterpret_cast<void const*>(&memRangeDesc);
    auto HeapHandle = MstxMemHeapRegisterFunc(domainHandle, &heapDesc);
    EXPECT_EQ(ATBMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 500);

    void const* ptr = reinterpret_cast<void const*>(123);
    memRangeDesc = {deviceId, ptr, 50};
    mstxMemRegionsRegisterBatch_t desc;
    desc.regionCount = 1;
    desc.regionDescArray = reinterpret_cast<const void *>(&memRangeDesc);
    auto mstxMemRegionHandle = mstxMemRegionHandle_t {};
    auto handleArrayOut = &mstxMemRegionHandle;
    desc.regionHandleArrayOut = handleArrayOut;
    MstxMemRegionsRegisterFunc(domainHandle, &desc);
    EXPECT_EQ(ATBMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 50);

    mstxMemRegionsUnregisterBatch_t unregisterBatch;
    unregisterBatch.refCount = 1;
    auto mstxMemRegionRef = new mstxMemRegionRef_t {};

    mstxMemRegionRef->handle = mstxMemRegionHandle;
    unregisterBatch.refArray = mstxMemRegionRef;

    MstxMemRegionsUnregisterFunc(domainHandle, &unregisterBatch);
    EXPECT_EQ(ATBMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 0);

    MstxMemHeapUnregisterFunc(domainHandle, HeapHandle);
    EXPECT_EQ(ATBMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 0);
}

TEST(MemPoolTraceTest, MemPoolTraceTestAllocateGetNullPtr)
{
    mstxDomainHandle_t domain = nullptr;
    mstxMemHeapDesc_t const *desc;
    EXPECT_EQ(ATBMemoryPoolTrace::GetInstance().Allocate(domain, desc), nullptr);
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().Allocate(domain, desc), nullptr);
}

TEST(MemPoolTraceTest, MemPoolTraceTestDeallocateGetNullPtr)
{
    mstxDomainHandle_t domain = nullptr;
    mstxMemHeapHandle_t desc;
    ATBMemoryPoolTrace::GetInstance().Deallocate(domain, desc);
    MindsporeMemoryPoolTrace::GetInstance().Deallocate(domain, desc);
}

TEST(MemPoolTraceTest, MemPoolTraceTestReallocateGetNullPtr)
{
    mstxDomainHandle_t domain = nullptr;
    mstxMemRegionsRegisterBatch_t const *desc;
    ATBMemoryPoolTrace::GetInstance().Reallocate(domain, desc);
    MindsporeMemoryPoolTrace::GetInstance().Reallocate(domain, desc);
}

TEST(MemPoolTraceTest, MemPoolTraceTestReleaseGetNullPtr)
{
    mstxDomainHandle_t domain = nullptr;
    mstxMemRegionsUnregisterBatch_t const *desc;
    ATBMemoryPoolTrace::GetInstance().Release(domain, desc);
    MindsporeMemoryPoolTrace::GetInstance().Release(domain, desc);
}

TEST(MemPoolTraceTest, ATBMemoryPoolTraceCreateDomainReturnNull)
{
    EXPECT_EQ(ATBMemoryPoolTrace::GetInstance().CreateDomain("test"), nullptr);
}

TEST(MemPoolTraceTest, MemPoolTraceTestMindsporeHeapRegisterAndRegionRegister)
{
    auto domainHandle = MstxDomainCreateAFunc("mindsporeMemPool");
    EXPECT_EQ(domainHandle, MindsporeMemoryPoolTrace::GetInstance().mindsporeDomain_);

    mstxMemHeapDesc_t heapDesc;
    uint32_t deviceId = 1;
    void const* ptrHeap = reinterpret_cast<void const*>(123);
    mstxMemVirtualRangeDesc_t memRangeDesc{deviceId, ptrHeap, 500};
    heapDesc.typeSpecificDesc = reinterpret_cast<void const*>(&memRangeDesc);
    auto HeapHandle = MstxMemHeapRegisterFunc(domainHandle, &heapDesc);
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 500);

    void const* ptr = reinterpret_cast<void const*>(123);
    memRangeDesc = {deviceId, ptr, 50};
    mstxMemRegionsRegisterBatch_t desc;
    desc.regionCount = 1;
    desc.regionDescArray = reinterpret_cast<const void *>(&memRangeDesc);
    auto mstxMemRegionHandle = mstxMemRegionHandle_t {};
    auto handleArrayOut = &mstxMemRegionHandle;
    desc.regionHandleArrayOut = handleArrayOut;
    MstxMemRegionsRegisterFunc(domainHandle, &desc);
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 50);

    mstxMemRegionsUnregisterBatch_t unregisterBatch;
    unregisterBatch.refCount = 1;
    auto mstxMemRegionRef = new mstxMemRegionRef_t {};

    mstxMemRegionRef->pointer = ptr;
    unregisterBatch.refArray = mstxMemRegionRef;

    MstxMemRegionsUnregisterFunc(domainHandle, &unregisterBatch);
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 0);

    MstxMemHeapUnregisterFunc(domainHandle, static_cast<mstxMemHeapHandle_t>(const_cast<void *>(ptrHeap)));
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 0);
}

TEST(MemPoolTraceTest, MemPoolTraceTestMindsporeHeapRegisterAndRegionRegisterReturnNull)
{
    auto domainHandle = MstxDomainCreateAFunc("mindspore");
    EXPECT_NE(domainHandle, MindsporeMemoryPoolTrace::GetInstance().mindsporeDomain_);

    mstxMemHeapDesc_t heapDesc;
    uint32_t deviceId = 1;
    void const* ptrHeap = reinterpret_cast<void const*>(123);
    mstxMemVirtualRangeDesc_t memRangeDesc{deviceId, ptrHeap, 500};
    heapDesc.typeSpecificDesc = reinterpret_cast<void const*>(&memRangeDesc);
    auto HeapHandle = MstxMemHeapRegisterFunc(domainHandle, &heapDesc);
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 0);

    void const* ptr = reinterpret_cast<void const*>(123);
    memRangeDesc = {deviceId, ptr, 50};
    mstxMemRegionsRegisterBatch_t desc;
    desc.regionCount = 1;
    desc.regionDescArray = reinterpret_cast<const void *>(&memRangeDesc);
    auto mstxMemRegionHandle = mstxMemRegionHandle_t {};
    auto handleArrayOut = &mstxMemRegionHandle;
    desc.regionHandleArrayOut = handleArrayOut;
    MstxMemRegionsRegisterFunc(domainHandle, &desc);
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 0);

    mstxMemRegionsUnregisterBatch_t unregisterBatch;
    unregisterBatch.refCount = 1;
    auto mstxMemRegionRef = new mstxMemRegionRef_t {};

    mstxMemRegionRef->handle = mstxMemRegionHandle;
    unregisterBatch.refArray = mstxMemRegionRef;

    MstxMemRegionsUnregisterFunc(domainHandle, &unregisterBatch);
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 0);

    MstxMemHeapUnregisterFunc(domainHandle, HeapHandle);
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 0);
}

TEST(MemPoolTraceTest, MindsporeMemoryPoolTraceReleaseRegionHandleMpNull)
{
    mstxDomainRegistration_t domainRegistration{};
    mstxDomainHandle_t  domainHandle = &domainRegistration;
    mstxMemRegionsUnregisterBatch_t desc;
    void const* ptr = reinterpret_cast<void const*>(123);
    mstxMemRegionRef_t memRangeDesc = {mstxMemRegionRefType::MSTX_MEM_REGION_REF_TYPE_HANDLE, ptr};
    desc.refCount = 1;
    desc.refArray = &memRangeDesc;
    MindsporeMemoryPoolTrace::GetInstance().regionHandleMp_ = {};
    MindsporeMemoryPoolTrace::GetInstance().Release(domainHandle, &desc);
}

TEST(MemPoolTraceTest, MemoryPoolTraceManagerRelease)
{
    mstxDomainRegistration_t domainRegistration{};
    mstxDomainHandle_t  domainHandle = &domainRegistration;
    mstxMemRegionsUnregisterBatch_t desc;
    void const* ptr = reinterpret_cast<void const*>(123);
    mstxMemRegionRef_t memRangeDesc = {mstxMemRegionRefType::MSTX_MEM_REGION_REF_TYPE_HANDLE, ptr};
    desc.refCount = 1;
    desc.refArray = &memRangeDesc;
    MemoryPoolTraceManager::GetInstance().Release(domainHandle, &desc);
}

TEST(MemPoolTraceTest, MemoryPoolTraceManagerAllocate)
{
    mstxDomainRegistration_t domainRegistration{};
    mstxDomainHandle_t  domainHandle = &domainRegistration;
    mstxMemHeapDesc_t heapDesc;
    uint32_t deviceId = 1;
    void const* ptrHeap = reinterpret_cast<void const*>(123);
    mstxMemVirtualRangeDesc_t memRangeDesc{deviceId, ptrHeap, 500};
    heapDesc.typeSpecificDesc = reinterpret_cast<void const*>(&memRangeDesc);
    MemoryPoolTraceManager::GetInstance().Allocate(domainHandle, &heapDesc);
}

TEST(MemPoolTraceTest, MemoryPoolTraceManagerDeallocate)
{
    mstxDomainRegistration_t domainRegistration{};
    mstxDomainHandle_t  domainHandle = &domainRegistration;
    mstxMemHeapHandle_t heap;
    MemoryPoolTraceManager::GetInstance().Deallocate(domainHandle, heap);
}

TEST(MemPoolTraceTest, MemoryPoolTraceManagerReallocate)
{
    mstxDomainRegistration_t domainRegistration{};
    mstxDomainHandle_t  domainHandle = &domainRegistration;
    void const* ptr = reinterpret_cast<void const*>(123);
    mstxMemVirtualRangeDesc_t memRangeDesc = {1, ptr, 50};
    mstxMemRegionsRegisterBatch_t desc;
    desc.regionCount = 1;
    desc.regionDescArray = reinterpret_cast<const void *>(&memRangeDesc);
    MemoryPoolTraceManager::GetInstance().Reallocate(domainHandle, &desc);
}

TEST(MemPoolTraceTest, MindsporeMemoryPoolTraceCreateDomainReturnNull)
{
    EXPECT_EQ(MindsporeMemoryPoolTrace::GetInstance().CreateDomain("mdspe"), nullptr);
}

TEST(MemPoolTraceTest, PTACachingPoolTraceTestPtaHeapRegisterAndRegionRegister)
{
    auto domainHandle = MstxDomainCreateAFunc("ptaCaching");
    EXPECT_EQ(domainHandle, PTACachingPoolTrace::GetInstance().ptaCachingDomain_);
 
    mstxMemHeapDesc_t heapDesc;
    uint32_t deviceId = 1;
    void const* ptrHeap = reinterpret_cast<void const*>(123);
    mstxMemVirtualRangeDesc_t memRangeDesc{deviceId, ptrHeap, 500};                 // 生成一个虚拟内存结构体
    heapDesc.typeSpecificDesc = reinterpret_cast<void const*>(&memRangeDesc);
    auto HeapHandle = MstxMemHeapRegisterFunc(domainHandle, &heapDesc);
    EXPECT_EQ(PTACachingPoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 500);
 
    void const* ptr = reinterpret_cast<void const*>(123);
    memRangeDesc = {deviceId, ptr, 50};
    mstxMemRegionsRegisterBatch_t desc;
    desc.regionCount = 1;
    desc.regionDescArray = reinterpret_cast<const void *>(&memRangeDesc);
    auto mstxMemRegionHandle = mstxMemRegionHandle_t {};
    auto handleArrayOut = &mstxMemRegionHandle;
    desc.regionHandleArrayOut = handleArrayOut;
    MstxMemRegionsRegisterFunc(domainHandle, &desc);
    EXPECT_EQ(PTACachingPoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 50);
 
    mstxMemRegionsUnregisterBatch_t unregisterBatch;
    unregisterBatch.refCount = 1;
    auto mstxMemRegionRef = new mstxMemRegionRef_t {};
 
    mstxMemRegionRef->pointer = ptr;
    unregisterBatch.refArray = mstxMemRegionRef;
 
    MstxMemRegionsUnregisterFunc(domainHandle, &unregisterBatch);
    EXPECT_EQ(PTACachingPoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 0);
 
    MstxMemHeapUnregisterFunc(domainHandle, static_cast<mstxMemHeapHandle_t>(const_cast<void *>(ptrHeap)));
    EXPECT_EQ(PTACachingPoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 0);
}

TEST(MemPoolTraceTest, PTACachingPoolTraceReleaseRegionHandleMpNull)
{
    mstxDomainRegistration_t domainRegistration{};
    mstxDomainHandle_t  domainHandle = &domainRegistration;
    mstxMemRegionsUnregisterBatch_t desc;
    void const* ptr = reinterpret_cast<void const*>(123);
    mstxMemRegionRef_t memRangeDesc = {mstxMemRegionRefType::MSTX_MEM_REGION_REF_TYPE_HANDLE, ptr};
    desc.refCount = 1;
    desc.refArray = &memRangeDesc;
    PTACachingPoolTrace::GetInstance().regionHandleMp_ = {};
    PTACachingPoolTrace::GetInstance().Release(domainHandle, &desc);
}

TEST(MemPoolTraceTest, PTACachingPoolTraceCreateDomainReturnNull)
{
    EXPECT_EQ(PTACachingPoolTrace::GetInstance().CreateDomain("msmemscope"), nullptr);
}

TEST(MemPoolTraceTest, PTAWorkspacePoolTraceTestPtaHeapRegisterAndRegionRegister)
{
    auto domainHandle = MstxDomainCreateAFunc("ptaWorkspace");
    EXPECT_EQ(domainHandle, PTAWorkspacePoolTrace::GetInstance().ptaWorkspaceDomain_);
 
    mstxMemHeapDesc_t heapDesc;
    uint32_t deviceId = 1;
    void const* ptrHeap = reinterpret_cast<void const*>(123);
    mstxMemVirtualRangeDesc_t memRangeDesc{deviceId, ptrHeap, 500};                 // 生成一个虚拟内存结构体
    heapDesc.typeSpecificDesc = reinterpret_cast<void const*>(&memRangeDesc);
    auto HeapHandle = MstxMemHeapRegisterFunc(domainHandle, &heapDesc);
    EXPECT_EQ(PTAWorkspacePoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 500);
 
    void const* ptr = reinterpret_cast<void const*>(123);
    memRangeDesc = {deviceId, ptr, 50};
    mstxMemRegionsRegisterBatch_t desc;
    desc.regionCount = 1;
    desc.regionDescArray = reinterpret_cast<const void *>(&memRangeDesc);
    auto mstxMemRegionHandle = mstxMemRegionHandle_t {};
    auto handleArrayOut = &mstxMemRegionHandle;
    desc.regionHandleArrayOut = handleArrayOut;
    MstxMemRegionsRegisterFunc(domainHandle, &desc);
    EXPECT_EQ(PTAWorkspacePoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 50);
 
    mstxMemRegionsUnregisterBatch_t unregisterBatch;
    unregisterBatch.refCount = 1;
    auto mstxMemRegionRef = new mstxMemRegionRef_t {};
 
    mstxMemRegionRef->pointer = ptr;
    unregisterBatch.refArray = mstxMemRegionRef;
 
    MstxMemRegionsUnregisterFunc(domainHandle, &unregisterBatch);
    EXPECT_EQ(PTAWorkspacePoolTrace::GetInstance().memUsageMp_[deviceId].totalAllocated, 0);
 
    MstxMemHeapUnregisterFunc(domainHandle, static_cast<mstxMemHeapHandle_t>(const_cast<void *>(ptrHeap)));
    EXPECT_EQ(PTAWorkspacePoolTrace::GetInstance().memUsageMp_[deviceId].totalReserved, 0);
}

TEST(MemPoolTraceTest, PTAWorkspacePoolTraceReleaseRegionHandleMpNull)
{
    mstxDomainRegistration_t domainRegistration{};
    mstxDomainHandle_t  domainHandle = &domainRegistration;
    mstxMemRegionsUnregisterBatch_t desc;
    void const* ptr = reinterpret_cast<void const*>(123);
    mstxMemRegionRef_t memRangeDesc = {mstxMemRegionRefType::MSTX_MEM_REGION_REF_TYPE_HANDLE, ptr};
    desc.refCount = 1;
    desc.refArray = &memRangeDesc;
    PTAWorkspacePoolTrace::GetInstance().regionHandleMp_ = {};
    PTAWorkspacePoolTrace::GetInstance().Release(domainHandle, &desc);
}

TEST(MemPoolTraceTest, PTAWorkspacePoolTraceCreateDomainReturnNull)
{
    EXPECT_EQ(PTAWorkspacePoolTrace::GetInstance().CreateDomain("msmemscope"), nullptr);
}

// manager未知domain/重复注册路径
TEST(MemPoolTraceTest, MemoryPoolTraceManagerUnknownAndDuplicateDomain)
{
    auto &manager = MemoryPoolTraceManager::GetInstance();
    const std::string dupName = "ut_dup_domain";
    manager.tracers_.erase(dupName);

    EXPECT_TRUE(manager.RegisterMemoryPoolTracer(dupName, &ATBMemoryPoolTrace::GetInstance()));
    // 同名重复注册返回false
    EXPECT_FALSE(manager.RegisterMemoryPoolTracer(dupName, &ATBMemoryPoolTrace::GetInstance()));

    manager.tracers_.erase(dupName);
    // 未注册的domain：查询与创建均返回空
    EXPECT_EQ(manager.GetMemoryPoolTracer(dupName), nullptr);
    EXPECT_EQ(manager.CreateDomain(dupName), nullptr);
}

// PTA Caching：domain为空/不匹配的提前返回；释放未登记指针时跳过
TEST(MemPoolTraceTest, PTACachingPoolTraceEarlyReturnsAndUnknownRelease)
{
    auto &tracer = PTACachingPoolTrace::GetInstance();
    mstxMemVirtualRangeDesc_t rangeDesc{1, reinterpret_cast<void const *>(0x7B7B0001), 10};
    mstxMemHeapDesc_t heapDesc;
    heapDesc.typeSpecificDesc = reinterpret_cast<void const *>(&rangeDesc);
    mstxMemHeapHandle_t heap = reinterpret_cast<mstxMemHeapHandle_t>(0x7B7B0001);

    EXPECT_EQ(tracer.Allocate(nullptr, &heapDesc), nullptr);
    EXPECT_EQ(tracer.Allocate(ATBMemoryPoolTrace::GetInstance().atbDomain_, &heapDesc), nullptr);
    tracer.Deallocate(nullptr, heap);
    tracer.Deallocate(ATBMemoryPoolTrace::GetInstance().atbDomain_, heap);
    tracer.Reallocate(nullptr, nullptr);
    tracer.Reallocate(tracer.ptaCachingDomain_, nullptr);
    tracer.Release(nullptr, nullptr);

    mstxMemRegionRef_t ref;
    ref.refType = MSTX_MEM_REGION_REF_TYPE_POINTER;
    ref.pointer = reinterpret_cast<void const *>(0x7B7B00FF);
    mstxMemRegionsUnregisterBatch_t batch;
    batch.refCount = 1;
    batch.refArray = &ref;
    size_t before = tracer.regionHandleMp_.size();
    tracer.Release(tracer.ptaCachingDomain_, &batch);
    EXPECT_EQ(tracer.regionHandleMp_.size(), before);
}

// PTA Workspace：domain为空/不匹配的提前返回；释放未登记指针时跳过
TEST(MemPoolTraceTest, PTAWorkspacePoolTraceEarlyReturnsAndUnknownRelease)
{
    auto &tracer = PTAWorkspacePoolTrace::GetInstance();
    mstxMemVirtualRangeDesc_t rangeDesc{1, reinterpret_cast<void const *>(0x7B7C0001), 10};
    mstxMemHeapDesc_t heapDesc;
    heapDesc.typeSpecificDesc = reinterpret_cast<void const *>(&rangeDesc);
    mstxMemHeapHandle_t heap = reinterpret_cast<mstxMemHeapHandle_t>(0x7B7C0001);

    EXPECT_EQ(tracer.Allocate(nullptr, &heapDesc), nullptr);
    EXPECT_EQ(tracer.Allocate(ATBMemoryPoolTrace::GetInstance().atbDomain_, &heapDesc), nullptr);
    tracer.Deallocate(nullptr, heap);
    tracer.Deallocate(ATBMemoryPoolTrace::GetInstance().atbDomain_, heap);
    tracer.Reallocate(nullptr, nullptr);
    tracer.Reallocate(tracer.ptaWorkspaceDomain_, nullptr);
    tracer.Release(nullptr, nullptr);

    mstxMemRegionRef_t ref;
    ref.refType = MSTX_MEM_REGION_REF_TYPE_POINTER;
    ref.pointer = reinterpret_cast<void const *>(0x7B7C00FF);
    mstxMemRegionsUnregisterBatch_t batch;
    batch.refCount = 1;
    batch.refArray = &ref;
    size_t before = tracer.regionHandleMp_.size();
    tracer.Release(tracer.ptaWorkspaceDomain_, &batch);
    EXPECT_EQ(tracer.regionHandleMp_.size(), before);
}

// ATB：同device重复注册heap累加reserved；未登记device的二次分配与未登记handle的释放均跳过
TEST(MemPoolTraceTest, ATBMemoryPoolTraceRepeatedHeapAndUnknownPaths)
{
    auto &tracer = ATBMemoryPoolTrace::GetInstance();
    ASSERT_NE(tracer.atbDomain_, nullptr);
    mstxDomainHandle_t domain = tracer.atbDomain_;
    constexpr uint32_t deviceId = 92;
    tracer.memUsageMp_.erase(deviceId);

    void const *ptrHeap1 = reinterpret_cast<void const *>(0x7C7C0001);
    void const *ptrHeap2 = reinterpret_cast<void const *>(0x7C7C0002);
    mstxMemVirtualRangeDesc_t rangeDesc{deviceId, ptrHeap1, 100};
    mstxMemHeapDesc_t heapDesc;
    heapDesc.typeSpecificDesc = reinterpret_cast<void const *>(&rangeDesc);

    // 首次插入；第二次同device走累加分支
    mstxMemHeapHandle_t heap1 = tracer.Allocate(domain, &heapDesc);
    ASSERT_NE(heap1, nullptr);
    rangeDesc.ptr = ptrHeap2;
    rangeDesc.size = 50;
    mstxMemHeapHandle_t heap2 = tracer.Allocate(domain, &heapDesc);
    ASSERT_NE(heap2, nullptr);
    EXPECT_EQ(tracer.memUsageMp_[deviceId].totalReserved, 150);

    // 未登记device的二次分配直接跳过，不写region handle
    mstxMemVirtualRangeDesc_t unknownDesc{deviceId + 100, reinterpret_cast<void const *>(0x7C7C0003), 10};
    mstxMemRegionHandle_t regionHandle = nullptr;
    mstxMemRegionsRegisterBatch_t registerBatch;
    registerBatch.regionCount = 1;
    registerBatch.regionDescArray = reinterpret_cast<void const *>(&unknownDesc);
    registerBatch.regionHandleArrayOut = &regionHandle;
    tracer.Reallocate(domain, &registerBatch);
    EXPECT_EQ(regionHandle, nullptr);
    EXPECT_EQ(tracer.memUsageMp_.count(deviceId + 100), 0U);

    // 未登记handle的释放直接跳过
    mstxMemRegionRef_t unknownRef;
    unknownRef.refType = MSTX_MEM_REGION_REF_TYPE_HANDLE;
    unknownRef.handle = reinterpret_cast<mstxMemRegionHandle_t>(0x7C7C00FF);
    mstxMemRegionsUnregisterBatch_t unknownBatch;
    unknownBatch.refCount = 1;
    unknownBatch.refArray = &unknownRef;
    tracer.Release(domain, &unknownBatch);

    // 清理两个heap，reserved归零
    tracer.Deallocate(domain, heap1);
    tracer.Deallocate(domain, heap2);
    EXPECT_EQ(tracer.memUsageMp_[deviceId].totalReserved, 0);
    tracer.memUsageMp_.erase(deviceId);
}
