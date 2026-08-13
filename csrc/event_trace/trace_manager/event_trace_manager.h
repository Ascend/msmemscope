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

    // 配置是否已就绪：单例已初始化 且 配置已被显式设置（命令行启动或调用python接口config()）。
    // 仅因日志/钩子等消费方首次触碰而加载的缺省物化不算"已设置"：
    // 缺省配置不应消费once-only策略，也不应让日志等消费方以缺省outputDir提前落盘。
    static bool HasInited()
    {
        if (!Inited)
        {
            return false;
        }
        return Instance().GetConfig().isEffective;
    }

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

// 追踪模式：用于Hooks层统一判断采集策略
enum class TraceMode : uint8_t
{
    NORMAL = 0,  // 正常采集（含调用栈）
    SHADOW,      // 影子采集（仅addr/size/device，无调用栈）
    SKIP,        // 不采集
};

TraceMode DetermineTraceMode();

// 事件上报抑制机制：仪器自身调用真实运行时接口（如aclrtGetMemInfo）期间，
// 运行时内部的内存申请会被hook捕获并尝试上报，形成递归上报/幻影事件。
// 检查点为DetermineTraceMode（所有内存事件上报的咽喉），置位窗口见EventReportSuppressor
bool IsEventReportSuppressed();

// RAII守卫：进入真实运行时调用窗口时构造（计数+1），离开时析构（计数-1），
// 支持嵌套置位；窗口内同线程的所有内存事件上报均被跳过
class EventReportSuppressor
{
   public:
    EventReportSuppressor();
    ~EventReportSuppressor();

    EventReportSuppressor(const EventReportSuppressor&) = delete;
    EventReportSuppressor& operator=(const EventReportSuppressor&) = delete;
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
    bool ShouldCollectShadowEvents();
    void PromoteHistoricalStates();
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
