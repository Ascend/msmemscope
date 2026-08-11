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

#ifndef EVENT_DISPATCHER_H
#define EVENT_DISPATCHER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "event.h"
#include "memory_state_manager.h"

namespace MemScope
{

enum class SubscriberId : uint8_t
{
    DECOMPOSE_ANALYZER = 0,
    INEFFICIENT_ANALYZER,
    LEAKS_ANALYZER,
    DUMP,
    HEALTH_ANALYZER,  // 统计职责（原StepInnerAnalyzer的Gap/池状态）
    // 废弃条目：HAL_ANALYZER/STEP_INNER_ANALYZER已合并进LEAKS_ANALYZER/HEALTH_ANALYZER，保留占位避免序号漂移
    HAL_ANALYZER,
    STEP_INNER_ANALYZER,
};

class EventDispatcher
{
   public:
    enum class Priority : uint8_t
    {
        High = 3,  // decompose, inefficient, memscope
        Medium = 2,
        Low = 1,
        Lowest = 0,  // dump
    };

    using HandlerFunc = std::function<void(std::shared_ptr<EventBase>&, MemoryState*)>;

    struct Subscriber
    {
        SubscriberId id;
        Priority priority;
        HandlerFunc handler;

        // 用于排序，优先级高的排在前面
        bool operator<(const Subscriber& other) const
        {
            return static_cast<uint8_t>(priority) > static_cast<uint8_t>(other.priority);
        }

        // 用于查找
        bool operator==(SubscriberId otherId) const { return id == otherId; }
    };

    static EventDispatcher& GetInstance();
    void DispatchEvent(std::shared_ptr<EventBase>& event, MemoryState* state);
    void Subscribe(const SubscriberId& id, const std::vector<EventBaseType>& eventTypes, const Priority& priority,
                   const HandlerFunc& func);
    void UnSubscribe(const SubscriberId& id);

   private:
    EventDispatcher() = default;
    ~EventDispatcher() = default;

    EventDispatcher(const EventDispatcher&) = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;
    EventDispatcher(EventDispatcher&&) = delete;
    EventDispatcher& operator=(EventDispatcher&&) = delete;

    std::unordered_map<EventBaseType, std::vector<Subscriber>> eventSubscribers_;
};

}  // namespace MemScope

#endif
