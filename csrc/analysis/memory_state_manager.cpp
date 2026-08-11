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

#include <algorithm>

#include "analysis/event_dispatcher.h"
#include "framework/event_router.h"
#include "log.h"
#include "utility/file_write_manager.h"
#include "utility/utils.h"

namespace MemScope
{

namespace
{
// 按(pid, addr)遍历定位state：FREE事件device缺失（hal/host内存）时key无法哈希定位，
// 遍历匹配已分配块（含地址区间containment）；key.device有效时限定同device（RFC 3.2.4：
// containment扫描条件增加device匹配，不同device的块独立state，不跨卡误并入/误回填）
MemoryState* FindStateByPidAndAddr(
    std::unordered_map<MemoryStateKey, MemoryState, MemoryStateKeyHasher>& statesMap, const MemoryStateKey& key,
    const uint64_t& size)
{
    uint64_t addr = key.addr;
    // addr和size在循环内不变，提前计算一次
    uint64_t addrLimit = Utility::GetAddResult(addr, size);
    for (auto& pair : statesMap)
    {
        if (key.pid != pair.first.pid)
        {
            continue;
        }
        if (key.device != GD_INVALID_NUM && pair.first.device != key.device)
        {
            continue;
        }
        uint64_t startingAddr = pair.first.addr;
        if (addr >= startingAddr && addrLimit <= Utility::GetAddResult(startingAddr, pair.second.size))
        {
            return &(pair.second);
        }
    }
    return nullptr;
}
}  // namespace

void OwnerLabelManager::AddLabel(OwnerLevel level, std::string label)
{
    if (level >= OwnerLevel::OWNER_LEVEL_NUM)
    {
        return;
    }
    labelList[static_cast<size_t>(level)] = std::move(label);
}

std::string OwnerLabelManager::GetLabel(OwnerLevel level) const
{
    if (level >= OwnerLevel::OWNER_LEVEL_NUM)
    {
        return "";
    }
    return labelList[static_cast<size_t>(level)];
}

std::string OwnerLabelManager::GetOwnerStr() const
{
    std::string result;
    for (uint8_t level = 0; level < static_cast<uint8_t>(OwnerLevel::OWNER_LEVEL_NUM); ++level)
    {
        const auto& label = labelList[level];
        if (label.empty())
        {
            continue;
        }
        result += result.empty() ? label : "@" + label;
    }
    return result;
}

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

MemoryState* MemoryStateManager::AddEvent(std::shared_ptr<MemoryEvent>& event)
{
    if (event->poolType == PoolType::INVALID)
    {
        // LOG_DEBUG
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    // operator[]在key缺失时插入默认Pool，一次查找完成
    auto& statesPool = poolsMap_[event->poolType];
    auto& statesMap = statesPool.statesMap;

    // 定位事件所属state：
    // - 先按key(pid, device, addr)精确哈希；未命中时对非MALLOC事件（FREE/ACCESS等）遍历匹配
    //   （含地址区间containment）：device缺失的FREE事件（hal/host内存）借此回填device后再哈希
    //   精确定位；ACCESS等地址落在已分配块区间内的事件并入所属块state
    // - MALLOC事件不做模糊匹配（MALLOC冲突语义）
    MemoryState* located = nullptr;
    MemoryStateKey key{event->pid, event->device, event->addr};
    auto stateIt = statesMap.find(key);
    if (stateIt != statesMap.end())
    {
        located = &stateIt->second;
    }
    else if (event->eventType != EventBaseType::MALLOC)
    {
        located = FindStateByPidAndAddr(statesMap, key, static_cast<uint64_t>(event->size));
    }

    // 复用同一次查找补全device和size信息
    if (located != nullptr && !located->events.empty())
    {
        auto& firstEvent = located->events[0];
        // 如果device信息是缺失的，尝试补全
        if (event->device == GD_INVALID_NUM)
        {
            event->device = firstEvent->device;
        }

        // hal和host内存存在free事件没有size信息，在此处匹配到malloc事件并填写size
        if (event->eventType == EventBaseType::FREE && event->poolType == PoolType::HAL &&
            firstEvent->eventType == EventBaseType::MALLOC)
        {
            event->size = firstEvent->size;
            if (event->device == DEVICE_ID_CPU)
            {
                event->eventSubType = firstEvent->eventSubType;
                event->used = static_cast<int64_t>(Utility::GetProcessVmRss());
            }
        }
    }

    if (event->eventType == EventBaseType::MALLOC)
    {
        if (located != nullptr)
        {
            // 有一种情况会添加失败，malloc时仍有数据未释放
            return nullptr;
        }
        MemoryStateKey mallocKey{event->pid, event->device, event->addr};
        auto inserted = statesMap.emplace(mallocKey, MemoryState{event});
        MemoryState& newState = inserted.first->second;
        if (event->isShadowEvent)
        {
            newState.shadowState = ShadowState::SHADOW_CREATED;
        }
        return &newState;
    }
    else
    {
        if (located != nullptr)
        {
            located->events.push_back(event);
            return located;
        }
        // 当前事件没有匹配到已有的state，需要新建一个state表示新的内存块
        MemoryStateKey ghostKey{event->pid, event->device, event->addr};
        auto inserted = statesMap.emplace(ghostKey, MemoryState{event});
        return &inserted.first->second;
    }
}

bool MemoryStateManager::DeteleState(const PoolType& poolType, const MemoryStateKey& key)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto poolIt = poolsMap_.find(poolType);
    if (poolIt == poolsMap_.end())
    {
        // LOG_DEBUG
        return false;
    }
    auto& statesMap = poolIt->second.statesMap;
    auto it = statesMap.find(key);
    if (it == statesMap.end())
    {
        // LOG_DEBUG
        return false;
    }
    statesMap.erase(it);
    return true;
}

MemoryState* MemoryStateManager::GetState(const std::shared_ptr<MemoryEvent>& event)
{
    std::lock_guard<std::mutex> lock(mtx_);
    return FindStateInPool(event->poolType, MemoryStateKey{event->pid, event->device, event->addr}, event->size);
}

MemoryState* MemoryStateManager::GetState(const std::shared_ptr<EventBase>& event)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto poolType = event->poolType;
    auto key = MemoryStateKey{event->pid, event->device, event->addr};
    auto poolIt = poolsMap_.find(poolType);
    if (poolIt == poolsMap_.end())
    {
        // LOG_DEBUG
        return nullptr;
    }
    auto& statesMap = poolIt->second.statesMap;
    auto stateIt = statesMap.find(key);
    if (stateIt != statesMap.end())
    {
        return &stateIt->second;
    }
    if (event->device == GD_INVALID_NUM)
    {
        // device缺失的事件（如MEMORY_OWNER直标）无法哈希定位，按(pid, addr)遍历匹配
        // （key.device为GD_INVALID_NUM，过滤条件天然不生效）
        return FindStateByPidAndAddr(statesMap, key, 0);
    }
    // LOG_DEBUG
    return nullptr;
}

MemoryState* MemoryStateManager::FindStateInPool(const PoolType& poolType, const MemoryStateKey& key, uint64_t size)
{
    auto poolIt = poolsMap_.find(poolType);
    if (poolIt == poolsMap_.end())
    {
        return nullptr;
    }
    auto& statesMap = poolIt->second.statesMap;
    auto stateIt = statesMap.find(key);
    if (stateIt != statesMap.end())
    {
        // 直接匹配到相同起始地址
        return &stateIt->second;
    }

    // 使用的地址空间位于某块已分配的内存内
    uint64_t addr = key.addr;
    // addr和size在循环内不变，提前计算一次
    uint64_t addrLimit = Utility::GetAddResult(addr, size);
    for (auto& pair : statesMap)
    {
        if (key.pid != pair.first.pid)
        {
            continue;
        }
        if (key.device != GD_INVALID_NUM && pair.first.device != key.device)
        {
            continue;
        }
        uint64_t startingAddr = pair.first.addr;
        if (addr >= startingAddr && addrLimit <= Utility::GetAddResult(startingAddr, pair.second.size))
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

std::vector<LiveBlockInfo> MemoryStateManager::QueryLiveBlocks(const LiveBlockFilter& filter) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<LiveBlockInfo> result;
    for (const auto& poolPair : poolsMap_)
    {
        if (!filter.poolTypes.empty() &&
            std::find(filter.poolTypes.begin(), filter.poolTypes.end(), poolPair.first) == filter.poolTypes.end())
        {
            continue;
        }
        for (const auto& statePair : poolPair.second.statesMap)
        {
            const auto& state = statePair.second;
            // 跳过幽灵state（events中只有FREE事件，无对应MALLOC）
            if (state.events.empty() || state.events[0]->eventType != EventBaseType::MALLOC)
            {
                continue;
            }
            // 排除影子期创建/已释放的块：未转正的影子块和影子期释放的块不参与存活判定
            if (filter.excludeShadowCreated &&
                (state.shadowState == ShadowState::SHADOW_CREATED || state.shadowState == ShadowState::SHADOW_FREED))
            {
                continue;
            }
            const auto& allocEvent = state.events[0];
            if (filter.device != GD_INVALID_NUM && allocEvent->device != filter.device)
            {
                continue;
            }
            if (filter.pid != 0 && statePair.first.pid != filter.pid)
            {
                continue;
            }
            LiveBlockInfo info;
            info.poolType = poolPair.first;
            info.pid = statePair.first.pid;
            info.device = allocEvent->device;
            info.addr = statePair.first.addr;
            info.size = state.size;
            info.allocationId = state.allocationId;
            info.allocTimestamp = allocEvent->timestamp;
            info.allocEventId = allocEvent->id;
            info.kernelIndex = allocEvent->kernelIndex;
            info.shadowState = state.shadowState;
            info.cCallStack = allocEvent->cCallStack;
            info.pyCallStack = allocEvent->pyCallStack;
            result.push_back(info);
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
        // 从key回填device，保证DeteleState的key(pid, device, addr)一致
        event->device = state.second.device;
        EventHandler(event);
    }
}

}  // namespace MemScope
