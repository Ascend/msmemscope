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
#include "call_stack.h"

#include "config_info.h"
#include "cpython.h"
#include "event_report.h"
#include "utils.h"

namespace Utility
{
void GetCallstack(MemScope::CallStackString& stack)
{
    const auto& config = MemScope::GetConfig();
    if (config.enableCStack)
    {
        Utility::GetCCallstack(config.cStackDepth, stack.cStack, MemScope::SKIP_DEPTH);
    }
    if (config.enablePyStack)
    {
        Utility::GetPythonCallstack(config.pyStackDepth, stack.pyStack);
    }
}

void GetPythonCallstack(uint32_t pyDepth, std::string& pyStack) { PythonCallstack(pyDepth, pyStack); }

void GetCCallstack(uint32_t cDepth, std::string& cStack, uint32_t skip)
{
    cDepth = GetAddResult(cDepth, skip);
    if (cDepth == skip)
    {
        return;
    }
    void* buffer[cDepth] = {0};

    int numEntries = backtrace(buffer, cDepth);
    char** symbols = backtrace_symbols(buffer, numEntries);
    if (symbols == nullptr)
    {
        return;
    }
    cStack += "\"";
    for (int i = 0; i < numEntries; i++)
    {
        if (skip > 0)
        {
            skip--;
            continue;
        }
        cStack += std::string(symbols[i]) + '\n';
    }
    cStack += "\"";
    free(symbols);
    return;
}
}  // namespace Utility
