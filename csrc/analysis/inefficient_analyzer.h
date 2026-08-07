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

#ifndef INEFFICIENT_ANALYZER_H
#define INEFFICIENT_ANALYZER_H

#include <atomic>

#include "analyzer_base.h"

namespace MemScope
{

constexpr uint64_t THREHOLD = 3000;
constexpr uint64_t MAX_UNIT64 = std::numeric_limits<uint64_t>::max();
constexpr uint64_t MIN_EVENTS_NUM = 2;  // state buffer中需要最少有两条内存访问记录
constexpr uint64_t LAST_EVENTS_NUM = 1;

class InefficientAnalyzer : public AnalyzerBase
{
   public:
    static InefficientAnalyzer& GetInstance();
    void EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state) override;
    struct PidState
    {
        std::vector<std::shared_ptr<MemoryEvent>> apiTmp;
        std::atomic<uint64_t> apiId;
        uint64_t mallocApiTmpId;
        uint64_t freeApiTmpId;
        bool isOpStart;
    };
    void Subscribe();
    void UnSubscribe() const;

   private:
    explicit InefficientAnalyzer();
    ~InefficientAnalyzer() override;
    InefficientAnalyzer(const InefficientAnalyzer&) = delete;
    InefficientAnalyzer& operator=(const InefficientAnalyzer&) = delete;
    InefficientAnalyzer(InefficientAnalyzer&& other) = delete;
    InefficientAnalyzer& operator=(InefficientAnalyzer&& other) = delete;

    // 辅助函数均直接传PidState&，避免每事件对同一pid反复哈希查找
    void InefficientAnalysis(std::shared_ptr<MemoryEvent>& event, MemoryState* state, PidState& pidState);
    void HandleOpLaunchEvent(std::shared_ptr<EventBase>& event, std::unordered_map<uint64_t, PidState>& pidStatesMap);
    void HandleMemoryEvent(std::shared_ptr<EventBase>& event, MemoryState* state,
                           std::unordered_map<uint64_t, PidState>& pidStatesMap);
    void EarlyAllocation(std::shared_ptr<MemoryEvent>& event, MemoryState* state, PidState& pidState);
    void LateDeallocation(std::shared_ptr<MemoryEvent>& event, MemoryState* state, PidState& pidState);
    void TemporaryIdleness(std::shared_ptr<MemoryEvent>& event, MemoryState* state);
    void InitNewPidState(const uint64_t pid, std::unordered_map<uint64_t, PidState>& pidStatesMap);
    void Init(PidState& pidState);
    void AddEventToTmps(const std::shared_ptr<MemoryEvent>& event, PidState& pidState);
    void AddApiIdToState(std::shared_ptr<MemoryEvent>& event, MemoryState* state, PidState& pidState);
    void ClassifyEventsTmp(PidState& pidState);
    void UpdateApiId(PidState& pidState);

    std::unordered_map<uint64_t, PidState> PTAPidStatesMap;
    std::unordered_map<uint64_t, PidState> ATBPidStatesMap;
};

}  // namespace MemScope

#endif
