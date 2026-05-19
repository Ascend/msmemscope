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

#include "event.h"
#include "utility/log.h"
#include "utility/ustring.h"
#include "mstx_analyzer.h"

namespace MemScope {

MstxAnalyzer& MstxAnalyzer::Instance()
{
    static MstxAnalyzer instance;
    return instance;
}

void MstxAnalyzer::Subscribe(const MstxEventSubscriber &subscriber, const MstxEventCallBackFunc &func)
{
    if (subscriberList_.find(subscriber) != subscriberList_.end()) {
        LOG_ERROR("Add elements repeatedly, subscriber : %u", static_cast<uint8_t>(subscriber));
        return;
    }

    subscriberList_.insert({subscriber, func});
    return;
}

void MstxAnalyzer::UnSubscribe(const MstxEventSubscriber &subscriber)
{
    if (subscriberList_.find(subscriber) == subscriberList_.end()) {
        LOG_ERROR("Cannot delete elements, subscriber : %u", static_cast<uint8_t>(subscriber));
        return;
    }

    subscriberList_.erase(subscriber);
    return;
}

void MstxAnalyzer::Notify(std::shared_ptr<const MstxEvent> mstxEvent)
{
    std::lock_guard<std::mutex> lock(mstxMutex_);
    for (auto &subscriber : subscriberList_) {
        if (subscriber.second != nullptr) {
            subscriber.second(mstxEvent);
        }
    }
    
    return;
}

bool MstxAnalyzer::RecordMstx(const ClientId &clientId, std::shared_ptr<const EventBase> event)
{ 
    std::shared_ptr<const MstxEvent> mstxEvent = std::dynamic_pointer_cast<const MstxEvent>(event);
    if (mstxEvent == nullptr) {
        LOG_WARN("[client %u]: MstxAnalyzer receive invalid event.", clientId);
        return false;
    }
    int32_t deviceId = mstxEvent->device;
    uint64_t stepId = mstxEvent->stepId;
    std::string markMessage = mstxEvent->name;
    Utility::ToSafeString(markMessage);
    if (event->eventSubType == EventSubType::MSTX_RANGE_START) {
        LOG_INFO("[npu %d][client %u][stepid %llu][streamid %d][start]: %s",
            deviceId,
            clientId,
            stepId,
            mstxEvent->streamId,
            markMessage.c_str());
        Notify(mstxEvent);
        return true;
    } else if (event->eventSubType == EventSubType::MSTX_RANGE_END) {
        LOG_INFO("[npu %d][client %u][stepid %llu][streamid %d][end]: %s",
            deviceId,
            clientId,
            stepId,
            mstxEvent->streamId,
            markMessage.c_str());
        Notify(mstxEvent);
        return true;
    } else if (event->eventSubType == EventSubType::MSTX_MARK) {
        LOG_INFO("[npu %d][client %u][stepid %llu][streamid %d][mark]: %s",
            deviceId,
            clientId,
            stepId,
            mstxEvent->streamId,
            markMessage.c_str());
        return true;
    }
    return false;
}

}