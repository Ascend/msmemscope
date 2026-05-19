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

#include "atb_hooks.h"

#include <cstdio>
#include <dlfcn.h>
#include <sstream>
#include <mutex>
#include <vector>

#include "event_report.h"
#include "bit_field.h"
#include "securec.h"
#include "memory_watch/memory_watch.h"
#include "trace_manager/event_trace_manager.h"

using namespace MemScope;

namespace atb {
    static std::string MemScopeGetTensorInfo(const atb::Tensor& tensor)
    {
        std::ostringstream oss;
        oss << "dtype:" << MemScopeEnumToString(tensor.desc.dtype)
            << ",format:" << MemScopeEnumToString(tensor.desc.format)
            << ",shape:[";
        for (size_t i = 0; i < tensor.desc.shape.dimNum; i++) {
            oss << tensor.desc.shape.dims[i] << ",";
        }
        oss << "]";
        return oss.str();
    }

    static std::string MemScopeGetTensorInfo(const Mki::Tensor& tensor)
    {
        std::ostringstream oss;
        oss << "dtype:" << MemScopeEnumToString(tensor.desc.dtype)
            << ",format:" << MemScopeEnumToString(tensor.desc.format)
            << ",shape:[";
        for (auto& dim : tensor.desc.dims) {
            oss << dim << ",";
        }
        oss << "]";
        return oss.str();
    }

    static std::string MemScopeGetOpParams(const atb::RunnerVariantPack& runnerVariantPack, const std::string& path)
    {
        std::ostringstream oss;
        oss << "path:" << path << ",workspace_ptr:"
            << static_cast<void*>(runnerVariantPack.workspaceBuffer) << ",workspace_size:"
            << Utility::GetAddResult(runnerVariantPack.workspaceBufferSize, runnerVariantPack.intermediateBufferSize);
        return oss.str();
    }

    static void MemScopeReportTensors(const atb::RunnerVariantPack& runnerVariantPack, const std::string& name)
    {
        for (auto& tensor : runnerVariantPack.inTensors) {
            char nameStr[ATB_STRING_MAX_LENGTH];
            char attrStr[ATB_STRING_MAX_LENGTH];
            if (strncpy_s(nameStr, ATB_STRING_MAX_LENGTH, name.c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
                nameStr[0] = '\0';
            }
            if (strncpy_s(attrStr, ATB_STRING_MAX_LENGTH, MemScopeGetTensorInfo(tensor).c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
                attrStr[0] = '\0';
            }
            EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAtbAccessMemory(
                nameStr, sizeof(nameStr),
                attrStr, sizeof(attrStr),
                static_cast<uint64_t>((std::uintptr_t)tensor.deviceData),
                tensor.dataSize, AccessType::UNKNOWN);
        }
        for (auto& tensor : runnerVariantPack.outTensors) {
            char nameStr[ATB_STRING_MAX_LENGTH];
            char attrStr[ATB_STRING_MAX_LENGTH];
            if (strncpy_s(nameStr, ATB_STRING_MAX_LENGTH, name.c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
                nameStr[0] = '\0';
            }
            if (strncpy_s(attrStr, ATB_STRING_MAX_LENGTH, MemScopeGetTensorInfo(tensor).c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
                attrStr[0] = '\0';
            }
            EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAtbAccessMemory(
                nameStr, sizeof(nameStr),
                attrStr, sizeof(attrStr),
                static_cast<uint64_t>((std::uintptr_t)tensor.deviceData),
                tensor.dataSize, AccessType::WRITE);
        }
    }

    static void MemScopeReportTensors(Mki::MemScopeOriginalGetInTensors &getInTensors,
        Mki::MemScopeOriginalGetOutTensors &getOutTensors,
        const Mki::LaunchParam &launchParam, const std::string& name)
    {
        for (auto& tensor : getInTensors(const_cast<Mki::LaunchParam*>(&launchParam))) {
            char nameStr[ATB_STRING_MAX_LENGTH];
            char attrStr[ATB_STRING_MAX_LENGTH];
            if (strncpy_s(nameStr, ATB_STRING_MAX_LENGTH, name.c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
                nameStr[0] = '\0';
            }
            if (strncpy_s(attrStr, ATB_STRING_MAX_LENGTH, MemScopeGetTensorInfo(tensor).c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
                attrStr[0] = '\0';
            }
            EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAtbAccessMemory(
                nameStr, sizeof(nameStr),
                attrStr, sizeof(attrStr),
                static_cast<uint64_t>((std::uintptr_t)tensor.data),
                tensor.dataSize, AccessType::UNKNOWN);
        }
        for (auto& tensor : getOutTensors(const_cast<Mki::LaunchParam*>(&launchParam))) {
            char nameStr[ATB_STRING_MAX_LENGTH];
            char attrStr[ATB_STRING_MAX_LENGTH];
            if (strncpy_s(nameStr, ATB_STRING_MAX_LENGTH, name.c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
                nameStr[0] = '\0';
            }
            if (strncpy_s(attrStr, ATB_STRING_MAX_LENGTH, MemScopeGetTensorInfo(tensor).c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
                attrStr[0] = '\0';
            }
            EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAtbAccessMemory(
                nameStr, sizeof(nameStr),
                attrStr, sizeof(attrStr),
                static_cast<uint64_t>((std::uintptr_t)tensor.data),
                tensor.dataSize, AccessType::WRITE);
        }
    }

    static void MemScopeReportOp(const std::string& name, const std::string& params, bool isStart)
    {
        char nameStr[ATB_STRING_MAX_LENGTH];
        char attrStr[ATB_STRING_MAX_LENGTH];
        if (strncpy_s(nameStr, ATB_STRING_MAX_LENGTH, name.c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
            nameStr[0] = '\0';
        }
        if (strncpy_s(attrStr, ATB_STRING_MAX_LENGTH, params.c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
            attrStr[0] = '\0';
        }
        RecordSubType type = isStart ? RecordSubType::ATB_START : RecordSubType::ATB_END;

        if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAtbOpExecute(nameStr, sizeof(nameStr),
            attrStr, sizeof(attrStr), type)) {
            LOG_ERROR("Report atb op start event failed.\n");
        }
    }

    static bool MemScopeGetOpNameAndDir(atb::Runner* thisPtr, std::string& name, std::string& dir)
    {
#if defined(_GLIBCXX_USE_CXX11_ABI) && (_GLIBCXX_USE_CXX11_ABI == 0)
        static auto funcGetOperationName = VallinaSymbol<ATBLibLoader>::Instance().Get<MemScopeOriginalGetOperationName>(
            "_ZNK3atb6Runner16GetOperationNameEv");
        static auto funcGetSaveTensorDir = VallinaSymbol<ATBLibLoader>::Instance().Get<MemScopeOriginalGetSaveTensorDir>(
            "_ZNK3atb6Runner16GetSaveTensorDirEv");
#else
        static auto funcGetOperationName = VallinaSymbol<ATBLibLoader>::Instance().Get<MemScopeOriginalGetOperationName>(
            "_ZNK3atb6Runner16GetOperationNameB5cxx11Ev");
        static auto funcGetSaveTensorDir = VallinaSymbol<ATBLibLoader>::Instance().Get<MemScopeOriginalGetSaveTensorDir>(
            "_ZNK3atb6Runner16GetSaveTensorDirB5cxx11Ev");
#endif
        if (funcGetOperationName == nullptr || funcGetSaveTensorDir == nullptr) {
            LOG_ERROR("Cannot find origin function of atb.\n");
            return false;
        }
        name = funcGetOperationName(thisPtr);
        dir = funcGetSaveTensorDir(thisPtr);
        return true;
    }

    static bool MemScopeGetAclrtStream(atb::Runner* thisPtr, const atb::RunnerVariantPack& runnerVariantPack,
        aclrtStream &stream)
    {
        static auto funcGetExecuteStream = VallinaSymbol<ATBLibLoader>::Instance().Get<MemScopeOriginalGetExecuteStream>(
            "_ZNK3atb6Runner16GetExecuteStreamEPNS_7ContextE");
        if (funcGetExecuteStream == nullptr) {
            LOG_ERROR("Cannot find origin function of atb.\n");
            return false;
        }
        stream = funcGetExecuteStream(thisPtr, runnerVariantPack.context);
        return true;
    }

    static void MemScopeParseKernelPath(bool& isBeforeLaunch, std::string& name, const std::string& dirPath)
    {
        auto beforePos = dirPath.find("/before");
        auto afterPos = dirPath.find("/after");
        isBeforeLaunch = true;
        std::string path;
        if (beforePos != std::string::npos) {
            path = dirPath.substr(0, beforePos);
        } else if (afterPos != std::string::npos) {
            isBeforeLaunch = false;
            path = dirPath.substr(0, afterPos);
        } else {
            name = "INVALID";
            LOG_ERROR("Cannot get kernel path.\n");
            return;
        }

        name = path;
        size_t lastSlashPos = name.find_last_of('/');
        if (lastSlashPos != std::string::npos) {
            name = name.substr(lastSlashPos + 1);
        }
    }

    static bool MemScopeReportAtbKernel(const std::string& name, const std::string& dirPath, const bool& isBeforeLaunch)
    {
        if (name == "INVALID") {
            return false;
        }

        std::ostringstream oss;
        oss << "path:" << dirPath;
        std::string params = oss.str();

        char nameStr[ATB_STRING_MAX_LENGTH];
        char attrStr[ATB_STRING_MAX_LENGTH];
        if (strncpy_s(nameStr, ATB_STRING_MAX_LENGTH, name.c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
            nameStr[0] = '\0';
        }
        if (strncpy_s(attrStr, ATB_STRING_MAX_LENGTH, params.c_str(), ATB_STRING_MAX_LENGTH - 1) != EOK) {
            attrStr[0] = '\0';
        }

        RecordSubType type = isBeforeLaunch ? RecordSubType::KERNEL_START : RecordSubType::KERNEL_END;

        if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAtbKernel(nameStr, sizeof(nameStr),
            attrStr, sizeof(attrStr), type)) {
            LOG_ERROR("Report atb run kernel event failed.\n");
        }
        return true;
    }
}

extern "C" atb::Status _ZN3atb6Runner7ExecuteERNS_17RunnerVariantPackE(atb::Runner* thisPtr,
    atb::RunnerVariantPack& runnerVariantPack)
{
    static auto funcExecute = VallinaSymbol<ATBLibLoader>::Instance().Get<atb::MemScopeOriginalRunnerExecuteFunc>(
        "_ZN3atb6Runner7ExecuteERNS_17RunnerVariantPackE");
    if (funcExecute == nullptr) {
        LOG_ERROR("Cannot find origin function of atb.\n");
        return 0;
    }
    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::OP_LAUNCH) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::ACCESS)) {
        return funcExecute(thisPtr, runnerVariantPack);
    }
    std::string params;
    std::string name;
    std::string dir;
    aclrtStream stream;
    if (!atb::MemScopeGetOpNameAndDir(thisPtr, name, dir)
        || !atb::MemScopeGetAclrtStream(thisPtr, runnerVariantPack, stream)) {
        return 0;
    }
    if (EventTraceManager::Instance().IsNeedTrace(EventBaseType::OP_LAUNCH)) {
        params = atb::MemScopeGetOpParams(runnerVariantPack, dir);
        atb::MemScopeReportOp(name, params, true);
    }

    if (EventTraceManager::Instance().IsNeedTrace(EventBaseType::ACCESS)) {
        atb::MemScopeReportTensors(runnerVariantPack, name);
    }
    char cDirPath[WATCH_OP_DIR_MAX_LENGTH];
    static Config config = GetConfig();
    if (config.watchConfig.isWatched || TensorMonitor::GetInstance().IsInMonitoring()) {
        if (strncpy_s(cDirPath, WATCH_OP_DIR_MAX_LENGTH, dir.c_str(), WATCH_OP_DIR_MAX_LENGTH - 1) != EOK) {
            LOG_ERROR("strncpy_s FAILED");
            cDirPath[0] = '\0';
        }
    }
    if (config.watchConfig.isWatched || TensorMonitor::GetInstance().IsInMonitoring()) {
        OpExcuteBegin(stream, cDirPath);
    }
    atb::Status st = funcExecute(thisPtr, runnerVariantPack);
    if (config.watchConfig.isWatched || TensorMonitor::GetInstance().IsInMonitoring()) {
        MonitoredTensor tensors[runnerVariantPack.outTensors.size()];
        size_t loop = 0;
        for (auto &item : runnerVariantPack.outTensors) {
            tensors[loop].dataSize = item.dataSize;
            tensors[loop].data = item.deviceData;
            loop++;
        }
        OpExcuteEnd(stream, cDirPath, tensors, runnerVariantPack.outTensors.size());
    }
    if (EventTraceManager::Instance().IsNeedTrace(EventBaseType::OP_LAUNCH)) {
        atb::MemScopeReportOp(name, params, false);
    }
    if (EventTraceManager::Instance().IsNeedTrace(EventBaseType::ACCESS)) {
        atb::MemScopeReportTensors(runnerVariantPack, name);
    }
    return st;
}

// 不调用原函数，原函数功能不与msmemscope兼容
#if defined(_GLIBCXX_USE_CXX11_ABI) && (_GLIBCXX_USE_CXX11_ABI == 0)
extern "C" void _ZN3atb9StoreUtil15SaveLaunchParamEPvRKN3Mki11LaunchParamERKSs
#else
extern "C" void _ZN3atb9StoreUtil15SaveLaunchParamEPvRKN3Mki11LaunchParamERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE
#endif
(aclrtStream stream, const Mki::LaunchParam& launchParam, const std::string& dirPath)
{
    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::KERNEL_LAUNCH) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::ACCESS)) {
        return;
    }
    static auto getInTensors = VallinaSymbol<ATBLibLoader>::Instance().Get<Mki::MemScopeOriginalGetInTensors>(
        "_ZN3Mki11LaunchParam12GetInTensorsEv");
    static auto getOutTensors = VallinaSymbol<ATBLibLoader>::Instance().Get<Mki::MemScopeOriginalGetOutTensors>(
        "_ZN3Mki11LaunchParam13GetOutTensorsEv");
    if (getInTensors == nullptr || getOutTensors == nullptr) {
        LOG_ERROR("Cannot find origin function of atb.\n");
        return;
    }
    
    if (GetConfig().watchConfig.isWatched || TensorMonitor::GetInstance().IsInMonitoring()) {
        char cDirPath[WATCH_OP_DIR_MAX_LENGTH];
        if (strncpy_s(cDirPath, WATCH_OP_DIR_MAX_LENGTH, dirPath.c_str(), WATCH_OP_DIR_MAX_LENGTH - 1) != EOK) {
            LOG_ERROR("strncpy_s FAILED");
            cDirPath[0] = '\0';
        }
        ATBKernelExcute(stream, cDirPath, getOutTensors(const_cast<Mki::LaunchParam*>(&launchParam)));
    }
    std::string name;
    bool isBeforeLaunch;
    atb::MemScopeParseKernelPath(isBeforeLaunch, name, dirPath);

    if (EventTraceManager::Instance().IsNeedTrace(EventBaseType::KERNEL_LAUNCH)) {
        if (!atb::MemScopeReportAtbKernel(name, dirPath, isBeforeLaunch)) {
            return;
        }
    }

    if (EventTraceManager::Instance().IsNeedTrace(EventBaseType::ACCESS)) {
        atb::MemScopeReportTensors(getInTensors, getOutTensors, launchParam, name);
    }
}

// 劫持判断函数，保证SaveLaunchParam函数可被调用
#if defined(_GLIBCXX_USE_CXX11_ABI) && (_GLIBCXX_USE_CXX11_ABI == 0)
extern "C" bool _ZN3atb5Probe16IsTensorNeedSaveERKSt6vectorIlSaIlEERKSs
#else
extern "C" bool _ZN3atb5Probe16IsTensorNeedSaveERKSt6vectorIlSaIlEERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE
#endif
(const std::vector<int64_t>& ids, const std::string& opType)

{
    return true;
}

extern "C" bool _ZN3atb5Probe17IsSaveTensorAfterEv()
{
    return true;
}

extern "C" bool _ZN3atb5Probe18IsSaveTensorBeforeEv()
{
    return true;
}

extern "C" bool _ZN3atb5Probe21IsExecuteCountInRangeEm(const uint64_t executeCount)
{
    return true;
}

// 劫持判断函数，保证path信息被配置
extern "C" bool _ZN3atb5Probe16IsSaveTensorDescEv()
{
    return true;
}

// 避免调用SaveVariantPack
extern "C" bool _ZNK3atb6Runner12IsSaveTensorEv()
{
    return false;
}