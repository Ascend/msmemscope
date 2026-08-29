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

#include "dump.h"

#include "bit_field.h"
#include "constant.h"
#include "event_dispatcher.h"
#include "event_trace/event_report.h"
#include "memory_state_manager.h"

namespace MemScope
{

Dump& Dump::GetInstance()
{
    static Dump dump;
    return dump;
}

Dump::Dump()
{
    // 确保 FileWriteManager 先于 Dump 构造，从而在 Dump 析构时 FileWriteManager 仍然存活
    Utility::FileWriteManager::GetInstance();
    auto func = std::bind(&Dump::EventHandle, this, std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{
        EventBaseType::FREE,   EventBaseType::MSTX,     EventBaseType::OP_LAUNCH, EventBaseType::KERNEL_LAUNCH,
        EventBaseType::SYSTEM, EventBaseType::SNAPSHOT, EventBaseType::CLEAN_UP,  EventBaseType::OOM_DETAIL};
    EventDispatcher::GetInstance().Subscribe(SubscriberId::DUMP, eventList, EventDispatcher::Priority::Lowest, func);
    return;
}

void Dump::EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    switch (event->eventType)
    {
        case EventBaseType::FREE:
            if (state)
            {
                DumpMemoryState(state);
            }
            break;
        case EventBaseType::MSTX:
            if (auto mstxEvent = std::dynamic_pointer_cast<MstxEvent>(event))
            {
                DumpMstxEvent(mstxEvent);
            }
            break;
        case EventBaseType::OP_LAUNCH:
            if (auto opLaunchEvent = std::dynamic_pointer_cast<OpLaunchEvent>(event))
            {
                DumpOpLaunchEvent(opLaunchEvent);
            }
            break;
        case EventBaseType::KERNEL_LAUNCH:
            if (auto kernelLaunchEvent = std::dynamic_pointer_cast<KernelLaunchEvent>(event))
            {
                DumpKernelLaunchEvent(kernelLaunchEvent);
            }
            break;
        case EventBaseType::SYSTEM:
            if (auto systemEvent = std::dynamic_pointer_cast<SystemEvent>(event))
            {
                DumpSystemEvent(systemEvent);
            }
            break;
        case EventBaseType::CLEAN_UP:
            if (state)
            {
                DumpMemoryState(state);
            }
            break;
        case EventBaseType::SNAPSHOT:
            if (auto snapshotEvent = std::dynamic_pointer_cast<SnapshotEvent>(event))
            {
                DumpSnapshotEvent(snapshotEvent);
            }
            break;
        case EventBaseType::OOM_DETAIL:
            if (event->eventSubType == EventSubType::OOM_TRIGGER)
            {
                DumpOOMTriggerEvent(std::static_pointer_cast<OOMTriggerEvent>(event));
            }
            else
            {
                DumpOOMMemRecordEvent(std::static_pointer_cast<OOMMemRecordEvent>(event));
            }
            break;
        default:
            break;
    }
}

void Dump::DumpMemoryState(MemoryState* state)
{
    for (auto it = state->events.begin(); it != state->events.end(); it++)
    {
        DumpMemoryEvent(*it, state);
    }
}

void Dump::DumpMemoryEvent(std::shared_ptr<MemoryEvent>& event, MemoryState* state)
{
    // 组装attr
    std::string attr;
    attr += "allocation_id:" + std::to_string(state->allocationId) + ",";
    attr += "addr:" + Uint64ToHexString(event->addr) + ",";
    attr += "size:" + std::to_string(event->size) + ",";
    // 统计键按事件类别输出（值<0 表示无统计值，对应字段直接省略）：
    // 合成事件（影子转正/虚拟释放）isShadowEvent=true，无事件时刻的累计/查询语义，不输出统计键
    // 锁页内存与CPU tensor数据内存统一为HOST事件类型（poolType=HOST），按 isPinned 区分：
    // 锁页内存 isPinned=true，标记pinned:true；CPU tensor数据内存 isPinned=false，输出total字段
    if (!event->isShadowEvent && event->eventType != EventBaseType::ACCESS)
    {
        if (event->poolType == PoolType::HAL)
        {
            // HAL事件（DEVICE空间）：本进程HAL维度使用量（used，MSM累计回填）
            // + 本进程用量（process_used，本次dcmi_get_npu_proc_mem_info查询值；查询失败/未知时省略）
            // + 整卡用量（device_used，本次dcmi_get_device_hbm_info查询值；查询失败/未知时省略）
            attr += "used:" + std::to_string(event->used) + ",";
            if (event->processUsed >= 0)
            {
                attr += "process_used:" + std::to_string(event->processUsed) + ",";
            }
            if (event->deviceUsed >= 0)
            {
                attr += "device_used:" + std::to_string(event->deviceUsed) + ",";
            }
        }
        else if (event->poolType == PoolType::HOST && event->isPinned)  // 锁页内存（pinned）
        {
            attr += "pinned:true,";
            attr += "used:" + std::to_string(event->used) + ",";  // host块活跃累计（原VmRSS语义迁移至process_used）
            if (event->processUsed >= 0)
            {
                attr += "process_used:" + std::to_string(event->processUsed) + ",";  // 进程VmRSS
            }
        }
        else if (event->poolType == PoolType::HOST)  // CPU tensor数据内存
        {
            attr += "total:" + std::to_string(event->total) + ",";  // 活跃CPU tensor数据内存累计
        }
        else if (IsMemoryPool(event->poolType))  // PTA_CACHING/PTA_WORKSPACE/ATB/MINDSPORE
        {
            attr += "used:" + std::to_string(event->used) + ",";  // 池内已分配（totalAllocated，报告时已填）
            attr += "total:" + std::to_string(event->total) + ",";  // 池总大小（totalReserved，键名/值不变）
            if (event->processUsed >= 0)
            {
                attr += "process_used:" + std::to_string(event->processUsed) +
                        ",";  // 本进程用量（最近一次dcmi_get_npu_proc_mem_info查询缓存值）
            }
            if (event->deviceUsed >= 0)
            {
                attr += "device_used:" + std::to_string(event->deviceUsed) + ",";  // 整卡用量（最近一次HAL查询缓存值）
            }
        }
    }

    if (event->isShadowEvent)
    {
        attr += "shadow:true,";
    }

    if (event->eventType == EventBaseType::ACCESS)
    {
        if (PoolTypeMap.find(event->poolType) != PoolTypeMap.end())
        {
            attr += "type:" + PoolTypeMap.at(event->poolType) + ",";
        }
        attr += event->attr + ",";
    }
    if (event->eventType == EventBaseType::MALLOC)
    {
        // 分级标签按级别拼接(框架@组件@流程@细化, 跳过空值)
        std::string ownerStr = state->owner.GetOwnerStr();
        if (!ownerStr.empty())
        {
            attr += "owner:" + ownerStr + ",";
        }
    }
    if (event->eventType == EventBaseType::MALLOC && !state->inefficientType.empty())
    {
        attr += "inefficient_type:" + state->inefficientType + ",";
    }
    if (event->eventType == EventBaseType::MALLOC && event->eventSubType == EventSubType::HAL)
    {
        std::string pageType = event->pageType == MemPageType::MEM_GIANT_PAGE_TYPE  ? "giant"
                               : event->pageType == MemPageType::MEM_HUGE_PAGE_TYPE ? "huge"
                                                                                    : "normal";
        attr += "page_type:" + pageType + ",";
        attr += event->flag != FLAG_INVALID ? "alloc_type:alloc," : "alloc_type:create,";
    }
    if (!attr.empty() && attr.back() == ',')
    {
        attr.pop_back();
    }
    event->attr = "\"{" + attr + "}\"";

    WriteToFile(event);
}

void Dump::DumpMstxEvent(std::shared_ptr<MstxEvent>& event) { WriteToFile(event); }

void Dump::DumpOpLaunchEvent(std::shared_ptr<OpLaunchEvent>& event)
{
    // 组装attr
    if (!event->attr.empty())
    {
        event->attr = "\"{" + event->attr + "}\"";
    }

    WriteToFile(event);
}

void Dump::DumpKernelLaunchEvent(std::shared_ptr<KernelLaunchEvent>& event)
{
    // 组装attr
    std::string attr;
    if (event->eventSubType == EventSubType::ATB_KERNEL_START || event->eventSubType == EventSubType::ATB_KERNEL_END)
    {
        attr = event->attr;
    }
    else
    {
        attr += "streamId:" + event->streamId + ",";
        attr += "taskId:" + event->taskId;
    }
    event->attr = "\"{" + attr + "}\"";

    WriteToFile(event);
}

void Dump::DumpSystemEvent(std::shared_ptr<SystemEvent>& event)
{
    // 在开始采集数据之前，落盘一次设备显存信息供可视化
    if (event->eventSubType == EventSubType::TRACE_START)
    {
        // dcmi 数据源（与事件 device_used 同源）；SystemEvent 无设备维度，记录 0 号设备
        uint64_t usedMb = 0;
        uint64_t totalMb = 0;
        if (GetDeviceInfo::Instance().GetDeviceHbmInfo(0, usedMb, totalMb))
        {
            // 保持 free/total 键语义：free = total - used，字节单位
            uint64_t freeBytes = (totalMb - usedMb) * 1024 * 1024;
            uint64_t totalBytes = totalMb * 1024 * 1024;
            std::string attr = "free:" + std::to_string(freeBytes) + ",total:" + std::to_string(totalBytes);
            event->attr = "\"{" + attr + "}\"";
        }
    }
    WriteToFile(event);
}

bool Dump::ShouldDumpEvent(EventBaseType type, const Config& config) const
{
    // 对于无法映射到用户可配 EventType 的事件类型（MSTX, SYSTEM, SNAPSHOT, CLEAN_UP, MEMORY_OWNER），始终落盘
    switch (type)
    {
        case EventBaseType::MALLOC:
            return BitField<decltype(config.dumpEventType)>(config.dumpEventType)
                .checkBit(static_cast<size_t>(EventType::ALLOC_EVENT));
        case EventBaseType::FREE:
            return BitField<decltype(config.dumpEventType)>(config.dumpEventType)
                .checkBit(static_cast<size_t>(EventType::FREE_EVENT));
        case EventBaseType::OP_LAUNCH:
        case EventBaseType::KERNEL_LAUNCH:
            return BitField<decltype(config.dumpEventType)>(config.dumpEventType)
                .checkBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
        case EventBaseType::ACCESS:
            return BitField<decltype(config.dumpEventType)>(config.dumpEventType)
                .checkBit(static_cast<size_t>(EventType::ACCESS_EVENT));
        default:
            // MSTX, SYSTEM, SNAPSHOT, CLEAN_UP, MEMORY_OWNER 等不可控事件始终落盘
            return true;
    }
}

void Dump::WriteToFile(const std::shared_ptr<EventBase>& event)
{
    // 动态读取当前配置（每个事件只取一次锁），保证落盘过滤与运行中修改的配置保持一致
    const Config& config = GetConfig();
    // 落盘前校验事件是否在用户配置的 dumpEventType 范围内
    if (!ShouldDumpEvent(event->eventType, config))
    {
        return;
    }
    if (event->device == GD_INVALID_NUM)
    {
        sharedEventLists_.push_back(event);
        return;
    }
    auto it = handlerMap_.find(event->device);
    if (it == handlerMap_.end())
    {
        handlerMap_.insert({event->device, MakeDataHandler(config, DataType::MEMORY_EVENT, event->device)});
    }
    // 如果是db文件，需要获取设备级锁;csv暂不需要
    if (config.dataFormat == static_cast<uint8_t>(DataFormat::DB))
    {
        auto& lock = Utility::FileWriteManager::GetInstance().GetLock(event->device);
        std::lock_guard<std::mutex> lock_guard(lock);
        handlerMap_[event->device]->Write(event);
    }
    else
    {
        handlerMap_[event->device]->Write(event);
    }
}

// 每一次结束采集,都需要将所有文件都存在的公共事件,写回到每个文件中,防止落盘文件缺失
// 这里不涉及trace文件，只涉及memory_event,这里无需加锁
void Dump::WritePublicEventToFile()
{
    for (auto& event : sharedEventLists_)
    {
        for (auto& handler : handlerMap_)
        {
            handler.second->Write(event);
        }
    }
    sharedEventLists_.clear();
}

void Dump::DumpSnapshotEvent(std::shared_ptr<SnapshotEvent>& snapshotEvent)
{
    if (!snapshotEvent)
    {
        return;
    }

    // 计算利用率
    double device_memory_usage_rate = 0.0;
    if (snapshotEvent->total_memory > 0)
    {
        uint64_t used_memory = snapshotEvent->total_memory - snapshotEvent->free_memory;
        device_memory_usage_rate = (static_cast<double>(used_memory) / snapshotEvent->total_memory) * 100;
    }

    double torch_reserved_memory_usage_rate = 0.0;
    if (snapshotEvent->memory_reserved > 0)
    {
        torch_reserved_memory_usage_rate =
            (static_cast<double>(snapshotEvent->memory_allocated) / snapshotEvent->memory_reserved) * 100;
    }

    // 构建attr字符串，格式：{memory_reserved: ,max_memory_reserved:,
    // memory_allocated:,max_memory_allocated:,total_memory:,free_memory:}
    std::string attr;
    attr += "reserved:" + std::to_string(snapshotEvent->memory_reserved) + ",";
    attr += "max_reserved:" + std::to_string(snapshotEvent->max_memory_reserved) + ",";
    attr += "allocated:" + std::to_string(snapshotEvent->memory_allocated) + ",";
    attr += "max_allocated:" + std::to_string(snapshotEvent->max_memory_allocated) + ",";
    attr += "total:" + std::to_string(snapshotEvent->total_memory) + ",";
    attr += "free:" + std::to_string(snapshotEvent->free_memory) + ",";
    attr += "device_rate:" + std::to_string(device_memory_usage_rate) + ",";
    attr += "reserved_rate:" + std::to_string(torch_reserved_memory_usage_rate);
    snapshotEvent->attr = "\"{" + attr + "}\"";

    // 调用WriteToFile函数写入事件
    WriteToFile(snapshotEvent);
}

void Dump::DumpOOMTriggerEvent(const std::shared_ptr<OOMTriggerEvent>& event)
{
    std::string attr;
    attr += "func:" + event->funcName + ",";
    attr += "req_size:" + std::to_string(event->requestSize) + ",";
    attr += "flag:" + std::to_string(event->flag);
    event->attr = "\"{" + attr + "}\"";

    WriteToFile(event);
}

void Dump::DumpOOMMemRecordEvent(const std::shared_ptr<OOMMemRecordEvent>& event)
{
    std::string attr;
    auto poolIt = PoolTypeMap.find(event->poolType);
    attr += "pool:" + (poolIt != PoolTypeMap.end() ? poolIt->second : "UNKNOWN") + ",";
    attr += "ptr:" + Uint64ToHexString(event->addr) + ",";
    attr += "size:" + std::to_string(event->memSize) + ",";
    attr += "timestamp:" + std::to_string(event->allocTimestamp);
    event->attr = "\"{" + attr + "}\"";

    WriteToFile(event);
}

void Dump::FflushEventToFile() const
{
    // 刷新数据缓冲区数据,同步到落盘文件中,防止缺失
    std::cout << "[msmemscope] Info: Fflush temporary cache events to file!" << std::endl;
    for (auto& handler : handlerMap_)
    {
        handler.second->FflushFile();
    }
}

void Dump::DumpHistoricalState(MemoryState* state) { DumpMemoryState(state); }

Dump::~Dump()
{
    EventDispatcher::GetInstance().UnSubscribe(SubscriberId::DUMP);
    WritePublicEventToFile();
    FflushEventToFile();
}

}  // namespace MemScope
