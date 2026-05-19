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
 
#ifndef TRACE_EVENT_H
#define TRACE_EVENT_H

#include <cstdint>
#include <string>
#include "data.h"

namespace MemScope {

class TraceEvent : public DataBase {
public:
    TraceEvent() : DataBase(DataType::PYTHON_TRACE_EVENT) {}
    TraceEvent(const TraceEvent&) = default;
    TraceEvent& operator=(const TraceEvent&) = default;
    TraceEvent(
        uint64_t startTs,
        uint64_t endTs,
        uint64_t tid,
        uint64_t pid,
        int32_t device,
        const std::string& info,
        const std::string& hash
    ) : DataBase(DataType::PYTHON_TRACE_EVENT), startTs(startTs), endTs(endTs),
        tid(tid), pid(pid), device(device), info(info), hash(hash) {}

    uint64_t startTs = 0;
    uint64_t endTs = 0;
    uint64_t tid;
    uint64_t pid;
    int32_t device;
    std::string info;
    std::string hash;
};

}

#endif