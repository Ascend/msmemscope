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

#include "event_dispatcher.h"

#include <algorithm>
#include <vector>

#include "utility/log.h"

namespace MemScope
{

EventDispatcher& EventDispatcher::GetInstance()
{
    static EventDispatcher dispatcher;
    return dispatcher;
}

void EventDispatcher::Subscribe(const SubscriberId& id, const std::vector<EventBaseType>& eventTypes,
                                const Priority& priority, const HandlerFunc& func)
{
    Subscriber newSubscriber{id, priority, func};

    for (auto eventType : eventTypes)
    {
        // operator[]在缺失时默认构造空vector，一次查找完成
        auto& subscribers = eventSubscribers_[eventType];
        auto subscriberIt = std::find(subscribers.begin(), subscribers.end(), id);
        if (subscriberIt == subscribers.end())
        {
            // 按序插入
            auto it = std::lower_bound(subscribers.begin(), subscribers.end(), newSubscriber);
            subscribers.insert(it, newSubscriber);
        }
    }
}

void EventDispatcher::UnSubscribe(const SubscriberId& id)
{
    for (auto& pair : eventSubscribers_)
    {
        auto& subscribers = pair.second;
        auto subIt = std::find(subscribers.begin(), subscribers.end(), id);
        if (subIt != subscribers.end())
        {
            subscribers.erase(subIt);
        }
    }
}

void EventDispatcher::DispatchEvent(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    auto it = eventSubscribers_.find(event->eventType);
    if (it != eventSubscribers_.end())
    {
        auto& subscribers = it->second;
        for (const auto& subscriber : subscribers)
        {
            subscriber.handler(event, state);
        }
    }
}

}  // namespace MemScope
