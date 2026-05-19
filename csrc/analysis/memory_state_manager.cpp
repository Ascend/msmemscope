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

#include "log.h"
#include "utility/utils.h"
#include "process.h"

namespace MemScope {

uint64_t MemoryState::count = 0;
std::mutex MemoryState::mtx;

MemoryStateManager& MemoryStateManager::GetInstance()
{
    static MemoryStateManager manager{};
    return manager;
}

bool MemoryStateManager::AddEvent(std::shared_ptr<MemoryEvent>& event)
{
    if (event->poolType == PoolType::INVALID) {
        // LOG_DEBUG
        return false;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (poolsMap_.find(event->poolType) == poolsMap_.end()) {
        poolsMap_[event->poolType] = Pool{};
    }
    MemoryStateKey key = MemoryStateKey{event->pid, event->addr};
    auto& statesPool = poolsMap_[event->poolType];
    auto& statesMap = statesPool.statesMap;

    // 如果device信息是缺失的，尝试补全
    if (event->device == GD_INVALID_NUM
        && statesMap.find(key) != statesMap.end()
        && !statesMap[key].events.empty()) {
        event->device = statesMap[key].events[0]->device;
    }

    // hal和host内存存在free事件没有size信息，在此处匹配到malloc事件并填写size
    if (event->eventType == EventBaseType::FREE
        && event->poolType == PoolType::HAL
        && !statesMap[key].events.empty() && statesMap[key].events[0]->eventType == EventBaseType::MALLOC) {
        event->size = statesMap[key].events[0]->size;
    }

    if (event->eventType == EventBaseType::MALLOC) {
        if (statesMap.find(key) == statesMap.end()) {
            statesMap[key] = MemoryState{event};
        } else {
            // 有一种情况会添加失败，malloc时仍有数据未释放
            return false;
        }
    } else {
        auto state = FindStateInPool(event->poolType, key, event->size);
        if (state == nullptr) {
            // 当前事件没有匹配到已有的state，需要新建一个state表示新的内存块
            statesMap[key] = MemoryState{event};
        } else {
            state->events.push_back(event);
        }
    }
    return true;
}

bool MemoryStateManager::DeteleState(const PoolType& poolType, const MemoryStateKey& key)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (poolsMap_.find(poolType) == poolsMap_.end()) {
        // LOG_DEBUG
        return false;
    }
    auto it = poolsMap_[poolType].statesMap.find(key);
    if (it == poolsMap_[poolType].statesMap.end()) {
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
    if (poolsMap_.find(poolType) == poolsMap_.end()) {
        // LOG_DEBUG
        return nullptr;
    }
    if (poolsMap_[poolType].statesMap.find(key) == poolsMap_[poolType].statesMap.end()) {
        // LOG_DEBUG
        return nullptr;
    }
    return &(poolsMap_[poolType].statesMap[key]);
}

MemoryState* MemoryStateManager::FindStateInPool(const PoolType& poolType, const MemoryStateKey& key, uint64_t size)
{
    if (poolsMap_.find(poolType) == poolsMap_.end()) {
        return nullptr;
    }
    auto& statesPool = poolsMap_[poolType];
    auto& statesMap = statesPool.statesMap;
    if (statesMap.find(key) != statesMap.end()) {
        // 直接匹配到相同起始地址
        return &(statesMap[key]);
    }

    // 使用的地址空间位于某块已分配的内存内
    uint64_t addr = key.addr;
    for (auto& pair : statesMap) {
        if (key.pid != pair.first.pid) {
            continue;
        }
        uint64_t startingAddr = pair.first.addr;
        if (addr >= startingAddr
            && Utility::GetAddResult(addr, size) <= Utility::GetAddResult(startingAddr, pair.second.size)) {
            return &(pair.second);
        }
    }

    return nullptr;
}

std::vector<std::pair<PoolType, MemoryStateKey>> MemoryStateManager::GetAllStateKeys()
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::pair<PoolType, MemoryStateKey>> result;
    for (auto& poolPair : poolsMap_) {
        for (auto& statePair : poolPair.second.statesMap) {
            result.push_back(std::make_pair(poolPair.first, statePair.first));
        }
    }
    return result;
}

MemoryStateManager::~MemoryStateManager()
{
    for (auto& state : GetAllStateKeys()) {
        std::shared_ptr<EventBase> event = std::make_shared<CleanUpEvent>(state.first, state.second.pid, state.second.addr);
        EventHandler(event);
    }
}

}