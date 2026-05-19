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

#ifndef STEPINNER_ANALYZER_H
#define STEPINNER_ANALYZER_H

#include <unordered_map>
#include <atomic>
#include <mutex>

#include "config_info.h"
#include "comm_def.h"
#include "event.h"
#include "framework/record_info.h"

namespace MemScope {
/*
 * StepInnerAnalyzer类主要功能：
 * 1. 维护npu内存池的申请释放表，记录未释放的内存持续step时间
   2. 维护观察mstx和python_step的StepInfo表，用于分析step内的内存泄漏
*/

using DeviceId = int32_t;
using StepId = uint64_t;
constexpr uint64_t BYTE_TO_MB = 1024 * 1024;
constexpr double  PERCENT_SCALE_FACTOR = 100.0;

const std::unordered_map<PoolType, std::string> PoolTypeToString = {
    {PoolType::ATB, "ATB memory pool"},
    {PoolType::PTA_CACHING, "Pytorch Caching memory pool"},
    {PoolType::MINDSPORE, "Mindspore memory pool"}
};

// step信息的俩个来源mstx和msmemscope.step()不能同时存在，第一个接收的会被用作信息来源，后续其他来源将被无视
enum class StepSource {
    None,
    MSTX_SOURCE,
    PY_STEP_SOURCE
};

struct StepInfo {
    std::unordered_map<PoolType, int64_t> stepAllocTable;
    uint64_t rangeId = 0;  // rangeId唯一标识，用于判断mstx方式中是否为固化信息的打点信息，在接口方式中该项无效
};

using StepInfoTable = std::unordered_map<StepId, StepInfo>;

enum class MemActionType : uint8_t {
    MALLOC = 0,
    FREE = 1,
    BLOCK_FREE = 2
};

struct  LeakMemKey  {
    uint64_t ptr;
    PoolType type;
    uint64_t leakStepId;
    LeakMemKey(uint64_t p, PoolType t, uint64_t id) : ptr(p), type(t), leakStepId(id) {}
    bool operator==(const LeakMemKey& other) const;
};

struct LeakInfo {
    int64_t leakSize;
    uint64_t kernelIndex;
};

struct LeakMemKeyHash {
    std::size_t operator()(const LeakMemKey& leakKey) const;
};

using LeakSumsTable = std::unordered_map<LeakMemKey, LeakInfo, LeakMemKeyHash>;

// 单条内存操作信息
struct NpuMemInfo {
    PoolType type;
    int64_t memSize;
    uint64_t timestamp;
    uint64_t duration;      // 目前经历的duration
    uint64_t stepId;        // 来自哪个的step
    uint64_t kernelIndex;   // 处于哪个event中
};

struct GapInfo  {
    uint64_t gapStepId = 0;             // 记录计算比值时的stepId
    double  minMaxAllocRatio = 0;       // 最大allocated内存和最小allocated内存的比值
    int64_t minAllocMemory = 0;         // 最小allocated内存的值
};

// 用于记录不同类型内存池内的占用状态
struct MemoryPoolStatus {
    int64_t totalAllocated = 0;
    int64_t totalReserved = 0;
    int64_t stepMaxAllocated = 0;
    int64_t stepMinAllocated = 0;
    GapInfo maxGapInfo;    // 记录动态内存和静态内存比值最大的信息
    GapInfo minGapInfo;    // 记录动态内存和静态内存比值最小的信息
};

struct NpuMemKey {
    uint64_t ptr;
    PoolType memType;
    NpuMemKey(uint64_t p, PoolType t) : ptr(p), memType(t) {}
    bool operator==(const NpuMemKey& other) const;
};

struct NpuMemKeyHash {
    std::size_t operator()(const NpuMemKey& memKey) const;
};

struct NpuMemUsage {
    std::unordered_map<NpuMemKey, NpuMemInfo, NpuMemKeyHash> poolOpTable; // 维护内存池具体操作的表
    std::unordered_map<PoolType, MemoryPoolStatus> poolStatusTable;      // 维护内存池占用状态的表
    uint64_t duringStep = 0; // 用于更新当前到哪一个step，并将其应用于表中的stepId属性。
};

class StepInnerAnalyzer {
public:
    static StepInnerAnalyzer &GetInstance(Config config);
    bool Record(const ClientId &clientId, std::shared_ptr<const EventBase> event);
private:
    explicit StepInnerAnalyzer(Config config);
    ~StepInnerAnalyzer();
    StepInnerAnalyzer(const StepInnerAnalyzer&) = delete;
    StepInnerAnalyzer& operator=(const StepInnerAnalyzer&) = delete;
    StepInnerAnalyzer(StepInnerAnalyzer&& other) = delete;
    StepInnerAnalyzer& operator=(StepInnerAnalyzer&& other) = delete;
    
    const std::string& GetMemoryPoolName(const PoolType &poolType);
    void UpdateStepInfoTable(const DeviceId &deviceId, const uint64_t &stepId, const PoolType &poolType,
        const int64_t &startAllocated);
    void ReceiveMstxMsg(std::shared_ptr<const MstxEvent> mstxEvent);
    void ReceiveStepMsg(std::shared_ptr<const EventBase>);
    void HandleStepMsg(const DeviceId &deviceId, const uint64_t &stepId);
    void UpdateAllocated(const DeviceId &deviceId, const PoolType &poolType, const int64_t &totalAllocated);
    void AddDuration(const DeviceId &deviceId);
    void SetStepId(const DeviceId &deviceId, const uint64_t &stepId);
    void CheckGap(const DeviceId &deviceId);
    void CheckNpuLeak(const DeviceId &deviceId, const uint64_t stepId);
    bool CreateStepInfoTables(const DeviceId &deviceId);
    bool CreateTables(const DeviceId &deviceId);
    bool CreateLeakSumTables(const DeviceId &deviceId);
    void RecordNpuMalloc(const ClientId &clientId, const DeviceId &deviceId, std::shared_ptr<const MemoryEvent> memEvent);
    void RecordNpuFree(const ClientId &clientId, const DeviceId &deviceId, std::shared_ptr<const MemoryEvent> memEvent);
    bool SkipCheck(const NpuMemInfo &npuMemInfo) const;
    void ReportLeak(const DeviceId &deviceId);
    void ReportGap(const DeviceId &deviceId);
    bool IsStepInnerAnalysisEnable();
    std::unordered_map<DeviceId, NpuMemUsage> npuMemUsages_{};
    std::unordered_map<DeviceId, StepInfoTable> stepInfoTables_{};
    std::unordered_map<DeviceId, LeakSumsTable> leakMemSums_{};
    std::atomic<StepSource> crtStepSource_{StepSource::None};
    uint64_t durationThreshold_ = 1;  // 设置警告阈值, 可由用户更改
    uint64_t skipSteps_ = 1;
    Config config_;
    mutable std::mutex mutex_;  // 保护共享数据的互斥锁
};

}

#endif