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

#ifndef OOM_DETAILED_ANALYZER_H
#define OOM_DETAILED_ANALYZER_H

#include <algorithm>
#include <vector>

#include "config_info.h"
#include "hal_analyzer.h"
#include "record_info.h"
#include "stepinner_analyzer.h"

namespace MemScope
{

class OOMDetailedAnalyzer
{
   public:
    static OOMDetailedAnalyzer& GetInstance(Config config);
    bool IsEnabled() const;
    bool ShouldDumpDetails();
    std::vector<OOMMemRecord> QueryRecentAllocs(int32_t deviceId, uint32_t clientId);
    std::vector<OOMMemRecord> QueryTopAllocs(int32_t deviceId, uint32_t clientId);

   private:
    explicit OOMDetailedAnalyzer(Config config) : config_(config) {}
    ~OOMDetailedAnalyzer() = default;
    OOMDetailedAnalyzer(const OOMDetailedAnalyzer&) = delete;
    OOMDetailedAnalyzer& operator=(const OOMDetailedAnalyzer&) = delete;
    OOMDetailedAnalyzer(OOMDetailedAnalyzer&& other) = delete;
    OOMDetailedAnalyzer& operator=(OOMDetailedAnalyzer&& other) = delete;

    Config config_;
    uint64_t lastDetailDumpTimestamp_ = 0;
    static constexpr uint64_t kDetailDumpIntervalNs = 2000000000ULL;  // 2s 内重复OOM不重复dump详情
};

}  // namespace MemScope

#endif
