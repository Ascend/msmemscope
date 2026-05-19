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
#include "aten_manager.h"
#include <cstring>
#include "securec.h"
#include "call_stack.h"
#include "ustring.h"
#include "log.h"

#include <iostream>


namespace MemScope {
    
AtenManager& AtenManager::GetInstance()
{
    static AtenManager instance;
    return instance;
}

AtenManager::AtenManager()
{
    if (GetConfig().watchConfig.isWatched || TensorMonitor::GetInstance().IsInMonitoring()) {
        isWatchEnable_ = true;
    }
    firstWatchOp_ = std::string(GetConfig().watchConfig.start);
    lastWatchOp_ = std::string(GetConfig().watchConfig.end);
}

bool AtenManager::ExtractTensorInfo(const char* msg, const std::string &key, std::string &value)
{
    std::string msgString(msg);
    size_t startPos = msgString.find(key);
    if (startPos == std::string::npos) {
        return false;
    }
    startPos += key.length();
    size_t endPos = msgString.find_first_of(";}", startPos);
    if (endPos == std::string::npos) {
        endPos = msgString.length();
    }
    value = msgString.substr(startPos, endPos - startPos);
    return true;
}

void AtenManager::ProcessMsg(const char* msg, int32_t streamId)
{
    // 根据标识判断是否为aten算子下发或者tensor信息
    bool isAtenBegin;
    if (strncmp(msg, ATEN_BEGIN_MSG, strlen(ATEN_BEGIN_MSG)) == 0) {
        isAtenBegin = true;
        ReportAtenLaunch(msg, streamId, isAtenBegin);
        return;
    }
    if (strncmp(msg, ATEN_END_MSG, strlen(ATEN_END_MSG)) == 0) {
        isAtenBegin = false;
        ReportAtenLaunch(msg, streamId, isAtenBegin);
        return;
    }
    if (strncmp(msg, ACCESS_MSG, strlen(ACCESS_MSG)) == 0) {
        ReportAtenAccess(msg, streamId);
        return;
    }
}

void AtenManager::ReportAtenLaunch(const char* msg, int32_t streamId, bool isAtenBegin)
{
    if (GetConfig().watchConfig.isWatched || TensorMonitor::GetInstance().IsInMonitoring()) {
        isWatchEnable_ = true;
    }
    std::string name;
    ExtractTensorInfo(msg, "name=", name);
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("get device id failed.");
    }
    uint64_t tid = Utility::GetTid();

    std::string opName = std::to_string(devId) + "_" + std::to_string(tid) + "/" + name;
    if (isWatchEnable_ && isAtenBegin) {
        MemoryWatch::GetInstance().OpExcuteBegin(nullptr, opName);
    }
    if (isWatchEnable_ && !isAtenBegin) {
        MemoryWatch::GetInstance().OpExcuteEnd(nullptr, opName, outputTensors_);
        if (IsFirstWatchedOp(name.c_str()) && !isfirstWatchOpSet_) {
            isfirstWatchOpSet_ = true;
        }
        if (IsLastWatchedOp(name.c_str())) {
            outputTensors_.clear();
            isfirstWatchOpSet_ = false;
        }
    }

    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::OP_LAUNCH)) {
        return ;
    }

    std::string pyStack;
    if (GetConfig().enablePyStack) {
        Utility::GetPythonCallstack(GetConfig().pyStackDepth, pyStack);
    }

    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAtenLaunch(name, isAtenBegin, std::move(pyStack))) {
        LOG_ERROR("Report Aten Launch FAILED");
    }
    return;
}

bool AtenManager::IsFirstWatchedOp(const char* name)
{
    return firstWatchOp_ == std::string(name);
}

bool AtenManager::IsLastWatchedOp(const char* name)
{
    return lastWatchOp_ == std::string(name);
}

void AtenManager::ExtractTensorFields(const char* msg, AtenAccessTensorInfo& info)
{
    ExtractTensorInfo(msg, "ptr=", info.addr);
    ExtractTensorInfo(msg, "dtype=", info.dtype);
    ExtractTensorInfo(msg, "shape=", info.shape);
    ExtractTensorInfo(msg, "tensor_size=", info.size);
    ExtractTensorInfo(msg, "name=", info.name);
    ExtractTensorInfo(msg, "is_write=", info.isWrite);
    ExtractTensorInfo(msg, "is_read=", info.isRead);
    ExtractTensorInfo(msg, "is_output=", info.isOutput);
}

void AtenManager::ReportAtenAccess(const char* msg, int32_t streamId)
{
    if (GetConfig().watchConfig.isWatched || TensorMonitor::GetInstance().IsInMonitoring()) {
        isWatchEnable_ = true;
    }
    AtenAccessTensorInfo atenInfo;
    ExtractTensorFields(msg, atenInfo);

    // 组装attr属性
    std::ostringstream oss;
    oss << "dtype:" << atenInfo.dtype << ",shape:" << atenInfo.shape;
    std::string attr = oss.str();
    std::string pyStack;
    if (GetConfig().enablePyStack) {
        Utility::GetPythonCallstack(GetConfig().pyStackDepth, pyStack);
    }
    AccessType type;
    uint64_t addr = 0;
    uint64_t size = 0;


    if (atenInfo.isWrite == "False" && atenInfo.isRead == "False") {
        type = AccessType::UNKNOWN;
    } else if (atenInfo.isWrite == "True") {
        type = AccessType::WRITE;
    } else {
        type = AccessType::READ;
    }
 
    if (!Utility::StrToUint64(addr, atenInfo.addr)) {
        LOG_ERROR("Aten Tensor's addr StrToUint64 failed");
    }
    if (!Utility::StrToUint64(size, atenInfo.size)) {
        LOG_ERROR("Aten Tensor's memSize StrToUint64 failed");
    }

    if (atenInfo.isOutput == "True" && isWatchEnable_ && IsFirstWatchedOp(atenInfo.name.c_str())
        && !isfirstWatchOpSet_) {
        MonitoredTensor tensorInfo{};
        tensorInfo.data =  reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(addr));
        tensorInfo.dataSize = size;
        outputTensors_.push_back(tensorInfo);
    }

    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::ACCESS)) {
        return ;
    }
    
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAtenAccess(atenInfo.name, attr, type, addr, size, std::move(pyStack))) {
        LOG_ERROR("Report Aten Access FAILED");
    }
    return;
}

}