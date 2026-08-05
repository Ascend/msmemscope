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
#include "event_trace/trace_manager/event_trace_manager.h"
#include "utility/log.h"

namespace MemScope
{

HalAnalyzer& HalAnalyzer::GetInstance()
{
    static HalAnalyzer analyzer;
    return analyzer;
}

HalAnalyzer::HalAnalyzer()
{
    // 确保Utility::Log先于当前对象构造，利用C++静态对象析构逆序规则，使~HalAnalyzer中LOG宏安全
    Utility::Log::GetLog();
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
    if (event->poolType != PoolType::HAL || event->device == GD_INVALID_NUM)
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
    // 动态读取当前配置（每个事件只取一次锁），保证分析开关与运行中修改的配置保持一致
    const Config& config = GetConfig();
    // 确认analysis设置中是否包含泄漏分析或OOM详细分析
    BitField<decltype(config.analysisType)> analysisType(config.analysisType);
    if (!(analysisType.checkBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS))) &&
        !(analysisType.checkBit(static_cast<size_t>(AnalysisType::OOM_ANALYSIS))))
    {
        return false;
    }
    // 当开启--steps时，关闭所有分析功能
    if (config.stepList.stepCount != 0)
    {
        return false;
    }

    // 非默认采集模式，关闭分析功能
    if (config.collectMode == static_cast<uint8_t>(CollectMode::DEFERRED))
    {
        return false;
    }

    // 当malloc和free采集并非都开启时，关闭分析功能
    BitField<decltype(config.eventType)> eventType(config.eventType);
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
    // 同一地址在不同device上是独立的内存块，key需为(deviceId, addr)组合
    HalAddrKey memkey{memEvent->device, memEvent->addr};
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

    // 表仅保存未释放的存活块：free后条目会被删除（RecordFree），此处条目存在即表示地址仍存活
    if (memtables_[clientId].find(memkey) != memtables_[clientId].end())
    {
        LOG_WARN("[client %u]: server already has malloc record in addr: 0x%lx ,", clientId, memEvent->addr);
        LOG_WARN("[client %u]: but now malloc again in index: %u, addr: 0x%lx, size: %u, space: %u", clientId,
                 memEvent->id, memEvent->addr, memEvent->size, memEvent->space);
    }
    else
    {
        HalMemInfo halMemInfo{};
        halMemInfo.size = memEvent->size;
        halMemInfo.timestamp = memEvent->timestamp;
        if (!memEvent->cCallStack.empty())
        {
            halMemInfo.cCallStack = memEvent->cCallStack;
        }
        if (!memEvent->pyCallStack.empty())
        {
            halMemInfo.pyCallStack = memEvent->pyCallStack;
        }
        memtables_[clientId].emplace(memkey, halMemInfo);
    }
}

void HalAnalyzer::RecordFree(const ClientId& clientId, std::shared_ptr<const MemoryEvent> memEvent)
{
    // 与RecordMalloc的key保持一致：同一地址在不同device上是独立的内存块
    HalAddrKey memkey{memEvent->device, memEvent->addr};
    auto it = memtables_[clientId].find(memkey);
    if (it != memtables_[clientId].end())
    {
        // 内存块已释放，直接从表中删除，避免历史条目无限累积
        memtables_[clientId].erase(it);
    }
    else
    {
        // 地址已释放/从未申请，或double free（条目已在首次free时删除）
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
            // 表内条目均为未释放的存活块（free即删除），全部视为泄漏
            foundLeaks = true;
            LOG_WARN("[client %u][device: %d]: Leak memory in Malloc operator, addr: 0x%lx", clientId,
                     pair.first.deviceId, pair.first.addr);
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
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OOMMemRecord> records;
    auto it = memtables_.find(clientId);
    if (it == memtables_.end())
    {
        return records;
    }
    for (const auto& pair : it->second)
    {
        OOMMemRecord rec;
        rec.poolType = PoolType::HAL;
        rec.ptr = pair.first.addr;
        rec.memSize = pair.second.size;
        rec.allocTimestamp = pair.second.timestamp;
        rec.cCallStack = pair.second.cCallStack;
        rec.pyCallStack = pair.second.pyCallStack;
        records.push_back(rec);
    }
    return records;
}

}  // namespace MemScope
