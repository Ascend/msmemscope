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

#include "process.h"

#include <unistd.h>
#include <sys/wait.h>
#include <iostream>
#include <memory>
#include "path.h"
#include "ustring.h"
#include "log.h"
#include "analysis/mstx_analyzer.h"
#include "analysis/py_step_manager.h"
#include "analysis/hal_analyzer.h"
#include "analysis/stepinner_analyzer.h"
#include "analysis/event.h"
#include "analysis/memory_state_manager.h"
#include "analysis/event_dispatcher.h"
#include "analysis/decompose_analyzer.h"
#include "analysis/inefficient_analyzer.h"

namespace MemScope {

using std::string;
using namespace Utility;

ExecCmd::ExecCmd(std::vector<std::string> const &args) : path_{}, argc_{0}, args_{args}
{
    if (args_.empty()) {
        return;
    }

    /// filename to absolute path
    char *absPath = realpath(args[0].c_str(), nullptr);
    if (absPath) {
        path_ = std::string(absPath);
        free(absPath);
        absPath = nullptr;
    } else {
        path_ = args[0];
    }

    argc_ = static_cast<int>(args.size());
    for (auto &arg : args_) {
        argv_.push_back(const_cast<char *>(arg.data()));
    }
    argv_.push_back(nullptr);
}

std::string const &ExecCmd::ExecPath(void) const
{
    return path_;
}

char *const *ExecCmd::ExecArgv(void) const
{
    return argv_.data();
}

Process& Process::GetInstance(Config config)
{
    static Process process{config};
    return process;
}

bool Process::SendEvent(std::shared_ptr<EventBase> event)
{
    std::lock_guard<std::mutex> lock(processMutex_);
    EventHandler(event);
    switch (event->eventSubType) {
        case EventSubType::HAL:
            HalAnalyzer::GetInstance(config_).Record(event->pid, event);
            break;
        case EventSubType::PTA_CACHING:
        case EventSubType::ATB:
        case EventSubType::MINDSPORE:
            StepInnerAnalyzer::GetInstance(config_).Record(event->pid, event);
            break;
        case EventSubType::MSTX_MARK:
        case EventSubType::MSTX_RANGE_START:
        case EventSubType::MSTX_RANGE_END:
            MstxAnalyzer::Instance().RecordMstx(event->pid, event);
            break;
        case EventSubType::STEP:
            PyStepManager::Instance().RecordPyStep(event->pid, event);
            break;
        default:
            break;
    }
    return true;
}

void Process::Launch(const std::vector<std::string> &execParams)
{
    ExecCmd cmd(execParams);
    SetPreloadEnv();
    DoLaunch(cmd);  // 执行被检测程序

    return;
}

void Process::SetPreloadEnv()
{
    string hookLibDir = "../lib64/";
    char const *preloadPath = getenv("LD_PRELOAD_PATH");
    if (preloadPath != nullptr && !string(preloadPath).empty()) {
        hookLibDir = preloadPath;
    }
    std::vector<string> hookLibNames{
        "libleaks_ascend_hal_hook.so",
        "libascend_mstx_hook.so",
        "libascend_kernel_hook.so"
    };

    const char* atbHomePath = std::getenv("ATB_HOME_PATH");
    if (atbHomePath == nullptr || string(atbHomePath).empty()) {
        LOG_WARN("The environment variable ATB_HOME_PATH is not set.");
    } else {
        std::string pathStr(atbHomePath);
        std::string abi0Str = "atb/cxx_abi_0";
        std::string abi1Str = "atb/cxx_abi_1";
        if (pathStr.length() >= abi0Str.length() &&
            pathStr.substr(pathStr.length() - abi0Str.length()) == abi0Str) {
            hookLibNames.push_back("libatb_abi_0_hook.so");
        } else if (pathStr.length() >= abi1Str.length() &&
                   pathStr.substr(pathStr.length() - abi1Str.length()) == abi1Str) {
            hookLibNames.push_back("libatb_abi_1_hook.so");
        } else {
            LOG_ERROR("Please set the valid environment variable ATB_HOME_PATH.");
        }
    }

    for (string &hookLib : hookLibNames) {
        Path hookLibPath = (Path(hookLibDir) / Path(hookLib)).Resolved();
        if (hookLibPath.ErrorOccured()) { return; }
        if (hookLibPath.Exists()) {
            hookLib = hookLibPath.ToString();
            LOG_INFO("Use preload lib [%s]", hookLib.c_str());
        } else {
            LOG_ERROR("No such preload lib [%s]", hookLibPath.ToString().c_str());
        }
    }

    // 按照桩使能模式配置LD_PRELOAD环境变量
    string preloadEnv = Utility::Join(hookLibNames.cbegin(), hookLibNames.cend(), ":");
    const string envName = "LD_PRELOAD";
    auto prevLdPreEnv = getenv(envName.c_str());
    if (prevLdPreEnv && !string(prevLdPreEnv).empty()) {
        preloadEnv += ":" + string(prevLdPreEnv);
    }
    setenv(envName.c_str(), preloadEnv.c_str(), 1);
}

void Process::DoLaunch(const ExecCmd &cmd) const
{
    // pass all env-vars from global variable "environ"
    execvpe(cmd.ExecPath().c_str(), cmd.ExecArgv(), environ);
    _exit(EXIT_FAILURE);
}

void EventHandler(std::shared_ptr<EventBase> event)
{
    if (event == nullptr) {
        return;
    }

    MemoryState* state = nullptr;
    if (event->eventType == EventBaseType::MALLOC || event->eventType == EventBaseType::FREE
        || event->eventType == EventBaseType::ACCESS) {
        auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event);
        if (memEvent && !MemoryStateManager::GetInstance().AddEvent(memEvent)) {
            // 添加事件失败时，表明对应位置已存在事件，需先清空事件列表
            std::shared_ptr<EventBase> cleanUpEvent = std::make_shared<CleanUpEvent>(
                memEvent->poolType, memEvent->pid, memEvent->addr);
            EventHandler(cleanUpEvent);                                 // 最大递归深度为2，因为这里传入事件的类型为CLEAN_UP
            MemoryStateManager::GetInstance().AddEvent(memEvent);       // 再次尝试添加
        }
        if (memEvent) {
            state = MemoryStateManager::GetInstance().GetState(memEvent);
        }
    } else if (event->eventType == EventBaseType::MEMORY_OWNER || event->eventType == EventBaseType::CLEAN_UP) {
        state = MemoryStateManager::GetInstance().GetState(event);
    } else if (event->eventSubType == EventSubType::TRACE_START || event->eventSubType == EventSubType::TRACE_STOP) {
        for (auto& state : MemoryStateManager::GetInstance().GetAllStateKeys()) {
            // 清空对应pid的所有事件
            if (event->pid == state.second.pid) {
                std::shared_ptr<EventBase> cleanUpEvent = std::make_shared<CleanUpEvent>(
                    state.first, state.second.pid, state.second.addr);
                EventHandler(cleanUpEvent);
            }
        }
    }

    EventDispatcher::GetInstance().DispatchEvent(event, state);

    // 内存块生命周期结束，删除相关缓存数据
    if (event->eventType == EventBaseType::FREE || event->eventType == EventBaseType::CLEAN_UP) {
        MemoryStateManager::GetInstance().DeteleState(event->poolType, MemoryStateKey{event->pid, event->addr});
    }
}

}  // namespace MemScope