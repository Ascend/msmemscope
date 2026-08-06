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

#include "ustring.h"
#include <unordered_map>

namespace Utility {

void ToSafeString(std::string &str)
{
    static std::unordered_map<char, std::string> invalidChar = {{'\n', "\\n"},
        {'\f', "\\f"},
        {'\r', "\\r"},
        {'\b', "\\b"},
        {'\t', "\\t"},
        {'\v', "\\v"},
        {'\u007F', "\\u007F"}};
    for (size_t i = 0; i < str.size();) {
        if (invalidChar.find(str[i]) != invalidChar.end()) {
            std::string validStr = invalidChar[str[i]];
            str.replace(i, 1, validStr);
            i += validStr.length();
            continue;
        }
        i++;
    }
    return;
}

std::string ExtractAttrValueByKey(const std::string& str, const std::string& key)
{
    std::string attrValue = "";
    // 避免构造 key+":" 临时串，直接校验后续字符是否为冒号
    size_t startPos = str.find(key);
    if (startPos != std::string::npos && startPos + key.length() < str.size() && str[startPos + key.length()] == ':') {
        // 跳过键和冒号
        startPos += key.length() + 1;
        size_t endPos = str.find(",", startPos);
        if (endPos == std::string::npos) {
            endPos = str.find("}", startPos);
        }
        if (endPos != std::string::npos) {
            attrValue = str.substr(startPos, endPos - startPos);
        }
    }
    return attrValue;
}

} // namespace Utility