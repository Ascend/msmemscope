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

#include "decompose_analyzer.h"

#include <string>

#include "constant.h"
#include "event_dispatcher.h"

namespace MemScope
{

const std::string DecomposeAnalyzer::cannStr = "CANN";
const std::string DecomposeAnalyzer::ptaStr = "PTA";
const std::string DecomposeAnalyzer::ptaWorkspaceStr = "PTA_WORKSPACE";
const std::string DecomposeAnalyzer::atbStr = "ATB";
const std::string DecomposeAnalyzer::mindsporeStr = "MINDSPORE";
const size_t DecomposeAnalyzer::ptaStrLen = DecomposeAnalyzer::ptaStr.length();

const std::string DecomposeAnalyzer::atenStr = "@ops@aten";

DecomposeAnalyzer& DecomposeAnalyzer::GetInstance()
{
    static DecomposeAnalyzer analyzer{};
    return analyzer;
}

DecomposeAnalyzer::DecomposeAnalyzer() { DecomposeAnalyzer::Subscribe(); }

void DecomposeAnalyzer::EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    // Skip shadow/historical events
    if (auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event))
    {
        if (memEvent->isShadowEvent)
        {
            return;
        }
    }

    if (event->eventType == EventBaseType::MALLOC)
    {
        auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event);
        if (memEvent != nullptr && state != nullptr)
        {
            InitOwner(memEvent, state);
        }
    }
    else if (event->eventType == EventBaseType::ACCESS)
    {
        auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event);
        if (memEvent != nullptr && state != nullptr)
        {
            UpdateOwnerByAtenAccess(memEvent, state);
        }
    }
    else if (event->eventType == EventBaseType::MEMORY_OWNER)
    {
        auto memOwnerEvent = std::dynamic_pointer_cast<MemoryOwnerEvent>(event);
        if (memOwnerEvent != nullptr && state != nullptr)
        {
            UpdateOwner(memOwnerEvent, state);
        }
    }
}

void DecomposeAnalyzer::InitOwner(std::shared_ptr<MemoryEvent>& event, MemoryState* state)
{
    switch (event->eventSubType)
    {
        case EventSubType::HAL:
        {
            auto it = MODULE_HASH_TABLE.find(event->moduleId);
            if (it != MODULE_HASH_TABLE.end())
            {
                state->memscopeDefinedOwner = cannStr + "@" + it->second;
            }
            else
            {
                state->memscopeDefinedOwner = cannStr + "@UNKNOWN";
            }
            state->userDefinedOwner = event->describeOwner;
            break;
        }
        case EventSubType::PTA_CACHING:
        {
            state->memscopeDefinedOwner = ptaStr;
            state->userDefinedOwner = event->describeOwner;
            break;
        }
        case EventSubType::PTA_WORKSPACE:
        {
            state->memscopeDefinedOwner = ptaWorkspaceStr;
            state->userDefinedOwner = event->describeOwner;
            break;
        }
        case EventSubType::MINDSPORE:
        {
            state->memscopeDefinedOwner = mindsporeStr;
            state->userDefinedOwner = event->describeOwner;
            break;
        }
        case EventSubType::ATB:
        {
            state->memscopeDefinedOwner = atbStr;
            state->userDefinedOwner = event->describeOwner;
            break;
        }
        default:
            break;
    }
}

void DecomposeAnalyzer::UpdateOwnerByAtenAccess(std::shared_ptr<MemoryEvent>& event, MemoryState* state)
{
    if (event->eventSubType != EventSubType::ATEN_READ && event->eventSubType != EventSubType::ATEN_WRITE &&
        event->eventSubType != EventSubType::ATEN_READ_OR_WRITE)
    {
        return;
    }

    if (state->memscopeDefinedOwner.rfind(ptaStr, 0) != 0)
    {
        return;
    }

    if (state->memscopeDefinedOwner.length() == ptaStrLen)
    {
        state->memscopeDefinedOwner += atenStr;
    }
}

void DecomposeAnalyzer::UpdateOwner(std::shared_ptr<MemoryOwnerEvent>& event, MemoryState* state)
{
    if (event->eventSubType == EventSubType::DESCRIBE_OWNER && !(event->owner).empty())
    {
        state->userDefinedOwner += event->owner;
    }
    else if (event->eventSubType == EventSubType::TORCH_OPTIMIZER_STEP_OWNER && !(event->owner).empty())
    {
        if (state->memscopeDefinedOwner.rfind(ptaStr, 0) != 0)
        {
            return;
        }

        if (state->memscopeDefinedOwner.length() == ptaStrLen)
        {
            state->memscopeDefinedOwner += event->owner;
        }
        else if (event->owner != atenStr)
        {
            // 部分内存有可能先作为算子操作的内容，然后被识别为其他类型，如weight，
            // 则优先用weight覆盖aten，而aten不能覆盖其他类型
            state->memscopeDefinedOwner = ptaStr + event->owner;
        }
    }
}

DecomposeAnalyzer::~DecomposeAnalyzer() { UnSubscribe(); }

void DecomposeAnalyzer::Subscribe()
{
    auto func = std::bind(&DecomposeAnalyzer::EventHandle, this, std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::MEMORY_OWNER};
    EventDispatcher::GetInstance().Subscribe(SubscriberId::DECOMPOSE_ANALYZER, eventList,
                                             EventDispatcher::Priority::High, func);
}

void DecomposeAnalyzer::UnSubscribe() const
{
    EventDispatcher::GetInstance().UnSubscribe(SubscriberId::DECOMPOSE_ANALYZER);
}

}  // namespace MemScope
