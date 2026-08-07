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

#include "memory_watch.h"

namespace MemScope
{

uint64_t MemoryWatch::CountOpName(const std::string &name)
{
    auto it = targetNameCnt_.find(name);
    if (it == targetNameCnt_.end())
    {
        targetNameCnt_.emplace(name, 1);
        return 1;
    }
    ++it->second;
    return it->second;
}

void MemoryWatch::BeginExcute(aclrtStream stream, const std::string &rawItem)
{
    auto name = rawItem + "_" + std::to_string(CountOpName(rawItem.substr(rawItem.find("/") + 1)));
    if (TensorMonitor::GetInstance().IsInMonitoring())
    {
        TensorDumper::GetInstance().Dump(stream, name, true);
        return;
    }
    return;
}

void MemoryWatch::EndExcute(aclrtStream stream, const std::string &excuteItem, const std::string &rawItem,
                            const std::vector<MonitoredTensor> &outputTensors, uint32_t outputId)
{
    std::string name;
    if (IsFirstWatchTarget(excuteItem) && watchedTargetName_.empty())
    {
        SetWatchedTargetName(excuteItem);
        name = rawItem + "_" + std::to_string(firstWatchTargetCnt_++);
        TensorMonitor::GetInstance().AddWatchTensor(outputTensors, outputId);
        TensorDumper::GetInstance().Dump(stream, name, false);

        return;
    }
    name = rawItem + "_" + std::to_string(targetNameCnt_[excuteItem]);
    if (IsLastWatchTarget(excuteItem))
    {
        TensorDumper::GetInstance().Dump(stream, name, false);

        ClearWatchedTargetName();
        TensorMonitor::GetInstance().ClearCmdWatchTensor();

        return;
    }
    if (TensorMonitor::GetInstance().IsInMonitoring())
    {
        TensorDumper::GetInstance().Dump(stream, name, false);
        return;
    }

    return;
}

void OpExcuteBegin(aclrtStream stream, char *rawOp)
{
    std::string str(rawOp);
    return MemScope::MemoryWatch::GetInstance().OpExcuteBegin(stream, str);
}

void OpExcuteEnd(aclrtStream stream, char *rawOp, MonitoredTensor *tensorsInput, size_t size)
{
    std::vector<MonitoredTensor> tensors;

    tensors.reserve(size);

    for (size_t i = 0; i < size; ++i)
    {
        tensors.push_back(tensorsInput[i]);
    }
    std::string str(rawOp);
    return MemScope::MemoryWatch::GetInstance().OpExcuteEnd(stream, str, tensors);
}

void MemoryWatch::OpExcuteBegin(aclrtStream stream, const std::string &rawOp)
{
    std::lock_guard<std::mutex> guard(mutex_);
    return BeginExcute(stream, rawOp);
}

void MemoryWatch::OpExcuteEnd(aclrtStream stream, const std::string &rawOp, const std::vector<MonitoredTensor> &tensors)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto op = rawOp.substr(rawOp.find("/") + 1);
    if (!IsFirstWatchTarget(op))
    {
        return EndExcute(stream, op, rawOp);
    }
    if (outputId_ < tensors.size())
    {
        std::vector<MonitoredTensor> dumpTensors;
        MonitoredTensor tensor = tensors[outputId_];
        dumpTensors.emplace_back(tensor);
        return EndExcute(stream, op, rawOp, dumpTensors, outputId_);
    }
    else
    {
        outputId_ = UINT32_MAX;
    }
    return EndExcute(stream, op, rawOp, tensors);
}

void MemoryWatch::KernelExcuteBegin(aclrtStream stream, const std::string &rawKernel, bool isOuterLayer)
{
    std::lock_guard<std::mutex> guard(mutex_);
    uint64_t tid = Utility::GetTid();
    // 防止atb的kernel监控与python接口的kernel监控重复
    if (isOuterLayer)
    {
        isRepeatWatch_[tid] = true;
    }
    if (isRepeatWatch_[tid] && !isOuterLayer)
    {
        return;
    }
    BeginExcute(stream, rawKernel);
}

void MemoryWatch::KernelExcuteEnd(aclrtStream stream, const std::string &rawKernel, bool isOuterLayer,
                                  const Mki::SVector<Mki::Tensor> &tensors)
{
    std::lock_guard<std::mutex> guard(mutex_);
    uint64_t tid = Utility::GetTid();
    // 防止atb的kernel监控与python接口的kernel监控重复
    if (isRepeatWatch_[tid] && !isOuterLayer)
    {
        return;
    }
    if (isOuterLayer)
    {
        isRepeatWatch_[tid] = false;
    }
    std::string kernelDir = rawKernel.substr(rawKernel.find("/") + 1);
    if (!IsFirstWatchTarget(kernelDir))
    {
        return EndExcute(stream, kernelDir, rawKernel);
    }
    std::vector<MonitoredTensor> dumpTensors;
    if (outputId_ < tensors.size())
    {
        MonitoredTensor tensor{};
        tensor.data = tensors[outputId_].data;
        tensor.dataSize = static_cast<uint64_t>(tensors[outputId_].dataSize);
        dumpTensors.emplace_back(tensor);
        return EndExcute(stream, kernelDir, rawKernel, dumpTensors, outputId_);
    }
    else
    {
        outputId_ = UINT32_MAX;
    }
    for (auto &item : tensors)
    {
        MonitoredTensor tensor{};
        tensor.data = item.data;
        tensor.dataSize = static_cast<uint64_t>(item.dataSize);
        dumpTensors.emplace_back(tensor);
    }
    EndExcute(stream, kernelDir, rawKernel, dumpTensors, outputId_);
}

void ATBKernelExcute(aclrtStream stream, char *rawKernel, const Mki::SVector<Mki::Tensor> &tensors)
{
    std::string str(rawKernel);
    MemScope::MemoryWatch::GetInstance().ATBKernelExcute(stream, str, tensors);
}

void MemoryWatch::ATBKernelExcute(aclrtStream stream, const std::string &rawKernel,
                                  const Mki::SVector<Mki::Tensor> &tensors)
{
    auto beforPos = rawKernel.find("/before");
    auto afterPos = rawKernel.find("/after");
    if (beforPos != std::string::npos)
    {
        KernelExcuteBegin(stream, rawKernel.substr(0, beforPos), true);
    }
    else if (afterPos != std::string::npos)
    {
        KernelExcuteEnd(stream, rawKernel.substr(0, afterPos), true, tensors);
    }
    else
    {
        LOG_ERROR("Invalid kernel info.\n");
        return;
    }
}

bool MemoryWatch::IsFirstWatchTarget(const std::string &name) { return name == firstWatchTarget_; }

bool MemoryWatch::IsLastWatchTarget(const std::string &name) { return name == lastWatchTarget_; }

void MemoryWatch::SetWatchedTargetName(const std::string &name) { watchedTargetName_ = name; }

std::string MemoryWatch::GetWatchedTargetName() { return watchedTargetName_; }

void MemoryWatch::ClearWatchedTargetName() { watchedTargetName_ = ""; }

}  // namespace MemScope
