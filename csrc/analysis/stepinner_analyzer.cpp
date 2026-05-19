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

#include "stepinner_analyzer.h"
#include <cstring>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include "mstx_analyzer.h"
#include "py_step_manager.h"
#include "utility/log.h"
#include "utility/utils.h"
#include "config_info.h"
#include "bit_field.h"

namespace MemScope {

StepInnerAnalyzer &StepInnerAnalyzer::GetInstance(Config config)
{
    static StepInnerAnalyzer analyzer(config);
    return analyzer;
}

StepInnerAnalyzer::StepInnerAnalyzer(Config config)
{
    config_ = config;

    // 注册mstx信息观察者
    auto mstxFunc = std::bind(&StepInnerAnalyzer::ReceiveMstxMsg, this, std::placeholders::_1);
    MstxAnalyzer::Instance().Subscribe(MstxEventSubscriber::STEP_INNER_ANALYZER, mstxFunc);
    // 注册pyStep信息观察者
    auto pyStepFunc = std::bind(&StepInnerAnalyzer::ReceiveStepMsg, this, std::placeholders::_1);
    PyStepManager::Instance().Subscribe(PyStepEventSubscriber::STEP_INNER_ANALYZER, pyStepFunc);

    return;
}

bool StepInnerAnalyzer::CreateTables(const DeviceId &deviceId)
{
    if (npuMemUsages_.find(deviceId) != npuMemUsages_.end()) {
        return true;
    }
    NpuMemUsage npumemusage{};
    LOG_INFO("[device %ld]: Start Record npu Memory.", deviceId);
    auto result = npuMemUsages_.emplace(deviceId, npumemusage);
    if (result.second) {
        return true;
    }
    return false;
}

bool StepInnerAnalyzer::CreateStepInfoTables(const DeviceId &deviceId)
{
    if (stepInfoTables_.find(deviceId) != stepInfoTables_.end()) {
        return true;
    }
    LOG_INFO("[device %ld]: Start Record step info.", deviceId);
    StepInfoTable stepInfoTable{};
    auto result = stepInfoTables_.emplace(deviceId, stepInfoTable);
    if (result.second) {
        return true;
    }
    return false;
}

bool StepInnerAnalyzer::CreateLeakSumTables(const DeviceId &deviceId)
{
    if (leakMemSums_.find(deviceId) != leakMemSums_.end()) {
        return true;
    }
    LOG_INFO("[device %ld]: Start Record leak memory info.", deviceId);
    LeakSumsTable leaksumstable{};
    auto result = leakMemSums_.emplace(deviceId, leaksumstable);
    if (result.second) {
        return true;
    }
    return false;
}

bool StepInnerAnalyzer::IsStepInnerAnalysisEnable()
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

    // 当malloc和free采集并非都开启时，关闭分析功能
    BitField<decltype(config_.eventType)> eventType(config_.eventType);
    if (!(eventType.checkBit(static_cast<size_t>(EventType::ALLOC_EVENT))) ||
        !(eventType.checkBit(static_cast<size_t>(EventType::FREE_EVENT)))) {
        return false;
    }
    return true;
}
    
std::size_t LeakMemKeyHash::operator()(const LeakMemKey &leakKey) const
{
    return std::hash<uint64_t>()(leakKey.ptr) ^ std::hash<uint64_t>()(leakKey.leakStepId)
        ^ std::hash<PoolType>()(leakKey.type);
}

bool LeakMemKey::operator==(const LeakMemKey& other) const
{
    return ptr == other.ptr && leakStepId == other.leakStepId && type == other.type;
}

std::size_t NpuMemKeyHash::operator()(const NpuMemKey &memKey) const
{
    return std::hash<uint64_t>()(memKey.ptr) ^ std::hash<PoolType>()(memKey.memType);
}

bool NpuMemKey::operator==(const NpuMemKey& other) const
{
    return ptr == other.ptr && memType == other.memType;
}

void StepInnerAnalyzer::SetStepId(const DeviceId &deviceId, const uint64_t &stepId)
{
    npuMemUsages_[deviceId].duringStep = stepId;
}

bool StepInnerAnalyzer::SkipCheck(const NpuMemInfo &npuMemInfo) const
{
    // stepId为0和1，即step 1及之前申请的内存，风险低暂不告警
    if (npuMemInfo.stepId <= skipSteps_) {
        return true;
    }
    return false;
}

const std::string& StepInnerAnalyzer::GetMemoryPoolName(const PoolType &poolType)
{
    auto it = PoolTypeToString.find(poolType);
    if (it != PoolTypeToString.end()) {
        return it->second;
    } else {
        static const std::string EMPTY_STRING;
        LOG_ERROR("Undefined memorypool type!");
        return EMPTY_STRING;
    }
}

void StepInnerAnalyzer::CheckNpuLeak(const DeviceId &deviceId, const uint64_t stepId)
{
    for (const auto& pair :npuMemUsages_[deviceId].poolOpTable) {
        if (SkipCheck(pair.second)) {
            continue;
        }
        if (pair.second.duration < durationThreshold_) {
            continue;
        }

        std::string memoryPoolName = GetMemoryPoolName(pair.second.type);
        LOG_WARN(
            "[npu %d][step %lu]: ptr: %llx has last for %lu steps. Please check if there is leaks in %s.",
            deviceId, stepId, pair.first.ptr, pair.second.duration, memoryPoolName.c_str());
        if (!CreateLeakSumTables(deviceId)) {
            LOG_ERROR("[device %ld]: Create leaksums table failed.", deviceId);
            return;
        }
        if (leakMemSums_[deviceId].find(LeakMemKey(pair.first.ptr, pair.first.memType, pair.second.stepId)) ==
            leakMemSums_[deviceId].end()) {
            LeakInfo leakInfo{};
            leakMemSums_[deviceId].emplace(LeakMemKey(pair.first.ptr, pair.first.memType, pair.second.stepId),
                leakInfo);
        }

        leakMemSums_[deviceId][LeakMemKey(pair.first.ptr, pair.first.memType, pair.second.stepId)].kernelIndex =
            pair.second.kernelIndex;
        leakMemSums_[deviceId][LeakMemKey(pair.first.ptr, pair.first.memType, pair.second.stepId)].leakSize =
            npuMemUsages_[deviceId].poolOpTable[pair.first].memSize;
    }
    return;
}

void StepInnerAnalyzer::UpdateAllocated(const DeviceId &deviceId, const PoolType &poolType,
    const int64_t &totalAllocated)
{
    if (!IsStepInnerAnalysisEnable()) {
        return;
    }
    // 当step为0和1时，allocated尚未稳定不进行更新
    if (npuMemUsages_[deviceId].duringStep <= skipSteps_) {
        return;
    }
    // 初值为0，Step开始，第一次更新
    if (npuMemUsages_[deviceId].poolStatusTable[poolType].stepMaxAllocated == 0) {
        npuMemUsages_[deviceId].poolStatusTable[poolType].stepMaxAllocated = totalAllocated;
        npuMemUsages_[deviceId].poolStatusTable[poolType].stepMinAllocated = totalAllocated;
        return;
    }
    if (totalAllocated > npuMemUsages_[deviceId].poolStatusTable[poolType].stepMaxAllocated) {
        npuMemUsages_[deviceId].poolStatusTable[poolType].stepMaxAllocated = totalAllocated;
    }
    if (totalAllocated < npuMemUsages_[deviceId].poolStatusTable[poolType].stepMinAllocated) {
        npuMemUsages_[deviceId].poolStatusTable[poolType].stepMinAllocated = totalAllocated;
    }
    return;
}

void StepInnerAnalyzer::CheckGap(const DeviceId &deviceId)
{
    // 当step为0和1时，allocated尚未稳定不进行更新
    if (npuMemUsages_[deviceId].duringStep <= skipSteps_) {
        return;
    }
    for (auto &poolStatus : npuMemUsages_[deviceId].poolStatusTable) {
        std::string poolName = GetMemoryPoolName(poolStatus.first);
        if (poolStatus.second.stepMaxAllocated == 0) {
            LOG_WARN("[npu %d]: %s StepMaxAllocated is 0, please check!", deviceId, poolName.c_str());
            continue;
        }
        double gap =
        poolStatus.second.stepMinAllocated / static_cast<double>(poolStatus.second.stepMaxAllocated);
        // 第一次计算
        if (poolStatus.second.maxGapInfo.minMaxAllocRatio == 0) {
            poolStatus.second.maxGapInfo.gapStepId = npuMemUsages_[deviceId].duringStep;
            poolStatus.second.maxGapInfo.minMaxAllocRatio = gap;
            poolStatus.second.maxGapInfo.minAllocMemory = poolStatus.second.stepMinAllocated;

            poolStatus.second.minGapInfo.gapStepId = npuMemUsages_[deviceId].duringStep;
            poolStatus.second.minGapInfo.minMaxAllocRatio = gap;
            poolStatus.second.minGapInfo.minAllocMemory = poolStatus.second.stepMinAllocated;

            // Step结束，还原初始化
            poolStatus.second.stepMaxAllocated = 0;
            poolStatus.second.stepMinAllocated = 0;
            continue;
        }
        // 后续计算查看是否比值变化
        if (gap > poolStatus.second.maxGapInfo.minMaxAllocRatio) {
            LOG_WARN(
                "[npu %d]: %s Min/Max Allocated memory largest gap increases to %f, last is %f",
                deviceId, poolName.c_str(), gap, poolStatus.second.maxGapInfo.minMaxAllocRatio);
            poolStatus.second.maxGapInfo.gapStepId = npuMemUsages_[deviceId].duringStep;
            poolStatus.second.maxGapInfo.minMaxAllocRatio = gap;
            poolStatus.second.maxGapInfo.minAllocMemory = poolStatus.second.stepMinAllocated;
        }
        if (gap < poolStatus.second.minGapInfo.minMaxAllocRatio) {
            LOG_WARN(
                "[npu %d]: %s Min/Max Allocated memory smallest gap decreases to %f, last is %f",
                deviceId, poolName.c_str(), gap, poolStatus.second.minGapInfo.minMaxAllocRatio);
            poolStatus.second.minGapInfo.gapStepId = npuMemUsages_[deviceId].duringStep;
            poolStatus.second.minGapInfo.minMaxAllocRatio = gap;
            poolStatus.second.minGapInfo.minAllocMemory = poolStatus.second.stepMinAllocated;
        }
        // Step结束，还原初始化
        poolStatus.second.stepMaxAllocated = 0;
        poolStatus.second.stepMinAllocated = 0;
    }
    return;
}

void StepInnerAnalyzer::RecordNpuMalloc(const ClientId &clientId, const DeviceId &deviceId,
    std::shared_ptr<const MemoryEvent> memEvent)
{
    const uint64_t& npumemptr = memEvent->addr;
    const PoolType& poolType = memEvent->poolType;
    if ((npuMemUsages_[deviceId].poolOpTable.find(NpuMemKey(npumemptr, poolType))
        != npuMemUsages_[deviceId].poolOpTable.end())) {
        std::string poolName = GetMemoryPoolName(poolType);
        LOG_WARN(
            "[npu%d malloc][client %u]:!!! ------double malloc in %s------!!!, ptr: %llu",
                deviceId, clientId, poolName.c_str(), npumemptr);
    }

    // 不同内存池建立各自的占用表
    if ((npuMemUsages_[deviceId].poolStatusTable.find(memEvent->poolType) ==
        npuMemUsages_[deviceId].poolStatusTable.end())) {
        MemoryPoolStatus memPoolStatus{};
        npuMemUsages_[deviceId].poolStatusTable.emplace(poolType, memPoolStatus);
    }

    NpuMemInfo npuMemInfo = {
    poolType, memEvent->size, memEvent->timestamp, 0, npuMemUsages_[deviceId].duringStep,
    memEvent->kernelIndex};
    npuMemUsages_[deviceId].poolOpTable.emplace(NpuMemKey(npumemptr, poolType), npuMemInfo);
    UpdateAllocated(deviceId, poolType, memEvent->used);
    npuMemUsages_[deviceId].poolStatusTable[poolType].totalAllocated = memEvent->used;
    npuMemUsages_[deviceId].poolStatusTable[poolType].totalReserved = memEvent->total;
    return;
}

void  StepInnerAnalyzer::RecordNpuFree(const ClientId &clientId, const DeviceId &deviceId,
    std::shared_ptr<const MemoryEvent> memEvent)
{
    uint64_t npumemptr = memEvent->addr;
    const PoolType& poolType = memEvent->poolType;
    if ((npuMemUsages_[deviceId].poolOpTable.find(NpuMemKey(npumemptr, poolType))
        == npuMemUsages_[deviceId].poolOpTable.end())) {
        std::string poolName = GetMemoryPoolName(poolType);
        LOG_WARN(
            "[npu%d free][client %u]:!!! ------free error in %s------!!!, ptr: %llu",
                deviceId, clientId, poolName.c_str(), npumemptr);
    }

    // 不同内存池建立各自的占用表
    if ((npuMemUsages_[deviceId].poolStatusTable.find(poolType) ==
        npuMemUsages_[deviceId].poolStatusTable.end())) {
        MemoryPoolStatus memPoolStatus{};
        npuMemUsages_[deviceId].poolStatusTable.emplace(poolType, memPoolStatus);
    }

    npuMemUsages_[deviceId].poolOpTable.erase(NpuMemKey(npumemptr, poolType));
    UpdateAllocated(deviceId, poolType, memEvent->used);
    npuMemUsages_[deviceId].poolStatusTable[poolType].totalAllocated = memEvent->used;
    npuMemUsages_[deviceId].poolStatusTable[poolType].totalReserved = memEvent->total;
    return;
}

void StepInnerAnalyzer::AddDuration(const DeviceId &deviceId)
{
    if (npuMemUsages_.find(deviceId) == npuMemUsages_.end()) {
        LOG_ERROR("[device %ld]: No npu memorypool record!", deviceId);
        return;
    }
    for (auto& pair :npuMemUsages_[deviceId].poolOpTable) {
        pair.second.duration += 1;
    }
    return;
}

bool StepInnerAnalyzer::Record(const ClientId &clientId, std::shared_ptr<const EventBase> event)
{
    // 当开启--steps时，关闭所有step内分析功能
    if (!IsStepInnerAnalysisEnable()) {
        return true;
    }
    std::shared_ptr<const MemoryEvent> memEvent = std::dynamic_pointer_cast<const MemoryEvent>(event);
    if (memEvent == nullptr) {
        LOG_WARN("[client %u]: StepInnerAnalyzer receive invalid event.", clientId);
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    DeviceId deviceId = memEvent->device;
    if (!CreateTables(deviceId)) {
        LOG_ERROR("[device %ld]: Create npu Memory table failed.", deviceId);
        return false;
    }
    // 目前不处理BLOCK_FREE操作
    if (memEvent->eventType == EventBaseType::MALLOC) {
        RecordNpuMalloc(clientId, deviceId, memEvent);
    } else if (memEvent->eventType == EventBaseType::FREE) {
        RecordNpuFree(clientId, deviceId, memEvent);
    }
    return true;
}

void StepInnerAnalyzer::UpdateStepInfoTable(const DeviceId &deviceId, const uint64_t &stepId,
    const PoolType &poolType, const int64_t &startAllocated)
{
    if (stepInfoTables_[deviceId].find(stepId) == stepInfoTables_[deviceId].end()) {
        StepInfo stepInfo{};
        stepInfoTables_[deviceId].emplace(stepId, stepInfo);
    }
    if (stepInfoTables_[deviceId][stepId].stepAllocTable.find(poolType) ==
        stepInfoTables_[deviceId][stepId].stepAllocTable.end()) {
        stepInfoTables_[deviceId][stepId].stepAllocTable.emplace(poolType, startAllocated);
    } else {
        stepInfoTables_[deviceId][stepId].stepAllocTable[poolType] = startAllocated;
    }
    return;
}

void StepInnerAnalyzer::HandleStepMsg(const DeviceId &deviceId, const uint64_t &stepId)
{
    for (auto &poolStatus : npuMemUsages_[deviceId].poolStatusTable) {
        std::string poolName = GetMemoryPoolName(poolStatus.first);
        int64_t endAllocated = poolStatus.second.totalAllocated;
        LOG_INFO("[npu %ld][step %llu][end]: ------End totalAllocated (%s): %lld------",
            deviceId, stepId, poolName.c_str(), endAllocated);
        int64_t startAllocated = stepInfoTables_[deviceId][stepId].stepAllocTable[poolStatus.first];
        // step1不考虑前后内存不一致
        if (stepId == 1) {
            return;
        }
        if (startAllocated == endAllocated) {
            LOG_INFO("[npu %ld][step %llu][end]: ------No leaks (%s)------", deviceId, stepId, poolName.c_str());
        } else {
            LOG_INFO("[npu %ld][step %llu][end]: ------leaks (%s)------", deviceId, stepId, poolName.c_str());
        }
    }
    CheckNpuLeak(deviceId, stepId);
    CheckGap(deviceId);
}

void StepInnerAnalyzer::ReceiveMstxMsg(std::shared_ptr<const MstxEvent> mstxEvent)
{
    auto deviceId = mstxEvent->device;
    auto stepId = mstxEvent->stepId;
    if (!IsStepInnerAnalysisEnable()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    EventSubType markType = mstxEvent->eventSubType;
    if (!CreateStepInfoTables(deviceId) || !CreateTables(deviceId)) {
        LOG_WARN("[device %ld]: Create StepInfo table failed.", deviceId);
        return;
    }

    if (markType == EventSubType::MSTX_RANGE_START) {
        if (strcmp(mstxEvent->name.c_str(), "step start") != 0) {
            return;
        }
        // 判断是否使用mstx信息源
        StepSource crtSource = crtStepSource_.load(std::memory_order_acquire);
        if (crtSource == StepSource::PY_STEP_SOURCE) {
            LOG_ERROR("[device %ld]: 'Mstx' and 'msmemcope.step()' cannot be used simultaneously to update the step.",
                deviceId);
            return;
        }
        crtStepSource_.store(StepSource::MSTX_SOURCE, std::memory_order_release);
        for (auto &poolStatus : npuMemUsages_[deviceId].poolStatusTable) {
            int64_t startAllocated = poolStatus.second.totalAllocated;
            LOG_INFO("[npu %ld][step %llu][start]: ------Start totalAllocated (%s): %lld------",
                deviceId, stepId, GetMemoryPoolName(poolStatus.first).c_str(), startAllocated);
            UpdateStepInfoTable(deviceId, stepId, poolStatus.first, startAllocated);
            stepInfoTables_[deviceId][stepId].rangeId = mstxEvent->rangeId;
        }
        SetStepId(deviceId, stepId);
        AddDuration(deviceId);
    } else if (markType == EventSubType::MSTX_RANGE_END) {
        // 如果是end看stepid和rangeid是否在table中
        if (stepInfoTables_[deviceId].find(stepId) == stepInfoTables_[deviceId].end() ||
            stepInfoTables_[deviceId][stepId].rangeId != mstxEvent->rangeId) {
            return;
        }
        HandleStepMsg(deviceId, stepId);
    }
    return;
}

void StepInnerAnalyzer::ReceiveStepMsg(std::shared_ptr<const EventBase> event)
{
    auto deviceId = event->device;
    auto stepId = std::stoull(event->name);
    if (!IsStepInnerAnalysisEnable()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CreateStepInfoTables(deviceId) || !CreateTables(deviceId)) {
        LOG_WARN("[device %ld]: Create StepInfo table failed.", deviceId);
        return;
    }
    // 判断是否使用python信息源
    StepSource crtSource = crtStepSource_.load(std::memory_order_acquire);
    if (crtSource == StepSource::MSTX_SOURCE) {
        LOG_ERROR("[device %ld]: 'msmemcope.step()' and 'Mstx' cannot be used simultaneously to update the step.",
            deviceId);
        return;
    }
    crtStepSource_.store(StepSource::PY_STEP_SOURCE, std::memory_order_release);

    // msmemcope.step()同时标识了上一个step的结束和(step+1)的开始，这里先处理上一个step结束，检测泄漏与gap
    // 如果是第一次msmemcope.step()的调用，即step=1，表中此时没有不处理
    HandleStepMsg(deviceId, stepId);

    // 处理(step+1)的开始
    for (auto &poolStatus : npuMemUsages_[deviceId].poolStatusTable) {
        int64_t startAllocated = poolStatus.second.totalAllocated;
        LOG_INFO("[npu %ld][step %llu][start]: ------Start totalAllocated (%s): %lld------",
            deviceId, (stepId+1), GetMemoryPoolName(poolStatus.first).c_str(), startAllocated);
        UpdateStepInfoTable(deviceId, (stepId+1), poolStatus.first, startAllocated);
    }
    SetStepId(deviceId, (stepId+1));
    AddDuration(deviceId);

    return;
}

void StepInnerAnalyzer::ReportLeak(const DeviceId &deviceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout<< "====== ERROR: Detected memory leaks on device " <<
        deviceId << " ======" << std::endl;

    // 依照step排序
    std::vector<std::pair<LeakMemKey, LeakInfo>> leakVec(
        leakMemSums_[deviceId].begin(), leakMemSums_[deviceId].end());
    auto leakCompare = [](const std::pair<LeakMemKey, LeakInfo> &a, const std::pair<LeakMemKey, LeakInfo> &b) {
            return a.first.leakStepId < b.first.leakStepId;
    };
    std::sort(leakVec.begin(), leakVec.end(), leakCompare);
    // 输出泄漏信息总结
    uint64_t leakInfoCounts = 0;
    long double leakSizeSums = 0;
    for (const auto& pair :leakVec) {
        const std::string poolName = GetMemoryPoolName(pair.first.type);
        printf("Direct %s leak of %f Mb(s) at 0x%lx in kernel_%lu at step %lu.\n",
            poolName.c_str(),
            (pair.second.leakSize / static_cast<double>(BYTE_TO_MB)),
            pair.first.ptr,
            pair.second.kernelIndex,
            pair.first.leakStepId);
        leakInfoCounts++;
        long double leakTempSize = static_cast<long double>(pair.second.leakSize);
        leakSizeSums = Utility::GetAddResult(leakTempSize, leakSizeSums);
    }
    std::cout << "====== SUMMARY: " << leakSizeSums / BYTE_TO_MB << " Mb(s) leaked in " <<
        leakInfoCounts << " allocation(s) ======" << std::endl;
    return;
}

void StepInnerAnalyzer::ReportGap(const DeviceId &deviceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 打屏为保持格式统一需调整小数精度，打屏结束后还原
    int currentPrecision = std::cout.precision();
    int outputPrecision = 4;
    int outputWidth = 25;
    for (auto &poolStatus : npuMemUsages_[deviceId].poolStatusTable) {
        std::string poolName = GetMemoryPoolName(poolStatus.first);
        std::cout << "======= " << poolName << " Gap Analysis of Device " << deviceId << " =======" << std::endl;
        std::cout << "\t"
                << std::setw(outputWidth) << std::left << "MinAlloc/MaxAlloc(%)"
                << std::setw(outputWidth) << std::left << "MinAllocMem(MB)"
                << std::setw(outputWidth) << std::left << "StepId"
                << std::endl;
        std::cout << "MinGap\t"
                << std::fixed << std::setprecision(outputPrecision)
                << std::setw(outputWidth) << std::left
                << poolStatus.second.minGapInfo.minMaxAllocRatio * PERCENT_SCALE_FACTOR
                << std::setw(outputWidth) << std::left
                << poolStatus.second.minGapInfo.minAllocMemory / static_cast<double>(BYTE_TO_MB)
                << std::setw(outputWidth) << std::left
                << poolStatus.second.minGapInfo.gapStepId
                << std::endl;
        std::cout << "MaxGap\t"
                << std::setw(outputWidth) << std::left
                << poolStatus.second.maxGapInfo.minMaxAllocRatio * PERCENT_SCALE_FACTOR
                << std::setw(outputWidth) << std::left
                << poolStatus.second.maxGapInfo.minAllocMemory / static_cast<double>(BYTE_TO_MB)
                << std::setw(outputWidth) << std::left
                << poolStatus.second.maxGapInfo.gapStepId
                << std::setprecision(currentPrecision)
                << std::endl;
    }
}

StepInnerAnalyzer::~StepInnerAnalyzer()
{
    MstxAnalyzer::Instance().UnSubscribe(MstxEventSubscriber::STEP_INNER_ANALYZER);
    PyStepManager::Instance().UnSubscribe(PyStepEventSubscriber::STEP_INNER_ANALYZER);

    if (!IsStepInnerAnalysisEnable()) {
        return;
    }
    if (!leakMemSums_.empty()) {
        for (const auto& pair :leakMemSums_) {
            ReportLeak(pair.first);
        }
    }
    // 输出内存波动与模型的权重内存大小
    for (auto &device : npuMemUsages_) {
        ReportGap(device.first);
    }

    return;
}

}