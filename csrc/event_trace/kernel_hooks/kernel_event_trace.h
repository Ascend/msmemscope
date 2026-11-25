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

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "record_info.h"

namespace MemScope
{

using TaskKey = std::tuple<uint16_t, uint16_t, uint16_t>;  // <deviceId, streamId, taskId> 唯一标识一个算子任务
using ThreadId = uint64_t;

// 单线程 kernel 关联表容量上限：SOC日志通道异常时 KERNEL_END 不会到达，
// 限制长度避免长训练任务内存与匹配开销无界增长；取值需大于驱动日志缓冲可积压的记录量级
constexpr size_t KERNEL_TASK_MAP_MAX_PER_THREAD = 65536;
// 超限时批量淘汰最旧记录（而不是逐条淘汰），使均摊淘汰开销为O(1)
constexpr size_t KERNEL_TASK_MAP_EVICT_BATCH = KERNEL_TASK_MAP_MAX_PER_THREAD / 4;
// 关联失败/回退命中的日志限频间隔（每个计数首条总是打印）
constexpr uint64_t KERNEL_TASK_LOG_INTERVAL = 10000;

struct AclnnKernelMapInfo
{
    uint64_t timestamp;
    TaskKey taskKey;
    std::string kernelName;
    // 是否已被回退匹配（device+taskId）的KERNEL_START消费：回退匹配可靠性低于精确匹配，
    // 每条记录只允许被一次START使用，避免END丢失的残留记录持续吸收后续同taskId的事件
    bool fallbackUsed;
};

// 该类关注kernel下发、开始执行和结束执行
// 作为信息采集（hook、驱动上报）和event report之间的桥梁
class KernelEventTrace
{
   public:
    KernelEventTrace(const KernelEventTrace&) = delete;
    KernelEventTrace& operator=(const KernelEventTrace&) = delete;

    static KernelEventTrace& GetInstance()
    {
        static KernelEventTrace instance;
        return instance;
    }

    void KernelLaunch(const AclnnKernelMapInfo& kernelLaunchInfo);  // kernel下发
    void KernelStartExcute(TaskKey& key, uint64_t time);            // kernel开始执行
    void KernelEndExcute(TaskKey& key, uint64_t time);              // kernel结束执行

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
class RuntimeKernelLinker
{
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
    void SetHashInfo(uint64_t hashId, const std::string& hashInfo);
    std::string GetHashInfo(uint64_t hashId);
    std::string GetKernelName(TaskKey& key, RecordSubType type);
    std::string GetLastKernelName(uint64_t tid);
    // 仅测试用：返回当前线程关联表的记录数（用于覆盖超限淘汰分支）
    size_t GetTaskMapSizeForTest();

   private:
    RuntimeKernelLinker() = default;
    ~RuntimeKernelLinker() = default;

   private:
    std::mutex mutex_;
    std::unordered_map<ThreadId, std::vector<AclnnKernelMapInfo>> kernelNameMp_;

    std::unordered_map<uint64_t, std::string> hashInfo_map_;
    std::unordered_map<uint64_t, std::string> currentNameMap_;
    // 关联诊断计数（均在持锁路径更新）：用于暴露 SOC日志key 与 runtime key 不一致时的静默丢事件
    uint64_t fallbackMatchCount_ = 0;   // 精确key失配、按device+taskId唯一回退命中
    uint64_t ambiguousMatchCount_ = 0;  // 回退存在多个候选，放弃匹配
    uint64_t unmatchedCount_ = 0;       // 精确与回退均未命中
    uint64_t emptyNameCount_ = 0;       // 命中关联记录但kernel名为空
    uint64_t unresolvedNameCount_ = 0;  // runtime hash无法解析出kernel名（使用占位名）
    uint64_t overflowEvictCount_ = 0;   // 关联表超限淘汰最旧记录
};

}  // namespace MemScope

#endif
