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

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
    // host内存泄漏检测分析器，置于废弃占位之后追加，不改变既有序号
    HOST_LEAKS_ANALYZER,
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

    // 订阅表并发访问互斥:Subscribe/UnSubscribe(config线程/构造期)与
    // DispatchEvent(钩子上报线程高频)并发操作同一unordered_map为数据竞争——
    // operator[]可触发rehash(节点重挂/桶数组扩容),与find/迭代并发即UB,可破坏
    // SYSTEM订阅者链,静默丢弃关窗STAGE_END→分析器窗口孤儿化(兜底unknown报告)。
    // 持锁调用handler:handler再锁自身分析器mutex,锁序恒为dispatcher→analyzer,
    // 无逆序死锁(Subscribe路径不持任何分析器锁)。
    // timed_mutex:退出期防护——任何订阅者handler死锁(持锁不还,实测退出期竞态
    // 曾致钩子闭窗变体交付挂起>120s)都会让DispatchEvent永久阻塞,拖死上报线程
    // →进程退出挂起。DispatchEvent用try_lock_for(15s)超时跳过本次派发(事件
    // 丢失,完整度如实下降;由~HostLeakAnalyzer析构兜底报告),进程得以退出;
    // 正常路径锁竞争毫秒级,15s上界不可达
    mutable std::timed_mutex mutex_;
    std::unordered_map<EventBaseType, std::vector<Subscriber>> eventSubscribers_;
};

}  // namespace MemScope

#endif
