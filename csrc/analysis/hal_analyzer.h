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

#ifndef HAL_ANALYZER_H
#define HAL_ANALYZER_H

#include <unordered_map>
#include "constant.h"
#include "record_info.h"
#include "config_info.h"
#include "comm_def.h"

namespace MemScope {
/*
 * HalAnalyzer类主要功能：
 * 1. 维护halmemalloc/halmemfree操作记录表
   2. 分析hal侧内存使用问题，泄漏问题
*/

enum class AddrStatus : uint8_t {
    FREE_ALREADY = 0U,
    FREE_WAIT,
};

struct HalMemInfo {
    int32_t deviceId;
    AddrStatus addrStatus;
};

using MemoryRecordTable = std::unordered_map<uint64_t, HalMemInfo>;

class HalAnalyzer {
public:
    static HalAnalyzer& GetInstance(Config config);
    bool Record(const ClientId &clientId, std::shared_ptr<const EventBase> event);
private:
    explicit HalAnalyzer(Config config);
    ~HalAnalyzer();
    HalAnalyzer(const HalAnalyzer&) = delete;
    HalAnalyzer& operator=(const HalAnalyzer&) = delete;
    HalAnalyzer(HalAnalyzer&& other) = delete;
    HalAnalyzer& operator=(HalAnalyzer&& other) = delete;

    std::unordered_map<ClientId, MemoryRecordTable> memtables_{};
    bool IsHalAnalysisEnable();
    bool CreateMemTables(const ClientId &clientId);
    void RecordMalloc(const ClientId &clientId, std::shared_ptr<const EventBase> event);
    void RecordFree(const ClientId &clientId, std::shared_ptr<const EventBase> event);
    void LeakAnalyze();
    void CheckLeak(const size_t clientId);
    Config config_;
};

}

#endif