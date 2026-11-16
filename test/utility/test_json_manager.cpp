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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
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

// ==================== SaveToFile/LoadFromFile 失败分支补充 ====================

// SaveToFile：待序列化内容含非法 UTF-8 字节，dump() 抛异常被 catch 捕获并返回 false
TEST_F(JsonManagerTest, save_to_file_dump_exception_expect_false)
{
    auto& jsonMgr = JsonManager::GetInstance();
    jsonMgr.SetValue("invalidUtf8", std::string("\xff\xfe", 2));

    std::ostringstream oss;
    std::streambuf* oldBuf = std::cout.rdbuf(oss.rdbuf());
    bool ret = jsonMgr.SaveToFile("./testmsmemscope");
    std::cout.rdbuf(oldBuf);

    EXPECT_FALSE(ret);
    EXPECT_NE(oss.str().find("Exception during dump"), std::string::npos);
    // 配置文件已由 CreateConfigFile 创建，异常发生在序列化阶段
    EXPECT_FALSE(jsonMgr.jsonFilePath_.empty());
    EXPECT_TRUE(Utility::FileExists(jsonMgr.jsonFilePath_));
    // 清理本次落盘的配置文件，保证 fixture 能删除临时目录
    remove(jsonMgr.jsonFilePath_.c_str());
}

// LoadFromFile 失败分支：路径能通过输入路径校验（存在且可读），但无法按普通文件打开。
// Unix domain socket 文件满足 stat/access 校验，open 时返回 ENXIO，从而覆盖
// ReadJsonConfig 中 LoadFromFile 失败的日志分支。
TEST_F(JsonManagerTest, read_json_config_unopenable_file_expect_false)
{
    const std::string sockPath = "./msms_json_cfg.sock";
    unlink(sockPath.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    ASSERT_LT(sockPath.size(), sizeof(addr.sun_path));
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    ASSERT_EQ(bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);

    // 前置条件：输入路径校验通过（socket 文件存在且可读），失败只发生在文件打开阶段
    ASSERT_TRUE(Utility::CheckIsValidInputPath(sockPath));
    setenv(MSMEMSCOPE_CONFIG_ENV, sockPath.c_str(), 1);

    MemScope::Config config = {};
    std::ostringstream oss;
    std::streambuf* oldBuf = std::cout.rdbuf(oss.rdbuf());
    bool ret = JsonConfig::GetInstance().ReadJsonConfig(config);
    std::cout.rdbuf(oldBuf);

    close(fd);
    unlink(sockPath.c_str());

    EXPECT_FALSE(ret);
    EXPECT_NE(oss.str().find("Failed to load json config file"), std::string::npos);
}

// ReadJsonConfig：配置文件中的 outputDir 为空，输出路径校验失败返回 false
TEST_F(JsonManagerTest, read_json_config_invalid_output_dir_expect_false)
{
    ASSERT_TRUE(Utility::MakeDir("./testmsmemscope"));
    std::string filePath = "./testmsmemscope/config.json";
    ASSERT_TRUE(CreateTestJsonFile(filePath, R"({"outputDir":""})"));
    setenv(MSMEMSCOPE_CONFIG_ENV, filePath.c_str(), 1);

    MemScope::Config config = {};
    std::ostringstream oss;
    std::streambuf* oldBuf = std::cout.rdbuf(oss.rdbuf());
    bool ret = JsonConfig::GetInstance().ReadJsonConfig(config);
    std::cout.rdbuf(oldBuf);

    EXPECT_FALSE(ret);
    EXPECT_EQ(config.outputDir[0], '\0');
    EXPECT_NE(oss.str().find("Invalid output path in file"), std::string::npos);
    // 清理配置文件，保证 fixture 能删除临时目录
    remove(filePath.c_str());
}

// ==================== Get* 接口缺失 key / 类型不匹配 / 缓冲区边界补充 ====================

// 各 Get* 接口：key 不存在时提前返回，输出参数保持原值
TEST_F(JsonManagerTest, get_values_missing_key_keep_default)
{
    auto& jsonMgr = JsonManager::GetInstance();
    uint8_t u8 = 7;
    uint32_t u32 = 8;
    uint64_t u64 = 9;
    bool flag = true;
    std::vector<int> vec{9};

    jsonMgr.GetUint8Value("absentKey", u8);
    jsonMgr.GetUint32Value("absentKey", u32);
    jsonMgr.GetUint64Value("absentKey", u64);
    jsonMgr.GetBoolValue("absentKey", flag);
    jsonMgr.GetVectorIntValue("absentKey", vec);

    EXPECT_EQ(u8, 7);
    EXPECT_EQ(u32, 8u);
    EXPECT_EQ(u64, 9u);
    EXPECT_TRUE(flag);
    EXPECT_EQ(vec, std::vector<int>({9}));
}

// 各 Get* 接口：JSON 节点类型不匹配时抛出的异常被捕获，输出参数保持原值
TEST_F(JsonManagerTest, get_values_type_mismatch_keep_default)
{
    auto& jsonMgr = JsonManager::GetInstance();
    jsonMgr.SetValue("numValue", 42);      // 非 string
    jsonMgr.SetValue("textValue", "abc");  // 非数值/布尔/数组

    std::string str = "keep";
    jsonMgr.GetStringValue("numValue", str);
    EXPECT_EQ(str, "keep");

    uint8_t u8 = 1;
    jsonMgr.GetUint8Value("textValue", u8);
    EXPECT_EQ(u8, 1);

    uint32_t u32 = 2;
    jsonMgr.GetUint32Value("textValue", u32);
    EXPECT_EQ(u32, 2u);

    uint64_t u64 = 3;
    jsonMgr.GetUint64Value("textValue", u64);
    EXPECT_EQ(u64, 3u);

    bool flag = false;
    jsonMgr.GetBoolValue("textValue", flag);
    EXPECT_FALSE(flag);

    std::vector<int> vec{4};
    jsonMgr.GetVectorIntValue("textValue", vec);
    EXPECT_EQ(vec, std::vector<int>({4}));
}

// GetCharListValue：取到的字符串为空（key 缺失）时向 buffer 写入 '\0'
TEST_F(JsonManagerTest, get_char_list_empty_value_expect_terminate)
{
    auto& jsonMgr = JsonManager::GetInstance();
    char buffer[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', '\0'};

    jsonMgr.GetCharListValue("absentKey", buffer, sizeof(buffer));

    EXPECT_EQ(buffer[0], '\0');
}

// GetUint32ListsValue：key 缺失导致取到的列表为空时提前返回，不改写输出缓冲区
TEST_F(JsonManagerTest, get_uint32_lists_empty_value_expect_unchanged)
{
    auto& jsonMgr = JsonManager::GetInstance();
    uint32_t lists[4] = {1, 2, 3, 4};

    jsonMgr.GetUint32ListsValue("absentKey", lists, 4);

    EXPECT_EQ(lists[0], 1u);
    EXPECT_EQ(lists[3], 4u);
}
