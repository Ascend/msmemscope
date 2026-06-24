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

#ifndef KERNEL_EVENT_TRACE_H
#define KERNEL_EVENT_TRACE_H

#include <unordered_map>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include "record_info.h"

namespace MemScope {

using TaskKey = std::tuple<uint16_t, uint16_t, uint16_t>; // <deviceId, streamId, taskId> 唯一标识一个算子任务
using ThreadId = uint64_t;

struct AclnnKernelMapInfo {
    uint64_t timestamp;
    TaskKey taskKey;
    std::string kernelName;
};

// 该类关注kernel下发、开始执行和结束执行
// 作为信息采集（hook、驱动上报）和event report之间的桥梁
class KernelEventTrace {
public:
    KernelEventTrace(const KernelEventTrace&) = delete;
    KernelEventTrace& operator=(const KernelEventTrace&) = delete;

    static KernelEventTrace& GetInstance()
    {
        static KernelEventTrace instance;
        return instance;
    }

    void KernelLaunch(const AclnnKernelMapInfo &kernelLaunchInfo); // kernel下发
    void KernelStartExcute(TaskKey& key, uint64_t time); // kernel开始执行
    void KernelEndExcute(TaskKey& key, uint64_t time); // kernel结束执行

    void StartKernelEventTrace();
    void EndKernelEventTrace() const;
private:
    KernelEventTrace() = default;
    ~KernelEventTrace();
    void CreateReadDataChannel(uint32_t devId);

private:
    std::atomic<bool> started_{true};
    std::thread readTh_;
};

// 用于关联runtime task下发和kernel下发
// 在一个线程内，runtime任务下发和对应的kernel下发是保序的，中间不会插入runtime任务和kernel下发
class RuntimeKernelLinker {
public:
    RuntimeKernelLinker(const RuntimeKernelLinker&) = delete;
    RuntimeKernelLinker& operator=(const RuntimeKernelLinker&) = delete;

    static RuntimeKernelLinker& GetInstance()
    {
        static RuntimeKernelLinker instance;
        return instance;
    }

    void RuntimeTaskInfoLaunch(const TaskKey& key, uint64_t hashId);
    void KernelLaunch();
    void SetHashInfo(uint64_t hashId, const std::string &hashInfo);
    std::string GetHashInfo(uint64_t hashId);
    std::string GetKernelName(TaskKey& key, RecordSubType type);
    std::string GetLastKernelName(uint64_t tid);
private:
    RuntimeKernelLinker() = default;
    ~RuntimeKernelLinker() = default;

private:
    std::mutex mutex_;
    std::unordered_map<ThreadId, std::vector<AclnnKernelMapInfo>> kernelNameMp_;

    std::unordered_map<uint64_t, std::string> hashInfo_map_;
    std::unordered_map<uint64_t, std::string> currentNameMap_;
};

}

#endif