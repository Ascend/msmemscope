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
#include <cstdio>
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

    std::lock_guard<std::timed_mutex> lock(mutex_);
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
    std::lock_guard<std::timed_mutex> lock(mutex_);
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
    // 退出期逃生:持有dispatcher锁的handler若死锁(持锁不还),正常加锁会永久阻塞
    // 上报线程→进程退出挂起(实测竞态曾致钩子闭窗变体交付挂起>120s)。
    // try_lock_for(15s)超时则跳过本次派发(事件丢失,完整度如实下降;由
    // ~HostLeakAnalyzer析构兜底报告),进程得以退出;正常路径锁竞争毫秒级,15s不可达。
    if (!mutex_.try_lock_for(std::chrono::seconds(15)))
    {
        fprintf(stderr, "[msmemscope] event dispatcher lock busy >15s, dispatch skipped (eventType=%d)\n",
                static_cast<int>(event->eventType));
        return;
    }
    std::lock_guard<std::timed_mutex> lock(mutex_, std::adopt_lock);
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
