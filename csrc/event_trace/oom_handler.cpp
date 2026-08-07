/* ------------------------------------------------------------------------- * This file is part of the MindStudio
 * project. Copyright (c) 2025 Huawei Technologies Co.,Ltd.
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
 * ------------------------------------------------------------------------- */

#include "oom_handler.h"

namespace MemScope
{

OOMHandler& OOMHandler::Instance()
{
    static OOMHandler instance;
    return instance;
}

void OOMHandler::SetOOMStack(const CallStackString& stack)
{
    std::lock_guard<std::mutex> lock(oomStackMutex_);
    oomStackQueue_.push(stack);
}

CallStackString OOMHandler::GetOOMStack()
{
    std::lock_guard<std::mutex> lock(oomStackMutex_);
    if (!oomStackQueue_.empty())
    {
        CallStackString stack = std::move(oomStackQueue_.front());
        oomStackQueue_.pop();
        return stack;
    }
    return CallStackString();  // 返回空调用栈
}

}  // namespace MemScope
