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

#include "json_manager.h"

#include <fstream>

#include "client_parser.h"
#include "path.h"
#include "securec.h"

namespace Utility
{

JsonManager& JsonManager::GetInstance()
{
    static JsonManager jsonManager{};
    return jsonManager;
}

bool JsonManager::SaveToFile(const std::string& configOutputDir)
{
    if (!FileCreateManager::GetInstance(configOutputDir).CreateConfigFile(&fp_, MemScope::CONFIG_FILE, jsonFilePath_))
    {
        return false;
    }
    std::ofstream ofs(jsonFilePath_);
    if (!ofs.is_open())
    {
        std::cout << "[msmemscope] Error: Failed to save json file: " << jsonFilePath_ << std::endl;
        return false;
    }
    else
    {
        try
        {
            ofs << jsonConfig_.dump(JSON_INDENT);
        }
        catch (const std::exception& e)
        {
            std::cout << "[msmemscope] Error: Exception during dump: " << e.what() << std::endl;
            ofs.close();
            return false;
        }
    }
    ofs.close();
    return true;
}

bool JsonManager::LoadFromFile(std::string filePath)
{
    std::ifstream ifs(filePath);
    if (!ifs.is_open())
    {
        std::cout << "[msmemscope] Error: Failed to open json file: " << filePath << std::endl;
        return false;
    }
    else
    {
        ifs >> jsonConfig_;
        ifs.close();
    }
    return true;
}

bool JsonManager::CheckKeyIsValid(const std::string& key, nlohmann::json& current)
{
    std::vector<std::string> parts;
    Split(key, std::back_inserter(parts), ".");

    if (parts.empty())
    {
        std::cout << "[msmemscope] Error: Empty key path provided!" << std::endl;
        return false;
    }

    for (auto& part : parts)
    {
        if (!current.contains(part))
        {
            std::cout << "[msmemscope] Error: Key path: " << part << "keyword not exist!" << std::endl;
            return false;
        }

        current = current[part];
    }
    return true;
}

void JsonManager::GetStringValue(const std::string& key, std::string& value)
{
    nlohmann::json current = jsonConfig_;
    if (!CheckKeyIsValid(key, current))
    {
        return;
    }

    try
    {
        value = current.get<std::string>();
    }
    catch (const std::exception& e)
    {
        std::cout << "[msmemscope] Error: Exception while reading nested key " << key << ": " << e.what() << std::endl;
        return;
    }
}

void JsonManager::GetCharListValue(const std::string& key, char* buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
    {
        std::cout << "[msmemscope] Error: Invalid buffer or buffer size!" << std::endl;
        return;
    }
    std::string str;
    GetStringValue(key, str);
    if (str.empty())
    {
        buffer[0] = '\0';
        return;
    }

    if (strncpy_s(buffer, bufferSize, str.c_str(), bufferSize - 1) != EOK)
    {
        std::cout << "[msmemscope] Error: strncpy_s FAILED" << std::endl;
        return;
    }
}

void JsonManager::GetUint8Value(const std::string& key, uint8_t& value)
{
    nlohmann::json current = jsonConfig_;
    if (!CheckKeyIsValid(key, current))
    {
        return;
    }

    try
    {
        value = current.get<uint8_t>();
    }
    catch (const std::exception& e)
    {
        std::cout << "[msmemscope] Error: Exception while reading nested key " << key << ": " << e.what() << std::endl;
        return;
    }
}

void JsonManager::GetUint32Value(const std::string& key, uint32_t& value)
{
    nlohmann::json current = jsonConfig_;
    if (!CheckKeyIsValid(key, current))
    {
        return;
    }

    try
    {
        value = current.get<uint32_t>();
    }
    catch (const std::exception& e)
    {
        std::cout << "[msmemscope] Error: Exception while reading nested key " << key << ": " << e.what() << std::endl;
        return;
    }
}

void JsonManager::GetBoolValue(const std::string& key, bool& value)
{
    nlohmann::json current = jsonConfig_;
    if (!CheckKeyIsValid(key, current))
    {
        return;
    }

    try
    {
        value = current.get<bool>();
    }
    catch (const std::exception& e)
    {
        std::cout << "[msmemscope] Error: Exception while reading nested key " << key << ": " << e.what() << std::endl;
        return;
    }
}

void JsonManager::GetVectorIntValue(const std::string& key, std::vector<int>& value)
{
    nlohmann::json current = jsonConfig_;
    if (!CheckKeyIsValid(key, current))
    {
        return;
    }

    try
    {
        value = current.get<std::vector<int>>();
    }
    catch (const std::exception& e)
    {
        std::cout << "[msmemscope] Error: Exception while reading nested key " << key << ": " << e.what() << std::endl;
        return;
    }
}

void JsonManager::GetUint32ListsValue(const std::string& key, uint32_t* lists, size_t length)
{
    if (!lists || length == 0)
    {
        std::cout << "[msmemscope] Error: Invalid lists or lists size!" << std::endl;
        return;
    }

    std::vector<int> valueLists;
    GetVectorIntValue(key, valueLists);
    if (valueLists.empty())
    {
        return;
    }

    std::copy(valueLists.begin(), valueLists.begin() + std::min(length, valueLists.size()), lists);
}

JsonConfig& JsonConfig::GetInstance()
{
    static JsonConfig jsonConfig;
    return jsonConfig;
}

bool JsonConfig::EnsureConfigPathConsistency(const std::string& configOutputDir)
{
    const char* envPath = std::getenv(MSMEMSCOPE_CONFIG_ENV);
    std::string currentEnvPath = envPath ? std::string(envPath) : "";

    // 创建 FileCreateManager 实例并获取实际配置路径
    auto& fileManager = FileCreateManager::GetInstance(configOutputDir);
    std::string actualConfigPath = fileManager.GetProjectDir() + '/' + MemScope::CONFIG_FILE + ".json";

    bool needUpdate = false;
    if (currentEnvPath.empty() || currentEnvPath != actualConfigPath)
    {
        needUpdate = true;
    }

    if (needUpdate)
    {
        if (setenv(MSMEMSCOPE_CONFIG_ENV, actualConfigPath.c_str(), 1) != 0)
        {
            std::cout << "[msmemscope] Error: Failed to set MSMEMSCOPE_CONFIG_ENV to " << actualConfigPath << std::endl;
            return false;
        }
    }

    return true;
}

void JsonConfig::SaveConfigToJson(const MemScope::Config& config)
{
    Utility::JsonManager::GetInstance().SetValue("enableCStack", config.enableCStack);
    Utility::JsonManager::GetInstance().SetValue("enablePyStack", config.enablePyStack);
    Utility::JsonManager::GetInstance().SetValue("enableCompare", config.enableCompare);
    Utility::JsonManager::GetInstance().SetValue("cStackDepth", config.cStackDepth);
    Utility::JsonManager::GetInstance().SetValue("pyStackDepth", config.pyStackDepth);
    Utility::JsonManager::GetInstance().SetValue("levelType", config.levelType);
    Utility::JsonManager::GetInstance().SetValue("eventType", config.eventType);
    Utility::JsonManager::GetInstance().SetValue("analysisType", config.analysisType);
    Utility::JsonManager::GetInstance().SetValue("collectMode", config.collectMode);
    Utility::JsonManager::GetInstance().SetValue("outputDir", config.outputDir);
    Utility::JsonManager::GetInstance().SetValue("dataFormat", config.dataFormat);
    Utility::JsonManager::GetInstance().SetValue("collectAllNpu", config.collectAllNpu);
    Utility::JsonManager::GetInstance().SetValue("collectCpu", config.collectCpu);
    Utility::JsonManager::GetInstance().SetValue("npuSlots", config.npuSlots);
    Utility::JsonManager::GetInstance().SetValue("logLevel", config.logLevel);
    Utility::JsonManager::GetInstance().SetValue("isEffective", config.isEffective);

    std::vector<uint32_t> stepIdList{config.stepList.stepIdList,
                                     config.stepList.stepIdList + MemScope::SELECTED_STEP_MAX_NUM};
    Utility::JsonManager::GetInstance().SetNestedValue("SelectedStepList.stepIdList", stepIdList);
    Utility::JsonManager::GetInstance().SetNestedValue("SelectedStepList.stepCount", config.stepList.stepCount);
    Utility::JsonManager::GetInstance().SetNestedValue("watchConfig.isWatched", config.watchConfig.isWatched);
    Utility::JsonManager::GetInstance().SetNestedValue("watchConfig.fullContent", config.watchConfig.fullContent);
    Utility::JsonManager::GetInstance().SetNestedValue("watchConfig.start", config.watchConfig.start);
    Utility::JsonManager::GetInstance().SetNestedValue("watchConfig.end", config.watchConfig.end);
    Utility::JsonManager::GetInstance().SetNestedValue("watchConfig.outputId", config.watchConfig.outputId);

    if (!EnsureConfigPathConsistency(config.outputDir))
    {
        std::cout << "[msmemscope] Error: Failed to ensure config path consistency." << std::endl;
        return;
    }

    if (!Utility::JsonManager::GetInstance().SaveToFile(config.outputDir))
    {
        std::cout << "[msmemscope] Error: Save Json config to file failed!" << std::endl;
        return;
    }
}

bool JsonConfig::ReadJsonConfig(MemScope::Config& config)
{
    const char* path = std::getenv(MSMEMSCOPE_CONFIG_ENV);
    if (!path)
    {
        return false;
    }

    if (!Utility::CheckIsValidInputPath(path))
    {
        return false;
    }

    if (!Utility::JsonManager::GetInstance().LoadFromFile(path))
    {
        std::cout << "[msmemscope] Error: Failed to load json config file: " << path << std::endl;
        return false;
    }

    Utility::JsonManager::GetInstance().GetCharListValue("outputDir", config.outputDir, sizeof(config.outputDir));
    Utility::JsonManager::GetInstance().GetUint8Value("analysisType", config.analysisType);
    Utility::JsonManager::GetInstance().GetUint8Value("collectMode", config.collectMode);
    Utility::JsonManager::GetInstance().GetUint8Value("dataFormat", config.dataFormat);
    Utility::JsonManager::GetInstance().GetUint8Value("eventType", config.eventType);
    Utility::JsonManager::GetInstance().GetUint8Value("levelType", config.levelType);
    Utility::JsonManager::GetInstance().GetUint8Value("logLevel", config.logLevel);

    Utility::JsonManager::GetInstance().GetUint32Value("cStackDepth", config.cStackDepth);
    Utility::JsonManager::GetInstance().GetUint32Value("pyStackDepth", config.pyStackDepth);
    Utility::JsonManager::GetInstance().GetUint32Value("npuSlots", config.npuSlots);

    Utility::JsonManager::GetInstance().GetBoolValue("collectAllNpu", config.collectAllNpu);
    Utility::JsonManager::GetInstance().GetBoolValue("collectCpu", config.collectCpu);
    Utility::JsonManager::GetInstance().GetBoolValue("enableCStack", config.enableCStack);
    Utility::JsonManager::GetInstance().GetBoolValue("enablePyStack", config.enablePyStack);
    Utility::JsonManager::GetInstance().GetBoolValue("enableCompare", config.enableCompare);
    Utility::JsonManager::GetInstance().GetBoolValue("isEffective", config.isEffective);

    Utility::JsonManager::GetInstance().GetUint32ListsValue("SelectedStepList.stepIdList", config.stepList.stepIdList,
                                                            MemScope::SELECTED_STEP_MAX_NUM);
    Utility::JsonManager::GetInstance().GetUint8Value("SelectedStepList.stepCount", config.stepList.stepCount);

    Utility::JsonManager::GetInstance().GetBoolValue("watchConfig.isWatched", config.watchConfig.isWatched);
    Utility::JsonManager::GetInstance().GetBoolValue("watchConfig.fullContent", config.watchConfig.fullContent);
    Utility::JsonManager::GetInstance().GetUint32Value("watchConfig.outputId", config.watchConfig.outputId);
    Utility::JsonManager::GetInstance().GetCharListValue("watchConfig.start", config.watchConfig.start,
                                                         sizeof(config.watchConfig.start));
    Utility::JsonManager::GetInstance().GetCharListValue("watchConfig.end", config.watchConfig.end,
                                                         sizeof(config.watchConfig.end));

    if (!Utility::CheckIsValidOutputPath(config.outputDir))
    {
        std::cout << "[msmemscope] Error:Invalid output path in file: " << path << std::endl;
        return false;
    }
    constexpr uint32_t MAX_STACK_DEPTH = 1000;
    config.cStackDepth = std::min(config.cStackDepth, MAX_STACK_DEPTH);
    config.pyStackDepth = std::min(config.pyStackDepth, MAX_STACK_DEPTH);

    return true;
}

}  // namespace Utility
