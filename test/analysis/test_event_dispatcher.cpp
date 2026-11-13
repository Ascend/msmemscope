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

#include <memory>
#include <string>
#include <vector>

#include "event_dispatcher.h"

using namespace MemScope;

namespace {
// 废弃占位id，生产无订阅者；探针用例用后即退订，避免影响其他用例
constexpr SubscriberId UT_PROBE_ID = SubscriberId::STEP_INNER_ANALYZER;
}  // namespace

// 派发按事件类型路由到订阅者；退订后不再回调
TEST(EventDispatcherExtraTest, dispatch_routes_by_event_type_and_respects_unsubscribe)
{
    EventDispatcher &dispatcher = EventDispatcher::GetInstance();
    dispatcher.UnSubscribe(UT_PROBE_ID);
    int calls = 0;
    EventBaseType lastType = EventBaseType::CONTROL;
    auto handler = [&calls, &lastType](std::shared_ptr<EventBase> &event, MemoryState *state) {
        (void)state;
        ++calls;
        lastType = event->eventType;
    };
    // INVALID类型无生产订阅者，探针事件不会触达分析器
    dispatcher.Subscribe(UT_PROBE_ID, {EventBaseType::INVALID}, EventDispatcher::Priority::High, handler,
                         "ut_disp_probe");

    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::INVALID;
    MemoryState *state = nullptr;
    dispatcher.DispatchEvent(event, state);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(lastType, EventBaseType::INVALID);

    // 退订后同类型事件不再回调
    dispatcher.UnSubscribe(UT_PROBE_ID);
    dispatcher.DispatchEvent(event, state);
    EXPECT_EQ(calls, 1);
}
