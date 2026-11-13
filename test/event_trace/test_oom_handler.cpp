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

#include <string>
#include <gtest/gtest.h>

#include "event_trace/oom_handler.h"

using namespace MemScope;

namespace {
// 清空单例队列，保证用例从确定状态开始
void DrainOOMStacks()
{
    while (true) {
        CallStackString stack = OOMHandler::Instance().GetOOMStack();
        if (stack.cStack.empty() && stack.pyStack.empty()) {
            return;
        }
    }
}
}  // namespace

TEST(OOMHandlerTest, stacks_are_fifo_and_consumed_once)
{
    DrainOOMStacks();

    std::string c1 = "c-stack-1";
    std::string p1 = "py-stack-1";
    CallStackString first(c1, p1);
    CallStackString second;
    second.cStack = "c-stack-2";
    second.pyStack = "py-stack-2";

    OOMHandler::Instance().SetOOMStack(first);
    OOMHandler::Instance().SetOOMStack(second);

    CallStackString gotFirst = OOMHandler::Instance().GetOOMStack();
    EXPECT_EQ(gotFirst.cStack, "c-stack-1");
    EXPECT_EQ(gotFirst.pyStack, "py-stack-1");

    CallStackString gotSecond = OOMHandler::Instance().GetOOMStack();
    EXPECT_EQ(gotSecond.cStack, "c-stack-2");
    EXPECT_EQ(gotSecond.pyStack, "py-stack-2");

    // 队列已消费完毕，再取返回空栈
    CallStackString empty = OOMHandler::Instance().GetOOMStack();
    EXPECT_TRUE(empty.cStack.empty());
    EXPECT_TRUE(empty.pyStack.empty());
}
