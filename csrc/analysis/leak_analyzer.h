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

#ifndef LEAK_ANALYZER_H
#define LEAK_ANALYZER_H

#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "comm_def.h"
#include "config_info.h"
#include "constant.h"
#include "event.h"
#include "event_dispatcher.h"
#include "memory_state_manager.h"
#include "record_info.h"
#include "step_tracker.h"

namespace MemScope
{
/*
 * LeakAnalyzer类主要功能（统一原StepInnerAnalyzer与HalAnalyzer的泄漏检测职责）：
 * 1. 不维护内存块生命周期表，通过MemoryStateManager::QueryLiveBlocks条件查询存活块
 * 2. 通过StepTracker成员组件订阅MSTX/STEP事件，step结束回调时按时间戳区间推导分配step，
 *    判定持续duration≥阈值的NPU块并写入泄漏候选集合（不随FREE删除，累计告警语义）
 * 3. 订阅CLEAN_UP(RESIDUAL_BLOCK)还原double malloc告警；FREE事件幽灵state判定还原free无匹配告警
 * 4. HAL池泄漏在析构时按clientId分组全量检查（原HalAnalyzer语义）
 * 5. 退出时输出泄漏汇总报告（原ReportLeak格式）
 */

// 泄漏候选集合key：ptr+池+分配step（与现状一致，命中后不随FREE删除）
struct LeakMemKey
{
    uint64_t ptr;
    PoolType type;
    uint64_t leakStepId;
    LeakMemKey(uint64_t p, PoolType t, uint64_t id) : ptr(p), type(t), leakStepId(id) {}
    bool operator==(const LeakMemKey& other) const;
};

struct LeakInfo
{
    int64_t leakSize = 0;
    uint64_t kernelIndex = 0;
};

struct LeakMemKeyHash
{
    std::size_t operator()(const LeakMemKey& leakKey) const;
};

using LeakSumsTable = std::unordered_map<LeakMemKey, LeakInfo, LeakMemKeyHash>;

class LeakAnalyzer
{
   public:
    static LeakAnalyzer& GetInstance();
    void EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state);
    void Subscribe();
    void UnSubscribe() const;

    // 统一查询接口：NPU池按deviceId过滤（原StepInnerAnalyzer语义），HAL池按clientId(pid)过滤（原HalAnalyzer语义）
    std::vector<OOMMemRecord> QueryUnfreedRecords(int32_t deviceId, uint32_t clientId) const;

   private:
    explicit LeakAnalyzer();
    ~LeakAnalyzer();
    LeakAnalyzer(const LeakAnalyzer&) = delete;
    LeakAnalyzer& operator=(const LeakAnalyzer&) = delete;
    LeakAnalyzer(LeakAnalyzer&& other) = delete;
    LeakAnalyzer& operator=(LeakAnalyzer&& other) = delete;

    bool IsNpuAnalysisEnable();
    bool IsHalAnalysisEnable();
    void HandleNpuMemEvent(std::shared_ptr<EventBase>& event, MemoryState* state);
    void HandleHalMemEvent(std::shared_ptr<EventBase>& event, MemoryState* state);
    void HandleResidualBlock(std::shared_ptr<EventBase>& event, MemoryState* state);
    void CheckNpuLeak(const DeviceId& deviceId, const StepId& stepId);
    void ReportLeak(const DeviceId& deviceId);
    void CheckHalLeak();
    const std::string& GetMemoryPoolName(const PoolType& poolType);
    bool IsGhostState(MemoryState* state, const std::shared_ptr<EventBase>& event) const;
    // 块分配step推导：找最后一个边界满足 (b.timestamp, b.eventId) <= (allocTs, allocId)，无则0
    uint64_t GetAllocStep(const std::vector<StepBoundary>& boundaries, const uint64_t& allocTs,
                          const uint64_t& allocId) const;

    StepTracker stepTracker_;  // step边界维护组件，回调触发CheckNpuLeak
    std::unordered_map<DeviceId, LeakSumsTable> leakMemSums_{};
    std::set<ClientId> halClients_{};  // 见过的HAL事件client，析构时逐client检查（原memtables_分表语义）
    uint64_t durationThreshold_ = 1;   // 设置警告阈值, 可由用户更改
    uint64_t skipSteps_ = 1;
    mutable std::mutex mutex_;  // 保护共享数据的互斥锁
};

}  // namespace MemScope

#endif
