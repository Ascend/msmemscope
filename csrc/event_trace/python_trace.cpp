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
#include "python_trace.h"
#include <string>
#include <iostream>
#include <vector>
#include "kernel_hooks/runtime_hooks.h"
#include "event_report.h"
#include "trace_manager/event_trace_manager.h"

namespace MemScope {

bool PythonTrace::IsIgnore(std::string funcName) const
{
    for (auto s : ignorePyFunc_) {
        if (s == funcName) {
            return true;
        }
    }
    return false;
}

bool PythonTrace::IsIgnoreRecordFunc(std::string funcHash)
{
    for (auto s : ignoreRecordFunc_) {
        if (funcHash.find(s) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void PythonTrace::RecordPyCall(const std::string& funcHash, const std::string& funcInfo, uint64_t timestamp)
{
    uint64_t tid = Utility::GetTid();
    if (throw_[tid]) {
        return;
    }
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[trace] RT_ERROR_INVALID_VALUE, %d", devId);
        return;
    }
    std::shared_ptr<TraceEvent> event = std::make_shared<TraceEvent>();
    event->startTs = timestamp ? timestamp : Utility::GetTimeNanoseconds();
    event->hash = funcHash;
    event->info = funcInfo;
    event->pid = Utility::GetPid();
    event->tid = tid;
    event->device = devId;
    std::string funcName = funcHash.substr(funcHash.find(":") + 1);
    if (IsIgnore(funcName) && !throw_[tid]) {
        throw_[tid] = true;
    }
    frameStack_[tid].push(event);
}

void PythonTrace::DumpTraceEvent(std::shared_ptr<TraceEvent>& event)
{
    if (event->device == GD_INVALID_NUM) {
        return ;
    }
    auto it = handlerMap_.find(event->device);
    if (it == handlerMap_.end()) {
        handlerMap_.insert({event->device, MakeDataHandler(GetConfig(), DataType::PYTHON_TRACE_EVENT, event->device)});
    }
    // db文件需要文件锁
    if (GetConfig().dataFormat == static_cast<uint8_t>(DataFormat::DB)) {
        auto& lock = Utility::FileWriteManager::GetInstance().GetLock(event->device);
        std::lock_guard<std::mutex> lock_guard(lock);
        handlerMap_[event->device]->Write(event);
    } else {
        handlerMap_[event->device]->Write(event);
    }
}

void PythonTrace::RecordCCall(std::string funcHash, std::string funcInfo)
{
    uint64_t tid = Utility::GetTid();
    if (throw_[tid]) {
        return;
    }
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[trace] RT_ERROR_INVALID_VALUE, %d", devId);
        return;
    }
    std::shared_ptr<TraceEvent> event = std::make_shared<TraceEvent>();
    event->startTs = Utility::GetTimeNanoseconds();
    event->hash = funcHash;
    event->info = funcInfo;
    event->pid = Utility::GetPid();
    event->tid = tid;
    event->device = devId;
    frameStack_[tid].push(event);
}

void PythonTrace::RecordReturn(std::string funcHash, std::string funcInfo)
{
    uint64_t tid = Utility::GetTid();
    if (!frameStack_[tid].empty()) {
        auto event = frameStack_[tid].top();
        if (funcHash == event->hash) {
            throw_[tid] = false;
            event->endTs = Utility::GetTimeNanoseconds();
            DumpTraceEvent(event);
            frameStack_[tid].pop();
        } else if (throw_[tid] == false) {
            int32_t devId = GD_INVALID_NUM;
            if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
                LOG_ERROR("[trace] RT_ERROR_INVALID_VALUE, %d", devId);
                return;
            }
            std::shared_ptr<TraceEvent> event = std::make_shared<TraceEvent>(
                0, Utility::GetTimeNanoseconds(), tid, Utility::GetPid(), devId, funcInfo, funcHash);
            DumpTraceEvent(event);
        }
    }
}

// record_function的python_trace记录和回调函数
void PythonTrace::RecordFuncPyCall(const std::string& funcHash, const std::string& funcInfo, uint64_t timestamp)
{
    // record_function不涉及throw_tid的操作
    uint64_t tid = Utility::GetTid();
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[trace] RT_ERROR_INVALID_VALUE, %d", devId);
    }
    std::shared_ptr<TraceEvent> event = std::make_shared<TraceEvent>();
    event->startTs = timestamp ? timestamp : Utility::GetTimeNanoseconds();
    event->hash = funcHash;
    event->info = funcInfo;
    event->pid = Utility::GetPid();
    event->tid = tid;
    event->device = devId;
    std::string funcName = funcHash.substr(funcHash.find(":") + 1);
    frameStackRecordFunc_[tid].push(event);
}

void PythonTrace::RecordFuncReturn(std::string funcHash, std::string funcInfo)
{
    uint64_t tid = Utility::GetTid();
    if (!frameStackRecordFunc_[tid].empty()) {
        auto event = frameStackRecordFunc_[tid].top();
        if (funcHash == event->hash) {
            event->endTs = Utility::GetTimeNanoseconds();
            DumpTraceEvent(event);
            frameStackRecordFunc_[tid].pop();
        } else {
            LOG_ERROR("[trace] ERROR RECORD EVENT %s", funcHash.c_str());
        }
    } 
}

void callback(const std::string& hash, const std::string& info, PyTraceType what, uint64_t timestamp)
{
    if (!EventTraceManager::Instance().IsTracingEnabled()) {
        return;
    }
    // 忽略record_func调用过程中自身的python_trace事件
    if (PythonTrace::GetInstance().IsIgnoreRecordFunc(hash)) {
        return;
    }
    switch (what) {
        case PyTraceType::PYCALL: {
            PythonTrace::GetInstance().RecordPyCall(hash, info, timestamp);
            break;
        }
        case PyTraceType::PYRETURN: {
            PythonTrace::GetInstance().RecordReturn(hash, info);
            break;
        }
        case PyTraceType::CCALL: {
            PythonTrace::GetInstance().RecordCCall(hash, info);
            break;
        }
        case PyTraceType::CRETURN: {
            PythonTrace::GetInstance().RecordReturn(hash, info);
            break;
        }
        default:
            break;
    }
}

void PythonTrace::Start()
{
    // 命令行中events开启trackback暂不支持，因为此时python解释器并未初始化，无法开启。
    if (!Utility::IsPyInterpRepeInited()) {
        std::cout << "[msmemscope] Warn: Python interpreter is not initialized. Start python trace failed."<< std::endl;
        return;
    }

    if (Utility::GetPyVersion() < Utility::Version("3.9")) {
        std::cout << "[msmemscope] Warn: The current Python version is below 3.9, python trace cannot be enabled."
                  << std::endl;
        return;
    }
    bool expected{false};
    bool active = active_.compare_exchange_strong(expected, true);
    if (!active) {
        std::cout << "[msmemscope] Warn: There is already an active PythonTracer. Refusing to register profile functions."
                  << std::endl;
        return;
    }
    Utility::PyInterpGuard stat;
    Utility::RegisterTraceCb(callback);
}

void PythonTrace::Stop()
{
    if (!active_) {
        std::cout << "[msmemscope] Warn: The tracer is not start." << std::endl;
        return;
    }
    if (!Utility::IsPyInterpRepeInited()) {
        return;
    }
    Utility::PyInterpGuard stat;
    Utility::UnRegisterTraceCb();
    for (auto &p : frameStack_) {
        while (!p.second.empty()) {
            DumpTraceEvent(p.second.top());
            p.second.pop();
        }
    }
    active_ = false;
}

bool PythonTrace::IsTraceActive()
{
    return active_;
}

}