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

#include "leak_analyzer.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

#include "bit_field.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#include "utility/log.h"
#include "utility/utils.h"

namespace MemScope
{

LeakAnalyzer& LeakAnalyzer::GetInstance()
{
    // 确保依赖的单例先于LeakAnalyzer构造（MemoryStateManager内部已触发EventDispatcher/FileWriteManager），
    // 利用C++静态对象析构逆序规则，使~LeakAnalyzer中QueryLiveBlocks与LOG宏安全
    MemoryStateManager::GetInstance();
    Utility::Log::GetLog();
    static LeakAnalyzer analyzer;
    return analyzer;
}

LeakAnalyzer::LeakAnalyzer()
    : stepTracker_(std::bind(&LeakAnalyzer::CheckNpuLeak, this, std::placeholders::_1, std::placeholders::_2))
{
    Subscribe();
}

void LeakAnalyzer::Subscribe()
{
    auto func = std::bind(&LeakAnalyzer::EventHandle, this, std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::FREE, EventBaseType::MSTX,
                                         EventBaseType::SYSTEM, EventBaseType::CLEAN_UP};
    EventDispatcher::GetInstance().Subscribe(SubscriberId::LEAKS_ANALYZER, eventList, EventDispatcher::Priority::High,
                                             func);
}

void LeakAnalyzer::UnSubscribe() const { EventDispatcher::GetInstance().UnSubscribe(SubscriberId::LEAKS_ANALYZER); }

bool LeakAnalyzer::IsNpuAnalysisEnable()
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

    // 当malloc和free采集并非都开启时，关闭分析功能
    BitField<decltype(config.eventType)> eventType(config.eventType);
    if (!(eventType.checkBit(static_cast<size_t>(EventType::ALLOC_EVENT))) ||
        !(eventType.checkBit(static_cast<size_t>(EventType::FREE_EVENT))))
    {
        return false;
    }
    return true;
}

bool LeakAnalyzer::IsHalAnalysisEnable()
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

void LeakAnalyzer::EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    // 跳过shadow/historical events（EventDispatcher已过滤，防御性保留）
    if (auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event))
    {
        if (memEvent->isShadowEvent)
        {
            return;
        }
    }

    if (event->eventType == EventBaseType::MALLOC || event->eventType == EventBaseType::FREE)
    {
        if (event->poolType == PoolType::PTA_CACHING || event->poolType == PoolType::ATB ||
            event->poolType == PoolType::MINDSPORE)
        {
            if (IsNpuAnalysisEnable())
            {
                HandleNpuMemEvent(event, state);
            }
        }
        else if (event->poolType == PoolType::HAL)
        {
            // 与原HalAnalyzer一致：device无效的HAL事件（halPtrs_查表失败回退）不处理
            if (IsHalAnalysisEnable() && event->device != GD_INVALID_NUM)
            {
                HandleHalMemEvent(event, state);
            }
        }
    }
    else if (event->eventType == EventBaseType::MSTX ||
             (event->eventType == EventBaseType::SYSTEM && event->eventSubType == EventSubType::STEP))
    {
        if (IsNpuAnalysisEnable())
        {
            stepTracker_.OnEvent(event);
        }
    }
    else if (event->eventType == EventBaseType::CLEAN_UP && event->eventSubType == EventSubType::RESIDUAL_BLOCK)
    {
        HandleResidualBlock(event, state);
    }
}

void LeakAnalyzer::HandleNpuMemEvent(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    const DeviceId& deviceId = event->device;
    const ClientId& clientId = event->pid;

    auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event);
    if (memEvent == nullptr)
    {
        LOG_WARN("[npu %d][client %u]: LeakAnalyzer receive invalid memory event.", deviceId, clientId);
        return;
    }

    if (event->eventType == EventBaseType::FREE)
    {
        // free无匹配告警：MSM对无匹配FREE新建仅含当前FREE事件的幽灵state
        if (IsGhostState(state, event))
        {
            std::string poolName = GetMemoryPoolName(event->poolType);
            LOG_WARN("[npu%d free][client %u]:!!! ------free error in %s------!!!, ptr: %llu", deviceId, clientId,
                     poolName.c_str(), event->addr);
        }
    }
    // MALLOC：double malloc告警由CLEAN_UP(RESIDUAL_BLOCK)路径还原，此处无表维护
}

void LeakAnalyzer::HandleHalMemEvent(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    const ClientId& clientId = event->pid;

    auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event);
    if (memEvent == nullptr)
    {
        LOG_WARN("[client %u]: LeakAnalyzer receive invalid event.", clientId);
        return;
    }

    // 原CreateMemTables语义：首次见到该client记录（不维护块表，仅跟踪client集合用于析构分组检查）
    if (halClients_.insert(clientId).second)
    {
        LOG_INFO("[client %u]: Start Record hal Memory.", clientId);
    }

    if (event->eventType == EventBaseType::MALLOC)
    {
        // moduleId解析检查（原RecordMalloc行为，不维护块表）
        if (MODULE_HASH_TABLE.find(memEvent->moduleId) == MODULE_HASH_TABLE.end())
        {
            LOG_WARN("[client %u][device: %d]: Malloc operator did not find %d Module in index %u malloc record.",
                     clientId, memEvent->device, memEvent->moduleId, memEvent->id);
        }
        // double malloc告警由CLEAN_UP(RESIDUAL_BLOCK)路径还原
    }
    else if (event->eventType == EventBaseType::FREE)
    {
        // free无匹配告警（原RecordFree语义）
        if (IsGhostState(state, event))
        {
            LOG_WARN("[client %u]: No matching malloc operation found for free operator: addr: 0x%lx", clientId,
                     event->addr);
        }
    }
}

void LeakAnalyzer::HandleResidualBlock(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    const uint64_t& ptr = event->addr;
    const ClientId& clientId = event->pid;

    if (event->poolType == PoolType::PTA_CACHING || event->poolType == PoolType::ATB ||
        event->poolType == PoolType::MINDSPORE)
    {
        if (!IsNpuAnalysisEnable())
        {
            return;
        }
        // 原RecordNpuMalloc的double malloc告警文案
        std::string poolName = GetMemoryPoolName(event->poolType);
        LOG_WARN("[npu%d malloc][client %u]:!!! ------double malloc in %s------!!!, ptr: %llu", event->device, clientId,
                 poolName.c_str(), ptr);
    }
    else if (event->poolType == PoolType::HAL)
    {
        if (!IsHalAnalysisEnable() || event->device == GD_INVALID_NUM)
        {
            return;
        }
        // 原RecordMalloc的double malloc告警文案；冲突块信息来自已存在的旧块
        // （MALLOC冲突时新事件的id/size/space经事件路由不可达，与原文案字段来源有差异）
        LOG_WARN("[client %u]: server already has malloc record in addr: 0x%lx ,", clientId, ptr);
        if (state != nullptr && !state->events.empty() && state->events[0]->eventType == EventBaseType::MALLOC)
        {
            LOG_WARN("[client %u]: but now malloc again in index: %llu, addr: 0x%lx, size: %lld, space: %u", clientId,
                     state->events[0]->id, state->events[0]->addr, state->events[0]->size, state->events[0]->space);
        }
    }
}

bool LeakAnalyzer::IsGhostState(MemoryState* state, const std::shared_ptr<EventBase>& event) const
{
    // FREE事件无匹配块时MSM新建仅含当前FREE事件的幽灵state，据此判定free无匹配
    if (state == nullptr || state->events.size() != 1)
    {
        return false;
    }
    if (state->events[0]->id != event->id)
    {
        return false;
    }
    return state->events[0]->eventType == EventBaseType::FREE;
}

uint64_t LeakAnalyzer::GetAllocStep(const std::vector<StepBoundary>& boundaries, const uint64_t& allocTs,
                                    const uint64_t& allocId) const
{
    // 找最后一个边界满足 (b.timestamp, b.eventId) <= (allocTs, allocId)：
    // 块分配时刻的当前step即分配step；首个边界前分配归step 0（SkipCheck跳过）
    uint64_t allocStep = 0;
    for (const auto& b : boundaries)
    {
        if (b.timestamp < allocTs || (b.timestamp == allocTs && b.eventId <= allocId))
        {
            allocStep = b.stepId;
        }
        else
        {
            break;  // 边界按(timestamp, eventId)升序
        }
    }
    return allocStep;
}

void LeakAnalyzer::CheckNpuLeak(const DeviceId& deviceId, const StepId& stepId)
{
    // 条件查询MSM存活块（含SHADOW_PROMOTED转正块，与原表语义差异见RFC行为对照）
    LiveBlockFilter filter;
    filter.poolTypes = {PoolType::PTA_CACHING, PoolType::ATB, PoolType::MINDSPORE};
    filter.device = deviceId;
    auto blocks = MemoryStateManager::GetInstance().QueryLiveBlocks(filter);
    const auto& boundaries = stepTracker_.GetStepBoundaries(deviceId);

    for (const auto& blk : blocks)
    {
        const uint64_t allocStep = GetAllocStep(boundaries, blk.allocTimestamp, blk.allocEventId);
        // stepId为0和1，即step 1及之前申请的内存，风险低暂不告警
        if (allocStep <= skipSteps_)
        {
            continue;
        }
        // duration = 检查时的step - 分配时的step（原AddDuration逐step累加语义等价）
        if (stepId <= allocStep || stepId - allocStep < durationThreshold_)
        {
            continue;
        }

        std::string memoryPoolName = GetMemoryPoolName(blk.poolType);
        LOG_WARN("[npu %d][step %lu]: ptr: %llx has last for %lu steps. Please check if there is leaks in %s.",
                 deviceId, stepId, blk.addr, stepId - allocStep, memoryPoolName.c_str());

        // 泄漏候选集合：命中后不随FREE删除（累计告警语义）
        LeakMemKey key{blk.addr, blk.poolType, allocStep};
        auto& table = leakMemSums_[deviceId];
        auto it = table.find(key);
        if (it == table.end())
        {
            table.emplace(key, LeakInfo{});
        }
        table[key].kernelIndex = blk.kernelIndex;
        table[key].leakSize = static_cast<int64_t>(blk.size);
    }
}

void LeakAnalyzer::ReportLeak(const DeviceId& deviceId)
{
    std::cout << "====== ERROR: Detected memory leaks on device " << deviceId << " ======" << std::endl;

    // 依照step排序
    std::vector<std::pair<LeakMemKey, LeakInfo>> leakVec(leakMemSums_[deviceId].begin(), leakMemSums_[deviceId].end());
    auto leakCompare = [](const std::pair<LeakMemKey, LeakInfo>& a, const std::pair<LeakMemKey, LeakInfo>& b)
    { return a.first.leakStepId < b.first.leakStepId; };
    std::sort(leakVec.begin(), leakVec.end(), leakCompare);
    // 输出泄漏信息总结
    uint64_t leakInfoCounts = 0;
    long double leakSizeSums = 0;
    for (const auto& pair : leakVec)
    {
        const std::string poolName = GetMemoryPoolName(pair.first.type);
        printf("Direct %s leak of %f Mb(s) at 0x%lx in kernel_%lu at step %lu.\n", poolName.c_str(),
               (pair.second.leakSize / static_cast<double>(BYTE_TO_MB)), pair.first.ptr, pair.second.kernelIndex,
               pair.first.leakStepId);
        leakInfoCounts++;
        long double leakTempSize = static_cast<long double>(pair.second.leakSize);
        leakSizeSums = Utility::GetAddResult(leakTempSize, leakSizeSums);
    }
    std::cout << "====== SUMMARY: " << leakSizeSums / BYTE_TO_MB << " Mb(s) leaked in " << leakInfoCounts
              << " allocation(s) ======" << std::endl;
}

void LeakAnalyzer::CheckHalLeak()
{
    if (halClients_.empty())
    {
        LOG_ERROR("No memory records available.");
        return;
    }
    for (const auto& clientId : halClients_)
    {
        LiveBlockFilter filter;
        filter.poolTypes = {PoolType::HAL};
        filter.pid = static_cast<uint64_t>(clientId);
        auto blocks = MemoryStateManager::GetInstance().QueryLiveBlocks(filter);
        if (blocks.empty())
        {
            LOG_INFO("[client %u]: There is no hal leak memory.", clientId);
            continue;
        }
        for (const auto& blk : blocks)
        {
            // 表内条目均为未释放的存活块，全部视为泄漏
            LOG_WARN("[client %u][device: %d]: Leak memory in Malloc operator, addr: 0x%lx", clientId, blk.device,
                     blk.addr);
        }
    }
}

LeakAnalyzer::~LeakAnalyzer()
{
    UnSubscribe();

    // 原输出顺序为：NPU泄漏汇总（StepInnerAnalyzer析构）→ Gap统计（StepInnerAnalyzer析构）→
    // HAL泄漏检查（HalAnalyzer析构）；合并后NPU与HAL在此输出，Gap统计由HealthAnalyzer析构输出，
    // 输出顺序调整为：NPU泄漏汇总 → HAL泄漏检查 → Gap统计（顺序差异见RFC行为对照）
    if (IsNpuAnalysisEnable() && !leakMemSums_.empty())
    {
        for (const auto& pair : leakMemSums_)
        {
            ReportLeak(pair.first);
        }
    }
    if (IsHalAnalysisEnable())
    {
        CheckHalLeak();
    }
}

std::vector<OOMMemRecord> LeakAnalyzer::QueryUnfreedRecords(int32_t deviceId, uint32_t clientId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OOMMemRecord> records;

    // NPU池：按device过滤（原StepInnerAnalyzer::QueryUnfreedRecords语义）
    LiveBlockFilter npuFilter;
    npuFilter.poolTypes = {PoolType::PTA_CACHING, PoolType::ATB, PoolType::MINDSPORE};
    npuFilter.device = deviceId;
    for (const auto& blk : MemoryStateManager::GetInstance().QueryLiveBlocks(npuFilter))
    {
        OOMMemRecord rec;
        rec.poolType = blk.poolType;
        rec.ptr = blk.addr;
        rec.memSize = static_cast<int64_t>(blk.size);
        rec.allocTimestamp = blk.allocTimestamp;
        rec.cCallStack = blk.cCallStack;
        rec.pyCallStack = blk.pyCallStack;
        records.push_back(rec);
    }

    // HAL池：按clientId(pid)过滤（原HalAnalyzer::QueryUnfreedRecords语义）
    LiveBlockFilter halFilter;
    halFilter.poolTypes = {PoolType::HAL};
    halFilter.pid = static_cast<uint64_t>(clientId);
    for (const auto& blk : MemoryStateManager::GetInstance().QueryLiveBlocks(halFilter))
    {
        OOMMemRecord rec;
        rec.poolType = blk.poolType;
        rec.ptr = blk.addr;
        rec.memSize = static_cast<int64_t>(blk.size);
        rec.allocTimestamp = blk.allocTimestamp;
        rec.cCallStack = blk.cCallStack;
        rec.pyCallStack = blk.pyCallStack;
        records.push_back(rec);
    }
    return records;
}

const std::string& LeakAnalyzer::GetMemoryPoolName(const PoolType& poolType)
{
    auto it = PoolTypeToString.find(poolType);
    if (it != PoolTypeToString.end())
    {
        return it->second;
    }
    static const std::string EMPTY_STRING;
    LOG_ERROR("Undefined memorypool type!");
    return EMPTY_STRING;
}

std::size_t LeakMemKeyHash::operator()(const LeakMemKey& leakKey) const
{
    return std::hash<uint64_t>()(leakKey.ptr) ^ std::hash<uint64_t>()(leakKey.leakStepId) ^
           std::hash<PoolType>()(leakKey.type);
}

bool LeakMemKey::operator==(const LeakMemKey& other) const
{
    return ptr == other.ptr && leakStepId == other.leakStepId && type == other.type;
}

}  // namespace MemScope
