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
#include "command.h"

#include <map>
#include <memory>

#include "analysis/decompose_analyzer.h"
#include "analysis/dump.h"
#include "analysis/inefficient_analyzer.h"
#include "analysis/memory_compare.h"
#include "bit_field.h"
#include "process.h"
#include "utils.h"

namespace MemScope
{

void Command::Exec() const
{
    LOG_INFO("Msmemscope starts executing commands");

    if (userCommand_.config.enableCompare)
    {
        MemoryCompare::GetInstance().RunComparison(userCommand_.inputPaths);
        return;
    }

    Process::GetInstance().Launch(userCommand_.cmd);

    return;
}

}  // namespace MemScope
