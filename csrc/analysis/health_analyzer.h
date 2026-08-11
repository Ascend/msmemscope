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

#ifndef HEALTH_ANALYZER_H
#define HEALTH_ANALYZER_H

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "comm_def.h"
#include "config_info.h"
#include "event.h"
#include "event_dispatcher.h"
#include "step_tracker.h"

namespace MemScope
{
/*
 * HealthAnalyzer类主要功能（承载原StepInnerAnalyzer的统计职责）：
 * 1. 维护各NPU池占用状态（totalAllocated/totalReserved/stepMax/stepMin），不维护内存块生命周期表
 * 2. 通过StepTracker成员组件订阅MSTX/STEP事件：step结束输出End日志与前后内存增减判断，
 *    step开始输出Start日志并记录起始占用；每次step结束结算CheckGap（Min/Max比值波动告警）
 * 3. 析构时输出Gap统计报告（原ReportGap格式）
 */

// 用于记录不同类型内存池内的占用状态（原StepInnerAnalyzer结构平移）
struct GapInfo
{
    uint64_t gapStepId = 0;       // 记录计算比值时的stepId
    double minMaxAllocRatio = 0;  // 最大allocated内存和最小allocated内存的比值
    int64_t minAllocMemory = 0;   // 最小allocated内存的值
};

struct MemoryPoolStatus
{
    int64_t totalAllocated = 0;
    int64_t totalReserved = 0;
    int64_t stepMaxAllocated = 0;
    int64_t stepMinAllocated = 0;
    GapInfo maxGapInfo;  // 记录动态内存和静态内存比值最大的信息
    GapInfo minGapInfo;  // 记录动态内存和静态内存比值最小的信息
};

class HealthAnalyzer
{
   public:
    static HealthAnalyzer& GetInstance();
    void EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state);
    void Subscribe();
    void UnSubscribe() const;

   private:
    explicit HealthAnalyzer();
    ~HealthAnalyzer();
    HealthAnalyzer(const HealthAnalyzer&) = delete;
    HealthAnalyzer& operator=(const HealthAnalyzer&) = delete;
    HealthAnalyzer(HealthAnalyzer&& other) = delete;
    HealthAnalyzer& operator=(HealthAnalyzer&& other) = delete;

    bool IsAnalysisEnable();
    void HandleMemEvent(std::shared_ptr<EventBase>& event);
    void OnStepEnd(const DeviceId& deviceId, const StepId& stepId);
    void OnStepStart(const DeviceId& deviceId, const StepId& stepId);
    void UpdateAllocated(const DeviceId& deviceId, const PoolType& poolType, const int64_t& totalAllocated);
    void CheckGap(const DeviceId& deviceId);
    void ReportGap(const DeviceId& deviceId);
    const std::string& GetMemoryPoolName(const PoolType& poolType);

    StepTracker stepTracker_;  // step边界维护组件，回调触发End/Start日志与CheckGap
    std::unordered_map<DeviceId, std::unordered_map<PoolType, MemoryPoolStatus>> poolStatusTables_{};
    std::unordered_map<DeviceId, std::unordered_map<PoolType, int64_t>> stepStartAllocated_{};  // 当前step起始占用
    uint64_t skipSteps_ = 1;
    mutable std::mutex mutex_;  // 保护共享数据的互斥锁
};

}  // namespace MemScope

#endif
