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

#include <memory>
#include "utility/log.h"
#include "bit_field.h"
#include "hal_analyzer.h"

namespace MemScope {

HalAnalyzer& HalAnalyzer::GetInstance(Config config)
{
    static HalAnalyzer analyzer(config);
    return analyzer;
}

HalAnalyzer::HalAnalyzer(Config config)
{
    config_ = config;
    return;
}

bool HalAnalyzer::IsHalAnalysisEnable()
{
    // 确认analysis设置中是否包含泄漏分析
    BitField<decltype(config_.analysisType)> analysisType(config_.analysisType);
    if (!(analysisType.checkBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS)))) {
        return false;
    }
    // 当开启--steps时，关闭所有分析功能
    if (config_.stepList.stepCount!=0) {
        return false;
    }

    // 非默认采集模式，关闭分析功能
    if (config_.collectMode == static_cast<uint8_t>(CollectMode::DEFERRED)) {
        return false;
    }
    
    // 当malloc和free采集并非都开启时，关闭分析功能
    BitField<decltype(config_.eventType)> eventType(config_.eventType);
    if (!(eventType.checkBit(static_cast<size_t>(EventType::ALLOC_EVENT))) ||
        !(eventType.checkBit(static_cast<size_t>(EventType::FREE_EVENT)))) {
        return false;
    }
    return true;
}

bool HalAnalyzer::CreateMemTables(const ClientId &clientId)
{
    if (memtables_.find(clientId) != memtables_.end()) {
        return true;
    }
    LOG_INFO("[client %u]: Start Record hal Memory.", clientId);
    MemoryRecordTable memrecordtable{};
    auto result = memtables_.emplace(clientId, memrecordtable);
    if (result.second) {
        return true;
    }
    return false;
}

void HalAnalyzer::RecordMalloc(const ClientId &clientId, std::shared_ptr<const EventBase> event)
{
    std::shared_ptr<const MemoryEvent> memEvent = std::dynamic_pointer_cast<const MemoryEvent>(event);
    if (memEvent == nullptr) {
        LOG_WARN("[client %u]: HalAnalyzer receive invalid event.", clientId);
        return;
    }
    uint64_t memkey = memEvent->addr;
    // malloc操作需解析当前moduleId
    bool foundModule = false;
    std::string modulename = "INVLID_MOUDLE_ID";
    if (MODULE_HASH_TABLE.find(memEvent->moduleId) != MODULE_HASH_TABLE.end()) {
        modulename = MODULE_HASH_TABLE.find(memEvent->moduleId)->second;
        foundModule = true;
    }
    if (!foundModule) {
        LOG_WARN("[client %u][device: %d]: Malloc operator did not find %d Module in index %u malloc record.",
            clientId, memEvent->device, memEvent->moduleId, memEvent->id);
    }

    if (memtables_[clientId].find(memkey) != memtables_[clientId].end()) {
        if ((memtables_[clientId].find(memkey)->second.addrStatus == AddrStatus::FREE_WAIT)) {
            LOG_WARN(
                "[client %u]: server already has malloc record in addr: 0x%lx ,", clientId, memEvent->addr);
            LOG_WARN("[client %u]: but now malloc again in index: %u, addr: 0x%lx, size: %u, space: %u",
                clientId, memEvent->id, memEvent->addr, memEvent->size, memEvent->space);
        }
    } else {
        HalMemInfo halMemInfo{};
        memtables_[clientId].emplace(memkey, halMemInfo);
    }
    memtables_[clientId][memkey].deviceId = memEvent->device;
    memtables_[clientId][memkey].addrStatus = AddrStatus::FREE_WAIT;
}

void HalAnalyzer::RecordFree(const ClientId &clientId, std::shared_ptr<const EventBase> event)
{
    uint64_t memkey = event->addr;
    auto it = memtables_[clientId].find(memkey);
    if (it != memtables_[clientId].end()) {
        if (it->second.addrStatus == AddrStatus::FREE_WAIT) {
            memtables_[clientId][memkey].addrStatus = AddrStatus::FREE_ALREADY;
        } else {
            LOG_WARN("[client %u]: Double free operator found for malloc operation : addr: 0x%lx",
                clientId, event->addr);
        }
    } else {
            LOG_WARN("[client %u]: No matching malloc operation found for free operator: addr: 0x%lx",
                clientId, event->addr);
    }
}

bool HalAnalyzer::Record(const ClientId &clientId, std::shared_ptr<const EventBase> event)
{
    // 判断是否满足功能开启条件
    if (!IsHalAnalysisEnable()) {
        return true;
    }

    if (!CreateMemTables(clientId)) {
        LOG_ERROR("[client %u]: Create hal Memory table failed.", clientId);
        return false;
    }
    if (event->eventType == EventBaseType::MALLOC) {
        RecordMalloc(clientId, event);
        return true;
    } else if (event->eventType == EventBaseType::FREE) {
        RecordFree(clientId, event);
        return true;
    }
    return false;
}

void HalAnalyzer::CheckLeak(const size_t clientId)
{
    bool foundLeaks = false;
    if (memtables_.find(clientId) != memtables_.end()) {
        for (const auto& pair :memtables_[clientId]) {
            if (pair.second.addrStatus != AddrStatus::FREE_ALREADY) {
                foundLeaks = true;
                LOG_WARN("[client %u]: Leak memory in Malloc operator, addr: 0x%lx", clientId, pair.first);
            }
        }
    }
    if (!foundLeaks) {
        LOG_INFO("[client %u]: There is no hal leak memory.", clientId);
    }
}

void HalAnalyzer::LeakAnalyze()
{
    if (!IsHalAnalysisEnable()) {
        return;
    }

    if (memtables_.empty()) {
        LOG_ERROR("No memory records available.");
    } else {
        for (const auto& pair :memtables_) {
            CheckLeak(pair.first);
        }
    }

    return;
}

HalAnalyzer::~HalAnalyzer()
{
    try {
        LeakAnalyze();
    } catch (const std::exception &ex) {
        std::cerr << "HalAnalyzer destructor catch exception: " << ex.what();
    }
}

}