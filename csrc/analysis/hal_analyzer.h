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

#ifndef HAL_ANALYZER_H
#define HAL_ANALYZER_H

#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "comm_def.h"
#include "config_info.h"
#include "constant.h"
#include "event_dispatcher.h"
#include "memory_state_manager.h"
#include "record_info.h"

namespace MemScope
{
/*
 * HalAnalyzer类主要功能：
 * 1. 维护halmemalloc/halmemfree操作记录表
   2. 分析hal侧内存使用问题，泄漏问题
   3. 通过EventDispatcher订阅MALLOC/FREE事件（替代Process::SendEvent中的switch-case分发）
*/

// HAL内存块标识：同一进程在不同device上可能分配相同地址的独立内存块，key需为device与addr的组合
struct HalAddrKey
{
    int32_t deviceId;
    uint64_t addr;

    HalAddrKey(int32_t device, uint64_t memAddr) : deviceId(device), addr(memAddr) {}
    bool operator==(const HalAddrKey& other) const { return deviceId == other.deviceId && addr == other.addr; }
};

struct HalAddrKeyHash
{
    std::size_t operator()(const HalAddrKey& key) const
    {
        size_t deviceHash = std::hash<int32_t>()(key.deviceId);
        size_t addrHash = std::hash<uint64_t>()(key.addr);
        return deviceHash ^ (addrHash << 1);
    }
};

struct HalMemInfo
{
    int64_t size = 0;
    uint64_t timestamp = 0;
    std::string cCallStack;
    std::string pyCallStack;
};

// 表内仅保存未释放的存活块（malloc插入、free删除），key为(deviceId, addr)组合
using MemoryRecordTable = std::unordered_map<HalAddrKey, HalMemInfo, HalAddrKeyHash>;

class HalAnalyzer
{
   public:
    static HalAnalyzer& GetInstance();
    void EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state);
    void Subscribe();
    void UnSubscribe() const;
    std::vector<OOMMemRecord> QueryUnfreedRecords(uint32_t clientId) const;

   private:
    explicit HalAnalyzer();
    ~HalAnalyzer();
    HalAnalyzer(const HalAnalyzer&) = delete;
    HalAnalyzer& operator=(const HalAnalyzer&) = delete;
    HalAnalyzer(HalAnalyzer&& other) = delete;
    HalAnalyzer& operator=(HalAnalyzer&& other) = delete;

    std::unordered_map<ClientId, MemoryRecordTable> memtables_{};
    bool IsHalAnalysisEnable();
    bool CreateMemTables(const ClientId& clientId);
    void RecordMalloc(const ClientId& clientId, std::shared_ptr<const MemoryEvent> memEvent);
    void RecordFree(const ClientId& clientId, std::shared_ptr<const MemoryEvent> memEvent);
    void LeakAnalyze();
    void CheckLeak(const size_t clientId);
    mutable std::mutex mutex_;
};

}  // namespace MemScope

#endif
