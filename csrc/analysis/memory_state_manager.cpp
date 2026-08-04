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

#include "memory_state_manager.h"

#include "analysis/event_dispatcher.h"
#include "framework/event_router.h"
#include "log.h"
#include "utility/file_write_manager.h"
#include "utility/utils.h"

namespace MemScope
{

uint64_t MemoryState::count = 0;
std::mutex MemoryState::mtx;

MemoryStateManager& MemoryStateManager::GetInstance()
{
    // 确保依赖的单例先于 MemoryStateManager 构造，从而在本对象析构时它们仍然存活。
    // C++ 保证函数内静态变量按构造的相反顺序析构，因此先触发构造的单例会后析构。
    EventDispatcher::GetInstance();
    Utility::FileWriteManager::GetInstance();
    static MemoryStateManager manager{};
    return manager;
}

bool MemoryStateManager::AddEvent(std::shared_ptr<MemoryEvent>& event)
{
    if (event->poolType == PoolType::INVALID)
    {
        // LOG_DEBUG
        return false;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (poolsMap_.find(event->poolType) == poolsMap_.end())
    {
        poolsMap_[event->poolType] = Pool{};
    }
    MemoryStateKey key = MemoryStateKey{event->pid, event->addr};
    auto& statesPool = poolsMap_[event->poolType];
    auto& statesMap = statesPool.statesMap;

    // 如果device信息是缺失的，尝试补全
    if (event->device == GD_INVALID_NUM && statesMap.find(key) != statesMap.end() && !statesMap[key].events.empty())
    {
        event->device = statesMap[key].events[0]->device;
    }

    // hal和host内存存在free事件没有size信息，在此处匹配到malloc事件并填写size
    if (event->eventType == EventBaseType::FREE && event->poolType == PoolType::HAL && !statesMap[key].events.empty() &&
        statesMap[key].events[0]->eventType == EventBaseType::MALLOC)
    {
        event->size = statesMap[key].events[0]->size;
        if (event->device == DEVICE_ID_CPU)
        {
            event->eventSubType = statesMap[key].events[0]->eventSubType;
            event->used = static_cast<int64_t>(Utility::GetProcessVmRss());
        }
    }

    if (event->eventType == EventBaseType::MALLOC)
    {
        if (statesMap.find(key) == statesMap.end())
        {
            statesMap[key] = MemoryState{event};
            if (event->isShadowEvent)
            {
                statesMap[key].shadowState = ShadowState::SHADOW_CREATED;
            }
        }
        else
        {
            // 有一种情况会添加失败，malloc时仍有数据未释放
            return false;
        }
    }
    else
    {
        auto state = FindStateInPool(event->poolType, key, event->size);
        if (state == nullptr)
        {
            // 当前事件没有匹配到已有的state，需要新建一个state表示新的内存块
            statesMap[key] = MemoryState{event};
        }
        else
        {
            state->events.push_back(event);
        }
    }
    return true;
}

bool MemoryStateManager::DeteleState(const PoolType& poolType, const MemoryStateKey& key)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (poolsMap_.find(poolType) == poolsMap_.end())
    {
        // LOG_DEBUG
        return false;
    }
    auto it = poolsMap_[poolType].statesMap.find(key);
    if (it == poolsMap_[poolType].statesMap.end())
    {
        // LOG_DEBUG
        return false;
    }
    poolsMap_[poolType].statesMap.erase(it);
    return true;
}

MemoryState* MemoryStateManager::GetState(std::shared_ptr<MemoryEvent>& event)
{
    std::lock_guard<std::mutex> lock(mtx_);
    return FindStateInPool(event->poolType, MemoryStateKey{event->pid, event->addr}, event->size);
}

MemoryState* MemoryStateManager::GetState(std::shared_ptr<EventBase>& event)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto poolType = event->poolType;
    auto key = MemoryStateKey{event->pid, event->addr};
    if (poolsMap_.find(poolType) == poolsMap_.end())
    {
        // LOG_DEBUG
        return nullptr;
    }
    if (poolsMap_[poolType].statesMap.find(key) == poolsMap_[poolType].statesMap.end())
    {
        // LOG_DEBUG
        return nullptr;
    }
    return &(poolsMap_[poolType].statesMap[key]);
}

MemoryState* MemoryStateManager::FindStateInPool(const PoolType& poolType, const MemoryStateKey& key, uint64_t size)
{
    if (poolsMap_.find(poolType) == poolsMap_.end())
    {
        return nullptr;
    }
    auto& statesPool = poolsMap_[poolType];
    auto& statesMap = statesPool.statesMap;
    if (statesMap.find(key) != statesMap.end())
    {
        // 直接匹配到相同起始地址
        return &(statesMap[key]);
    }

    // 使用的地址空间位于某块已分配的内存内
    uint64_t addr = key.addr;
    for (auto& pair : statesMap)
    {
        if (key.pid != pair.first.pid)
        {
            continue;
        }
        uint64_t startingAddr = pair.first.addr;
        if (addr >= startingAddr &&
            Utility::GetAddResult(addr, size) <= Utility::GetAddResult(startingAddr, pair.second.size))
        {
            return &(pair.second);
        }
    }

    return nullptr;
}

std::vector<std::pair<PoolType, MemoryStateKey>> MemoryStateManager::GetAllStateKeys()
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::pair<PoolType, MemoryStateKey>> result;
    for (auto& poolPair : poolsMap_)
    {
        for (auto& statePair : poolPair.second.statesMap)
        {
            result.push_back(std::make_pair(poolPair.first, statePair.first));
        }
    }
    return result;
}

void MemoryStateManager::PromoteShadowStates(const PromoteCallback& dumpFunc)
{
    std::lock_guard<std::mutex> lock(mtx_);

    uint64_t promotionTime = Utility::GetTimeNanoseconds();

    for (auto& poolPair : poolsMap_)
    {
        auto& statesMap = poolPair.second.statesMap;

        // 收集需要处理的key，避免在遍历中修改map
        std::vector<MemoryStateKey> keysToProcess;
        for (auto& statePair : statesMap)
        {
            auto& state = statePair.second;
            if (state.shadowState == ShadowState::SHADOW_CREATED || state.shadowState == ShadowState::SHADOW_FREED)
            {
                keysToProcess.push_back(statePair.first);
            }
        }

        for (auto& key : keysToProcess)
        {
            auto it = statesMap.find(key);
            if (it == statesMap.end())
            {
                continue;
            }
            auto& state = it->second;

            if (state.shadowState == ShadowState::SHADOW_CREATED)
            {
                // 影子期申请：更新MALLOC事件timestamp为start时间，标记已转正
                // MALLOC事件保留在events中，等FREE到达时由DumpMemoryState自然落盘
                state.shadowState = ShadowState::SHADOW_PROMOTED;
                if (!state.events.empty())
                {
                    state.events[0]->timestamp = promotionTime;
                }
            }

            if (state.shadowState == ShadowState::SHADOW_FREED)
            {
                // 正常申请+影子释放：更新size和时间戳后落盘
                if (!state.events.empty() && state.events.back()->eventType == EventBaseType::FREE &&
                    state.events.back()->isShadowEvent)
                {
                    auto freeEvent = state.events.back();
                    freeEvent->size = static_cast<int64_t>(state.size);
                    freeEvent->timestamp = promotionTime;
                }

                dumpFunc(&state);  // 落盘 [MALLOC, 合成FREE]
                statesMap.erase(it);
            }
        }
    }
}

MemoryStateManager::~MemoryStateManager()
{
    for (auto& state : GetAllStateKeys())
    {
        std::shared_ptr<EventBase> event =
            std::make_shared<CleanUpEvent>(EventSubType::PROC_EXIT, state.first, state.second.pid, state.second.addr);
        EventHandler(event);
    }
}

}  // namespace MemScope
