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

#ifndef PY_STEP_MANAGER_H
#define PY_STEP_MANAGER_H

#include <unordered_map>
#include <mutex>
#include <functional>
#include "record_info.h"
#include "comm_def.h"

namespace MemScope {
/*
 * PyStepManager类主要功能：
 * 1. 注册观察者，提醒观察者
 * 2. 标识打点信息
*/

using DeviceId = int32_t;
using PyStepEventCallBackFunc = std::function<void(std::shared_ptr<const EventBase>)>;

enum class PyStepEventSubscriber : uint8_t {
    STEP_INNER_ANALYZER = 0,
};

class PyStepManager {
public:
    static PyStepManager& Instance();
    void RecordPyStep(const ClientId &clientId, std::shared_ptr<const EventBase> event);
    void Subscribe(const PyStepEventSubscriber &subscriber, const PyStepEventCallBackFunc &func);
    void UnSubscribe(const PyStepEventSubscriber &subscriber);
private:
    PyStepManager() = default;
    ~PyStepManager() = default;

    PyStepManager(const PyStepManager&) = delete;
    PyStepManager& operator=(const PyStepManager&) = delete;
    PyStepManager(PyStepManager&&) = delete;
    PyStepManager& operator=(PyStepManager&&) = delete;

    void Notify(std::shared_ptr<const EventBase> event);
    std::mutex pyStepMutex_;
    std::unordered_map<PyStepEventSubscriber, PyStepEventCallBackFunc> subscriberList_;
};

}

#endif