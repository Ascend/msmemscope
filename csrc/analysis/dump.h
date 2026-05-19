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

#ifndef DUMP_H
#define DUMP_H

#include <unordered_map>
#include <mutex>

#include "analyzer_base.h"
#include "config_info.h"
#include "data_handler.h"
#include "file_write_manager.h"

namespace MemScope {

class Dump : public AnalyzerBase {
public:
    static Dump& GetInstance(Config config);
    void EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state) override;
    void WritePublicEventToFile();
    void FflushEventToFile() const;
private:
    explicit Dump(Config config);
    ~Dump() override;
    Dump(const Dump&) = delete;
    Dump& operator=(const Dump&) = delete;
    Dump(Dump&& other) = delete;
    Dump& operator=(Dump&& other) = delete;

    void DumpMemoryState(MemoryState* state);
    void DumpMemoryEvent(std::shared_ptr<MemoryEvent>& event, MemoryState* state);

    void DumpMstxEvent(std::shared_ptr<MstxEvent>& event);
    void DumpOpLaunchEvent(std::shared_ptr<OpLaunchEvent>& event);
    void DumpKernelLaunchEvent(std::shared_ptr<KernelLaunchEvent>& event);
    void DumpSystemEvent(std::shared_ptr<SystemEvent>& event);
    void DumpSnapshotEvent(std::shared_ptr<SnapshotEvent>& snapshotEvent);

    void WriteToFile(const std::shared_ptr<EventBase>& event);

    Config config_;
    std::unordered_map<int32_t, std::unique_ptr<DataHandler>> handlerMap_;
    std::vector<std::shared_ptr<EventBase>> sharedEventLists_;
};

}

#endif