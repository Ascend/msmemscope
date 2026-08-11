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
#include <gtest/gtest.h>
#include <gtest/internal/gtest-port.h>
#include <string>
#include <vector>

#include "step_tracker.h"

using namespace MemScope;

namespace
{
// MSTX step start打点（两种引号形态分别测试）
std::shared_ptr<MstxEvent> CreateMstxStart(uint64_t stepId, uint64_t rangeId, int32_t device = 0,
                                           uint64_t timestamp = 0, uint64_t eventId = 0,
                                           const std::string& name = "\"step start\"")
{
    auto event = std::make_shared<MstxEvent>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = EventSubType::MSTX_RANGE_START;
    event->device = device;
    event->stepId = stepId;
    event->rangeId = rangeId;
    event->streamId = 0;
    event->timestamp = timestamp;
    event->id = eventId;
    event->name = name;
    return event;
}

std::shared_ptr<MstxEvent> CreateMstxEnd(uint64_t stepId, uint64_t rangeId, int32_t device = 0,
                                         uint64_t timestamp = 0, uint64_t eventId = 0)
{
    auto event = std::make_shared<MstxEvent>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = EventSubType::MSTX_RANGE_END;
    event->device = device;
    event->stepId = stepId;
    event->rangeId = rangeId;
    event->streamId = 0;
    event->timestamp = timestamp;
    event->id = eventId;
    return event;
}

// python接口msmemscope.step()事件，name为stepId
std::shared_ptr<EventBase> CreatePyStep(uint64_t stepId, int32_t device = 0, uint64_t timestamp = 0,
                                        uint64_t eventId = 0)
{
    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = EventSubType::STEP;
    event->device = device;
    event->timestamp = timestamp;
    event->id = eventId;
    event->name = std::to_string(stepId);
    return event;
}
}  // namespace

class StepTrackerTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        endCalls_.clear();
        startCalls_.clear();
        tracker_.reset(new StepTracker(
            std::bind(&StepTrackerTest::OnStepEnd, this, std::placeholders::_1, std::placeholders::_2),
            std::bind(&StepTrackerTest::OnStepStart, this, std::placeholders::_1, std::placeholders::_2)));
    }

    void OnStepEnd(const DeviceId& deviceId, const StepId& stepId) { endCalls_.emplace_back(deviceId, stepId); }
    void OnStepStart(const DeviceId& deviceId, const StepId& stepId) { startCalls_.emplace_back(deviceId, stepId); }

    std::unique_ptr<StepTracker> tracker_;
    std::vector<std::pair<DeviceId, StepId>> endCalls_;
    std::vector<std::pair<DeviceId, StepId>> startCalls_;
};

TEST_F(StepTrackerTest, mstx_start_records_boundary_and_current_step)
{
    tracker_->OnEvent(CreateMstxStart(1, 100, 0, 1000, 1));
    tracker_->OnEvent(CreateMstxStart(2, 200, 0, 2000, 2));
    tracker_->OnEvent(CreateMstxStart(3, 300, 0, 3000, 3));

    EXPECT_EQ(tracker_->GetCurrentStep(0), 3u);
    EXPECT_EQ(tracker_->GetStepSource(), StepSource::MSTX_SOURCE);

    const auto& boundaries = tracker_->GetStepBoundaries(0);
    ASSERT_EQ(boundaries.size(), 3u);
    // 按(timestamp, eventId)升序
    EXPECT_EQ(boundaries[0].stepId, 1u);
    EXPECT_EQ(boundaries[0].rangeId, 100u);
    EXPECT_EQ(boundaries[2].stepId, 3u);
    EXPECT_EQ(boundaries[2].rangeId, 300u);
}

TEST_F(StepTrackerTest, mstx_start_accepts_unquoted_step_name)
{
    // 兼容"step start"（无引号）形态
    EXPECT_TRUE(tracker_->OnEvent(CreateMstxStart(1, 100, 0, 1000, 1, "step start")));
    EXPECT_EQ(tracker_->GetCurrentStep(0), 1u);
}

TEST_F(StepTrackerTest, mstx_ignores_non_step_range_start)
{
    // 非step打点的MSTX range（如host采集）不消费
    EXPECT_FALSE(tracker_->OnEvent(CreateMstxStart(1, 100, 0, 1000, 1, "host range")));
    EXPECT_EQ(tracker_->GetCurrentStep(0), 0u);
}

TEST_F(StepTrackerTest, mstx_end_triggers_step_end_by_step_id_and_range_id)
{
    tracker_->OnEvent(CreateMstxStart(1, 100, 0, 1000, 1));
    tracker_->OnEvent(CreateMstxStart(2, 200, 0, 2000, 2));

    // end匹配stepId+rangeId
    EXPECT_TRUE(tracker_->OnEvent(CreateMstxEnd(2, 200, 0, 2500, 3)));
    ASSERT_EQ(endCalls_.size(), 1u);
    EXPECT_EQ(endCalls_[0].first, 0);
    EXPECT_EQ(endCalls_[0].second, 2u);
    // stepId相同但rangeId不匹配 → 不消费
    EXPECT_FALSE(tracker_->OnEvent(CreateMstxEnd(2, 999, 0, 2600, 4)));
}

TEST_F(StepTrackerTest, mstx_end_without_start_ignored)
{
    EXPECT_FALSE(tracker_->OnEvent(CreateMstxEnd(1, 100, 0, 1000, 1)));
    EXPECT_TRUE(endCalls_.empty());
}

TEST_F(StepTrackerTest, py_step_sequence_ends_prev_then_starts_next)
{
    // msmemscope.step(1)：无条件回调end(1)（表中无记录也回调），开始step2
    tracker_->OnEvent(CreatePyStep(1, 0, 1000, 1));
    EXPECT_EQ(tracker_->GetCurrentStep(0), 2u);  // 记录的是stepId+1

    // msmemscope.step(2)：先回调end(2)，再回调start(3)
    tracker_->OnEvent(CreatePyStep(2, 0, 2000, 2));
    ASSERT_EQ(endCalls_.size(), 2u);
    EXPECT_EQ(endCalls_[0].second, 1u);
    EXPECT_EQ(endCalls_[1].second, 2u);
    ASSERT_EQ(startCalls_.size(), 2u);
    EXPECT_EQ(startCalls_[0].second, 2u);
    EXPECT_EQ(startCalls_[1].second, 3u);
    EXPECT_EQ(tracker_->GetCurrentStep(0), 3u);

    // 边界序列：py模式记录stepId+1
    const auto& boundaries = tracker_->GetStepBoundaries(0);
    ASSERT_EQ(boundaries.size(), 2u);
    EXPECT_EQ(boundaries[0].stepId, 2u);
    EXPECT_EQ(boundaries[0].rangeId, 0u);  // py模式rangeId为0
    EXPECT_EQ(boundaries[1].stepId, 3u);
}

TEST_F(StepTrackerTest, mutex_source_rejects_other_source)
{
    // mstx先行，py step被拒
    tracker_->OnEvent(CreateMstxStart(1, 100, 0, 1000, 1));
    EXPECT_TRUE(tracker_->OnEvent(CreatePyStep(1, 0, 1100, 2)));  // 事件被消费（不处理）
    EXPECT_EQ(tracker_->GetStepSource(), StepSource::MSTX_SOURCE);
    EXPECT_EQ(tracker_->GetCurrentStep(0), 1u);  // py step未推进
}

TEST_F(StepTrackerTest, mutex_source_rejects_mstx_after_py)
{
    // py step先行，mstx start被拒
    tracker_->OnEvent(CreatePyStep(1, 0, 1000, 1));
    EXPECT_TRUE(tracker_->OnEvent(CreateMstxStart(1, 100, 0, 1100, 2)));  // 事件被消费（不处理）
    EXPECT_EQ(tracker_->GetStepSource(), StepSource::PY_STEP_SOURCE);
    EXPECT_EQ(tracker_->GetCurrentStep(0), 2u);  // mstx未推进
}

TEST_F(StepTrackerTest, devices_are_independent)
{
    tracker_->OnEvent(CreateMstxStart(1, 100, 0, 1000, 1));
    tracker_->OnEvent(CreateMstxStart(5, 500, 1, 1000, 2));
    EXPECT_EQ(tracker_->GetCurrentStep(0), 1u);
    EXPECT_EQ(tracker_->GetCurrentStep(1), 5u);
    EXPECT_EQ(tracker_->GetCurrentStep(2), 0u);  // 无记录设备

    const auto& empty = tracker_->GetStepBoundaries(2);
    EXPECT_TRUE(empty.empty());
}

TEST_F(StepTrackerTest, boundaries_sorted_by_timestamp_then_event_id)
{
    // 乱序到达：eventId靠前的先
    tracker_->OnEvent(CreateMstxStart(2, 200, 0, 2000, 2));
    tracker_->OnEvent(CreateMstxStart(1, 100, 0, 1000, 1));
    tracker_->OnEvent(CreateMstxStart(3, 300, 0, 2000, 3));  // 同timestamp，eventId 3 > 2

    const auto& boundaries = tracker_->GetStepBoundaries(0);
    ASSERT_EQ(boundaries.size(), 3u);
    EXPECT_EQ(boundaries[0].stepId, 1u);
    EXPECT_EQ(boundaries[1].stepId, 2u);  // (2000,2) < (2000,3)
    EXPECT_EQ(boundaries[2].stepId, 3u);
}
