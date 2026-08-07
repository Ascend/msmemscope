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

#ifndef memory_watch_H
#define memory_watch_H

#include <mutex>
#include <string>
#include <vector>

#include "atb_hooks/atb_stub.h"
#include "atb_hooks/mki_stub.h"
#include "event_report.h"
#include "record_info.h"
#include "tensor_dumper.h"
#include "tensor_monitor.h"

namespace MemScope
{

class MemoryWatch
{
   public:
    MemoryWatch(const MemoryWatch &) = delete;
    MemoryWatch &operator=(const MemoryWatch &) = delete;

    static MemoryWatch &GetInstance()
    {
        static MemoryWatch instance;
        return instance;
    }

    void OpExcuteBegin(aclrtStream stream, const std::string &rawOp);
    void OpExcuteEnd(aclrtStream stream, const std::string &rawOp, const std::vector<MonitoredTensor> &tensors);
    void ATBKernelExcute(aclrtStream stream, const std::string &rawKernel, const Mki::SVector<Mki::Tensor> &tensors);
    void KernelExcuteBegin(aclrtStream stream, const std::string &rawItem, bool isOuterLayer = false);
    void KernelExcuteEnd(aclrtStream stream, const std::string &excuteItem, bool isOuterLayer = false,
                         const Mki::SVector<Mki::Tensor> &tensors = {});

    std::string GetWatchedTargetName();

   private:
    MemoryWatch()
        : firstWatchTarget_(std::string(GetConfig().watchConfig.start)),
          lastWatchTarget_(std::string(GetConfig().watchConfig.end)),
          outputId_(GetConfig().watchConfig.outputId) {};
    ~MemoryWatch() = default;

    // 落盘时需要用完整的opName，包含卡号和线程号。
    void BeginExcute(aclrtStream stream, const std::string &rawItem);
    void EndExcute(aclrtStream stream, const std::string &excuteItem, const std::string &rawItem,
                   const std::vector<MonitoredTensor> &outputTensors = {}, uint32_t outputId = UINT32_MAX);

    bool IsFirstWatchTarget(const std::string &name);
    bool IsLastWatchTarget(const std::string &name);

    void SetWatchedTargetName(const std::string &name);
    void ClearWatchedTargetName();
    uint64_t CountOpName(const std::string &name);

   private:
    std::string watchedTargetName_{};
    std::string firstWatchTarget_;
    std::string lastWatchTarget_;
    uint32_t outputId_ = UINT32_MAX;
    std::mutex mutex_;
    std::unordered_map<std::string, uint64_t> targetNameCnt_;
    uint32_t firstWatchTargetCnt_ = 1;
    std::unordered_map<uint64_t, bool> isRepeatWatch_;
};

// atb场景存在abi0和abi1两种编译方式，调用函数接口需使用C风格
void OpExcuteBegin(aclrtStream stream, char *rawOp);
void OpExcuteEnd(aclrtStream stream, char *rawOp, MonitoredTensor *tensors, size_t size);
void ATBKernelExcute(aclrtStream stream, char *rawKernel, const Mki::SVector<Mki::Tensor> &tensors);

}  // namespace MemScope
#endif
