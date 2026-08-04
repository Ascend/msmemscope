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

#include "oom_detailed_analyzer.h"

#include "bit_field.h"
#include "utility/utils.h"

namespace MemScope
{

OOMDetailedAnalyzer& OOMDetailedAnalyzer::GetInstance(Config config)
{
    static OOMDetailedAnalyzer instance(config);
    return instance;
}

bool OOMDetailedAnalyzer::IsEnabled() const
{
    BitField<decltype(config_.analysisType)> analysisType(config_.analysisType);
    return analysisType.checkBit(static_cast<size_t>(AnalysisType::OOM_ANALYSIS));
}

bool OOMDetailedAnalyzer::ShouldDumpDetails()
{
    uint64_t now = Utility::GetTimeNanoseconds();
    if (now - lastDetailDumpTimestamp_ < kDetailDumpIntervalNs)
    {
        return false;
    }
    lastDetailDumpTimestamp_ = now;
    return true;
}

std::vector<OOMMemRecord> OOMDetailedAnalyzer::QueryRecentAllocs(int32_t deviceId, uint32_t clientId)
{
    std::vector<OOMMemRecord> allRecords;

    auto npuRecords = StepInnerAnalyzer::GetInstance().QueryUnfreedRecords(deviceId);
    allRecords.insert(allRecords.end(), npuRecords.begin(), npuRecords.end());

    auto halRecords = HalAnalyzer::GetInstance().QueryUnfreedRecords(clientId);
    allRecords.insert(allRecords.end(), halRecords.begin(), halRecords.end());

    uint16_t k = config_.oomTopK;
    if (k > allRecords.size())
    {
        k = static_cast<uint16_t>(allRecords.size());
    }
    std::partial_sort(allRecords.begin(), allRecords.begin() + k, allRecords.end(),
                      [](const OOMMemRecord& a, const OOMMemRecord& b) { return a.allocTimestamp > b.allocTimestamp; });
    allRecords.resize(k);
    return allRecords;
}

std::vector<OOMMemRecord> OOMDetailedAnalyzer::QueryTopAllocs(int32_t deviceId, uint32_t clientId)
{
    std::vector<OOMMemRecord> allRecords;

    auto npuRecords = StepInnerAnalyzer::GetInstance().QueryUnfreedRecords(deviceId);
    allRecords.insert(allRecords.end(), npuRecords.begin(), npuRecords.end());

    auto halRecords = HalAnalyzer::GetInstance().QueryUnfreedRecords(clientId);
    allRecords.insert(allRecords.end(), halRecords.begin(), halRecords.end());

    uint16_t k = config_.oomTopK;
    if (k > allRecords.size())
    {
        k = static_cast<uint16_t>(allRecords.size());
    }
    std::partial_sort(allRecords.begin(), allRecords.begin() + k, allRecords.end(),
                      [](const OOMMemRecord& a, const OOMMemRecord& b) { return a.memSize > b.memSize; });
    allRecords.resize(k);
    return allRecords;
}

}  // namespace MemScope
