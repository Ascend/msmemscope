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
#ifndef DESCRIBE_TRACE_H
#define DESCRIBE_TRACE_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "event.h"

namespace MemScope
{

// 分级标签模型的 range 信息维护者, 内部含两个数据区(均以 thread_local 存放, 线程退出自动释放,
// 峰值内存 = 并发线程数 × 每线程开销, 无死线程条目残留):
//   数据区1(系统标签): 线程局部, [level=FRAMEWORK..DETAIL_2] 每级一个栈<pair<标签, 计数>>, 每级栈上限3;
//                     同级别同标签嵌套以计数处理(计数为0出栈), 同级别不同标签嵌套各自入栈, 输出取栈顶(最新)
//   数据区2(用户标签): 线程局部, 栈<string>, 上限3, 超出静默丢弃; 输出按栈序映射 USER_DEFINED_1..3
class DescribeTrace
{
   public:
    static DescribeTrace& GetInstance()
    {
        static DescribeTrace instance{};
        return instance;
    }
    DescribeTrace(const DescribeTrace&) = delete;
    DescribeTrace& operator=(const DescribeTrace&) = delete;

    // 内部接口(系统标签, 数据区1): 带级别, 供框架钩子(hijack_map等)使用
    void AddDescribe(OwnerLevel level, const std::string& label);
    void EraseDescribe(OwnerLevel level, const std::string& label);
    // 用户接口(用户标签, 数据区2): 无级别, 供 describe.py 用户场景使用
    void AddUserDescribe(const std::string& label);
    void EraseUserDescribe(const std::string& label);
    // 地址直标: 不经过标签栈, 直接上报块级 owner 更新事件(携带分级标签列表, 同级别以地址标签为准)
    void DescribeAddr(uint64_t addr, const std::vector<std::pair<OwnerLevel, std::string>>& labels);
    // 获取当前线程各级标签: 长度OWNER_LEVEL_NUM, FRAMEWORK..DETAIL_2取各级栈顶(最新), USER_DEFINED_1..3取用户栈
    std::vector<std::string> GetDescribe();

   private:
    DescribeTrace() = default;
    ~DescribeTrace() = default;
    // 获取当前线程系统标签数组(thread_local, 线程退出自动释放), 确保外层长度固定为
    // systemLevelNum(DETAIL_2 + 1), 避免空 vector 越界索引
    std::vector<std::vector<std::pair<std::string, int>>>& GetThreadSystemLabels();
    // 获取当前线程用户标签栈(thread_local, 线程退出自动释放, 数据区2)
    std::vector<std::string>& GetUserLabels();
    static const uint8_t maxStackSize{3};
    static const uint8_t systemLevelNum{static_cast<uint8_t>(OwnerLevel::DETAIL_2) + 1};
};

}  // namespace MemScope

#endif
