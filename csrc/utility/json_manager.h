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

#ifndef JSON_MANAGER_H
#define JSON_MANAGER_H

#include "config_info.h"
#include "log.h"
#include "nlohmann/json.hpp"
#include "ustring.h"

namespace Utility
{

constexpr uint8_t JSON_INDENT = 4;  // 缩进4个空格
constexpr const char* MSMEMSCOPE_CONFIG_ENV = "MSMEMSCOPE_CONFIG_PATH";

class JsonManager
{
   public:
    static JsonManager& GetInstance();
    bool SaveToFile(const std::string& configOutputDir);
    bool LoadFromFile(std::string filePath);

    // 设置基础类型
    template <typename T>
    void SetValue(const std::string& key, const T& value)
    {
        jsonConfig_[key] = value;
    }

    // 设置嵌套类型
    template <typename T>
    void SetNestedValue(const std::string& key, const T& value)
    {
        std::vector<std::string> parts;
        Split(key, std::back_inserter(parts), ".");

        nlohmann::json* curr = &jsonConfig_;

        // 遍历路径，创建中间节点
        for (size_t i = 0; i < parts.size() - 1; ++i)
        {
            auto it = curr->find(parts[i]);
            if (it == curr->end())
            {
                it = curr->emplace(parts[i], nlohmann::json::object()).first;
            }
            curr = &it.value();
        }
        (*curr)[parts.back()] = value;
    }

    void GetStringValue(const std::string& key, std::string& value);
    void GetCharListValue(const std::string& key, char* buffer, size_t bufferSize);
    void GetUint8Value(const std::string& key, uint8_t& value);
    void GetUint32Value(const std::string& key, uint32_t& value);
    void GetBoolValue(const std::string& key, bool& value);
    void GetVectorIntValue(const std::string& key, std::vector<int>& value);
    void GetUint32ListsValue(const std::string& key, uint32_t* lists, size_t length);

   private:
    bool CheckKeyIsValid(const std::string& key, nlohmann::json& current);

   private:
    nlohmann::json jsonConfig_;
    std::string jsonFilePath_;
    FILE* fp_{nullptr};
};

class JsonConfig
{
   public:
    static JsonConfig& GetInstance();
    void SaveConfigToJson(const MemScope::Config& config);
    bool ReadJsonConfig(MemScope::Config& config);
    bool EnsureConfigPathConsistency(const std::string& configOutputDir);
};

}  // namespace Utility

#endif
