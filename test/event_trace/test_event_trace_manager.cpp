/* -------------------------------------------------------------------------
 * This file is part of the MindStudio project.
 * Copyright (c) 2026 Huawei Technologies Co.,Ltd.
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
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <typeinfo>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// 先引入第三方库，避免后续 private/public 宏污染其内部实现
#include "nlohmann/json.hpp"

#include "securec.h"

#include "bit_field.h"

// 白盒访问 ConfigManager/EventTraceManager/FileCreateManager/JsonManager/Log 的私有成员，
// 用于用例自归一（SetUp 注入、TearDown 恢复），避免对跨用例单例做绝对断言
#define private public
#include "event_trace/trace_manager/event_trace_manager.h"
#include "file.h"
#include "json_manager.h"
#include "log.h"
#undef private

#include "client_parser.h"
#include "cpython.h"

using namespace MemScope;

namespace
{
// 与 Utility::MSMEMSCOPE_CONFIG_ENV 取值保持一致（JsonConfig 读写均经该环境变量）
constexpr const char* kConfigEnvName = "MSMEMSCOPE_CONFIG_PATH";
constexpr const char* kPythonInitFailed = "Python Interpreter initialization FAILED";

std::string MakeTempDir()
{
    char pathTemplate[] = "/tmp/msms_etm_XXXXXX";
    char* dir = ::mkdtemp(pathTemplate);
    return dir == nullptr ? std::string() : std::string(dir);
}

// 递归删除用例创建的临时目录；路径不存在时静默返回
void RemoveDirTree(const std::string& path)
{
    struct stat st;
    if (path.empty() || ::lstat(path.c_str(), &st) != 0)
    {
        return;
    }
    if (!S_ISDIR(st.st_mode))
    {
        (void)::unlink(path.c_str());
        return;
    }
    DIR* dir = ::opendir(path.c_str());
    if (dir != nullptr)
    {
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(dir)) != nullptr)
        {
            const std::string name(entry->d_name);
            if (name == "." || name == "..")
            {
                continue;
            }
            RemoveDirTree(path + "/" + name);
        }
        (void)::closedir(dir);
    }
    (void)::rmdir(path.c_str());
}

bool WriteTextFile(const std::string& path, const std::string& content)
{
    std::ofstream ofs(path);
    if (!ofs.is_open())
    {
        return false;
    }
    ofs << content;
    ofs.close();
    return true;
}

std::string ReadFromFilePtr(std::FILE* fp)
{
    if (fp == nullptr)
    {
        return std::string();
    }
    (void)::fflush(fp);
    const int fd = ::fileno(fp);
    if (fd < 0 || ::lseek(fd, 0, SEEK_SET) < 0)
    {
        return std::string();
    }
    std::string content;
    char buffer[1024];
    ssize_t len = 0;
    while ((len = ::read(fd, buffer, sizeof(buffer))) > 0)
    {
        content.append(buffer, static_cast<size_t>(len));
    }
    return content;
}

size_t CountSubString(const std::string& text, const std::string& token)
{
    if (token.empty())
    {
        return 0;
    }
    size_t count = 0;
    size_t pos = text.find(token);
    while (pos != std::string::npos)
    {
        ++count;
        pos = text.find(token, pos + token.size());
    }
    return count;
}

Config MakeBaselineConfig(const std::string& outputDir)
{
    Config config = {};
    ClientParser parser;
    parser.InitialConfig(config);
    // python config接口路径固定按DEFERRED进入（GetConfigAfterInit 的语义）
    config.collectMode = static_cast<uint8_t>(CollectMode::DEFERRED);
    config.isEffective = false;
    EXPECT_EQ(strncpy_s(config.outputDir, sizeof(config.outputDir), outputDir.c_str(),
                        sizeof(config.outputDir) - 1), EOK);
    return config;
}

// 用例自归一：SetUp保存并注入临时目录/配置/单例状态，TearDown逐项恢复
class EventTraceManagerTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        tmpDir_ = MakeTempDir();
        ASSERT_FALSE(tmpDir_.empty());

        auto& fileManager = Utility::FileCreateManager::GetInstance("");
        savedOutputDir_ = fileManager.outputDir_;
        savedProjectDir_ = fileManager.projectDir_;
        savedProjectDirPinned_ = fileManager.projectDirPinned_;
        fileManager.outputDir_ = tmpDir_;
        fileManager.projectDir_ = tmpDir_;
        fileManager.projectDirPinned_ = true;

        auto& configManager = ConfigManager::Instance();
        savedConfig_ = configManager.config_;
        savedFirstConfig_ = configManager.firstConfig;
        savedInited_ = ConfigManager::Inited;
        baselineConfig_ = MakeBaselineConfig(tmpDir_);
        configManager.config_ = baselineConfig_;
        configManager.firstConfig = false;
        ConfigManager::Inited = true;

        auto& traceManager = EventTraceManager::Instance();
        savedStatus_ = traceManager.status_;
        savedAclInit_ = traceManager.aclInit_.load();
        savedDeviceReady_ = traceManager.deviceReady_.load();
        savedDestroyed_ = traceManager.destroyed_.load();
        traceManager.status_ = EventTraceStatus::NOT_IN_TRACING;
        traceManager.aclInit_.store(false);
        traceManager.deviceReady_.store(false);
        traceManager.destroyed_.store(false);
        traceManager.InitJudgeFuncTable();

        // 日志重定向到临时文件，便于断言关键分支的日志条数，且不向工作目录落盘
        logFp_ = std::tmpfile();
        ASSERT_NE(logFp_, nullptr);
        savedLogFp_ = Utility::Log::GetLog().fp_;
        savedLogLv_ = Utility::Log::GetLog().lv_;
        Utility::Log::GetLog().fp_ = logFp_;
        Utility::Log::GetLog().lv_ = LogLv::DEBUG;

        auto& jsonManager = Utility::JsonManager::GetInstance();
        savedJsonConfig_ = jsonManager.jsonConfig_;
        savedJsonFilePath_ = jsonManager.jsonFilePath_;
        savedJsonFp_ = jsonManager.fp_;
        jsonManager.jsonConfig_ = nlohmann::json::object();
        jsonManager.jsonFilePath_.clear();
        jsonManager.fp_ = nullptr;

        const char* envPath = std::getenv(kConfigEnvName);
        hadConfigEnv_ = envPath != nullptr;
        if (hadConfigEnv_)
        {
            savedConfigEnv_ = envPath;
        }
        (void)::unsetenv(kConfigEnvName);
    }

    void TearDown() override
    {
        if (hadConfigEnv_)
        {
            (void)::setenv(kConfigEnvName, savedConfigEnv_.c_str(), 1);
        }
        else
        {
            (void)::unsetenv(kConfigEnvName);
        }

        auto& jsonManager = Utility::JsonManager::GetInstance();
        jsonManager.jsonConfig_ = savedJsonConfig_;
        jsonManager.jsonFilePath_ = savedJsonFilePath_;
        jsonManager.fp_ = savedJsonFp_;

        Utility::Log::GetLog().fp_ = savedLogFp_;
        Utility::Log::GetLog().lv_ = savedLogLv_;
        if (logFp_ != nullptr)
        {
            (void)std::fclose(logFp_);
            logFp_ = nullptr;
        }

        auto& traceManager = EventTraceManager::Instance();
        traceManager.status_ = savedStatus_;
        traceManager.aclInit_.store(savedAclInit_);
        traceManager.deviceReady_.store(savedDeviceReady_);
        traceManager.destroyed_.store(savedDestroyed_);
        traceManager.InitJudgeFuncTable();

        auto& configManager = ConfigManager::Instance();
        configManager.config_ = savedConfig_;
        configManager.firstConfig = savedFirstConfig_;
        ConfigManager::Inited = savedInited_;

        auto& fileManager = Utility::FileCreateManager::GetInstance("");
        fileManager.outputDir_ = savedOutputDir_;
        fileManager.projectDir_ = savedProjectDir_;
        fileManager.projectDirPinned_ = savedProjectDirPinned_;

        RemoveDirTree(tmpDir_);
    }

    size_t LogCount(const std::string& token) { return CountSubString(ReadFromFilePtr(logFp_), token); }

    std::string tmpDir_;
    Config baselineConfig_;

    Config savedConfig_;
    bool savedFirstConfig_ = false;
    bool savedInited_ = false;

    EventTraceStatus savedStatus_ = EventTraceStatus::NOT_IN_TRACING;
    bool savedAclInit_ = false;
    bool savedDeviceReady_ = false;
    bool savedDestroyed_ = false;

    std::string savedOutputDir_;
    std::string savedProjectDir_;
    bool savedProjectDirPinned_ = false;

    std::FILE* logFp_ = nullptr;
    std::FILE* savedLogFp_ = nullptr;
    LogLv savedLogLv_ = LogLv::WARN;
    nlohmann::json savedJsonConfig_;
    std::string savedJsonFilePath_;
    std::FILE* savedJsonFp_ = nullptr;

    bool hadConfigEnv_ = false;
    std::string savedConfigEnv_;
};

// InitConfig：firstConfig为true且配置文件可读时走JSON分支，读取后置false并标记Inited
TEST_F(EventTraceManagerTest, init_config_reads_json_file_and_marks_inited)
{
    nlohmann::json content;
    content["outputDir"] = tmpDir_;
    // analysisType在配置文件中按位图存取（位序号为AnalysisType枚举值）
    BitField<uint8_t> jsonAnalysisType;
    jsonAnalysisType.setBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS));
    content["analysisType"] = jsonAnalysisType.getValue();
    content["oomTopK"] = 20;
    content["hostLeakMode"] = 1;
    content["blockSizeThreshold"] = 4096;
    content["sampleRate"] = 2;
    content["collectMode"] = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    content["dataFormat"] = static_cast<uint8_t>(DataFormat::DB);
    content["eventType"] = 0x0F;
    content["dumpEventType"] = 0x03;
    content["levelType"] = 3;
    content["logLevel"] = static_cast<uint8_t>(LogLv::DEBUG);
    content["cStackDepth"] = 2000;  // 超过上限，读取时应钳位到1000
    content["pyStackDepth"] = 3;
    content["npuSlots"] = 5;
    content["collectAllNpu"] = false;
    content["collectCpu"] = true;
    content["enableCStack"] = true;
    content["enablePyStack"] = true;
    content["enableCompare"] = true;
    content["isEffective"] = true;
    content["SelectedStepList"] = {{"stepIdList", {11, 22}}, {"stepCount", 2}};
    content["watchConfig"] = {{"isWatched", true}, {"fullContent", true}, {"start", "start_op"},
                              {"end", "end_op"}, {"outputId", 5}};
    const std::string configPath = tmpDir_ + "/input_config.json";
    ASSERT_TRUE(WriteTextFile(configPath, content.dump()));
    ASSERT_EQ(::setenv(kConfigEnvName, configPath.c_str(), 1), 0);

    auto& configManager = ConfigManager::Instance();
    configManager.firstConfig = true;
    ConfigManager::Inited = false;

    configManager.InitConfig();

    const Config& config = configManager.GetConfig();
    EXPECT_FALSE(configManager.firstConfig);
    EXPECT_TRUE(ConfigManager::Inited);
    EXPECT_STREQ(config.outputDir, tmpDir_.c_str());
    EXPECT_EQ(config.analysisType, jsonAnalysisType.getValue());
    EXPECT_EQ(config.eventType, 0x0F);
    EXPECT_EQ(config.levelType, 3);
    EXPECT_EQ(config.dataFormat, static_cast<uint8_t>(DataFormat::DB));
    EXPECT_EQ(config.sampleRate, 2u);
    EXPECT_EQ(config.blockSizeThreshold, 4096u);
    EXPECT_EQ(config.cStackDepth, 1000u);
    EXPECT_EQ(config.pyStackDepth, 3u);
    EXPECT_TRUE(config.isEffective);
    EXPECT_TRUE(config.watchConfig.isWatched);
    EXPECT_TRUE(config.watchConfig.fullContent);
    EXPECT_STREQ(config.watchConfig.start, "start_op");
    EXPECT_STREQ(config.watchConfig.end, "end_op");
    EXPECT_EQ(config.watchConfig.outputId, 5u);
    EXPECT_EQ(config.stepList.stepCount, 2);
    EXPECT_EQ(config.stepList.stepIdList[0], 11u);
    EXPECT_EQ(config.stepList.stepIdList[1], 22u);
}

// GetConfigAfterInit：collectMode固定DEFERRED，isEffective/dataFormat/watchConfig/outputDir继承已生效配置
TEST_F(EventTraceManagerTest, get_config_after_init_inherits_effective_fields)
{
    auto& configManager = ConfigManager::Instance();
    Config effective = baselineConfig_;
    effective.isEffective = true;
    effective.dataFormat = static_cast<uint8_t>(DataFormat::DB);
    effective.watchConfig.isWatched = true;
    effective.watchConfig.fullContent = true;
    effective.watchConfig.outputId = 7;
    ASSERT_EQ(strncpy_s(effective.watchConfig.start, sizeof(effective.watchConfig.start), "start_op",
                        sizeof(effective.watchConfig.start) - 1), EOK);
    ASSERT_EQ(strncpy_s(effective.watchConfig.end, sizeof(effective.watchConfig.end), "end_op",
                        sizeof(effective.watchConfig.end) - 1), EOK);
    configManager.config_ = effective;

    Config out = {};
    configManager.GetConfigAfterInit(out);

    EXPECT_EQ(out.collectMode, static_cast<uint8_t>(CollectMode::DEFERRED));
    EXPECT_TRUE(out.isEffective);
    EXPECT_EQ(out.dataFormat, static_cast<uint8_t>(DataFormat::DB));
    EXPECT_TRUE(out.watchConfig.isWatched);
    EXPECT_TRUE(out.watchConfig.fullContent);
    EXPECT_STREQ(out.watchConfig.start, "start_op");
    EXPECT_STREQ(out.watchConfig.end, "end_op");
    EXPECT_EQ(out.watchConfig.outputId, 7u);
    EXPECT_STREQ(out.outputDir, tmpDir_.c_str());
    // 未在已生效配置中定制的字段回到InitialConfig默认值（analysisType为位图，默认仅置leaks位）
    EXPECT_EQ(out.levelType, 1);
    EXPECT_EQ(out.eventType, 7);
    BitField<uint8_t> defaultAnalysisType;
    defaultAnalysisType.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    EXPECT_EQ(out.analysisType, defaultAnalysisType.getValue());
}

// SetConfig(map)：未知键直接返回false，且不改变当前配置
TEST_F(EventTraceManagerTest, set_config_map_rejects_unknown_key)
{
    auto& configManager = ConfigManager::Instance();
    std::unordered_map<std::string, std::string> pythonConfig = {{"bogus_key", "value"}};

    EXPECT_FALSE(configManager.SetConfig(pythonConfig));

    const Config& config = configManager.GetConfig();
    EXPECT_EQ(config.eventType, baselineConfig_.eventType);
    EXPECT_EQ(config.levelType, baselineConfig_.levelType);
    EXPECT_FALSE(config.isEffective);
}

// SetConfig(map)：兼容旧键data_format会提示迁移，但解析结果照常生效并置isEffective
TEST_F(EventTraceManagerTest, set_config_map_deprecated_key_prints_hint_and_applies)
{
    std::ostringstream errOutput;
    std::streambuf* oldErrBuf = std::cerr.rdbuf(errOutput.rdbuf());
    const bool result = ConfigManager::Instance().SetConfig({{"data_format", "db"}});
    std::cerr.rdbuf(oldErrBuf);

    EXPECT_TRUE(result);
    EXPECT_NE(errOutput.str().find("Deprecation: 'data_format' is deprecated, use 'format' instead."),
              std::string::npos);
    const Config& config = ConfigManager::Instance().GetConfig();
    EXPECT_EQ(config.dataFormat, static_cast<uint8_t>(DataFormat::DB));
    EXPECT_TRUE(config.isEffective);
}

// SetConfig(map)：同一次调用中标准名与旧名属于同一策略组，仅首次生效并打印一次"只能设置一次"告警
TEST_F(EventTraceManagerTest, set_config_map_duplicate_policy_group_warns_once)
{
    std::ostringstream outOutput;
    std::ostringstream errOutput;
    std::streambuf* oldOutBuf = std::cout.rdbuf(outOutput.rdbuf());
    std::streambuf* oldErrBuf = std::cerr.rdbuf(errOutput.rdbuf());
    std::unordered_map<std::string, std::string> pythonConfig = {{"format", "db"}, {"data_format", "db"}};
    const bool result = ConfigManager::Instance().SetConfig(pythonConfig);
    std::cout.rdbuf(oldOutBuf);
    std::cerr.rdbuf(oldErrBuf);

    EXPECT_TRUE(result);
    EXPECT_EQ(CountSubString(outOutput.str(), "can only be set once."), 1u);
    EXPECT_EQ(CountSubString(errOutput.str(), "is deprecated, use 'format' instead."), 1u);
    EXPECT_EQ(ConfigManager::Instance().GetConfig().dataFormat, static_cast<uint8_t>(DataFormat::DB));
}

// SetConfig(map)：配置已生效时不能再修改once-only策略组，仅告警且保持原值
TEST_F(EventTraceManagerTest, set_config_map_effective_config_rejects_policy_group)
{
    auto& configManager = ConfigManager::Instance();
    Config effective = baselineConfig_;
    effective.isEffective = true;
    configManager.config_ = effective;

    std::ostringstream outOutput;
    std::streambuf* oldOutBuf = std::cout.rdbuf(outOutput.rdbuf());
    const bool result = configManager.SetConfig({{"format", "db"}});
    std::cout.rdbuf(oldOutBuf);

    EXPECT_TRUE(result);
    EXPECT_NE(outOutput.str().find("can only be set once."), std::string::npos);
    EXPECT_EQ(configManager.GetConfig().dataFormat, static_cast<uint8_t>(DataFormat::CSV));
}

// SetConfig(map)：参数解析失败立即返回false，配置保持未生效且不被部分改写
TEST_F(EventTraceManagerTest, set_config_map_parse_failure_keeps_config)
{
    auto& configManager = ConfigManager::Instance();
    const uint8_t eventTypeBefore = configManager.GetConfig().eventType;

    EXPECT_FALSE(configManager.SetConfig({{"sample_rate", "0"}}));

    const Config& config = configManager.GetConfig();
    EXPECT_FALSE(config.isEffective);
    EXPECT_EQ(config.sampleRate, 1u);
    EXPECT_EQ(config.eventType, eventTypeBefore);
}

// SetConfig(map)：全部键合法时依次解析并落配置，随后刷新判据表与已生效标记
TEST_F(EventTraceManagerTest, set_config_map_applies_all_keys_and_refreshes_judge_table)
{
    auto& configManager = ConfigManager::Instance();
    std::unordered_map<std::string, std::string> pythonConfig = {{"events", "alloc,free"},
                                                                 {"level", "kernel"},
                                                                 {"analysis", "leaks"},
                                                                 {"output_path", tmpDir_}};

    ASSERT_TRUE(configManager.SetConfig(pythonConfig));

    const Config& config = configManager.GetConfig();
    BitField<uint8_t> expectedEvents;
    expectedEvents.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    expectedEvents.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    EXPECT_EQ(config.eventType, expectedEvents.getValue());
    BitField<uint8_t> expectedLevel;
    expectedLevel.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    EXPECT_EQ(config.levelType, expectedLevel.getValue());
    // ParseAnalysis按位图写入（位序号即枚举值），"leaks"对应bit0
    BitField<uint8_t> expectedAnalysis;
    expectedAnalysis.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    EXPECT_EQ(config.analysisType, expectedAnalysis.getValue());
    EXPECT_TRUE(config.isEffective);
    EXPECT_STREQ(config.outputDir, tmpDir_.c_str());

    auto& traceManager = EventTraceManager::Instance();
    traceManager.status_ = EventTraceStatus::IN_TRACING;
    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::MALLOC));
    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::FREE));
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::KERNEL_LAUNCH));
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::OP_LAUNCH));
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::ACCESS));
}

// InitJudgeFuncTable：判据表随levelType/eventType位动态求值
TEST_F(EventTraceManagerTest, judge_table_gates_by_level_and_event_bits)
{
    auto& configManager = ConfigManager::Instance();
    auto& traceManager = EventTraceManager::Instance();
    traceManager.status_ = EventTraceStatus::IN_TRACING;
    traceManager.destroyed_.store(false);

    Config config = baselineConfig_;
    BitField<uint8_t> levelBits;
    levelBits.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    levelBits.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    BitField<uint8_t> eventBits;
    eventBits.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBits.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBits.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    eventBits.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    config.levelType = levelBits.getValue();
    config.eventType = eventBits.getValue();
    configManager.config_ = config;
    traceManager.InitJudgeFuncTable();

    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::MALLOC));
    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::FREE));
    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::KERNEL_LAUNCH));
    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::OP_LAUNCH));
    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::ACCESS));

    // 事件位全零：表内类型全部判定为不采集
    config.eventType = 0;
    configManager.config_ = config;
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::MALLOC));
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::FREE));
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::KERNEL_LAUNCH));
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::OP_LAUNCH));
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::ACCESS));

    // 仅op级+launch：OP_LAUNCH置位，KERNEL_LAUNCH不置位
    BitField<uint8_t> opLevel;
    opLevel.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    BitField<uint8_t> launchOnly;
    launchOnly.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.levelType = opLevel.getValue();
    config.eventType = launchOnly.getValue();
    configManager.config_ = config;
    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::OP_LAUNCH));
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::KERNEL_LAUNCH));
}

// IsNeedTrace/IsTracingEnabled：非采集态、单例已析构、判据表外类型的分支
TEST_F(EventTraceManagerTest, is_need_trace_respects_status_destroyed_and_unknown_type)
{
    auto& traceManager = EventTraceManager::Instance();

    traceManager.status_ = EventTraceStatus::NOT_IN_TRACING;
    traceManager.destroyed_.store(false);
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::MALLOC));
    EXPECT_FALSE(traceManager.IsTracingEnabled());

    traceManager.status_ = EventTraceStatus::IN_TRACING;
    traceManager.destroyed_.store(true);
    EXPECT_FALSE(traceManager.IsNeedTrace(EventBaseType::MALLOC));
    EXPECT_FALSE(traceManager.IsTracingEnabled());
    // 恢复后采集态可正常判定
    traceManager.destroyed_.store(false);
    EXPECT_TRUE(traceManager.IsTracingEnabled());
    // 判据表未登记的类型默认采集
    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::INVALID));
    EXPECT_TRUE(traceManager.IsNeedTrace(EventBaseType::MSTX));
}

// DetermineTraceMode：非采集态且无shadow需求时返回SKIP；只有alloc位时走SHADOW
TEST_F(EventTraceManagerTest, determine_trace_mode_skip_and_shadow)
{
    auto& configManager = ConfigManager::Instance();
    auto& traceManager = EventTraceManager::Instance();
    traceManager.status_ = EventTraceStatus::NOT_IN_TRACING;
    traceManager.destroyed_.store(false);

    configManager.config_.eventType = 0;
    EXPECT_EQ(DetermineTraceMode(), TraceMode::SKIP);

    BitField<uint8_t> allocOnly;
    allocOnly.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    configManager.config_.eventType = allocOnly.getValue();
    EXPECT_EQ(DetermineTraceMode(), TraceMode::SHADOW);

    BitField<uint8_t> memoryEvents;
    memoryEvents.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    memoryEvents.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    configManager.config_.eventType = memoryEvents.getValue();
    traceManager.status_ = EventTraceStatus::IN_TRACING;
    EXPECT_EQ(DetermineTraceMode(), TraceMode::NORMAL);
}

// SetDeviceReadyStatus：非ready直接返回；ready仅首次生效（CAS）；仅采集态且开启CPU采集时触发python调用
TEST_F(EventTraceManagerTest, set_device_ready_status_transitions_once)
{
    auto& configManager = ConfigManager::Instance();
    auto& traceManager = EventTraceManager::Instance();
    configManager.config_.collectCpu = true;
    const size_t logBefore = LogCount(kPythonInitFailed);

    traceManager.status_ = EventTraceStatus::IN_TRACING;
    traceManager.deviceReady_.store(false);
    traceManager.SetDeviceReadyStatus(false);
    EXPECT_FALSE(traceManager.IsDeviceReady());

    traceManager.SetDeviceReadyStatus(true);
    EXPECT_TRUE(traceManager.IsDeviceReady());
    const size_t logAfterFirstReady = LogCount(kPythonInitFailed);
    if (!Utility::IsPyInterpRepeInited())
    {
        // 解释器未初始化时每次python调用固定打印一条错误日志
        EXPECT_EQ(logAfterFirstReady - logBefore, 1u);
    }

    // 再次置ready：CAS失败直接返回，不重复触发python调用
    traceManager.SetDeviceReadyStatus(true);
    EXPECT_TRUE(traceManager.IsDeviceReady());
    EXPECT_EQ(LogCount(kPythonInitFailed), logAfterFirstReady);

    // 非采集态置ready：仍标记就绪，但不触发python调用
    traceManager.deviceReady_.store(false);
    traceManager.status_ = EventTraceStatus::NOT_IN_TRACING;
    traceManager.SetDeviceReadyStatus(true);
    EXPECT_TRUE(traceManager.IsDeviceReady());
    EXPECT_EQ(LogCount(kPythonInitFailed), logAfterFirstReady);
}

// HandleWithDecompose：拆解分析位+采集态+acl已初始化时走启用分支，否则走禁用分支
TEST_F(EventTraceManagerTest, handle_with_decompose_enabled_and_disabled_branches)
{
    auto& configManager = ConfigManager::Instance();
    auto& traceManager = EventTraceManager::Instance();
    BitField<uint8_t> decomposeBit;
    decomposeBit.setBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS));
    configManager.config_.analysisType = decomposeBit.getValue();

    traceManager.status_ = EventTraceStatus::IN_TRACING;
    traceManager.aclInit_.store(true);
    const size_t logBefore = LogCount(kPythonInitFailed);

    traceManager.HandleWithDecompose();  // 启用分支：optimizer_step_hook + hijack_manager
    traceManager.aclInit_.store(false);
    traceManager.HandleWithDecompose();  // 禁用分支：同名两模块的disable调用

    if (!Utility::IsPyInterpRepeInited())
    {
        // 两条分支各调用两次MemScopePythonCall，解释器未初始化时各产生一条错误日志
        EXPECT_EQ(LogCount(kPythonInitFailed) - logBefore, 4u);
    }
    EXPECT_FALSE(traceManager.aclInit_.load());
}

}  // namespace
