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

#ifndef LEAK_ANALYZER_H
#define LEAK_ANALYZER_H

#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "analyzer_base.h"
#include "comm_def.h"
#include "config_info.h"
#include "constant.h"
#include "event.h"
#include "event_dispatcher.h"
#include "memory_state_manager.h"
#include "record_info.h"
#include "step_tracker.h"

namespace MemScope
{
/*
 * LeakAnalyzer类主要功能（统一原StepInnerAnalyzer与HalAnalyzer的泄漏检测职责）：
 * 1. 不维护内存块生命周期表，通过MemoryStateManager::QueryLiveBlocks条件查询存活块
 * 2. 判定①（跨step疑似泄漏）：每个step结束回调时对存活块拍allocationId快照，与上一次step结束快照
 *    求交集；两次结束stepId连续且配对起点为非首个step（>=2）、且块分配于首个step结束之后
 *    （allocStep>=2，过滤初始化期/首个step内分配的常驻块）时，交集中的块打疑似泄漏标记
 * 3. 判定②（退出未释放）：进程退出时（~LeakAnalyzer）对存活块打标，使PROC_EXIT落盘的MALLOC事件
 *    attr携带leak:true；无step打点时同样生效。与基线口径对齐的两个过滤（彻底跳过）：存在step
 *    打点时仅统计首个step结束点之后分配的块；与NPU层（存活块或判定①累计记录）同地址的HAL块
 *    不重复计入，避免同一块内存双层计数。落盘由~LeakAnalyzer末尾显式调用
 *    FlushProcExitResiduals完成，不依赖静态析构顺序（hook影子路径下~Dump可能先于~MSM析构）
 * 4. 泄漏记录按allocationId全局去重（地址复用不混淆），命中后不随FREE删除（累计告警语义）；
 *    MemoryState.isLeak为粘性标记，块生命周期内落盘均携带
 * 5. 订阅CLEAN_UP(RESIDUAL_BLOCK)还原double malloc告警；FREE事件幽灵state判定还原free无匹配告警
 * 6. HAL池泄漏在析构时按clientId分组全量检查（原HalAnalyzer语义）
 * 7. 退出时输出泄漏汇总报告（每设备疑似泄漏总块数+总大小）
 */

// 泄漏判定来源：①连续两个非首个step结束时都存活；②进程退出时仍未释放
enum class LeakSource : uint8_t
{
    STEP_SUSPECTED = 0,  // 判定①
    EXIT_UNFREED = 1,    // 判定②
};

// 泄漏块累计记录：按allocationId全局唯一（地址会复用，allocationId不会），命中后不随FREE删除
struct LeakRecord
{
    uint64_t ptr = 0;
    PoolType poolType = PoolType::INVALID;
    uint64_t pid = 0;
    DeviceId device = 0;
    uint64_t leakSize = 0;
    uint64_t kernelIndex = 0;
    uint64_t allocStep = 0;   // 分配step（GetAllocStep按边界推导，首个边界前为0）
    StepId detectedStep = 0;  // 判定①首次命中的step结束编号；判定②为0
    LeakSource source = LeakSource::STEP_SUSPECTED;
};

// step结束时刻的存活块快照（判定①求交集用）
struct StepEndSnapshot
{
    StepId stepId = 0;  // 0表示尚无快照
    std::unordered_set<uint64_t> allocationIds;
};

// 判定①配对(E_{k-1},E_k)要求k-1>=2：排除首个step结束，过滤初始化期一次性加载的块
constexpr uint64_t LEAK_CHECK_MIN_PAIR_START_STEP = 2;

class LeakAnalyzer : public AnalyzerBase
{
   public:
    static LeakAnalyzer& GetInstance();
    void EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state) override;
    const char* GetName() const override;  // "leak"(控制通道display analyzer)
    void Subscribe();
    void UnSubscribe() const;

    // 统一查询接口：NPU池按deviceId过滤（原StepInnerAnalyzer语义），HAL池按clientId(pid)过滤（原HalAnalyzer语义）
    std::vector<OOMMemRecord> QueryUnfreedRecords(int32_t deviceId, uint32_t clientId) const;

   private:
    explicit LeakAnalyzer();
    ~LeakAnalyzer();
    LeakAnalyzer(const LeakAnalyzer&) = delete;
    LeakAnalyzer& operator=(const LeakAnalyzer&) = delete;
    LeakAnalyzer(LeakAnalyzer&& other) = delete;
    LeakAnalyzer& operator=(LeakAnalyzer&& other) = delete;

    bool IsNpuAnalysisEnable();
    bool IsHalAnalysisEnable();
    void HandleNpuMemEvent(std::shared_ptr<EventBase>& event, MemoryState* state);
    void HandleHalMemEvent(std::shared_ptr<EventBase>& event, MemoryState* state);
    void HandleResidualBlock(std::shared_ptr<EventBase>& event, MemoryState* state);
    void CheckNpuLeak(const DeviceId& deviceId, const StepId& stepId);
    // 判定②：退出时对存活块打标并记录；须在PROC_EXIT残留落盘（~LeakAnalyzer末尾的
    // FlushProcExitResiduals）之前调用。存在step打点时跳过首个step结束点之前分配的块
    // （allocStep<2）；与NPU层（存活块或判定①累计记录）同地址的HAL块跳过（防双层计数）；
    // 二者均为彻底跳过（不打标不记录）
    void MarkExitUnfreedLeaks();
    // 打标MSM的MemoryState并写入累计表（须已持有mutex_，锁序约定mutex_→MSM::mtx_单向，禁止反向
    // 嵌套；MarkLeak失败时告警留痕。同allocationId重复调用保留首次记录）
    void RecordLeakLocked(const LiveBlockInfo& blk, LeakSource source, StepId detectedStep);
    // 退出汇总：每设备输出疑似泄漏明细与总块数/总大小
    void ReportLeak();
    void CheckHalLeak();
    const std::string& GetMemoryPoolName(const PoolType& poolType);
    bool IsGhostState(MemoryState* state, const std::shared_ptr<EventBase>& event) const;
    // 块分配step推导：找最后一个边界满足 (b.timestamp, b.eventId) <= (allocTs, allocId)，无则0
    uint64_t GetAllocStep(const std::vector<StepBoundary>& boundaries, const uint64_t& allocTs,
                          const uint64_t& allocId) const;

    StepTracker stepTracker_;                                 // step边界维护组件，回调触发CheckNpuLeak
    std::unordered_map<uint64_t, LeakRecord> leakRecords_{};  // 泄漏累计表，key为allocationId
    std::unordered_map<DeviceId, StepEndSnapshot> stepEndSnapshots_{};  // 判定①的上一次step结束快照
    std::set<ClientId> halClients_{};  // 见过的HAL事件client，析构时逐client检查（原memtables_分表语义）
    mutable std::mutex mutex_;         // 保护共享数据的互斥锁
};

}  // namespace MemScope

#endif
