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

#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "describe_trace.h"

using namespace MemScope;

// 注意: DescribeTrace 为单例且按线程维护状态, 各用例使用独立标签避免相互污染;
// 每个用例结束时清理自身添加的标签

TEST(DescribeTrace, GetDescribeLengthTest)
{
    std::vector<std::string> labels = DescribeTrace::GetInstance().GetDescribe();
    EXPECT_EQ(labels.size(), static_cast<size_t>(OwnerLevel::OWNER_LEVEL_NUM));
}

TEST(DescribeTrace, AddDescribeLevelTest)
{
    DescribeTrace& trace = DescribeTrace::GetInstance();
    trace.AddDescribe(OwnerLevel::COMPONENT, "ut_fsdp2");
    trace.AddDescribe(OwnerLevel::PROCESS, "ut_activation");
    trace.AddDescribe(OwnerLevel::DETAIL_1, "ut_all_gather");

    std::vector<std::string> labels = trace.GetDescribe();
    EXPECT_EQ(labels[static_cast<size_t>(OwnerLevel::COMPONENT)], "ut_fsdp2");
    EXPECT_EQ(labels[static_cast<size_t>(OwnerLevel::PROCESS)], "ut_activation");
    EXPECT_EQ(labels[static_cast<size_t>(OwnerLevel::DETAIL_1)], "ut_all_gather");
    // 未设置的级别为空
    EXPECT_TRUE(labels[static_cast<size_t>(OwnerLevel::FRAMEWORK)].empty());
    EXPECT_TRUE(labels[static_cast<size_t>(OwnerLevel::DETAIL_2)].empty());

    trace.EraseDescribe(OwnerLevel::COMPONENT, "ut_fsdp2");
    trace.EraseDescribe(OwnerLevel::PROCESS, "ut_activation");
    trace.EraseDescribe(OwnerLevel::DETAIL_1, "ut_all_gather");
}

TEST(DescribeTrace, SameLevelSameLabelCountTest)
{
    // 同级别同标签嵌套: 计数, 计数归零才出栈
    DescribeTrace& trace = DescribeTrace::GetInstance();
    trace.AddDescribe(OwnerLevel::COMPONENT, "ut_nested");
    trace.AddDescribe(OwnerLevel::COMPONENT, "ut_nested");
    EXPECT_EQ(trace.GetDescribe()[static_cast<size_t>(OwnerLevel::COMPONENT)], "ut_nested");

    trace.EraseDescribe(OwnerLevel::COMPONENT, "ut_nested");
    EXPECT_EQ(trace.GetDescribe()[static_cast<size_t>(OwnerLevel::COMPONENT)], "ut_nested");
    trace.EraseDescribe(OwnerLevel::COMPONENT, "ut_nested");
    EXPECT_TRUE(trace.GetDescribe()[static_cast<size_t>(OwnerLevel::COMPONENT)].empty());
}

TEST(DescribeTrace, SameLevelDiffLabelNewestWinsTest)
{
    // 同级别不同标签嵌套: 输出取最新(栈顶), 撤销后回退
    DescribeTrace& trace = DescribeTrace::GetInstance();
    trace.AddDescribe(OwnerLevel::PROCESS, "ut_outer");
    trace.AddDescribe(OwnerLevel::PROCESS, "ut_inner");
    EXPECT_EQ(trace.GetDescribe()[static_cast<size_t>(OwnerLevel::PROCESS)], "ut_inner");

    trace.EraseDescribe(OwnerLevel::PROCESS, "ut_inner");
    EXPECT_EQ(trace.GetDescribe()[static_cast<size_t>(OwnerLevel::PROCESS)], "ut_outer");
    trace.EraseDescribe(OwnerLevel::PROCESS, "ut_outer");
}

TEST(DescribeTrace, InvalidLevelIgnoredTest)
{
    DescribeTrace& trace = DescribeTrace::GetInstance();
    // 系统标签仅允许 FRAMEWORK..DETAIL_2, 非法级别忽略不崩溃
    trace.AddDescribe(OwnerLevel::OWNER_LEVEL_NUM, "ut_invalid");
    trace.EraseDescribe(OwnerLevel::OWNER_LEVEL_NUM, "ut_invalid");
}

TEST(DescribeTrace, UserDescribeTest)
{
    // 用户标签: 按栈序映射 USER_DEFINED_1..3, 超出3个静默丢弃
    DescribeTrace& trace = DescribeTrace::GetInstance();
    trace.AddUserDescribe("ut_user_1");
    trace.AddUserDescribe("ut_user_2");
    trace.AddUserDescribe("ut_user_3");
    trace.AddUserDescribe("ut_user_4");  // 超出静默丢弃

    std::vector<std::string> labels = trace.GetDescribe();
    EXPECT_EQ(labels[static_cast<size_t>(OwnerLevel::USER_DEFINED_1)], "ut_user_1");
    EXPECT_EQ(labels[static_cast<size_t>(OwnerLevel::USER_DEFINED_2)], "ut_user_2");
    EXPECT_EQ(labels[static_cast<size_t>(OwnerLevel::USER_DEFINED_3)], "ut_user_3");

    trace.EraseUserDescribe("ut_user_1");
    trace.EraseUserDescribe("ut_user_2");
    trace.EraseUserDescribe("ut_user_3");
    EXPECT_TRUE(trace.GetDescribe()[static_cast<size_t>(OwnerLevel::USER_DEFINED_1)].empty());
}

TEST(DescribeTrace, DescribeAddrTest)
{
    // 地址直标: 携带分级标签列表, 不经过标签栈
    std::vector<std::pair<OwnerLevel, std::string>> labels = {
        {OwnerLevel::COMPONENT, "ut_vllm"},
        {OwnerLevel::DETAIL_1, "ut_weights"},
    };
    DescribeTrace::GetInstance().DescribeAddr(123, labels);
}

// 同级别标签栈超过上限时静默丢弃；撤销不存在的系统/用户标签仅打印错误
TEST(DescribeTrace, MaxStackSizeAndUnknownEraseTest)
{
    DescribeTrace& trace = DescribeTrace::GetInstance();
    auto contains = [](const std::vector<std::string>& labels, const std::string& target) {
        return std::find(labels.begin(), labels.end(), target) != labels.end();
    };
    // 清空本线程DETAIL_2残留标签，保证栈上限判定从确定状态开始
    while (!trace.GetDescribe()[static_cast<size_t>(OwnerLevel::DETAIL_2)].empty())
    {
        trace.EraseDescribe(OwnerLevel::DETAIL_2,
                            trace.GetDescribe()[static_cast<size_t>(OwnerLevel::DETAIL_2)]);
    }

    // DETAIL_2级别压入4个不同标签，第4个超出每级上限(3)被丢弃
    trace.AddDescribe(OwnerLevel::DETAIL_2, "ut_ovf_1");
    trace.AddDescribe(OwnerLevel::DETAIL_2, "ut_ovf_2");
    trace.AddDescribe(OwnerLevel::DETAIL_2, "ut_ovf_3");
    trace.AddDescribe(OwnerLevel::DETAIL_2, "ut_ovf_4");
    EXPECT_EQ(trace.GetDescribe()[static_cast<size_t>(OwnerLevel::DETAIL_2)], "ut_ovf_3");
    EXPECT_TRUE(!contains(trace.GetDescribe(), "ut_ovf_4"));

    trace.EraseDescribe(OwnerLevel::DETAIL_2, "ut_ovf_1");
    trace.EraseDescribe(OwnerLevel::DETAIL_2, "ut_ovf_2");
    trace.EraseDescribe(OwnerLevel::DETAIL_2, "ut_ovf_3");
    EXPECT_TRUE(trace.GetDescribe()[static_cast<size_t>(OwnerLevel::DETAIL_2)].empty());

    // 撤销不存在的系统标签：打印错误，状态不变
    trace.EraseDescribe(OwnerLevel::DETAIL_2, "ut_never_added");
    EXPECT_TRUE(trace.GetDescribe()[static_cast<size_t>(OwnerLevel::DETAIL_2)].empty());

    // 撤销不存在的用户标签：打印错误，已存在的用户标签保留
    trace.AddUserDescribe("ut_user_keep");
    EXPECT_TRUE(contains(trace.GetDescribe(), "ut_user_keep"));
    trace.EraseUserDescribe("ut_never_added_user");
    EXPECT_TRUE(contains(trace.GetDescribe(), "ut_user_keep"));
    trace.EraseUserDescribe("ut_user_keep");
    EXPECT_TRUE(!contains(trace.GetDescribe(), "ut_user_keep"));
}
