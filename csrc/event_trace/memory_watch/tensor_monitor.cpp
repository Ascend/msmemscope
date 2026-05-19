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

#include "tensor_monitor.h"

namespace MemScope {

void TensorMonitor::AddWatchTensor(MonitoredTensor& tensorInfo)
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    uint64_t ptr = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(tensorInfo.data));
    auto it = pythonWatchedTensorsMap_.find(ptr);
    if (it != pythonWatchedTensorsMap_.end()) {
        pythonWatchedTensorsMap_[ptr] = tensorInfo;
    } else {
        pythonWatchedTensorsMap_.insert({ptr, tensorInfo});
    }
}

void TensorMonitor::AddWatchTensor(const std::vector<MonitoredTensor>& tensorInfoLists, uint32_t outputId)
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    outputId_ = outputId;
    for (auto& tensorInfo : tensorInfoLists) {
        uint64_t ptr = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(tensorInfo.data));
        auto it = cmdWatchedTensorsMap_.find(ptr);
        if (it != cmdWatchedTensorsMap_.end()) {
            cmdWatchedTensorsMap_[ptr] = tensorInfo;
        } else {
            cmdWatchedTensorsMap_.insert({ptr, tensorInfo});
        }
    }
}

std::unordered_map<uint64_t, MonitoredTensor> TensorMonitor::GetCmdWatchedTensorsMap()
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    return cmdWatchedTensorsMap_;
}

uint32_t TensorMonitor::GetCmdWatchedOutputId() const
{
    return outputId_;
}

std::unordered_map<uint64_t, MonitoredTensor> TensorMonitor::GetPythonWatchedTensorsMap()
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    return pythonWatchedTensorsMap_;
}

void TensorMonitor::DeleteWatchTensor(MonitoredTensor& tensorInfo)
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    uint64_t ptr = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(tensorInfo.data));
    auto it = pythonWatchedTensorsMap_.find(ptr);
    if (it != pythonWatchedTensorsMap_.end()) {
        pythonWatchedTensorsMap_.erase(ptr);
    } else {
        LOG_WARN("Failed to delete the tensor. The tensor ptr of %llu is not watched.", ptr);
    }
}

void TensorMonitor::ClearCmdWatchTensor()
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    cmdWatchedTensorsMap_.clear();
}

bool TensorMonitor::IsInMonitoring()
{
    if (cmdWatchedTensorsMap_.empty() && pythonWatchedTensorsMap_.empty()) {
        return false;
    } else {
        return true;
    }
}

}
