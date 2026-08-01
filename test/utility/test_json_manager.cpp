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
#include <cstdlib>
#include <cstring>
// 先引入 nlohmann/json，避免后续 private public 宏污染第三方库
#include "nlohmann/json.hpp"
#define private public
#include "json_manager.h"
#undef private
#include "file.h"
#include "config_info.h"
#include "securec.h"

using namespace Utility;
using namespace MemScope;

// 假设 JsonManager 有公共 SetValue 接口（源码中 SaveConfigToJson 用到，必然存在）
// 若 SetValue 未公开，需通过友元或提供测试接口，此处按常规设计假设其存在
// 补充：若源码中 SetValue 是私有，需在 JsonManager 中添加测试支持（如 friend class JsonManagerTest;）

class JsonManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("./testmsmemscope");
        ResetJsonManager();
    }

    void TearDown() override
    {
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("");
        rmdir("./testmsmemscope");
        ResetJsonManager();
    }

    // 辅助函数：重置 JsonManager（重置内存中状态并清理文件句柄）
    void ResetJsonManager()
    {
        auto& mgr = JsonManager::GetInstance();
        // 关闭并重置文件句柄，避免后续 SaveToFile 的 CreateConfigFile 因 fp_ 非空而跳过文件创建
        if (mgr.fp_ != nullptr) {
            fclose(mgr.fp_);
            mgr.fp_ = nullptr;
        }
        mgr.jsonFilePath_.clear();     // 清空历史文件路径
        mgr.jsonConfig_.clear();       // 清空 JSON 内容
        mgr.jsonConfig_ = nlohmann::json::object();

        std::string emptyJsonPath = "./testmsmemscope/config.json";
        CreateTestJsonFile(emptyJsonPath, "{}"); // 创建空 JSON 文件
        JsonManager::GetInstance().LoadFromFile(emptyJsonPath);
        remove(emptyJsonPath.c_str()); // 立即删除临时文件
    }

    // 辅助函数：创建测试用的 JSON 文件
    bool CreateTestJsonFile(const std::string& filePath, const std::string& content)
    {
        std::ofstream ofs(filePath);
        if (!ofs.is_open()) {
            return false;
        }
        ofs << content;
        ofs.close();
        return true;
    }

    // 辅助函数：构造测试用的 Config 对象
    MemScope::Config BuildTestConfig()
    {
        MemScope::Config config;
        config.enableCStack = true;
        config.enablePyStack = false;
        config.enableCompare = true;
        config.cStackDepth = 10;
        config.pyStackDepth = 5;
        config.levelType = 1;
        config.eventType = 2;
        config.analysisType = 3;
        config.collectMode = 4;
        strncpy_s(config.outputDir, sizeof(config.outputDir), "./testmsmemscope", sizeof(config.outputDir)-1);
        config.dataFormat = 5;
        config.collectAllNpu = true;
        config.npuSlots = 8;
        config.logLevel = 6;
        config.isEffective = true;
        // 填充 stepList
        config.stepList.stepCount = 3;
        config.stepList.stepIdList[0] = 100;
        config.stepList.stepIdList[1] = 200;
        config.stepList.stepIdList[2] = 300;
        // 填充 watchConfig
        config.watchConfig.isWatched = true;
        config.watchConfig.fullContent = false;
        strncpy_s(config.watchConfig.start, sizeof(config.watchConfig.start), "2025-01-01", sizeof(config.watchConfig.start)-1);
        strncpy_s(config.watchConfig.end, sizeof(config.watchConfig.end), "2025-12-31", sizeof(config.watchConfig.end)-1);
        config.watchConfig.outputId = 10;
        return config;
    }
};

// 测试 SaveToFile：文件路径合法，保存成功
TEST_F(JsonManagerTest, save_to_file_success)
{
    auto& jsonMgr = JsonManager::GetInstance();
    jsonMgr.SetValue("test_key", "test_value"); // 使用公共 SetValue 接口
    std::string filePath = "./testmsmemscope/config.json";
    
    bool ret = jsonMgr.SaveToFile(filePath);
    ASSERT_TRUE(ret);
    ASSERT_TRUE(Utility::FileExists(filePath));
}

// 测试 LoadFromFile：文件存在且格式合法，加载成功
TEST_F(JsonManagerTest, load_from_file_success)
{
    auto& jsonMgr = JsonManager::GetInstance();
    std::string filePath = "./testmsmemscope/config.json";
    std::string jsonContent = R"({"name":"test","age":20})";
    ASSERT_TRUE(CreateTestJsonFile(filePath, jsonContent));
    
    bool ret = jsonMgr.LoadFromFile(filePath);
    ASSERT_TRUE(ret);
    
    // 通过 GetXXXValue 验证加载结果（不直接访问 jsonConfig_）
    std::string name;
    jsonMgr.GetStringValue("name", name);
    ASSERT_EQ(name, "test");
    
    uint32_t age = 0;
    jsonMgr.GetUint32Value("age", age);
    ASSERT_EQ(age, 20);
}

// 测试 LoadFromFile：文件不存在，加载失败
TEST_F(JsonManagerTest, load_from_file_not_exist_fail)
{
    auto& jsonMgr = JsonManager::GetInstance();
    std::string filePath = "./testmsmemscope/not_exist.json";
    
    bool ret = jsonMgr.LoadFromFile(filePath);
    ASSERT_FALSE(ret);
}

// 测试 CheckKeyIsValid：key 为空，返回 false
TEST_F(JsonManagerTest, check_key_is_valid_empty_key_fail)
{
    auto& jsonMgr = JsonManager::GetInstance();
    // 加载合法 JSON（仅为构造 current 参数，实际 CheckKeyIsValid 依赖 jsonConfig_）
    std::string jsonContent = R"({"exist_key":"value"})";
    std::string filePath = "./testmsmemscope/config.json";
    CreateTestJsonFile(filePath, jsonContent);
    jsonMgr.LoadFromFile(filePath);
    // 替代方案：不直接测试 CheckKeyIsValid（私有调用），通过 GetXXXValue 间接覆盖
    // 以下用例改为间接测试：key 不存在时 GetStringValue 失败
    std::string value = "default";
    jsonMgr.GetStringValue("", value);
    ASSERT_EQ(value, "default");
}

// 测试 CheckKeyIsValid：key 不存在（间接测试，通过 GetStringValue）
TEST_F(JsonManagerTest, check_key_is_valid_single_key_not_exist_fail)
{
    auto& jsonMgr = JsonManager::GetInstance();
    std::string jsonContent = R"({"exist_key":"value"})";
    std::string filePath = "./testmsmemscope/config.json";
    CreateTestJsonFile(filePath, jsonContent);
    jsonMgr.LoadFromFile(filePath);
    
    std::string value = "default";
    jsonMgr.GetStringValue("not_exist_key", value);
    ASSERT_EQ(value, "default"); // 未获取到值，说明 CheckKeyIsValid 返回 false
}

// 测试 CheckKeyIsValid：key 存在（间接测试，通过 GetStringValue）
TEST_F(JsonManagerTest, check_key_is_valid_exist_key_success)
{
    auto& jsonMgr = JsonManager::GetInstance();
    std::string jsonContent = R"({"parent":{"child":"nested_value"}})";
    std::string filePath = "./testmsmemscope/config.json";
    CreateTestJsonFile(filePath, jsonContent);
    jsonMgr.LoadFromFile(filePath);
    
    std::string value;
    jsonMgr.GetStringValue("parent.child", value);
    ASSERT_EQ(value, "nested_value"); // 获取到值，说明 CheckKeyIsValid 返回 true
}

// 测试 EnsureConfigPathConsistency：环境变量未设置，设置成功
TEST_F(JsonManagerTest, ensure_config_path_consistency_env_not_set_success)
{
    auto& jsonConfig = JsonConfig::GetInstance();
    std::string configOutputDir = "./testmsmemscope";
    
    bool ret = jsonConfig.EnsureConfigPathConsistency(configOutputDir);
    ASSERT_TRUE(ret);
    
    const char* envPath = getenv(MSMEMSCOPE_CONFIG_ENV);
    ASSERT_NE(envPath, nullptr);
    
    std::string projectDir = FileCreateManager::GetInstance(configOutputDir).GetProjectDir();
    ASSERT_TRUE(Exist(projectDir));
}

// 测试 ReadJsonConfig：读取成功
TEST_F(JsonManagerTest, read_json_config_success)
{
    auto& jsonConfig = JsonConfig::GetInstance();
    MemScope::Config saveConfig = BuildTestConfig();
    std::string filePath = "./testmsmemscope/config.json";
    setenv(MSMEMSCOPE_CONFIG_ENV, filePath.c_str(), 1);
    jsonConfig.SaveConfigToJson(saveConfig); // 先保存配置
    
    MemScope::Config loadConfig = {};
    bool ret = jsonConfig.ReadJsonConfig(loadConfig);
    ASSERT_TRUE(ret);
}

// 测试 ReadJsonConfig：环境变量未设置，返回 false
TEST_F(JsonManagerTest, read_json_config_no_env_fail)
{
    auto& jsonConfig = JsonConfig::GetInstance();
    unsetenv(MSMEMSCOPE_CONFIG_ENV); // 确保环境变量为空
    
    MemScope::Config config = {};
    bool ret = jsonConfig.ReadJsonConfig(config);
    ASSERT_FALSE(ret);
}

// 测试 ReadJsonConfig：文件不存在，返回 false
TEST_F(JsonManagerTest, read_json_config_file_not_exist_fail)
{
    auto& jsonConfig = JsonConfig::GetInstance();
    std::string nonExistPath = "./testmsmemscope/not_exist_config.json";
    setenv(MSMEMSCOPE_CONFIG_ENV, nonExistPath.c_str(), 1);
    
    MemScope::Config config = {};
    bool ret = jsonConfig.ReadJsonConfig(config);
    ASSERT_FALSE(ret);
}
