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

#include "kernel_event_trace.h"

#include "bit_field.h"
#include "driver_prof_api.h"
#include "event_report.h"
#include "log.h"
#include "memory_watch/memory_watch.h"
#include "runtime_prof_api.h"
#include "securec.h"
#include "stars_common.h"
#include "utils.h"

namespace MemScope
{

void KernelEventTrace::KernelLaunch(const AclnnKernelMapInfo& kernelLaunchInfo)
{
    if (!EventTraceManager::Instance().IsTracingEnabled())
    {
        return;
    }
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportKernelLaunch(kernelLaunchInfo))
    {
        LOG_ERROR("KernelLaunch launch report failed");
    }

    return;
}

void KernelEventTrace::KernelStartExcute(TaskKey& key, uint64_t time)
{
    auto kernelName = RuntimeKernelLinker::GetInstance().GetKernelName(key, RecordSubType::KERNEL_START);
    if (!kernelName.empty())
    {
        if (!EventTraceManager::Instance().IsTracingEnabled())
        {
            return;
        }
        if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
                 .ReportKernelExcute(key, kernelName, time, RecordSubType::KERNEL_START))
        {
            LOG_ERROR("Kernel excute start report failed");
        }
    }
    return;
}

void KernelEventTrace::KernelEndExcute(TaskKey& key, uint64_t time)
{
    auto kernelName = RuntimeKernelLinker::GetInstance().GetKernelName(key, RecordSubType::KERNEL_END);
    if (!kernelName.empty())
    {
        if (!EventTraceManager::Instance().IsTracingEnabled())
        {
            return;
        }
        if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
                 .ReportKernelExcute(key, kernelName, time, RecordSubType::KERNEL_END))
        {
            LOG_ERROR("Kernel excute end report failed");
        }
    }
    return;
}

static void ReportStarsSocLog(uint32_t deviceId, const StarsSocLog* socLog)
{
    if (!socLog)
    {
        return;
    }
    constexpr int32_t bitOffset = 32;
    uint16_t streamId =
        StarsCommon::GetStreamId(static_cast<uint16_t>(socLog->streamId), static_cast<uint16_t>(socLog->taskId),
                                 static_cast<uint16_t>(socLog->sqeType));
    uint16_t taskId =
        StarsCommon::GetTaskId(static_cast<uint16_t>(socLog->streamId), static_cast<uint16_t>(socLog->taskId),
                               static_cast<uint16_t>(socLog->sqeType));
    auto taskKey = std::make_tuple(static_cast<uint16_t>(deviceId), streamId, taskId);
    if (socLog->funcType == STARS_FUNC_TYPE_BEGIN)
    {
        auto start = static_cast<uint64_t>(socLog->sysCntH) << bitOffset | socLog->sysCntL;
        start = GetRealTimeFromSysCnt(deviceId, start);
        KernelEventTrace::GetInstance().KernelStartExcute(taskKey, start);
    }
    else if (socLog->funcType == STARS_FUNC_TYPE_END)
    {
        auto end = static_cast<uint64_t>(socLog->sysCntH) << bitOffset | socLog->sysCntL;
        end = GetRealTimeFromSysCnt(deviceId, end);
        KernelEventTrace::GetInstance().KernelEndExcute(taskKey, end);
    }

    return;
}

static void ReportStarsSocLogA5A6(uint32_t deviceId, const StarsSocLogA5A6* socLog)
{
    if (!socLog)
    {
        return;
    }
    uint16_t streamId = 0;
    uint16_t taskId = static_cast<uint16_t>(socLog->taskId);
    auto taskKey = std::make_tuple(static_cast<uint16_t>(deviceId), streamId, taskId);
    if (socLog->funcType == STARS_FUNC_TYPE_BEGIN)
    {
        auto start = socLog->timestamp;
        start = GetRealTimeFromSysCnt(deviceId, start);
        KernelEventTrace::GetInstance().KernelStartExcute(taskKey, start);
    }
    else if (socLog->funcType == STARS_FUNC_TYPE_END)
    {
        auto end = socLog->timestamp;
        end = GetRealTimeFromSysCnt(deviceId, end);
        KernelEventTrace::GetInstance().KernelEndExcute(taskKey, end);
    }

    return;
}

static size_t TransStarsLog(char buffer[], size_t validSize, uint32_t deviceId)
{
    size_t pos = 0;
    DeviceSocType socType = GetDeviceSocType();
    bool isNewSoc = (socType == DeviceSocType::SOC_A5 || socType == DeviceSocType::SOC_A6);
    size_t size = isNewSoc ? sizeof(StarsSocLogA5A6) : sizeof(StarsSocLog);
    while (validSize - pos >= size)
    {
        if (isNewSoc)
        {
            const StarsSocLogA5A6* data = reinterpret_cast<StarsSocLogA5A6*>(buffer + pos);
            ReportStarsSocLogA5A6(deviceId, data);
        }
        else
        {
            const StarsSocLog* data = reinterpret_cast<StarsSocLog*>(buffer + pos);
            ReportStarsSocLog(deviceId, data);
        }
        pos += size;
    }
    return pos;
}

static size_t TransDataToActivityBuffer(char buffer[], size_t validSize, uint32_t deviceId, AI_DRV_CHANNEL channelId)
{
    switch (channelId)
    {
        case PROF_CHANNEL_STARS_SOC_LOG:
            return TransStarsLog(buffer, validSize, deviceId);
        default:
            return 0;
    }
}

void KernelEventTrace::CreateReadDataChannel(uint32_t devId)
{
    readTh_ = std::thread(
        [devId, this]() mutable
        {
            std::vector<char> buf(MAX_BUFFER_SIZE);
            size_t curPos = 0;
            int currLen = 0;
            using ReadFunc = int (*)(unsigned int, unsigned int, char*, unsigned int);
            while (started_)
            {
                static auto vallina = reinterpret_cast<ReadFunc>(GetSymbol("prof_channel_read"));
                if (vallina == nullptr)
                {
                    LOG_ERROR("ReadFunc is null");
                    return;
                }
                currLen = vallina(devId, PROF_CHANNEL_STARS_SOC_LOG, buf.data() + curPos, MAX_BUFFER_SIZE - curPos);
                if (currLen <= 0)
                {
                    continue;
                }
                auto uintCurrLen = static_cast<size_t>(currLen);
                if (uintCurrLen >= (MAX_BUFFER_SIZE - curPos))
                {
                    LOG_ERROR("Read invalid data len from driver");
                    continue;
                }
                size_t lastPos =
                    TransDataToActivityBuffer(buf.data(), curPos + uintCurrLen, devId, PROF_CHANNEL_STARS_SOC_LOG);
                if (lastPos < curPos + uintCurrLen)
                {
                    if (memcpy_s(buf.data(), MAX_BUFFER_SIZE, buf.data() + lastPos, curPos + uintCurrLen - lastPos) !=
                        EOK)
                    {
                        continue;
                    }
                }
                curPos = curPos + uintCurrLen - lastPos;
            }
        });
}

void KernelEventTrace::StartKernelEventTrace()
{
    // 先构造EventReport（内部注册prof回调），再开启驱动prof通道：
    // 部分场景首个kernel下发前未经过aclInit，晚注册会错过runtime的hash/compact回调
    EventReport::Instance(MemScopeCommType::SHARED_MEMORY);
    GetDeviceSocType();  // trigger SoC type detection early
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
    {
        LOG_ERROR("get device id failed");
    }

    StartDriverKernelInfoTrace(devId);
    CreateReadDataChannel(static_cast<uint32_t>(devId));
}

void KernelEventTrace::EndKernelEventTrace() const { return EndDriverKernelInfoTrace(); }

KernelEventTrace::~KernelEventTrace()
{
    started_.store(false);
    if (readTh_.joinable())
    {
        readTh_.join();
    }
}

std::string RuntimeKernelLinker::GetLastKernelName(uint64_t tid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return currentNameMap_[tid];
}

size_t RuntimeKernelLinker::GetTaskMapSizeForTest()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = kernelNameMp_.find(Utility::GetTid());
    return iter == kernelNameMp_.end() ? 0 : iter->second.size();
}

void RuntimeKernelLinker::RuntimeTaskInfoLaunch(const TaskKey& key, uint64_t hashId)
{
    if (hashId == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = kernelNameMp_.find(Utility::GetTid());
    if (iter == kernelNameMp_.end())
    {
        return;
    }
    auto& vec = iter->second;
    if (!vec.empty())
    {
        vec.back().taskKey = key;
        std::string kernelName = GetHashInfo(hashId);
        if (kernelName.empty())
        {
            // 部分CANN版本的MsprofGetHashId不回调工具注册的hash回调（hash由libmsprof自行计算），
            // 但名字仍保存在libmsprof内部，经MsprofId2Str反查取回并缓存
            kernelName = GetKernelNameByHashId(hashId);
            if (!kernelName.empty())
            {
                hashInfo_map_[hashId] = kernelName;  // 已持有mutex_，缓存后后续下发不再反查
            }
        }
        if (kernelName.empty())
        {
            // hash回调未生效且无法反查时不能用空名，否则执行事件会被丢弃；
            // 用hash生成占位名保证kernel_launch/start/end都能落盘且可相互关联
            uint64_t unresolvedCount = ++unresolvedNameCount_;
            if (unresolvedCount == 1 || unresolvedCount % KERNEL_TASK_LOG_INTERVAL == 0)
            {
                LOG_WARN("kernel name unresolved: hashId=%llu, knownHashNames=%zu, use placeholder (count=%llu)",
                         static_cast<unsigned long long>(hashId), hashInfo_map_.size(),
                         static_cast<unsigned long long>(unresolvedCount));
            }
            kernelName = "unknown_kernel_" + std::to_string(hashId);
        }
        vec.back().kernelName = kernelName;
        std::string name = std::to_string(std::get<0>(key)) + "_" + std::to_string(iter->first) + "/" + kernelName;
        currentNameMap_[iter->first] = name;
        bool isKernelLevel = BitPresent(GetConfig().levelType, static_cast<size_t>(LevelType::LEVEL_KERNEL));
        if ((GetConfig().watchConfig.isWatched || TensorMonitor::GetInstance().IsInMonitoring()) && isKernelLevel)
        {
            MemoryWatch::GetInstance().KernelExcuteBegin(nullptr, name);
        }
        KernelEventTrace::GetInstance().KernelLaunch(vec.back());
    }

    return;
}

void RuntimeKernelLinker::KernelLaunch()
{
    uint64_t timestamp = Utility::GetTimeNanoseconds();
    std::lock_guard<std::mutex> lock(mutex_);

    AclnnKernelMapInfo value = {timestamp, std::make_tuple(-1, -1, -1), ""};
    auto iter = kernelNameMp_.find(Utility::GetTid());
    if (iter == kernelNameMp_.end())
    {
        std::vector<AclnnKernelMapInfo> vec{};
        vec.push_back(value);
        kernelNameMp_.insert({Utility::GetTid(), vec});
    }
    else
    {
        auto& vec = iter->second;
        // SOC日志通道异常时KERNEL_END不会到达，关联表会按launch次数无界增长；
        // 达到上限时先批量淘汰最旧记录再push（均摊O(1)，且避免vector容量继续翻倍）
        if (vec.size() >= KERNEL_TASK_MAP_MAX_PER_THREAD)
        {
            vec.erase(vec.begin(), vec.begin() + static_cast<std::ptrdiff_t>(KERNEL_TASK_MAP_EVICT_BATCH));
            overflowEvictCount_ += KERNEL_TASK_MAP_EVICT_BATCH;
            // 每 KERNEL_TASK_MAP_EVICT_BATCH 次下发才触发一轮淘汰，无需再限频
            LOG_WARN("kernel task map reached %zu entries, evicted oldest %zu entries (evicted total=%llu)",
                     KERNEL_TASK_MAP_MAX_PER_THREAD, KERNEL_TASK_MAP_EVICT_BATCH,
                     static_cast<unsigned long long>(overflowEvictCount_));
        }
        vec.push_back(value);
    }

    return;
}

std::string RuntimeKernelLinker::GetKernelName(TaskKey& key, RecordSubType type)
{
    std::lock_guard<std::mutex> lock(mutex_);
    DeviceSocType socType = GetDeviceSocType();
    bool isNewSoc = (socType == DeviceSocType::SOC_A5 || socType == DeviceSocType::SOC_A6);

    // A5/A6的SOC日志key不含streamId，只比较deviceId+taskId；A2/A3先比较完整key
    auto exactMatched = [&key, isNewSoc](const TaskKey& candidate)
    {
        if (isNewSoc)
        {
            return std::get<0>(candidate) == std::get<0>(key) && std::get<2>(candidate) == std::get<2>(key);
        }
        return candidate == key;
    };

    for (auto& pair : kernelNameMp_)
    {
        auto& vec = pair.second;
        for (auto it = vec.begin(); it != vec.end(); ++it)
        {
            if (!exactMatched(it->taskKey))
            {
                continue;
            }
            std::string name = it->kernelName;
            if (name.empty())
            {
                uint64_t emptyCount = ++emptyNameCount_;
                if (emptyCount == 1 || emptyCount % KERNEL_TASK_LOG_INTERVAL == 0)
                {
                    LOG_WARN("kernel task matched but name is empty (device=%u, stream=%u, task=%u, count=%llu)",
                             static_cast<unsigned>(std::get<0>(key)), static_cast<unsigned>(std::get<1>(key)),
                             static_cast<unsigned>(std::get<2>(key)), static_cast<unsigned long long>(emptyCount));
                }
            }
            // backfill streamId into key for subsequent data reporting
            std::get<1>(key) = std::get<1>(it->taskKey);
            if (type == RecordSubType::KERNEL_END)
            {
                vec.erase(it);  // 使用完删除
            }
            return name;
        }
    }

    if (isNewSoc)
    {
        // A5/A6只按device+taskId匹配，无更宽松的键可用；失配时仅计数并限频告警
        uint64_t missCount = ++unmatchedCount_;
        if (missCount == 1 || missCount % KERNEL_TASK_LOG_INTERVAL == 0)
        {
            LOG_WARN("kernel execution event dropped: no task matched by device+taskId (device=%u, task=%u, miss=%llu)",
                     static_cast<unsigned>(std::get<0>(key)), static_cast<unsigned>(std::get<2>(key)),
                     static_cast<unsigned long long>(missCount));
        }
        return "";
    }

    // A2/A3精确key失配时回退为deviceId+taskId匹配：不同CANN版本的stream编码规则可能不一致，
    // 精确失配不代表不是同一个任务；仅当候选唯一时采用，避免把执行区间配到错误的kernel。
    // 记录时间为host时钟、事件时间为设备sysCnt按标称频率换算，长时间运行存在频偏漂移，
    // 不能用"记录时间<=事件时间"过滤，改用"每条记录只被一次START回退命中"约束重复误配
    size_t matchCount = 0;
    std::vector<AclnnKernelMapInfo>* matchedVec = nullptr;
    std::vector<AclnnKernelMapInfo>::iterator matchedIt;
    for (auto& pair : kernelNameMp_)
    {
        auto& vec = pair.second;
        for (auto it = vec.begin(); it != vec.end(); ++it)
        {
            if (std::get<0>(it->taskKey) != std::get<0>(key) || std::get<2>(it->taskKey) != std::get<2>(key))
            {
                continue;
            }
            // 回退匹配可靠性低于精确匹配：同一条记录只允许被一次KERNEL_START回退命中，
            // 避免END丢失的残留记录持续吸收后续同taskId的执行事件；END仍可命中同一记录完成配对
            if (type != RecordSubType::KERNEL_END && it->fallbackUsed)
            {
                continue;
            }
            ++matchCount;
            matchedVec = &vec;
            matchedIt = it;
        }
    }

    if (matchCount == 1 && matchedVec != nullptr)
    {
        std::string name = matchedIt->kernelName;
        uint16_t matchedStream = std::get<1>(matchedIt->taskKey);
        uint64_t hitCount = ++fallbackMatchCount_;
        if (hitCount == 1 || hitCount % KERNEL_TASK_LOG_INTERVAL == 0)
        {
            LOG_WARN(
                "kernel task key exact match failed, fallback by device+taskId hit "
                "(device=%u, soc stream=%u, task=%u, matched stream=%u, hit=%llu)",
                static_cast<unsigned>(std::get<0>(key)), static_cast<unsigned>(std::get<1>(key)),
                static_cast<unsigned>(std::get<2>(key)), static_cast<unsigned>(matchedStream),
                static_cast<unsigned long long>(hitCount));
        }
        std::get<1>(key) = matchedStream;
        if (type == RecordSubType::KERNEL_END)
        {
            matchedVec->erase(matchedIt);  // 使用完删除
        }
        else
        {
            matchedIt->fallbackUsed = true;  // 标记已消费，防止残留记录重复吸收
        }
        return name;
    }

    if (matchCount > 1)
    {
        uint64_t ambiguousCount = ++ambiguousMatchCount_;
        if (ambiguousCount == 1 || ambiguousCount % KERNEL_TASK_LOG_INTERVAL == 0)
        {
            LOG_WARN(
                "kernel task key exact match failed, fallback matched %zu candidates, skip "
                "(device=%u, soc stream=%u, task=%u, ambiguous=%llu)",
                matchCount, static_cast<unsigned>(std::get<0>(key)), static_cast<unsigned>(std::get<1>(key)),
                static_cast<unsigned>(std::get<2>(key)), static_cast<unsigned long long>(ambiguousCount));
        }
    }
    else
    {
        uint64_t missCount = ++unmatchedCount_;
        if (missCount == 1 || missCount % KERNEL_TASK_LOG_INTERVAL == 0)
        {
            LOG_WARN("kernel execution event dropped: no task matched (device=%u, soc stream=%u, task=%u, miss=%llu)",
                     static_cast<unsigned>(std::get<0>(key)), static_cast<unsigned>(std::get<1>(key)),
                     static_cast<unsigned>(std::get<2>(key)), static_cast<unsigned long long>(missCount));
        }
    }
    return "";
}

void RuntimeKernelLinker::SetHashInfo(uint64_t hashId, const std::string& hashInfo)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iter = hashInfo_map_.find(hashId);
    if (iter == hashInfo_map_.end())
    {
        hashInfo_map_.insert({hashId, hashInfo});
    }
}

std::string RuntimeKernelLinker::GetHashInfo(uint64_t hashId)
{
    const auto iter = hashInfo_map_.find(hashId);
    if (iter != hashInfo_map_.end())
    {
        return iter->second;
    }
    return "";
}
}  // namespace MemScope
