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

#include "step_tracker.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>

#include "utility/log.h"

namespace MemScope
{

StepTracker::StepTracker(StepEndCallback onStepEnd, StepStartCallback onStepStart)
    : onStepEnd_(std::move(onStepEnd)), onStepStart_(std::move(onStepStart))
{
}

bool StepTracker::OnEvent(const std::shared_ptr<EventBase>& event)
{
    if (event == nullptr)
    {
        return false;
    }
    if (event->eventType == EventBaseType::MSTX)
    {
        auto mstxEvent = std::dynamic_pointer_cast<MstxEvent>(event);
        if (mstxEvent == nullptr)
        {
            return false;
        }
        if (mstxEvent->eventSubType == EventSubType::MSTX_RANGE_START)
        {
            return HandleMstxRangeStart(mstxEvent);
        }
        if (mstxEvent->eventSubType == EventSubType::MSTX_RANGE_END)
        {
            return HandleMstxRangeEnd(mstxEvent);
        }
        return false;
    }
    if (event->eventType == EventBaseType::SYSTEM && event->eventSubType == EventSubType::STEP)
    {
        return HandlePyStep(event);
    }
    return false;
}

bool StepTracker::HandleMstxRangeStart(const std::shared_ptr<MstxEvent>& mstxEvent)
{
    // 仅识别step打点（两种引号形态），其余MSTX range（如host采集）忽略
    if (mstxEvent->name != "\"step start\"" && mstxEvent->name != "step start")
    {
        return false;
    }
    const DeviceId& deviceId = mstxEvent->device;
    const StepId& stepId = mstxEvent->stepId;
    // 判断是否使用mstx信息源
    if (stepSource_ == StepSource::PY_STEP_SOURCE)
    {
        LOG_ERROR("[device %ld]: 'Mstx' and 'msmemcope.step()' cannot be used simultaneously to update the step.",
                  deviceId);
        return true;
    }
    stepSource_ = StepSource::MSTX_SOURCE;

    auto& state = deviceStates_[deviceId];
    // 事件可能乱序到达（同device多stream），按(timestamp, eventId)字典序升序插入，
    // 保证GetAllocStep"边界有序+break"语义的正确性
    StepBoundary boundary{stepId, mstxEvent->timestamp, mstxEvent->id, mstxEvent->rangeId};
    auto pos = std::lower_bound(state.boundaries.begin(), state.boundaries.end(), boundary,
                                [](const StepBoundary& lhs, const StepBoundary& rhs) {
                                    return std::tie(lhs.timestamp, lhs.eventId) < std::tie(rhs.timestamp, rhs.eventId);
                                });
    state.boundaries.insert(pos, boundary);
    if (onStepStart_)
    {
        onStepStart_(deviceId, stepId);
    }
    state.currentStep = stepId;
    return true;
}

bool StepTracker::HandleMstxRangeEnd(const std::shared_ptr<MstxEvent>& mstxEvent)
{
    const DeviceId& deviceId = mstxEvent->device;
    const StepId& stepId = mstxEvent->stepId;
    // 如果是end看stepid和rangeid是否在table中
    auto it = deviceStates_.find(deviceId);
    if (it == deviceStates_.end())
    {
        return false;
    }
    bool matched = false;
    for (const auto& boundary : it->second.boundaries)
    {
        if (boundary.stepId == stepId && boundary.rangeId == mstxEvent->rangeId)
        {
            matched = true;
            break;
        }
    }
    if (!matched)
    {
        return false;
    }
    if (onStepEnd_)
    {
        onStepEnd_(deviceId, stepId);
    }
    return true;
}

bool StepTracker::HandlePyStep(const std::shared_ptr<EventBase>& event)
{
    const DeviceId& deviceId = event->device;
    const StepId stepId = std::stoull(event->name);
    // 判断是否使用python信息源
    if (stepSource_ == StepSource::MSTX_SOURCE)
    {
        LOG_ERROR("[device %ld]: 'msmemcope.step()' and 'Mstx' cannot be used simultaneously to update the step.",
                  deviceId);
        return true;
    }
    stepSource_ = StepSource::PY_STEP_SOURCE;

    // msmemcope.step()同时标识了上一个step的结束和(step+1)的开始，这里先处理上一个step结束，检测泄漏与gap
    // 如果是第一次msmemcope.step()的调用，即step=1，表中此时没有不处理
    if (onStepEnd_)
    {
        onStepEnd_(deviceId, stepId);
    }

    // 处理(step+1)的开始（与MSTX路径一致，按(timestamp, eventId)升序插入）
    auto& state = deviceStates_[deviceId];
    StepBoundary boundary{stepId + 1, event->timestamp, event->id, 0};
    auto pos = std::lower_bound(state.boundaries.begin(), state.boundaries.end(), boundary,
                                [](const StepBoundary& lhs, const StepBoundary& rhs) {
                                    return std::tie(lhs.timestamp, lhs.eventId) < std::tie(rhs.timestamp, rhs.eventId);
                                });
    state.boundaries.insert(pos, boundary);
    if (onStepStart_)
    {
        onStepStart_(deviceId, stepId + 1);
    }
    state.currentStep = stepId + 1;
    return true;
}

const std::vector<StepBoundary>& StepTracker::GetStepBoundaries(const DeviceId& deviceId) const
{
    static const std::vector<StepBoundary> EMPTY_BOUNDARIES;
    auto it = deviceStates_.find(deviceId);
    if (it == deviceStates_.end())
    {
        return EMPTY_BOUNDARIES;
    }
    return it->second.boundaries;
}

StepId StepTracker::GetCurrentStep(const DeviceId& deviceId) const
{
    auto it = deviceStates_.find(deviceId);
    if (it == deviceStates_.end())
    {
        return 0;
    }
    return it->second.currentStep;
}

}  // namespace MemScope
