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

#include "health_analyzer.h"

#include <iomanip>
#include <iostream>

#include "bit_field.h"
#include "constant.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#include "utility/log.h"

namespace MemScope
{

HealthAnalyzer& HealthAnalyzer::GetInstance()
{
    // 确保依赖的单例先于HealthAnalyzer构造（MemoryStateManager内部已触发EventDispatcher/FileWriteManager），
    // 利用C++静态对象析构逆序规则，使~HealthAnalyzer中LOG宏安全
    MemoryStateManager::GetInstance();
    Utility::Log::GetLog();
    static HealthAnalyzer analyzer;
    return analyzer;
}

HealthAnalyzer::HealthAnalyzer()
    : stepTracker_(std::bind(&HealthAnalyzer::OnStepEnd, this, std::placeholders::_1, std::placeholders::_2),
                   std::bind(&HealthAnalyzer::OnStepStart, this, std::placeholders::_1, std::placeholders::_2))
{
    Subscribe();
}

void HealthAnalyzer::Subscribe()
{
    auto func = std::bind(&HealthAnalyzer::EventHandle, this, std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::FREE, EventBaseType::MSTX,
                                         EventBaseType::SYSTEM};
    EventDispatcher::GetInstance().Subscribe(SubscriberId::HEALTH_ANALYZER, eventList, EventDispatcher::Priority::High,
                                             func);
}

void HealthAnalyzer::UnSubscribe() const { EventDispatcher::GetInstance().UnSubscribe(SubscriberId::HEALTH_ANALYZER); }

bool HealthAnalyzer::IsAnalysisEnable()
{
    // 动态读取当前配置（每个事件只取一次锁），保证分析开关与运行中修改的配置保持一致
    // 与原IsStepInnerAnalysisEnable一致：泄漏分析或OOM分析开启、未开启--steps、malloc和free采集均开启
    const Config& config = GetConfig();
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

void HealthAnalyzer::EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    // 跳过shadow/historical events（EventDispatcher已过滤，防御性保留）
    if (auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event))
    {
        if (memEvent->isShadowEvent)
        {
            return;
        }
    }

    if (!IsAnalysisEnable())
    {
        return;
    }

    if (event->eventType == EventBaseType::MALLOC || event->eventType == EventBaseType::FREE)
    {
        // 仅处理PTA_CACHING/ATB/MINDSPORE池的内存事件
        if (event->poolType != PoolType::PTA_CACHING && event->poolType != PoolType::ATB &&
            event->poolType != PoolType::MINDSPORE)
        {
            return;
        }
        HandleMemEvent(event);
    }
    else if (event->eventType == EventBaseType::MSTX ||
             (event->eventType == EventBaseType::SYSTEM && event->eventSubType == EventSubType::STEP))
    {
        stepTracker_.OnEvent(event);
    }
}

void HealthAnalyzer::HandleMemEvent(std::shared_ptr<EventBase>& event)
{
    const DeviceId& deviceId = event->device;
    const PoolType& poolType = event->poolType;

    auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event);
    if (memEvent == nullptr)
    {
        LOG_WARN("[npu %d]: HealthAnalyzer receive invalid memory event.", deviceId);
        return;
    }

    auto& poolStatusTable = poolStatusTables_[deviceId];
    if (poolStatusTable.find(poolType) == poolStatusTable.end())
    {
        // 原CreateTables语义：设备首次记录（仅首次打日志）
        LOG_INFO("[device %ld]: Start Record npu Memory.", deviceId);
        MemoryPoolStatus memPoolStatus{};
        poolStatusTable.emplace(poolType, memPoolStatus);
    }

    UpdateAllocated(deviceId, poolType, memEvent->used);
    poolStatusTable[poolType].totalAllocated = memEvent->used;
    poolStatusTable[poolType].totalReserved = memEvent->total;
}

void HealthAnalyzer::OnStepEnd(const DeviceId& deviceId, const StepId& stepId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto poolIt = poolStatusTables_.find(deviceId);
    if (poolIt == poolStatusTables_.end())
    {
        return;
    }
    // 原HandleStepMsg逻辑平移：End日志 + 前后内存增减判断（step1不考虑），末尾结算Gap
    for (auto& poolStatus : poolIt->second)
    {
        std::string poolName = GetMemoryPoolName(poolStatus.first);
        int64_t endAllocated = poolStatus.second.totalAllocated;
        LOG_INFO("[npu %ld][step %llu][end]: ------End totalAllocated (%s): %lld------", deviceId, stepId,
                 poolName.c_str(), endAllocated);
        // 当前step起始占用（原stepInfoTables_的stepAllocTable，缺失时为0）
        int64_t startAllocated = stepStartAllocated_[deviceId][poolStatus.first];
        // step1不考虑前后内存不一致
        if (stepId == 1)
        {
            return;
        }
        if (startAllocated == endAllocated)
        {
            LOG_INFO("[npu %ld][step %llu][end]: ------No leaks (%s)------", deviceId, stepId, poolName.c_str());
        }
        else
        {
            LOG_INFO("[npu %ld][step %llu][end]: ------leaks (%s)------", deviceId, stepId, poolName.c_str());
        }
    }
    CheckGap(deviceId);
}

void HealthAnalyzer::OnStepStart(const DeviceId& deviceId, const StepId& stepId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto poolIt = poolStatusTables_.find(deviceId);
    if (poolIt == poolStatusTables_.end())
    {
        return;
    }
    // 原ReceiveMstxMsg/ReceiveStepMsg的start处理平移：Start日志 + 记录当前step起始占用
    for (auto& poolStatus : poolIt->second)
    {
        int64_t startAllocated = poolStatus.second.totalAllocated;
        LOG_INFO("[npu %ld][step %llu][start]: ------Start totalAllocated (%s): %lld------", deviceId, stepId,
                 GetMemoryPoolName(poolStatus.first).c_str(), startAllocated);
        stepStartAllocated_[deviceId][poolStatus.first] = startAllocated;
    }
}

void HealthAnalyzer::UpdateAllocated(const DeviceId& deviceId, const PoolType& poolType, const int64_t& totalAllocated)
{
    // 当step为0和1时，allocated尚未稳定不进行更新
    if (stepTracker_.GetCurrentStep(deviceId) <= skipSteps_)
    {
        return;
    }
    auto& poolStatus = poolStatusTables_[deviceId][poolType];
    // 初值为0，Step开始，第一次更新
    if (poolStatus.stepMaxAllocated == 0)
    {
        poolStatus.stepMaxAllocated = totalAllocated;
        poolStatus.stepMinAllocated = totalAllocated;
        return;
    }
    if (totalAllocated > poolStatus.stepMaxAllocated)
    {
        poolStatus.stepMaxAllocated = totalAllocated;
    }
    if (totalAllocated < poolStatus.stepMinAllocated)
    {
        poolStatus.stepMinAllocated = totalAllocated;
    }
}

void HealthAnalyzer::CheckGap(const DeviceId& deviceId)
{
    auto poolIt = poolStatusTables_.find(deviceId);
    if (poolIt == poolStatusTables_.end())
    {
        return;
    }
    // 当step为0和1时，allocated尚未稳定不进行更新
    const StepId duringStep = stepTracker_.GetCurrentStep(deviceId);
    if (duringStep <= skipSteps_)
    {
        return;
    }
    for (auto& poolStatus : poolIt->second)
    {
        std::string poolName = GetMemoryPoolName(poolStatus.first);
        if (poolStatus.second.stepMaxAllocated == 0)
        {
            LOG_WARN("[npu %d]: %s StepMaxAllocated is 0, please check!", deviceId, poolName.c_str());
            continue;
        }
        double gap = poolStatus.second.stepMinAllocated / static_cast<double>(poolStatus.second.stepMaxAllocated);
        // 第一次计算
        if (poolStatus.second.maxGapInfo.minMaxAllocRatio == 0)
        {
            poolStatus.second.maxGapInfo.gapStepId = duringStep;
            poolStatus.second.maxGapInfo.minMaxAllocRatio = gap;
            poolStatus.second.maxGapInfo.minAllocMemory = poolStatus.second.stepMinAllocated;

            poolStatus.second.minGapInfo.gapStepId = duringStep;
            poolStatus.second.minGapInfo.minMaxAllocRatio = gap;
            poolStatus.second.minGapInfo.minAllocMemory = poolStatus.second.stepMinAllocated;

            // Step结束，还原初始化
            poolStatus.second.stepMaxAllocated = 0;
            poolStatus.second.stepMinAllocated = 0;
            continue;
        }
        // 后续计算查看是否比值变化
        if (gap > poolStatus.second.maxGapInfo.minMaxAllocRatio)
        {
            LOG_WARN("[npu %d]: %s Min/Max Allocated memory largest gap increases to %f, last is %f", deviceId,
                     poolName.c_str(), gap, poolStatus.second.maxGapInfo.minMaxAllocRatio);
            poolStatus.second.maxGapInfo.gapStepId = duringStep;
            poolStatus.second.maxGapInfo.minMaxAllocRatio = gap;
            poolStatus.second.maxGapInfo.minAllocMemory = poolStatus.second.stepMinAllocated;
        }
        if (gap < poolStatus.second.minGapInfo.minMaxAllocRatio)
        {
            LOG_WARN("[npu %d]: %s Min/Max Allocated memory smallest gap decreases to %f, last is %f", deviceId,
                     poolName.c_str(), gap, poolStatus.second.minGapInfo.minMaxAllocRatio);
            poolStatus.second.minGapInfo.gapStepId = duringStep;
            poolStatus.second.minGapInfo.minMaxAllocRatio = gap;
            poolStatus.second.minGapInfo.minAllocMemory = poolStatus.second.stepMinAllocated;
        }
        // Step结束，还原初始化
        poolStatus.second.stepMaxAllocated = 0;
        poolStatus.second.stepMinAllocated = 0;
    }
}

void HealthAnalyzer::ReportGap(const DeviceId& deviceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto poolIt = poolStatusTables_.find(deviceId);
    if (poolIt == poolStatusTables_.end())
    {
        return;
    }
    // 打屏为保持格式统一需调整小数精度，打屏结束后还原
    int currentPrecision = std::cout.precision();
    int outputPrecision = 4;
    int outputWidth = 25;
    for (auto& poolStatus : poolIt->second)
    {
        std::string poolName = GetMemoryPoolName(poolStatus.first);
        std::cout << "======= " << poolName << " Gap Analysis of Device " << deviceId << " =======" << std::endl;
        std::cout << "\t" << std::setw(outputWidth) << std::left << "MinAlloc/MaxAlloc(%)" << std::setw(outputWidth)
                  << std::left << "MinAllocMem(MB)" << std::setw(outputWidth) << std::left << "StepId" << std::endl;
        std::cout << "MinGap\t" << std::fixed << std::setprecision(outputPrecision) << std::setw(outputWidth)
                  << std::left << poolStatus.second.minGapInfo.minMaxAllocRatio * PERCENT_SCALE_FACTOR
                  << std::setw(outputWidth) << std::left
                  << poolStatus.second.minGapInfo.minAllocMemory / static_cast<double>(BYTE_TO_MB)
                  << std::setw(outputWidth) << std::left << poolStatus.second.minGapInfo.gapStepId << std::endl;
        std::cout << "MaxGap\t" << std::setw(outputWidth) << std::left
                  << poolStatus.second.maxGapInfo.minMaxAllocRatio * PERCENT_SCALE_FACTOR << std::setw(outputWidth)
                  << std::left << poolStatus.second.maxGapInfo.minAllocMemory / static_cast<double>(BYTE_TO_MB)
                  << std::setw(outputWidth) << std::left << poolStatus.second.maxGapInfo.gapStepId
                  << std::setprecision(currentPrecision) << std::endl;
    }
}

HealthAnalyzer::~HealthAnalyzer()
{
    UnSubscribe();

    if (!IsAnalysisEnable())
    {
        return;
    }
    // 输出内存波动与模型的权重内存大小
    for (const auto& device : poolStatusTables_)
    {
        ReportGap(device.first);
    }
}

const std::string& HealthAnalyzer::GetMemoryPoolName(const PoolType& poolType)
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

}  // namespace MemScope
