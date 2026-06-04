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

#include "event_trace_manager.h"

#include "bit_field.h"
#include "client_parser.h"
#include "cpython.h"
#include "dump.h"
#include "event_report.h"
#include "json_manager.h"

namespace MemScope
{

bool ConfigManager::Inited = false;

const std::unordered_map<std::string, std::function<void(const std::string &, Config &, bool &)>> parserConfigTable = {
    {"call_stack", ParseCallstack}, {"level", ParseDataLevel},        {"events", ParseEventTraceType},
    {"device", ParseDevice},        {"data_format", ParseDataFormat}, {"output", ParseOutputPath},
    {"analysis", ParseAnalysis},    {"watch", ParseWatchConfig},
};

// 只允许设置一次的config参数
const std::vector<std::string> configPolicyTable = {
    "output",
    "data_format",
    "watch",
};

ConfigManager::ConfigManager() { InitConfig(); }

void ConfigManager::InitConfig()
{
    Config config;
    // 命令行与python接口并存
    if (firstConfig && Utility::JsonConfig::GetInstance().ReadJsonConfig(config))
    {
        firstConfig = false;
        config_ = config;
    }
    else
    {
        // 单独python接口
        ClientParser parser;
        parser.InitialConfig(config);
        SetConfigImpl(config);
    }
    ConfigManager::Inited = true;
}

// 在python config接口时，将需要继承和不准修改的参数保留；不准修改的针对命令行传入的；
void ConfigManager::GetConfigAfterInit(Config &config)
{
    ClientParser parser;
    parser.InitialConfig(config);
    config.collectMode = static_cast<uint8_t>(CollectMode::DEFERRED);
    config.isEffective = config_.isEffective;
    config.dataFormat = config_.dataFormat;
    config.watchConfig = config_.watchConfig;

    if (strncpy_s(config.outputDir, sizeof(config.outputDir), config_.outputDir, sizeof(config.outputDir) - 1) != EOK)
    {
        std::cout << "[msmemscope] Error: strncpy dirpath FAILED" << std::endl;
        return;
    }
    config.outputDir[sizeof(config.outputDir) - 1] = '\0';
}

void ConfigManager::InitStartConfig()
{
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);
    Utility::JsonConfig::GetInstance().SaveConfigToJson(config_);
}

const Config &ConfigManager::GetConfig()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void ConfigManager::SetConfigImpl(const Config &config)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        config_ = config;
        SetEventDefaultConfig(config_);
        SetAnalysisDefaultConfig(config_);
    }
}

void ConfigManager::SetConfig(const Config &config)
{
    SetConfigImpl(config);
    SetEffectiveConfig(config_);
    Utility::SetLogLevel(static_cast<LogLv>(config_.logLevel));
    Utility::JsonConfig::GetInstance().SaveConfigToJson(config_);
    // HandleWithATenCollect 会初始化EventReport，需要在save之后
    // EventTraceManager的构造函数中会调用InitTraceStatus，会调用GetConfig，如果在InitConfig函数中调用SetConfig，可能
    // 会造成Instance()间接调用自身，导致单例的自引用，初始化死锁现象。
    EventTraceManager::Instance().HandleWithATenCollect();
    EventTraceManager::Instance().HandleWithDecompose();
}

bool ConfigManager::SetConfig(const std::unordered_map<std::string, std::string> &pythonConfig)
{
    Config config;
    GetConfigAfterInit(config);

    for (auto &p : pythonConfig)
    {
        const std::string &key = p.first;
        const std::string &value = p.second;
        auto itr = parserConfigTable.find(key);
        if (itr == parserConfigTable.end())
        {
            return false;
        }

        auto policyItr = std::find(configPolicyTable.begin(), configPolicyTable.end(), key);
        if (policyItr != configPolicyTable.end() && config.isEffective)
        {
            std::cout << "[msmemscope] Warn: Config:\"output\",\"data_format\",\"watch\" cannot be set twice."
                      << std::endl;
            continue;
        }
        bool parseFail = false;
        itr->second(value, config, parseFail);
        if (parseFail)
        {
            return false;
        }
    }
    SetConfigImpl(config);
    SetEffectiveConfig(config_);
    Utility::SetLogLevel(static_cast<LogLv>(config_.logLevel));
    Utility::JsonConfig::GetInstance().SaveConfigToJson(config_);
    EventTraceManager::Instance().HandleWithATenCollect();
    EventTraceManager::Instance().HandleWithDecompose();
    EventTraceManager::Instance().InitJudgeFuncTable();
    // 更新analysis参数
    EventReport::Instance(MemScopeCommType::SHARED_MEMORY).UpdateAnalysisType();

    return true;
}

bool IsNeedTraceOp() { return BitPresent(GetConfig().levelType, static_cast<size_t>(LevelType::LEVEL_OP)); }
bool IsNeedTraceKernel() { return BitPresent(GetConfig().levelType, static_cast<size_t>(LevelType::LEVEL_KERNEL)); }
bool IsNeedTraceAccess() { return BitPresent(GetConfig().eventType, static_cast<size_t>(EventType::ACCESS_EVENT)); }
bool IsNeedTraceLaunch() { return BitPresent(GetConfig().eventType, static_cast<size_t>(EventType::LAUNCH_EVENT)); }
bool IsNeedTraceAlloc() { return BitPresent(GetConfig().eventType, static_cast<size_t>(EventType::ALLOC_EVENT)); }
bool IsNeedTraceFree() { return BitPresent(GetConfig().eventType, static_cast<size_t>(EventType::FREE_EVENT)); }
bool IsNeedTraceKernelLaunch() { return IsNeedTraceKernel() && IsNeedTraceLaunch(); }
bool IsNeedTraceOpLaunch() { return IsNeedTraceOp() && IsNeedTraceLaunch(); }
bool IsNeedTraceMemory() { return IsNeedTraceAlloc() && IsNeedTraceFree(); }

void EventTraceManager::InitJudgeFuncTable()
{
    judgeFuncTable_ = {
        {EventBaseType::KERNEL_LAUNCH, []() { return IsNeedTraceKernelLaunch(); }},
        {EventBaseType::MALLOC, []() { return IsNeedTraceMemory(); }},
        {EventBaseType::FREE, []() { return IsNeedTraceMemory(); }},
        {EventBaseType::OP_LAUNCH, []() { return IsNeedTraceOpLaunch(); }},
        {EventBaseType::ACCESS, []() { return IsNeedTraceAccess(); }},
    };
};

// 1、判断是否处在采集范围
// 2、判断当前的采集项是否需要采集
bool EventTraceManager::IsNeedTrace(EventBaseType type)
{
    if (status_ != EventTraceStatus::IN_TRACING)
    {
        return false;
    }

    // 单例类析构之后不再访问其成员变量
    if (destroyed_.load())
    {
        return false;
    }

    auto itr = judgeFuncTable_.find(type);
    if (itr == judgeFuncTable_.end())
    {
        return true;
    }

    return itr->second();
}

bool EventTraceManager::IsTracingEnabled()
{
    if (status_ != EventTraceStatus::IN_TRACING)
    {
        return false;
    }

    return true;
}

void EventTraceManager::InitTraceStatus()
{
    auto status = (GetConfig().collectMode == static_cast<uint8_t>(CollectMode::IMMEDIATE)) && GetConfig().isEffective
                      ? EventTraceStatus::IN_TRACING
                      : EventTraceStatus::NOT_IN_TRACING;
    status_ = status;
    return;
}

void EventTraceManager::SetTraceStatus(const EventTraceStatus status)
{
    if (status == EventTraceStatus::IN_TRACING)
    {
        std::cout << "[msmemscope] Info: Start tracing.\n";
    }
    else if (status == EventTraceStatus::NOT_IN_TRACING)
    {
        std::cout << "[msmemscope] Info: Stop tracing.\n";
    }
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportTraceStatus(status))
    {
        std::cout << "[msmemscope] Error: Report trace status failed.\n";
    }

    std::lock_guard<std::mutex> lock(mutex_);

    status_ = status;

    HandleWithATenCollect();
    HandleWithDecompose();
    return;
}

void EventTraceManager::HandleWithATenCollect()
{
    if ((status_ == EventTraceStatus::IN_TRACING) && IsNeedTraceOp() && aclInit_)
    {
        Utility::MemScopePythonCall("msmemscope.aten_collection", "enable_aten_collector");
        return;
    }

    Utility::MemScopePythonCall("msmemscope.aten_collection", "disable_aten_collector");

    return;
}

void EventTraceManager::HandleWithDecompose()
{
    if ((status_ == EventTraceStatus::IN_TRACING) &&
        BitPresent(GetConfig().analysisType, static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS)) && aclInit_)
    {
        Utility::MemScopePythonCall("msmemscope.optimizer_step_hook", "enable_optimizer_step_hook");
        return;
    }

    Utility::MemScopePythonCall("msmemscope.optimizer_step_hook", "disable_optimizer_step_hook");

    return;
}

void EventTraceManager::SetAclInitStatus(bool isInit)
{
    aclInit_ = isInit;

    HandleWithATenCollect();
    HandleWithDecompose();
}

void EventTraceManager::CleanUpEventTraceManager()
{
    // 这里可以添加其他的CleanUp操作,最好把析构函数中的抽象出来,放到stop实现
    Config config = Config{};
    Dump::GetInstance(config).WritePublicEventToFile();
    Dump::GetInstance(config).FflushEventToFile();
}
}  // namespace MemScope
