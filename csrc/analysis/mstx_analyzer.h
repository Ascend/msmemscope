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

#ifndef MSTX_ANALYZER_H
#define MSTX_ANALYZER_H

#include <unordered_map>
#include <mutex>
#include <functional>
#include "record_info.h"
#include "comm_def.h"

namespace MemScope {
/*
 * MstxAnalyzer类主要功能：
 * 1. 注册观察者，提醒观察者
 * 2. 标识打点信息
*/

using DeviceId = int32_t;
using MstxEventCallBackFunc = std::function<void(std::shared_ptr<const MstxEvent> mstxEvent)>;

enum class MstxEventSubscriber : uint8_t {
    STEP_INNER_ANALYZER = 0,
};

class MstxAnalyzer {
public:
    static MstxAnalyzer& Instance();
    bool RecordMstx(const ClientId &clientId, std::shared_ptr<const EventBase> event);
    void Subscribe(const MstxEventSubscriber &subscriber, const MstxEventCallBackFunc &func);
    void UnSubscribe(const MstxEventSubscriber &subscriber);
private:
    MstxAnalyzer() = default;
    ~MstxAnalyzer() = default;

    MstxAnalyzer(const MstxAnalyzer&) = delete;
    MstxAnalyzer& operator=(const MstxAnalyzer&) = delete;
    MstxAnalyzer(MstxAnalyzer&&) = delete;
    MstxAnalyzer& operator=(MstxAnalyzer&&) = delete;

    void Notify(std::shared_ptr<const MstxEvent> mstxEvent);
    std::mutex mstxMutex_;
    std::unordered_map<MstxEventSubscriber, MstxEventCallBackFunc> subscriberList_;
};

}

#endif