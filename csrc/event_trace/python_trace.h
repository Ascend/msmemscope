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
#ifndef PYTHON_TRACE_H
#define PYTHON_TRACE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <stack>
#include <iostream>
#include "cpython.h"
#include "config_info.h"
#include "record_info.h"
#include "utils.h"
#include "file.h"
#include "event_report.h"
#include "file_write_manager.h"
#include "../analysis/data_handler.h"

namespace MemScope {

class PythonTrace {
public:
    static PythonTrace& GetInstance()
    {
        static PythonTrace instance;
        return instance;
    }
    PythonTrace(const PythonTrace&) = delete;
    PythonTrace& operator=(const PythonTrace&) = delete;
    void RecordPyCall(const std::string& funcHash, const std::string& funcInfo, uint64_t timestamp);
    void RecordCCall(std::string funcHash, std::string funcInfo);
    void RecordReturn(std::string funcHash, std::string funcInfo);
    // 自定义trace事件的回调
    void RecordFuncPyCall(const std::string& funcHash, const std::string& funcInfo, uint64_t timestamp);
    void RecordFuncReturn(std::string funcHash, std::string funcInfo);
    void Start();
    void Stop();
    bool IsTraceActive();
    bool IsIgnoreRecordFunc(std::string funcHash);
private:
    void DumpTraceEvent(std::shared_ptr<TraceEvent>& event);
    bool IsIgnore(std::string funcName) const;
    PythonTrace() = default;
    ~PythonTrace() = default;
    std::unordered_map<uint64_t, std::stack<std::shared_ptr<TraceEvent>>> frameStack_;
    std::unordered_map<uint64_t, std::stack<std::shared_ptr<TraceEvent>>> frameStackRecordFunc_;
    std::atomic<bool> active_{false};
    std::unordered_map<uint64_t, bool> throw_;
    std::string prefix_;
    std::string dirPath_;
    std::vector<std::string> ignorePyFunc_ = {"__torch_dispatch__"};
    std::vector<std::string> ignoreRecordFunc_ = {
        "msmemscope/record_function.py",
        "record_start of _msmemscope._record_function", 
        "record_end of _msmemscope._record_function",
    };
    std::unordered_map<int32_t, std::unique_ptr<DataHandler>> handlerMap_;
    std::vector<std::shared_ptr<TraceEvent>> sharedEventLists_;
};
void callback(const std::string& hash, const std::string& info, PyTraceType what, uint64_t timestamp);
}

#endif