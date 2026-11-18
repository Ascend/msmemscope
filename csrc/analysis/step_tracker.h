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

#ifndef STEP_TRACKER_H
#define STEP_TRACKER_H

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "event.h"

namespace MemScope
{

using DeviceId = int32_t;
using StepId = uint64_t;

// step边界：stepId对应的开始边界，(timestamp, eventId)构成字典序，用于块分配step归属推导
struct StepBoundary
{
    StepId stepId;
    uint64_t timestamp;
    uint64_t eventId;
    uint64_t rangeId;  // MSTX RANGE_START的rangeId，RANGE_END配对校验用；Python模式为0
};

// step事件的俩个来源mstx和msmemscope.step()不能同时存在，第一个接收的会被用作信息来源，后续其他来源将被无视
enum class StepSource : uint8_t
{
    NONE = 0,
    MSTX_SOURCE,
    PY_STEP_SOURCE
};

/*
 * StepTracker：关注STEP类事件的分析器的成员组件（成员对象而非订阅机制）
 * 统一维护step边界序列与当前step，供LeakAnalyzer（泄漏判定）与HealthAnalyzer（Gap统计）复用
 * 用法：在分析器EventHandle中转发MSTX_RANGE_START/END、SYSTEM(STEP)事件，OnEvent返回true表示已消费
 * 事件流为单线程串行驱动（分析器事件处理线程），回调内可安全访问本组件的查询接口
 */
class StepTracker
{
   public:
    using StepEndCallback = std::function<void(const DeviceId&, const StepId&)>;
    using StepStartCallback = std::function<void(const DeviceId&, const StepId&)>;

    explicit StepTracker(StepEndCallback onStepEnd, StepStartCallback onStepStart = nullptr);

    // 返回true表示事件被消费（step类事件）
    bool OnEvent(const std::shared_ptr<EventBase>& event);

    // 指定device的step边界序列（按(timestamp, eventId)升序，含当前未结束的step）
    const std::vector<StepBoundary>& GetStepBoundaries(const DeviceId& deviceId) const;

    // 当前step编号（step start后推进；无step记录返回0）
    StepId GetCurrentStep(const DeviceId& deviceId) const;

    // 指定device首个step结束事件的时间戳（0表示该device尚无step结束）。
    // 泄漏判定过滤口径为"分配于首个step结束点之后"，须用结束时刻而非step start边界推导，
    // 否则step k结束~step k+1开始间隙分配的块会被误归属到step k而漏判
    uint64_t GetFirstStepEndTimestamp(const DeviceId& deviceId) const;

    // 指定device首个stepId>=2的开始边界(timestamp, eventId)（timestamp为0表示尚无）。
    // 泄漏过滤的基线口径截止点：打点自step 2起（无step1）或step结束点缺失（mstx range_end
    // 丢失）时，仍按"第2个step开始之后"过滤，与基线allocStep>=2行为一致
    void GetFirstStep2Start(const DeviceId& deviceId, uint64_t& timestamp, uint64_t& eventId) const;

    StepSource GetStepSource() const { return stepSource_; }

   private:
    struct PerDeviceState
    {
        StepId currentStep = 0;
        std::vector<StepBoundary> boundaries;
        uint64_t firstEndTimestamp = 0;    // 首个step结束事件的时间戳，0表示尚无结束
        uint64_t step2StartTimestamp = 0;  // 首个stepId>=2开始边界的时间戳，0表示尚无
        uint64_t step2StartEventId = 0;    // 与step2StartTimestamp构成字典序截止点
    };

    bool HandleMstxRangeStart(const std::shared_ptr<MstxEvent>& mstxEvent);
    bool HandleMstxRangeEnd(const std::shared_ptr<MstxEvent>& mstxEvent);
    bool HandlePyStep(const std::shared_ptr<EventBase>& event);

    StepEndCallback onStepEnd_;
    StepStartCallback onStepStart_;
    StepSource stepSource_ = StepSource::NONE;
    std::unordered_map<DeviceId, PerDeviceState> deviceStates_;
};

}  // namespace MemScope

#endif
