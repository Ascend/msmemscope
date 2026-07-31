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

#ifndef DECOMPOSE_ANALYZER_H
#define DECOMPOSE_ANALYZER_H

#include "analyzer_base.h"

namespace MemScope
{

class DecomposeAnalyzer : public AnalyzerBase
{
   public:
    static DecomposeAnalyzer& GetInstance();
    void EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state) override;
    void Subscribe();
    void UnSubscribe() const;

   private:
    explicit DecomposeAnalyzer();
    ~DecomposeAnalyzer() override;
    DecomposeAnalyzer(const DecomposeAnalyzer&) = delete;
    DecomposeAnalyzer& operator=(const DecomposeAnalyzer&) = delete;
    DecomposeAnalyzer(DecomposeAnalyzer&& other) = delete;
    DecomposeAnalyzer& operator=(DecomposeAnalyzer&& other) = delete;

    void InitOwner(std::shared_ptr<MemoryEvent>& event, MemoryState* state);
    void UpdateOwnerByAtenAccess(std::shared_ptr<MemoryEvent>& event, MemoryState* state);
    void UpdateOwner(std::shared_ptr<MemoryOwnerEvent>& event, MemoryState* state);

    static const std::string cannStr;
    static const std::string ptaStr;
    static const std::string ptaWorkspaceStr;
    static const std::string atbStr;
    static const std::string mindsporeStr;
    static const size_t ptaStrLen;
    static const std::string atenStr;
};

}  // namespace MemScope

#endif
