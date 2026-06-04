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
#ifndef EVENT_TRACE_MANAGER_H
#define EVENT_TRACE_MANAGER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "config_info.h"
#include "event.h"
#include "record_info.h"

namespace MemScope
{

class ConfigManager
{
   public:
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    static ConfigManager& Instance()
    {
        static ConfigManager instance;
        return instance;
    }

    const Config& GetConfig();
    void InitConfig();
    void InitStartConfig();
    bool SetConfig(const std::unordered_map<std::string, std::string>& config);
    void SetConfig(const Config& config);

    static bool HasInited() { return Inited; }

   private:
    ConfigManager();

    ~ConfigManager() = default;

    void SetConfigImpl(const Config& config);
    void GetConfigAfterInit(Config& config);

    std::mutex mutex_;
    Config config_;
    bool firstConfig = true;

   private:
    static bool Inited;
};

inline const Config& GetConfig() { return ConfigManager::Instance().GetConfig(); }

enum class EventTraceStatus : uint8_t
{
    IN_TRACING = 0,
    NOT_IN_TRACING,
};

class EventTraceManager
{
   public:
    EventTraceManager(const EventTraceManager&) = delete;
    EventTraceManager& operator=(const EventTraceManager&) = delete;

    static EventTraceManager& Instance()
    {
        static EventTraceManager instance;
        return instance;
    }

    bool IsNeedTrace(EventBaseType type);
    bool IsTracingEnabled();
    void SetTraceStatus(const EventTraceStatus status);  // 通过python接口在运行时动态修改
    void InitJudgeFuncTable();
    void SetAclInitStatus(bool isInit);
    void HandleWithATenCollect();
    void HandleWithDecompose();
    void CleanUpEventTraceManager();

   private:
    EventTraceManager()
    {
        InitTraceStatus();
        InitJudgeFuncTable();
    }
    ~EventTraceManager() { destroyed_.store(true); }

    void InitTraceStatus();  // 命令行拉起时有一个初始化状态

    std::mutex mutex_;
    EventTraceStatus status_ = EventTraceStatus::NOT_IN_TRACING;

    std::atomic<bool> aclInit_{false};
    std::unordered_map<EventBaseType, std::function<bool()>> judgeFuncTable_;
    std::atomic<bool> destroyed_{false};
};

}  // namespace MemScope

#endif
