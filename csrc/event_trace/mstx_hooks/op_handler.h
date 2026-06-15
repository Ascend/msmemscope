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
#ifndef OP_HANDLER_H
#define OP_HANDLER_H

#include <cstdint>
#include <string>
#include <vector>

namespace MemScope
{

struct MemoryAccessItem
{
    char alias[32];
    uint64_t ptr;
    uint64_t size;
};

class SanitizerOpHandler
{
   public:
    static SanitizerOpHandler& GetInstance();

    // 处理一条 sanitizer-op: 消息，直接触发 kernel launch 事件
    void Handle(const char* msg, int32_t streamId);

    // 设置/获取 sanitizer 使能状态
    static void SetEnabled(bool enabled);
    static bool IsEnabled();

   private:
    SanitizerOpHandler() = default;
    ~SanitizerOpHandler() = default;
    SanitizerOpHandler(const SanitizerOpHandler&) = delete;
    SanitizerOpHandler& operator=(const SanitizerOpHandler&) = delete;

    // 提取顶级字段值（按 ; 分隔，匹配 key=value）
    bool ExtractField(const char* msg, const std::string& key, std::string& value);
    // 解析 read/write 列表: "alias:addr:size,alias:addr:size,..." → vector<MemoryAccessItem>
    std::vector<MemoryAccessItem> ParseAccessList(const std::string& listStr);
    // 解析单项 "alias:addr:size"
    bool ParseAccessItem(const std::string& item, MemoryAccessItem& out);
    // 组装数据并调用 Python 侧 _handle_kernel_launch
    void TriggerKernelLaunch(const std::string& name, int32_t streamId, const std::vector<MemoryAccessItem>& reads,
                             const std::vector<MemoryAccessItem>& writes);

    static bool sanitizerEnabled_;
};

}  // namespace MemScope

#endif  // OP_HANDLER_H
