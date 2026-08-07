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
#ifndef ATEN_MANAGER_H
#define ATEN_MANAGER_H

#include <cstdint>
#include <iostream>

#include "bit_field.h"
#include "event_report.h"
#include "memory_watch/memory_watch.h"
#include "memory_watch/tensor_monitor.h"
#include "utils.h"

namespace MemScope
{

struct AtenAccessTensorInfo
{
    std::string addr;
    std::string dtype;
    std::string shape;
    std::string size;
    std::string name;
    std::string isWrite;
    std::string isRead;
    std::string isOutput;
};

class AtenManager
{
   public:
    static AtenManager& GetInstance();
    void ProcessMsg(const char* msg, int32_t streamId);
    AtenManager();

   private:
    ~AtenManager() = default;
    AtenManager(const AtenManager&) = delete;
    AtenManager& operator=(const AtenManager&) = delete;
    AtenManager(AtenManager&& other) = delete;
    AtenManager& operator=(AtenManager&& other) = delete;

    bool ExtractTensorInfo(const std::string& msg, const std::string& key, std::string& value);
    void ExtractTensorFields(const std::string& msg, AtenAccessTensorInfo& info);
    void ReportAtenLaunch(const char* msg, int32_t streamId, bool isAtenBegin);
    void ReportAtenAccess(const char* msg, int32_t streamId);
    bool IsFirstWatchedOp(const char* name);
    bool IsLastWatchedOp(const char* name);

   private:
    std::vector<MonitoredTensor> outputTensors_ = {};
    bool isWatchEnable_ = false;
    bool isfirstWatchOpSet_ = false;
    std::string firstWatchOp_ = {};
    std::string lastWatchOp_ = {};
};

}  // namespace MemScope

#endif
