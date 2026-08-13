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

#ifndef EVENT_H
#define EVENT_H

#include <atomic>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "data.h"
#include "log.h"
#include "record_info.h"
#include "state_manager.h"
#include "ustring.h"
#include "utils.h"

namespace MemScope
{

// 内存块 owner 标签级别: 分级模型(框架@组件@流程@细化), 各级允许省略/多个(按序拼接)
enum class OwnerLevel : uint8_t
{
    FRAMEWORK = 0,  // 框架, 如 CANN、PTA 等(由分配器来源填充)
    COMPONENT,      // 组件, 如 FSDP、VLLM 等
    PROCESS,        // 流程, 如 forward、backward、warmup 等
    DETAIL_1,       // 细化分类1, 如 weight、gradient 等
    DETAIL_2,       // 细化分类2, 用于 DETAIL_1 补充描述(如 ATEN 访问)

    USER_DEFINED_1,  // 用户标签(describe.py 用户接口), 栈序映射, 下同
    USER_DEFINED_2,
    USER_DEFINED_3,

    OWNER_LEVEL_NUM
};

enum class EventBaseType : uint8_t
{
    MALLOC = 0,
    ACCESS,
    FREE,
    MEMORY_OWNER,
    MSTX,
    OP_LAUNCH,
    KERNEL_LAUNCH,
    SYSTEM,
    CLEAN_UP,
    SNAPSHOT,
    OOM_DETAIL,
    INVALID,
};
enum class EventSubType : uint8_t
{
    PTA_CACHING = 0,
    PTA_WORKSPACE,
    ATB,
    MINDSPORE,
    HAL,
    HOST,
    HOST_PINNED,

    ATB_READ,
    ATB_WRITE,
    ATB_READ_OR_WRITE,

    ATEN_READ,
    ATEN_WRITE,
    ATEN_READ_OR_WRITE,

    DESCRIBE_OWNER,
    TORCH_OPTIMIZER_STEP_OWNER,  // 已废弃: report_tensor 路径移除, 与 DESCRIBE_OWNER 统一处理(保留枚举值避免数值移位)

    ATB_START,
    ATB_END,
    ATEN_START,
    ATEN_END,

    KERNEL_LAUNCH,
    KERNEL_EXECUTE_START,
    KERNEL_EXECUTE_END,
    ATB_KERNEL_START,
    ATB_KERNEL_END,

    ACL_INIT,
    ACL_FINI,

    TRACE_START,
    TRACE_STOP,

    MSTX_MARK,
    MSTX_RANGE_START,
    MSTX_RANGE_END,

    RESIDUAL_BLOCK,
    PROC_EXIT,

    STEP,

    SNAPSHOT,

    OOM_TRIGGER,
    OOM_RECENT_ALLOC,
    OOM_TOP_ALLOC,

    INVALID,
};

class EventBase : public DataBase
{
   public:
    MemScope::PoolType poolType = PoolType::INVALID;
    EventBaseType eventType = EventBaseType::INVALID;
    EventSubType eventSubType = EventSubType::INVALID;
    uint64_t id = 0;
    uint64_t timestamp = 0;
    uint64_t pid = 0;
    uint64_t tid = 0;
    uint64_t addr = 0;
    // 默认无效值：未显式赋值的设备字段（如CleanUpEvent）安全参与key(pid, device, addr)定位
    int32_t device = GD_INVALID_NUM;
    std::string name;
    std::string attr;
    std::string cCallStack;
    std::string pyCallStack;

    EventBase() : DataBase(DataType::MEMORY_EVENT), id(idCounter.fetch_add(1))
    {
        timestamp = Utility::GetTimeNanoseconds();
        pid = Utility::GetPid();
        tid = Utility::GetTid();
    }

   private:
    static std::atomic<uint64_t> idCounter;
};

class MemoryEvent : public EventBase
{
   public:
    int64_t size = 0;
    int64_t total = 0;
    int64_t used = 0;
    // 统计字段（MemoryStateManager 累计/查询后回填，dump 时值<0 的字段省略不输出）：
    int64_t processUsed = -1;  // 本进程显存用量（池事件=该设备 HAL 维度活跃累计；HOST 事件=VmRSS；
                               // -1=无统计值（池事件且该设备无 HAL 活跃记录），dump 省略）
    int64_t deviceUsed = -1;   // 整卡用量（仅 NPU 显存事件；HAL 事件=本次 dcmi_get_device_hbm_info 查询值，
                               // 池事件=最近一次查询缓存值；-1=未知/查询失败），dump 省略
    uint64_t eventIndex = 0;
    unsigned long long flag = FLAG_INVALID;
    MemOpSpace space;
    int32_t moduleId = -1;
    MemPageType pageType = MemPageType::MEM_MAX_PAGE_TYPE;
    uint64_t kernelIndex;
    bool isShadowEvent = false;  // Shadow event created during NOT_IN_TRACING mode

    MemoryEvent() {}
};

class MemoryOwnerEvent : public EventBase
{
   public:
    std::vector<std::pair<OwnerLevel, std::string>> ownerLabels;  // 地址直标分级标签列表

    MemoryOwnerEvent()
    {
        eventType = EventBaseType::MEMORY_OWNER;
        poolType = PoolType::PTA_CACHING;  // 地址直标仅作用于 PTA 块, 不处理 HAL 等池
    }
};

class OpLaunchEvent : public EventBase
{
   public:
    OpLaunchEvent() {}
};

class KernelLaunchEvent : public EventBase
{
   public:
    std::string streamId;
    std::string taskId;
    uint64_t kernelIndex;

    KernelLaunchEvent() {}
};

class MstxEvent : public EventBase
{
   public:
    uint64_t rangeId = 0;
    int32_t streamId = -1;
    uint64_t stepId = 0;
    uint64_t kernelIndex;

    MstxEvent() {}
};

class SystemEvent : public EventBase
{
   public:
    SystemEvent() {}
};

class CleanUpEvent : public EventBase
{
   public:
    CleanUpEvent() {}

    CleanUpEvent(EventSubType reason, PoolType type, uint64_t pidKey, uint64_t addrKey)
    {
        eventType = EventBaseType::CLEAN_UP;
        eventSubType = reason;
        poolType = type;
        pid = pidKey;
        addr = addrKey;
    }
};

class SnapshotEvent : public EventBase
{
   public:
    uint64_t memory_reserved = 0;
    uint64_t max_memory_reserved = 0;
    uint64_t memory_allocated = 0;
    uint64_t max_memory_allocated = 0;
    uint64_t total_memory = 0;
    uint64_t free_memory = 0;

    SnapshotEvent() {}
};

class OOMTriggerEvent : public EventBase
{
   public:
    uint64_t requestSize = 0;
    uint64_t flag = 0;
    std::string funcName;

    OOMTriggerEvent()
    {
        eventType = EventBaseType::OOM_DETAIL;
        eventSubType = EventSubType::OOM_TRIGGER;
    }
};

class OOMMemRecordEvent : public EventBase
{
   public:
    int64_t memSize = 0;
    uint64_t allocTimestamp = 0;

    OOMMemRecordEvent() { eventType = EventBaseType::OOM_DETAIL; }
};

}  // namespace MemScope

#endif
