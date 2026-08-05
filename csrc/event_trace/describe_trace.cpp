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
#include "describe_trace.h"

#include <algorithm>

#include "event_report.h"
#include "log.h"
#include "record_info.h"
#include "ustring.h"
#include "utils.h"

namespace MemScope
{

std::vector<std::vector<std::pair<std::string, int>>>& DescribeTrace::GetThreadSystemLabels()
{
    // thread_local: 本设计所有访问均为当前线程自引用, 线程退出即自动释放,
    // 避免按 tid 建表时死线程条目永久残留(线程频繁创建+退出场景下的无界增长)
    static thread_local std::vector<std::vector<std::pair<std::string, int>>> labels{};
    if (labels.size() < systemLevelNum)
    {
        // 外层长度固定为系统级别数(DETAIL_2 + 1): 空 vector 直接 [level] 索引是越界堆访问
        labels.resize(systemLevelNum);
    }
    return labels;
}

std::vector<std::string>& DescribeTrace::GetUserLabels()
{
    static thread_local std::vector<std::string> labels{};
    return labels;
}

std::vector<std::string> DescribeTrace::GetDescribe()
{
    std::vector<std::string> result(static_cast<size_t>(OwnerLevel::OWNER_LEVEL_NUM));
    // 数据区1: 每级取栈顶(最新)标签
    const auto& perThreadLabels = GetThreadSystemLabels();
    for (uint8_t level = 0; level < systemLevelNum; ++level)
    {
        const auto& levelStack = perThreadLabels[level];
        if (!levelStack.empty())
        {
            result[level] = levelStack.back().first;
        }
    }
    // 数据区2: 用户标签按栈序映射 USER_DEFINED_1..3(超出部分在入栈时已静默丢弃)
    const auto& userLabels = GetUserLabels();
    for (size_t i = 0; i < userLabels.size(); ++i)
    {
        result[static_cast<size_t>(OwnerLevel::USER_DEFINED_1) + i] = userLabels[i];
    }
    return result;
}

void DescribeTrace::AddDescribe(OwnerLevel level, const std::string& label)
{
    if (level > OwnerLevel::DETAIL_2)
    {
        LOG_ERROR("Invalid owner level %u", static_cast<uint8_t>(level));
        return;
    }
    std::string safeLabel = label;
    Utility::ToSafeString(safeLabel);
    auto& levelStack = GetThreadSystemLabels()[static_cast<uint8_t>(level)];
    // 同级别同标签嵌套: 计数加一
    for (auto& item : levelStack)
    {
        if (item.first == safeLabel)
        {
            item.second += 1;
            return;
        }
    }
    if (levelStack.size() >= maxStackSize)
    {
        LOG_ERROR("The current level label stack exceeds %u", static_cast<uint32_t>(maxStackSize));
        return;
    }
    levelStack.emplace_back(safeLabel, 1);
}

void DescribeTrace::EraseDescribe(OwnerLevel level, const std::string& label)
{
    if (level > OwnerLevel::DETAIL_2)
    {
        LOG_ERROR("Invalid owner level %u", static_cast<uint8_t>(level));
        return;
    }
    auto& levelStack = GetThreadSystemLabels()[static_cast<uint8_t>(level)];
    for (auto it = levelStack.rbegin(); it != levelStack.rend(); ++it)
    {
        if (it->first == label)
        {
            it->second -= 1;
            if (it->second <= 0)
            {
                // 计数归零出栈(reverse_iterator 转正向迭代器删除)
                levelStack.erase(std::next(it).base());
            }
            return;
        }
    }
    LOG_ERROR("Tag %s not found", label.c_str());
}

void DescribeTrace::AddUserDescribe(const std::string& label)
{
    auto& stack = GetUserLabels();
    if (stack.size() >= maxStackSize)
    {
        // 超出3个静默丢弃
        return;
    }
    std::string safeLabel = label;
    Utility::ToSafeString(safeLabel);
    stack.push_back(safeLabel);
}

void DescribeTrace::EraseUserDescribe(const std::string& label)
{
    auto& stack = GetUserLabels();
    std::string safeLabel = label;
    Utility::ToSafeString(safeLabel);
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
    {
        if (*it == safeLabel)
        {
            stack.erase(std::next(it).base());
            return;
        }
    }
    LOG_ERROR("User tag %s not found", label.c_str());
}

void DescribeTrace::DescribeAddr(uint64_t addr, const std::vector<std::pair<OwnerLevel, std::string>>& labels)
{
    EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAddrInfo(EventSubType::DESCRIBE_OWNER, addr, labels);
}

}  // namespace MemScope
