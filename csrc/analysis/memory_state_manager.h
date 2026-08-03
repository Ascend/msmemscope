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

#ifndef MEMORY_STATE_MANAGER_H
#define MEMORY_STATE_MANAGER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "event.h"
#include "state.h"
#include "state_manager.h"

namespace MemScope
{

class MemoryStateKey : StateKey
{
   public:
    uint64_t pid;
    uint64_t addr;

    MemoryStateKey(uint64_t pid, uint64_t addr) : pid(pid), addr(addr) {}

    // 必须实现相等运算符
    bool operator==(const MemoryStateKey& other) const { return (pid == other.pid) && (addr == other.addr); }
};

struct MemoryStateKeyHasher
{
    std::size_t operator()(const MemoryStateKey& key) const
    {
        size_t pidHash = std::hash<uint64_t>()(key.pid);
        size_t addrHash = std::hash<uint64_t>()(key.addr);
        return pidHash ^ (addrHash << 1);
    }
};

// 影子状态：追踪NOT_IN_TRACING期间内存块的生命周期变化
enum class ShadowState : uint8_t
{
    NORMAL = 0,       // 正常申请（采集期内）
    SHADOW_CREATED,   // 影子期申请，尚未转正
    SHADOW_PROMOTED,  // 影子期申请，已转正（防止被后续影子释放消亡）
    SHADOW_FREED,     // 正常申请/已转正+影子期释放
};

// OwnerLevel 枚举定义于 event.h(分级标签模型), 此处复用

class OwnerLabelManager
{
   public:
    explicit OwnerLabelManager() : labelList(static_cast<size_t>(OwnerLevel::OWNER_LEVEL_NUM)) {}

    // 添加标签，若已有则会覆盖(地址直标优先语义)
    void AddLabel(OwnerLevel level, std::string label);
    // 获取指定级别标签(不存在返回空串)
    std::string GetLabel(OwnerLevel level) const;
    // 获取描述字符串，dump时用，按OwnerLevel拼接，'@'分隔，空白值直接跳过
    std::string GetOwnerStr() const;

   private:
    std::vector<std::string> labelList;
};

class MemoryState : public StateBase
{
   public:
    std::vector<std::shared_ptr<MemoryEvent>> events;
    std::vector<uint64_t> apiId;
    uint64_t size = 0;
    uint64_t allocationId = 0;
    ShadowState shadowState = ShadowState::NORMAL;
    OwnerLabelManager owner;
    std::string inefficientType;

    explicit MemoryState() {}

    explicit MemoryState(std::shared_ptr<MemoryEvent>& event)
    {
        events.push_back(event);
        size = static_cast<uint64_t>(event->size);
        inefficientType = "";
        std::lock_guard<std::mutex> lock(mtx);
        allocationId = ++count;
    }

   private:
    static std::mutex mtx;  // 修改count需要加锁
    static uint64_t count;  // static变量，用于分配唯一id
};

class Pool
{
   public:
    std::unordered_map<MemoryStateKey, MemoryState, MemoryStateKeyHasher> statesMap;

    Pool() {}
};

class MemoryStateManager : StateManager
{
   public:
    static MemoryStateManager& GetInstance();

    bool AddEvent(std::shared_ptr<MemoryEvent>& event);
    bool DeteleState(const PoolType& poolType, const MemoryStateKey& key);
    MemoryState* GetState(std::shared_ptr<EventBase>& event);
    MemoryState* GetState(std::shared_ptr<MemoryEvent>& event);
    std::vector<std::pair<PoolType, MemoryStateKey>> GetAllStateKeys();

    // 线程安全的历史转正：持有mtx_遍历所有影子标记的state
    // SHADOW_CREATED → SHADOW_PROMOTED（更新timestamp，等FREE时自然落盘）
    // SHADOW_FREED → 通过dumpFunc回调落盘完整state后删除
    using PromoteCallback = std::function<void(MemoryState*)>;
    void PromoteShadowStates(const PromoteCallback& dumpFunc);

    void SetLastStopTimestamp(uint64_t ts) { lastStopTimestamp_ = ts; }
    void ClearLastStopTimestamp() { lastStopTimestamp_ = 0; }
    uint64_t GetLastStopTimestamp() const { return lastStopTimestamp_; }

   private:
    MemoryState* FindStateInPool(const PoolType& poolType, const MemoryStateKey& key, uint64_t size);
    ~MemoryStateManager() override;
    std::unordered_map<PoolType, Pool> poolsMap_;
    std::mutex mtx_;
    uint64_t lastStopTimestamp_ = 0;  // 最后一次TRACE_STOP的时间戳，0表示无效
};

}  // namespace MemScope

#endif
