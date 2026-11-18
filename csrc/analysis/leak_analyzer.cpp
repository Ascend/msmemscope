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
#include <map>
#include <tuple>

#include "bit_field.h"
#include "dump.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#include "utility/log.h"
#include "utility/utils.h"

namespace MemScope
{

LeakAnalyzer& LeakAnalyzer::GetInstance()
{
    // 确保依赖的单例先于LeakAnalyzer构造（Dump内部触发FileWriteManager，MemoryStateManager内部
    // 已触发EventDispatcher/FileWriteManager），利用C++静态对象析构逆序规则，
    // 使~LeakAnalyzer中QueryLiveBlocks/显式落盘与LOG宏安全
    (void)Dump::GetInstance();
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

const char* LeakAnalyzer::GetName() const { return "leak"; }

void LeakAnalyzer::Subscribe()
{
    auto func = std::bind(&LeakAnalyzer::EventHandle, this, std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::FREE, EventBaseType::MSTX,
                                         EventBaseType::SYSTEM, EventBaseType::CLEAN_UP};
    EventDispatcher::GetInstance().Subscribe(SubscriberId::LEAKS_ANALYZER, eventList, EventDispatcher::Priority::High,
                                             func, GetName());
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

bool LeakAnalyzer::IsLeakAnalysisEnable()
{
    // 泄漏打标/报告专用开关：仅认可LEAKS_ANALYSIS，不连带OOM_ANALYSIS——oom-only配置时
    // QueryUnfreedRecords仍可用IsNpuAnalysisEnable/IsHalAnalysisEnable（OOM查询依赖），
    // 但不应产生泄漏打标与泄漏告警
    const Config& config = GetConfig();
    BitField<decltype(config.analysisType)> analysisType(config.analysisType);
    if (!(analysisType.checkBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS))))
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

    // FREE事件：按allocationId将命中过的泄漏累计记录置freed标志（判定②的NPU地址对照据此
    // 排除已释放记录，防止地址复用时HAL真泄漏被历史NPU记录误去重）。幽灵state的allocationId
    // 与任何MALLOC记录键不重合，查找天然落空，无副作用
    if (event->eventType == EventBaseType::FREE && state != nullptr)
    {
        MarkRecordFreed(state->allocationId);
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

bool LeakAnalyzer::IsAllocatedAfterFirstStepSpan(const LiveBlockInfo& blk) const
{
    if (stepTracker_.GetStepBoundaries(blk.device).empty())
    {
        // 无任何step打点：判定②全量兜底
        return true;
    }
    // 统一截止点：首个step结束点 与 首个stepId>=2开始边界（基线allocStep>=2口径）中
    // (timestamp, eventId)较早者：
    // - 结束点口径覆盖step k结束~step k+1开始间隙分配的块（按开始边界归属会误算到step k而漏判）
    // - 开始边界口径覆盖打点自step 2起（无step1）或结束点缺失（mstx range_end丢失）的场景，
    //   与基线skipSteps行为一致，避免上述场景下判定②静默归零
    uint64_t cutoffTs = 0;
    uint64_t cutoffId = 0;
    bool hasCutoff = false;
    const uint64_t firstEndTs = stepTracker_.GetFirstStepEndTimestamp(blk.device);
    if (firstEndTs != 0)
    {
        cutoffTs = firstEndTs;
        cutoffId = 0;
        hasCutoff = true;
    }
    uint64_t step2Ts = 0;
    uint64_t step2Id = 0;
    stepTracker_.GetFirstStep2Start(blk.device, step2Ts, step2Id);
    if (step2Ts != 0 && (!hasCutoff || step2Ts < cutoffTs))
    {
        cutoffTs = step2Ts;
        cutoffId = step2Id;
        hasCutoff = true;
    }
    if (!hasCutoff)
    {
        // 有打点但无任何截止点（如仅start1）：所有块均属首个step（初始化期），全部不通过
        return false;
    }
    return blk.allocTimestamp > cutoffTs || (blk.allocTimestamp == cutoffTs && blk.allocEventId >= cutoffId);
}

void LeakAnalyzer::CheckNpuLeak(const DeviceId& deviceId, const StepId& stepId)
{
    // 泄漏打标专用开关（仅认可LEAKS_ANALYSIS）：oom-only配置不判定①，仅保留幽灵/double malloc告警
    if (!IsLeakAnalysisEnable())
    {
        return;
    }
    // 条件查询MSM存活块（含SHADOW_PROMOTED转正块）作为本step结束时刻的快照
    LiveBlockFilter filter;
    filter.poolTypes = {PoolType::PTA_CACHING, PoolType::ATB, PoolType::MINDSPORE};
    filter.device = deviceId;
    auto blocks = MemoryStateManager::GetInstance().QueryLiveBlocks(filter);

    std::lock_guard<std::mutex> lock(mutex_);
    StepEndSnapshot& snapshot = stepEndSnapshots_[deviceId];
    // 判定①：与上一次step结束快照求交集；配对锚点为非首个step结束（stepId>=2，排除首个step结束，
    // 过滤初始化期一次性加载的块）。不做stepId算术相邻——mstx打点的stepId为进程内全局自增，
    // 多device交叉打点时各device的stepId序列不连续（如1,3,5…），算术相邻会误杀多卡场景；
    // 两次结束事件相邻由快照机制天然保证（本回调触发即该device上一次结束后无其他结束，
    // 中间丢失end只会使配对跨度变宽，命中块存活更久，只欠严不误报）。
    // 交集命中后还须按分配时刻过滤：初始化期或首个step内分配的常驻块（如模型权重、框架常驻
    // 资源）不判疑似泄漏，与判定②及基线skipSteps口径对齐，避免常驻块淹没真实泄漏信号
    if (snapshot.stepId != 0 && snapshot.stepId >= LEAK_CHECK_MIN_PAIR_START_STEP)
    {
        for (const auto& blk : blocks)
        {
            if (snapshot.allocationIds.count(blk.allocationId) != 0 && IsAllocatedAfterFirstStepSpan(blk))
            {
                RecordLeakLocked(blk, LeakSource::STEP_SUSPECTED, stepId);
            }
        }
    }
    // 滚动更新快照为本次step结束时刻的存活块
    snapshot.stepId = stepId;
    snapshot.allocationIds.clear();
    for (const auto& blk : blocks)
    {
        snapshot.allocationIds.insert(blk.allocationId);
    }
}

void LeakAnalyzer::RecordLeakLocked(const LiveBlockInfo& blk, LeakSource source, StepId detectedStep)
{
    // 锁序约定：持有mutex_时仅允许再获取MSM::mtx_（mutex_→mtx_单向），禁止反向嵌套。
    // 累计表按allocationId去重前置：①首次命中后每步重复命中、或②再次遇到同一块时直接返回，
    // 避免重复命中每步都跨MSM锁做必然成功的MarkLeak（粘性标记一次即生效）
    if (leakRecords_.find(blk.allocationId) != leakRecords_.end())
    {
        return;
    }
    // 粘性标记MSM的MemoryState：块生命周期内任何时刻落盘（FREE/PROC_EXIT）均携带leak:true；
    // 块不存在（幽灵或已删除）时MarkLeak返回false，告警留痕避免打标失败被静默吞掉
    if (!MemoryStateManager::GetInstance().MarkLeak(blk.poolType, MemoryStateKey{blk.pid, blk.device, blk.addr}))
    {
        std::string poolName = GetMemoryPoolName(blk.poolType);
        LOG_WARN("[%s][device %d]: MarkLeak failed for ptr: %llx, state may be ghost or already removed.",
                 poolName.c_str(), blk.device, blk.addr);
    }
    LeakRecord rec;
    rec.ptr = blk.addr;
    rec.poolType = blk.poolType;
    rec.pid = blk.pid;
    rec.device = blk.device;
    rec.leakSize = blk.size;
    rec.kernelIndex = blk.kernelIndex;
    rec.allocStep = GetAllocStep(stepTracker_.GetStepBoundaries(blk.device), blk.allocTimestamp, blk.allocEventId);
    rec.detectedStep = detectedStep;
    rec.source = source;
    leakRecords_.emplace(blk.allocationId, rec);

    if (source == LeakSource::STEP_SUSPECTED)
    {
        std::string memoryPoolName = GetMemoryPoolName(blk.poolType);
        LOG_WARN(
            "[npu %d][step %lu]: ptr: %llx suspected leak (alive at two consecutive step ends). "
            "Please check if there is leaks in %s.",
            blk.device, detectedStep, blk.addr, memoryPoolName.c_str());
    }
}

void LeakAnalyzer::MarkRecordFreed(uint64_t allocationId)
{
    // FREE事件：命中过的泄漏累计记录置freed标志（记录保留，累计告警语义不变），判定②的
    // NPU地址对照据此排除已释放记录，防止地址复用时HAL真泄漏被历史NPU记录误去重
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = leakRecords_.find(allocationId);
    if (it != leakRecords_.end())
    {
        it->second.freed = true;
    }
}

void LeakAnalyzer::MarkExitUnfreedLeaks()
{
    // 判定②：进程退出时仍未释放的块标记为疑似泄漏，无step打点时同样生效，作为唯一兜底。
    // 打标后由~LeakAnalyzer末尾显式调用FlushProcExitResiduals落盘（不依赖静态析构顺序）。
    // 打标/报告用IsLeakAnalysisEnable（仅认可LEAKS_ANALYSIS，oom-only配置不产生泄漏打标）。
    // 与基线口径对齐的过滤（均为彻底跳过，不打标不记录）：
    // 1. 分配时刻过滤（IsAllocatedAfterFirstStepSpan）：存在step打点时仅统计统一截止点（首个
    //    step结束点与首个stepId>=2开始边界取较早者）之后分配的块，初始化期常驻块不计入；
    //    无step打点时不过滤（全部存活块兜底）
    // 2. 与NPU层同(pid,device,addr)的HAL块跳过——NPU层（本次存活块或判定①累计未释放记录）已
    //    覆盖该内存，避免同一块内存双层计数。torch退出路径中PTA region常先于本函数被反注册，
    //    故除本次存活块外，还需对照leakRecords_中判定①的累计记录（排除已freed的：NPU块命中后
    //    释放、地址被HAL复用时，该HAL块是真泄漏候选，不能被历史NPU记录误去重）
    std::vector<LiveBlockInfo> npuBlocks;
    if (IsLeakAnalysisEnable())
    {
        LiveBlockFilter filter;
        filter.poolTypes = {PoolType::PTA_CACHING, PoolType::ATB, PoolType::MINDSPORE};
        npuBlocks = MemoryStateManager::GetInstance().QueryLiveBlocks(filter);
    }
    std::vector<LiveBlockInfo> halBlocks;
    if (IsLeakAnalysisEnable())
    {
        LiveBlockFilter filter;
        filter.poolTypes = {PoolType::HAL};
        halBlocks = MemoryStateManager::GetInstance().QueryLiveBlocks(filter);
        // device无效（GD_INVALID_NUM）的HAL块跳过：事件路径（EventHandle/HandleResidualBlock）同样
        // 跳过此类块，此处不过滤会与原HalAnalyzer口径不一致（MSM由事件路由先建状态、
        // 分析器侧跳过，因此存活块查询能捞到）
        halBlocks.erase(std::remove_if(halBlocks.begin(), halBlocks.end(),
                                       [](const LiveBlockInfo& blk) { return blk.device == GD_INVALID_NUM; }),
                        halBlocks.end());
    }
    if (npuBlocks.empty() && halBlocks.empty())
    {
        return;
    }
    // 锁外：NPU/HAL通用的分配时刻过滤（同一逻辑提取为一处，避免两份拷贝后续漂移不一致）。
    // IsAllocatedAfterFirstStepSpan为纯计算（无锁），提前到临界区外，锁内只做纯内存操作
    auto filterByAllocSpan = [this](const std::vector<LiveBlockInfo>& blocks)
    {
        std::vector<LiveBlockInfo> result;
        for (const auto& blk : blocks)
        {
            if (IsAllocatedAfterFirstStepSpan(blk))
            {
                result.push_back(blk);
            }
        }
        return result;
    };
    npuBlocks = filterByAllocSpan(npuBlocks);
    halBlocks = filterByAllocSpan(halBlocks);
    if (npuBlocks.empty() && halBlocks.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::tuple<uint64_t, int32_t, uint64_t>> npuAddrs;  // NPU层地址集合：(pid, device, addr)
    for (const auto& pair : leakRecords_)
    {
        // 仅未释放的NPU层记录参与HAL去重对照（freed记录的地址可能已被复用，对照会误吞HAL真泄漏）
        if (pair.second.poolType != PoolType::HAL && !pair.second.freed)
        {
            npuAddrs.emplace(pair.second.pid, pair.second.device, pair.second.ptr);
        }
    }
    for (const auto& blk : npuBlocks)
    {
        npuAddrs.emplace(blk.pid, blk.device, blk.addr);
        RecordLeakLocked(blk, LeakSource::EXIT_UNFREED, 0);
    }
    for (const auto& blk : halBlocks)
    {
        if (npuAddrs.count(std::make_tuple(blk.pid, blk.device, blk.addr)) != 0)
        {
            continue;
        }
        RecordLeakLocked(blk, LeakSource::EXIT_UNFREED, 0);
    }
}

void LeakAnalyzer::ReportLeak()
{
    if (!IsLeakAnalysisEnable() || leakRecords_.empty())
    {
        return;
    }
    // 按设备分组输出泄漏明细与统计（总块数+总大小）；std::map按device有序，多卡输出顺序稳定。
    // 明细为累计语义：含判定①命中后又被释放的块（"曾经疑似泄漏"也是定位信号，与基线
    // leakMemSums_口径一致），输出行标注判定来源与首次命中step以便区分
    std::map<DeviceId, std::vector<const LeakRecord*>> deviceRecords;
    for (const auto& pair : leakRecords_)
    {
        deviceRecords[pair.second.device].push_back(&pair.second);
    }
    for (auto& devicePair : deviceRecords)
    {
        // 依照分配step排序（与原ReportLeak一致）
        auto leakCompare = [](const LeakRecord* a, const LeakRecord* b) { return a->allocStep < b->allocStep; };
        std::sort(devicePair.second.begin(), devicePair.second.end(), leakCompare);
        std::cout << "====== ERROR: Detected memory leaks on device " << devicePair.first << " ======" << std::endl;
        uint64_t leakInfoCounts = 0;
        uint64_t printedDetails = 0;
        long double leakSizeSums = 0;
        for (const auto* rec : devicePair.second)
        {
            leakInfoCounts++;
            long double leakTempSize = static_cast<long double>(rec->leakSize);
            leakSizeSums = Utility::GetAddResult(leakTempSize, leakSizeSums);
            // 明细打印上限：无step打点场景判定②可能全量命中数万条，超出折叠提示改用dump文件
            // leak:true筛查看全量（SUMMARY统计始终完整）
            if (printedDetails >= LEAK_REPORT_MAX_DETAIL_COUNT)
            {
                continue;
            }
            const std::string poolName = GetMemoryPoolName(rec->poolType);
            std::string sourceNote = (rec->source == LeakSource::STEP_SUSPECTED)
                                         ? "suspected: alive at consecutive step ends, first detected at step " +
                                               std::to_string(rec->detectedStep)
                                         : "exit unfreed";
            printf("Direct %s leak of %f Mb(s) at 0x%lx in kernel_%lu at step %lu (%s).\n", poolName.c_str(),
                   (rec->leakSize / static_cast<double>(BYTE_TO_MB)), rec->ptr, rec->kernelIndex, rec->allocStep,
                   sourceNote.c_str());
            printedDetails++;
        }
        if (printedDetails < leakInfoCounts)
        {
            printf("... %lu more leak(s) not printed, filter leak:true in dump file to see all details.\n",
                   leakInfoCounts - printedDetails);
        }
        std::cout << "====== SUMMARY: " << leakSizeSums / BYTE_TO_MB << " Mb(s) leaked in " << leakInfoCounts
                  << " allocation(s) ======" << std::endl;
    }
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

    // 判定②：给退出未释放块打标，使其PROC_EXIT落盘的MALLOC事件attr携带leak:true
    MarkExitUnfreedLeaks();

    // 原输出顺序为：NPU泄漏汇总（StepInnerAnalyzer析构）→ Gap统计（StepInnerAnalyzer析构）→
    // HAL泄漏检查（HalAnalyzer析构）；合并后NPU与HAL在此输出，Gap统计由HealthAnalyzer析构输出，
    // 输出顺序调整为：NPU+HAL泄漏汇总 → Gap统计（顺序差异见RFC行为对照）
    ReportLeak();
    // CheckHalLeak门禁为IsLeakAnalysisEnable（仅认可LEAKS_ANALYSIS，泄漏告警归leaks分析，
    // oom-only不产生）；其内部保持原HalAnalyzer逐块存活告警语义，不应用判定②的过滤/去重
    // （口径为"存活块告警"而非泄漏统计，差异见类注释6）
    if (IsLeakAnalysisEnable())
    {
        CheckHalLeak();
    }

    // 显式触发PROC_EXIT残留落盘：所有构造路径下LeakAnalyzer均晚于Dump/MemoryStateManager构造
    // （初始化流程Dump先构造；hook影子采集路径MSM虽最先构造，仍早于LeakAnalyzer），此刻两者必然
    // 存活。不再依赖~MemoryStateManager析构中的静态析构顺序重放——hook路径下MSM先构造、~MSM最后
    // 执行，那时~Dump已析构退订，leak:true会静默丢失。状态经路由DeteleState删除，
    // ~MemoryStateManager析构中的兜底重放为空操作（幂等）
    MemoryStateManager::GetInstance().FlushProcExitResiduals();
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
        rec.deviceId = deviceId;
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
        rec.deviceId = deviceId;
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

}  // namespace MemScope
