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

#include "config_info.h"
#include "constant.h"
#include "comm_def.h"
#include "event_dispatcher.h"
#include "memory_state_manager.h"
#include "record_info.h"

namespace MemScope
{
/*
 * HalAnalyzer类主要功能：
 * 1. 维护halmemalloc/halmemfree操作记录表
   2. 分析hal侧内存使用问题，泄漏问题
   3. 通过EventDispatcher订阅MALLOC/FREE事件（替代Process::SendEvent中的switch-case分发）
*/

enum class AddrStatus : uint8_t
{
    FREE_ALREADY = 0U,
    FREE_WAIT,
};

struct HalMemInfo
{
    int32_t deviceId;
    AddrStatus addrStatus;
};

using MemoryRecordTable = std::unordered_map<uint64_t, HalMemInfo>;

class HalAnalyzer
{
   public:
    static HalAnalyzer& GetInstance(Config config);
    void EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state);
    void Subscribe();
    void UnSubscribe() const;

   private:
    explicit HalAnalyzer(Config config);
    ~HalAnalyzer();
    HalAnalyzer(const HalAnalyzer&) = delete;
    HalAnalyzer& operator=(const HalAnalyzer&) = delete;
    HalAnalyzer(HalAnalyzer&& other) = delete;
    HalAnalyzer& operator=(HalAnalyzer&& other) = delete;

    std::unordered_map<ClientId, MemoryRecordTable> memtables_{};
    bool IsHalAnalysisEnable();
    bool CreateMemTables(const ClientId& clientId);
    void RecordMalloc(const ClientId& clientId, std::shared_ptr<const MemoryEvent> memEvent);
    void RecordFree(const ClientId& clientId, std::shared_ptr<const MemoryEvent> memEvent);
    void LeakAnalyze();
    void CheckLeak(const size_t clientId);
    Config config_;
};

}  // namespace MemScope

#endif
