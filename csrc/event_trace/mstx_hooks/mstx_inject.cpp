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
#include "mstx_inject.h"
#include <iostream>
#include "log.h"
#include "mstx_manager.h"
#include "event_report.h"

namespace MemScope {

aclError GetStreamID(aclrtStream stream, int32_t *streamId)
{
    char const *sym = "aclrtStreamGetIdImpl";
    using AclrtGetStreamID = decltype(&GetStreamID);
    static AclrtGetStreamID vallina = nullptr;
    if (vallina == nullptr) {
        vallina = VallinaSymbol<ACLImplLibLoader>::Instance().Get<AclrtGetStreamID>(sym);
    }
    if (vallina == nullptr) {
        LOG_ERROR("vallina func get FAILED: %s", __func__);
        return ACL_ERROR_RT_FAILURE;
    }
    aclError ret = vallina(stream, streamId);
    return ret;
}

void MstxMarkAFunc(const char* msg, aclrtStream stream)
{
    if (!EventTraceManager::Instance().IsTracingEnabled()) {
        return;
    }
    int32_t streamId = -1;
    GetStreamID(stream, &streamId);
    MstxManager::GetInstance().ReportMarkA(msg, streamId);
}

uint64_t MstxRangeStartAFunc(const char* msg, aclrtStream stream)
{
    if (!EventTraceManager::Instance().IsTracingEnabled()) {
        return 0;
    }
    int32_t streamId = -1;
    GetStreamID(stream, &streamId);
    return MstxManager::GetInstance().ReportRangeStart(msg, streamId);
}

void  MstxRangeEndFunc(uint64_t id)
{
    if (!EventTraceManager::Instance().IsTracingEnabled()) {
        return;
    }
    MstxManager::GetInstance().ReportRangeEnd(id);
}

bool MstxTableCoreInject(MstxGetModuleFuncTableFunc getFuncTable)
{
    unsigned int outSize = 0;
    MstxFuncTable outTable;
    if (getFuncTable == nullptr ||
        getFuncTable(mstxFuncModule::MSTX_API_MODULE_CORE, &outTable, &outSize) != MSTX_SUCCESS ||
        outTable == nullptr) {
        LOG_ERROR("Failed to call getFuncTablecore");
        return false;
    }

    if (outSize >= static_cast<unsigned int>(mstxImplCoreFuncId::MSTX_API_CORE_MARK_A)) {
        *(outTable[static_cast<unsigned int>(mstxImplCoreFuncId::MSTX_API_CORE_MARK_A)]) =
            reinterpret_cast<MstxFuncPointer>(MstxMarkAFunc);
    }

    if (outSize >= static_cast<unsigned int>(mstxImplCoreFuncId::MSTX_API_CORE_RANGE_START_A)) {
        *(outTable[static_cast<unsigned int>(mstxImplCoreFuncId::MSTX_API_CORE_RANGE_START_A)]) =
            reinterpret_cast<MstxFuncPointer>(MstxRangeStartAFunc);
    }

    if (outSize >= static_cast<unsigned int>(mstxImplCoreFuncId::MSTX_API_CORE_RANGE_END)) {
        *(outTable[static_cast<unsigned int>(mstxImplCoreFuncId::MSTX_API_CORE_RANGE_END)]) =
            reinterpret_cast<MstxFuncPointer>(MstxRangeEndFunc);
    }
    return true;
}

bool MstxTableDomainCoreInject(MstxGetModuleFuncTableFunc getFuncTable)
{
    unsigned int outSize = 0;
    MstxFuncTable outTable;
    if (getFuncTable == nullptr ||
        getFuncTable(mstxFuncModule::MSTX_API_MODULE_CORE_DOMAIN, &outTable, &outSize) != MSTX_SUCCESS ||
        outTable == nullptr) {
        LOG_ERROR("Failed to call getFuncTableDomaincore");
        return false;
    }

    if (outSize >= static_cast<unsigned int>(mstxImplCoreDomainFuncId::MSTX_API_CORE2_DOMAIN_CREATE_A)) {
        *(outTable[static_cast<unsigned int>(mstxImplCoreDomainFuncId::MSTX_API_CORE2_DOMAIN_CREATE_A)]) =
        reinterpret_cast<MstxFuncPointer>(MstxDomainCreateAFunc);
    }
    return true;
}

bool MstxTableMemCoreInject(MstxGetModuleFuncTableFunc getFuncTable)
{
    unsigned int outSize = 0;
    MstxFuncTable outTable;
    if (getFuncTable == nullptr ||
        getFuncTable(mstxFuncModule::MSTX_API_MODULE_CORE_MEM, &outTable, &outSize) != MSTX_SUCCESS ||
        outTable == nullptr) {
        LOG_ERROR("Failed to call getFuncTableMemcore");
        return false;
    }

    if (outSize >= static_cast<unsigned int>(mstxImplCoreMemFuncId::MSTX_API_CORE_MEMHEAP_REGISTER)) {
        *(outTable[static_cast<unsigned int>(mstxImplCoreMemFuncId::MSTX_API_CORE_MEMHEAP_REGISTER)]) =
        reinterpret_cast<MstxFuncPointer>(MstxMemHeapRegisterFunc);
    }
    if (outSize >= static_cast<unsigned int>(mstxImplCoreMemFuncId::MSTX_API_CORE_MEMHEAP_UNREGISTER)) {
        *(outTable[static_cast<unsigned int>(mstxImplCoreMemFuncId::MSTX_API_CORE_MEMHEAP_UNREGISTER)]) =
        reinterpret_cast<MstxFuncPointer>(MstxMemHeapUnregisterFunc);
    }
    if (outSize >= static_cast<unsigned int>(mstxImplCoreMemFuncId::MSTX_API_CORE_MEM_REGIONS_REGISTER)) {
        *(outTable[static_cast<unsigned int>(mstxImplCoreMemFuncId::MSTX_API_CORE_MEM_REGIONS_REGISTER)]) =
        reinterpret_cast<MstxFuncPointer>(MstxMemRegionsRegisterFunc);
    }
    if (outSize >= static_cast<unsigned int>(mstxImplCoreMemFuncId::MSTX_API_CORE_MEM_REGIONS_UNREGISTER)) {
        *(outTable[static_cast<unsigned int>(mstxImplCoreMemFuncId::MSTX_API_CORE_MEM_REGIONS_UNREGISTER)]) =
        reinterpret_cast<MstxFuncPointer>(MstxMemRegionsUnregisterFunc);
    }
    
    return true;
}

mstxDomainHandle_t MstxDomainCreateAFunc(char const *domainName)
{
    // 总开关控制domain的创建和数据的上报
    if (!EventTraceManager::Instance().IsTracingEnabled()) {
        return nullptr;
    }
    return MstxManager::GetInstance().ReportDomainCreateA(domainName);
}

mstxMemHeapHandle_t MstxMemHeapRegisterFunc(mstxDomainHandle_t domain, mstxMemHeapDesc_t const *desc)
{
    return MstxManager::GetInstance().ReportHeapRegister(domain, desc);
}

void MstxMemHeapUnregisterFunc(mstxDomainHandle_t domain, mstxMemHeapHandle_t heap)
{
    MstxManager::GetInstance().ReportHeapUnregister(domain, heap);
}

void MstxMemRegionsRegisterFunc(mstxDomainHandle_t domain, mstxMemRegionsRegisterBatch_t const *desc)
{
    MstxManager::GetInstance().ReportRegionsRegister(domain, desc);
}

void MstxMemRegionsUnregisterFunc(mstxDomainHandle_t domain, mstxMemRegionsUnregisterBatch_t const *desc)
{
    MstxManager::GetInstance().ReportRegionsUnregister(domain, desc);
}

}

using namespace MemScope;

extern "C" int __attribute__((visibility("default"))) InitInjectionMstx(MstxGetModuleFuncTableFunc getFuncTable)
{
    bool isCoreInjection = MstxTableCoreInject(getFuncTable);
    bool isDomainCoreInjection = MstxTableDomainCoreInject(getFuncTable);
    bool isMemCoreInjection = MstxTableMemCoreInject(getFuncTable);
    if (isCoreInjection && isDomainCoreInjection && isMemCoreInjection) {
        return MSTX_SUCCESS;
    }
    return MSTX_FAIL;
}