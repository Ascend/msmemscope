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
 
#include <cstdint>
#include <string>
#include <tuple>
#include <atomic>
 
#include "data.h"
#include "state_manager.h"
#include "record_info.h"
#include "ustring.h"
#include "utils.h"
#include "log.h"
 
namespace MemScope {
 
enum class EventBaseType : uint8_t {
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
    INVALID,
};
 
enum class EventSubType : uint8_t {
    PTA_CACHING = 0,
    PTA_WORKSPACE,
    ATB,
    MINDSPORE,
    HAL,
    HOST,
 
    ATB_READ,
    ATB_WRITE,
    ATB_READ_OR_WRITE,
 
    ATEN_READ,
    ATEN_WRITE,
    ATEN_READ_OR_WRITE,
 
    DESCRIBE_OWNER,
    TORCH_OPTIMIZER_STEP_OWNER,
 
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
 
    CLEAN_UP,
 
    STEP,

    SNAPSHOT,

    INVALID,
};

 
class EventBase : public DataBase {
public:
    MemScope::PoolType poolType = PoolType::INVALID;
    EventBaseType eventType = EventBaseType::INVALID;
    EventSubType eventSubType = EventSubType::INVALID;
    uint64_t id = 0;
    uint64_t timestamp = 0;
    uint64_t pid = 0;
    uint64_t tid = 0;
    uint64_t addr = 0;
    int32_t device;
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
 
class MemoryEvent : public EventBase {
public:
    int64_t size = 0;
    int64_t total = 0;
    int64_t used = 0;
    uint64_t eventIndex = 0;
    unsigned long long flag = FLAG_INVALID;
    MemOpSpace space;
    int32_t moduleId = -1;
    MemPageType pageType = MemPageType::MEM_MAX_PAGE_TYPE;
    std::string describeOwner;
    uint64_t kernelIndex;
 
    MemoryEvent() {}
};
 
class MemoryOwnerEvent : public EventBase {
public:
    std::string owner;
 
    MemoryOwnerEvent()
    {
        eventType = EventBaseType::MEMORY_OWNER;
        poolType = PoolType::PTA_CACHING;
    }
};
 
class OpLaunchEvent : public EventBase {
public:
    OpLaunchEvent() {}
};
 
class KernelLaunchEvent : public EventBase {
public:
    std::string streamId;
    std::string taskId;
    uint64_t kernelIndex;
 
    KernelLaunchEvent() {}
};
 
class MstxEvent : public EventBase {
public:
    uint64_t rangeId = 0;
    int32_t streamId = -1;
    uint64_t stepId = 0;
    uint64_t kernelIndex;
 
    MstxEvent() {}
};
 
class SystemEvent : public EventBase {
public:
    SystemEvent() {}
};
 
class CleanUpEvent : public EventBase {
public:
    CleanUpEvent() {}
 
    CleanUpEvent(PoolType type, uint64_t pidKey, uint64_t addrKey)
    {
        eventType = EventBaseType::CLEAN_UP;
        eventSubType = EventSubType::CLEAN_UP;
        poolType = type;
        pid = pidKey;
        addr = addrKey;
    }
};

class SnapshotEvent : public EventBase {
public:
    uint64_t memory_reserved = 0;
    uint64_t max_memory_reserved = 0;
    uint64_t memory_allocated = 0;
    uint64_t max_memory_allocated = 0;
    uint64_t total_memory = 0;
    uint64_t free_memory = 0;

    SnapshotEvent() {}
};
 
}
 
#endif