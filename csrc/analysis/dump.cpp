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

#include "event_dispatcher.h"
#include "memory_state_manager.h"
#include "constant.h"
#include "event_trace/event_report.h"

namespace MemScope {

Dump& Dump::GetInstance(Config config)
{
    static Dump dump{config};
    return dump;
}

Dump::Dump(Config config)
{
    config_ = config;
    auto func = std::bind(&Dump::EventHandle, this, std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{
        EventBaseType::FREE,
        EventBaseType::MSTX,
        EventBaseType::OP_LAUNCH,
        EventBaseType::KERNEL_LAUNCH,
        EventBaseType::SYSTEM,
        EventBaseType::SNAPSHOT,
        EventBaseType::CLEAN_UP};
    EventDispatcher::GetInstance().Subscribe(SubscriberId::DUMP, eventList, EventDispatcher::Priority::Lowest, func);
    return;
}

void Dump::EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    switch (event->eventType) {
        case EventBaseType::FREE:
            if (state) {
                DumpMemoryState(state);
            }
            break;
        case EventBaseType::MSTX:
            if (auto mstxEvent = std::dynamic_pointer_cast<MstxEvent>(event)) {
                DumpMstxEvent(mstxEvent);
            }
            break;
        case EventBaseType::OP_LAUNCH:
            if (auto opLaunchEvent = std::dynamic_pointer_cast<OpLaunchEvent>(event)) {
                DumpOpLaunchEvent(opLaunchEvent);
            }
            break;
        case EventBaseType::KERNEL_LAUNCH:
            if (auto kernelLaunchEvent = std::dynamic_pointer_cast<KernelLaunchEvent>(event)) {
                DumpKernelLaunchEvent(kernelLaunchEvent);
            }
            break;
        case EventBaseType::SYSTEM:
            if (auto systemEvent = std::dynamic_pointer_cast<SystemEvent>(event)) {
                DumpSystemEvent(systemEvent);
            }
            break;
        case EventBaseType::CLEAN_UP:
            if (state) {
                DumpMemoryState(state);
            }
            break;
        case EventBaseType::SNAPSHOT:
            if (auto snapshotEvent = std::dynamic_pointer_cast<SnapshotEvent>(event)) {
                DumpSnapshotEvent(snapshotEvent);
            }
            break;
        default:
            break;
    }
}

void Dump::DumpMemoryState(MemoryState* state)
{
    for (auto it = state->events.begin(); it != state->events.end(); it++) {
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
    if (event->eventType != EventBaseType::ACCESS
        && event->eventSubType != EventSubType::HAL
        && event->eventSubType != EventSubType::HOST) {
        attr += "total:" + std::to_string(event->total) + ",";
        attr += "used:" + std::to_string(event->used) + ",";
    }
    if (event->eventType == EventBaseType::ACCESS) {
        if (PoolTypeMap.find(event->poolType) != PoolTypeMap.end()) {
            attr += "type:" + PoolTypeMap.at(event->poolType) + ",";
        }
        attr += event->attr + ",";
    }
    if (event->eventType == EventBaseType::MALLOC
        && !(state->memscopeDefinedOwner.empty() && state->userDefinedOwner.empty())) {
        attr += "owner:" + state->memscopeDefinedOwner + state->userDefinedOwner + ",";
    }
    if (event->eventType == EventBaseType::MALLOC && !state->inefficientType.empty()) {
        attr += "inefficient_type:" + state->inefficientType + ",";
    }
    if (event->eventType == EventBaseType::MALLOC && event->eventSubType == EventSubType::HAL) {
        std::string pageType = event->pageType == MemPageType::MEM_GIANT_PAGE_TYPE ? "giant" :
            event->pageType == MemPageType::MEM_HUGE_PAGE_TYPE ? "huge" : "normal";
        attr += "page_type:" + pageType + ",";
        attr += event->flag != FLAG_INVALID ? "alloc_type:alloc," : "alloc_type:create,";
    }
    if (!attr.empty() && attr.back() == ',') {
        attr.pop_back(); 
    }
    event->attr = "\"{" + attr + "}\"";

    WriteToFile(event);
}

void Dump::DumpMstxEvent(std::shared_ptr<MstxEvent>& event)
{
    WriteToFile(event);
}

void Dump::DumpOpLaunchEvent(std::shared_ptr<OpLaunchEvent>& event)
{
    // 组装attr
    if (!event->attr.empty()) {
        event->attr = "\"{" + event->attr + "}\"";
    }

    WriteToFile(event);
}

void Dump::DumpKernelLaunchEvent(std::shared_ptr<KernelLaunchEvent>& event)
{
    // 组装attr
    std::string attr;
    if (event->eventSubType == EventSubType::ATB_KERNEL_START || event->eventSubType == EventSubType::ATB_KERNEL_END) {
        attr = event->attr;
    } else {
        attr += "streamId:" + event->streamId + ",";
        attr += "taskId:" + event->taskId;
    }
    event->attr = "\"{" + attr + "}\"";

    WriteToFile(event);
}

void Dump::DumpSystemEvent(std::shared_ptr<SystemEvent>& event)
{
    // 在开始采集数据之前，落盘一次设备显存信息供可视化
    if (event->eventSubType == EventSubType::TRACE_START) {
        size_t freeMem = 0;
        size_t totalMem = 0;
        if (GetDeviceInfo::Instance().GetDeviceMemInfo(freeMem, totalMem)) {
            std::string attr = "free:" + std::to_string(freeMem) + ",total:" + std::to_string(totalMem);
            event->attr = "\"{" + attr + "}\"";
        }
    }
    WriteToFile(event);
}

void Dump::WriteToFile(const std::shared_ptr<EventBase>& event)
{
    if (event->device == GD_INVALID_NUM) {
        sharedEventLists_.push_back(event);
        return ;
    }
    auto it = handlerMap_.find(event->device);
    if (it == handlerMap_.end()) {
        handlerMap_.insert({event->device, MakeDataHandler(config_, DataType::MEMORY_EVENT, event->device)});
    }
    // 如果是db文件，需要获取设备级锁;csv暂不需要
    if (config_.dataFormat == static_cast<uint8_t>(DataFormat::DB)) {
        auto& lock = Utility::FileWriteManager::GetInstance().GetLock(event->device);
        std::lock_guard<std::mutex> lock_guard(lock);
        handlerMap_[event->device]->Write(event);
    } else {
        handlerMap_[event->device]->Write(event);
    }
}

// 每一次结束采集,都需要将所有文件都存在的公共事件,写回到每个文件中,防止落盘文件缺失
// 这里不涉及trace文件，只涉及memory_event,这里无需加锁
void Dump::WritePublicEventToFile()
{
    for (auto& event : sharedEventLists_) {
        for (auto& handler : handlerMap_) {
            handler.second->Write(event);
        }
    }
    sharedEventLists_.clear();
}

void Dump::DumpSnapshotEvent(std::shared_ptr<SnapshotEvent>& snapshotEvent)
{
    if (!snapshotEvent) {
        return;
    }

    // 计算利用率
    double device_memory_usage_rate = 0.0;
    if (snapshotEvent->total_memory > 0) {
        uint64_t used_memory = snapshotEvent->total_memory - snapshotEvent->free_memory;
        device_memory_usage_rate = (static_cast<double>(used_memory) / snapshotEvent->total_memory) * 100;
    }

    double torch_reserved_memory_usage_rate = 0.0;
    if (snapshotEvent->memory_reserved > 0) {
        torch_reserved_memory_usage_rate = (static_cast<double>(snapshotEvent->memory_allocated) / snapshotEvent->memory_reserved) * 100;
    }

    // 构建attr字符串，格式：{memory_reserved: ,max_memory_reserved:, memory_allocated:,max_memory_allocated:,total_memory:,free_memory:}
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

void Dump::FflushEventToFile() const
{
    // 刷新数据缓冲区数据,同步到落盘文件中,防止缺失
    std::cout << "[msmemscope] Info: Fflush temporary cache events to file!" << std::endl;
    for (auto& handler : handlerMap_) {
        handler.second->FflushFile();
    }
}

Dump::~Dump()
{
    WritePublicEventToFile();
    FflushEventToFile();
}

}