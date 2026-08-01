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

#include "hal_analyzer.h"

#include <memory>

#include "bit_field.h"
#include "utility/log.h"

namespace MemScope
{

HalAnalyzer& HalAnalyzer::GetInstance(Config config)
{
    static HalAnalyzer analyzer(config);
    return analyzer;
}

HalAnalyzer::HalAnalyzer(Config config)
{
    // 确保Utility::Log先于当前对象构造，利用C++静态对象析构逆序规则，使~HalAnalyzer中LOG宏安全
    Utility::Log::GetLog();
    config_ = config;
    Subscribe();
    return;
}

void HalAnalyzer::Subscribe()
{
    auto func = std::bind(&HalAnalyzer::EventHandle, this, std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::FREE};
    EventDispatcher::GetInstance().Subscribe(SubscriberId::HAL_ANALYZER, eventList, EventDispatcher::Priority::High,
                                             func);
}

void HalAnalyzer::UnSubscribe() const { EventDispatcher::GetInstance().UnSubscribe(SubscriberId::HAL_ANALYZER); }

void HalAnalyzer::EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    // 仅处理HAL池的内存事件
    if (event->poolType != PoolType::HAL)
    {
        return;
    }

    // Skip shadow/historical events
    if (auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event))
    {
        if (memEvent->isShadowEvent)
        {
            return;
        }
    }

    // 判断是否满足功能开启条件
    if (!IsHalAnalysisEnable())
    {
        return;
    }

    ClientId clientId = event->pid;
    if (!CreateMemTables(clientId))
    {
        LOG_ERROR("[client %u]: Create hal Memory table failed.", clientId);
        return;
    }

    auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event);
    if (memEvent == nullptr)
    {
        LOG_WARN("[client %u]: HalAnalyzer receive invalid event.", clientId);
        return;
    }

    if (event->eventType == EventBaseType::MALLOC)
    {
        RecordMalloc(clientId, memEvent);
    }
    else if (event->eventType == EventBaseType::FREE)
    {
        RecordFree(clientId, memEvent);
    }
}

bool HalAnalyzer::IsHalAnalysisEnable()
{
    // 确认analysis设置中是否包含泄漏分析或OOM详细分析
    BitField<decltype(config_.analysisType)> analysisType(config_.analysisType);
    if (!(analysisType.checkBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS))) &&
        !(analysisType.checkBit(static_cast<size_t>(AnalysisType::OOM_ANALYSIS))))
    {
        return false;
    }
    // 当开启--steps时，关闭所有分析功能
    if (config_.stepList.stepCount != 0)
    {
        return false;
    }

    // 非默认采集模式，关闭分析功能
    if (config_.collectMode == static_cast<uint8_t>(CollectMode::DEFERRED))
    {
        return false;
    }

    // 当malloc和free采集并非都开启时，关闭分析功能
    BitField<decltype(config_.eventType)> eventType(config_.eventType);
    if (!(eventType.checkBit(static_cast<size_t>(EventType::ALLOC_EVENT))) ||
        !(eventType.checkBit(static_cast<size_t>(EventType::FREE_EVENT))))
    {
        return false;
    }
    return true;
}

bool HalAnalyzer::CreateMemTables(const ClientId& clientId)
{
    if (memtables_.find(clientId) != memtables_.end())
    {
        return true;
    }
    LOG_INFO("[client %u]: Start Record hal Memory.", clientId);
    MemoryRecordTable memrecordtable{};
    auto result = memtables_.emplace(clientId, memrecordtable);
    if (result.second)
    {
        return true;
    }
    return false;
}

void HalAnalyzer::RecordMalloc(const ClientId& clientId, std::shared_ptr<const MemoryEvent> memEvent)
{
    if (memEvent == nullptr)
    {
        LOG_WARN("[client %u]: HalAnalyzer receive invalid event.", clientId);
        return;
    }
    uint64_t memkey = memEvent->addr;
    // malloc操作需解析当前moduleId
    bool foundModule = false;
    std::string modulename = "INVLID_MOUDLE_ID";
    if (MODULE_HASH_TABLE.find(memEvent->moduleId) != MODULE_HASH_TABLE.end())
    {
        modulename = MODULE_HASH_TABLE.find(memEvent->moduleId)->second;
        foundModule = true;
    }
    if (!foundModule)
    {
        LOG_WARN("[client %u][device: %d]: Malloc operator did not find %d Module in index %u malloc record.", clientId,
                 memEvent->device, memEvent->moduleId, memEvent->id);
    }

    if (memtables_[clientId].find(memkey) != memtables_[clientId].end())
    {
        if ((memtables_[clientId].find(memkey)->second.addrStatus == AddrStatus::FREE_WAIT))
        {
            LOG_WARN("[client %u]: server already has malloc record in addr: 0x%lx ,", clientId, memEvent->addr);
            LOG_WARN("[client %u]: but now malloc again in index: %u, addr: 0x%lx, size: %u, space: %u", clientId,
                     memEvent->id, memEvent->addr, memEvent->size, memEvent->space);
        }
    }
    else
    {
        HalMemInfo halMemInfo{};
        memtables_[clientId].emplace(memkey, halMemInfo);
    }
    memtables_[clientId][memkey].deviceId = memEvent->device;
    memtables_[clientId][memkey].addrStatus = AddrStatus::FREE_WAIT;
    memtables_[clientId][memkey].size = memEvent->size;
    memtables_[clientId][memkey].timestamp = memEvent->timestamp;
    if (!memEvent->cCallStack.empty())
    {
        memtables_[clientId][memkey].cCallStack = memEvent->cCallStack;
    }
    if (!memEvent->pyCallStack.empty())
    {
        memtables_[clientId][memkey].pyCallStack = memEvent->pyCallStack;
    }
}

void HalAnalyzer::RecordFree(const ClientId& clientId, std::shared_ptr<const MemoryEvent> memEvent)
{
    uint64_t memkey = memEvent->addr;
    auto it = memtables_[clientId].find(memkey);
    if (it != memtables_[clientId].end())
    {
        if (it->second.addrStatus == AddrStatus::FREE_WAIT)
        {
            memtables_[clientId][memkey].addrStatus = AddrStatus::FREE_ALREADY;
        }
        else
        {
            LOG_WARN("[client %u]: Double free operator found for malloc operation : addr: 0x%lx", clientId,
                     memEvent->addr);
        }
    }
    else
    {
        LOG_WARN("[client %u]: No matching malloc operation found for free operator: addr: 0x%lx", clientId,
                 memEvent->addr);
    }
}

void HalAnalyzer::CheckLeak(const size_t clientId)
{
    bool foundLeaks = false;
    if (memtables_.find(clientId) != memtables_.end())
    {
        for (const auto& pair : memtables_[clientId])
        {
            if (pair.second.addrStatus != AddrStatus::FREE_ALREADY)
            {
                foundLeaks = true;
                LOG_WARN("[client %u]: Leak memory in Malloc operator, addr: 0x%lx", clientId, pair.first);
            }
        }
    }
    if (!foundLeaks)
    {
        LOG_INFO("[client %u]: There is no hal leak memory.", clientId);
    }
}

void HalAnalyzer::LeakAnalyze()
{
    if (!IsHalAnalysisEnable())
    {
        return;
    }

    if (memtables_.empty())
    {
        LOG_ERROR("No memory records available.");
    }
    else
    {
        for (const auto& pair : memtables_)
        {
            CheckLeak(pair.first);
        }
    }

    return;
}

HalAnalyzer::~HalAnalyzer()
{
    // 构造函数中已保证Utility::Log先于HalAnalyzer构造，根据C++静态对象析构规则（逆序），
    // Log将在HalAnalyzer之后析构，因此此处调用LeakAnalyze使用LOG宏是安全的
    LeakAnalyze();
    UnSubscribe();
}

std::vector<OOMMemRecord> HalAnalyzer::QueryUnfreedRecords(uint32_t clientId) const
{
    std::vector<OOMMemRecord> records;
    auto it = memtables_.find(clientId);
    if (it == memtables_.end())
    {
        return records;
    }
    for (const auto& pair : it->second)
    {
        if (pair.second.addrStatus == AddrStatus::FREE_WAIT)
        {
            OOMMemRecord rec;
            rec.poolType = PoolType::HAL;
            rec.ptr = pair.first;
            rec.memSize = pair.second.size;
            rec.allocTimestamp = pair.second.timestamp;
            rec.clientId = clientId;
            rec.cCallStack = pair.second.cCallStack;
            rec.pyCallStack = pair.second.pyCallStack;
            records.push_back(rec);
        }
    }
    return records;
}

}  // namespace MemScope
