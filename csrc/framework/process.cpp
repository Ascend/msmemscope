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

#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <memory>

#include "log.h"
#include "path.h"
#include "ustring.h"

namespace MemScope
{

using std::string;
using namespace Utility;

ExecCmd::ExecCmd(std::vector<std::string> const &args) : path_{}, argc_{0}, args_{args}
{
    if (args_.empty())
    {
        return;
    }

    /// filename to absolute path
    char *absPath = realpath(args[0].c_str(), nullptr);
    if (absPath)
    {
        path_ = std::string(absPath);
        free(absPath);
        absPath = nullptr;
    }
    else
    {
        path_ = args[0];
    }

    argc_ = static_cast<int>(args.size());
    for (auto &arg : args_)
    {
        argv_.push_back(const_cast<char *>(arg.data()));
    }
    argv_.push_back(nullptr);
}

std::string const &ExecCmd::ExecPath(void) const { return path_; }

char *const *ExecCmd::ExecArgv(void) const { return argv_.data(); }

Process &Process::GetInstance(Config config)
{
    static Process process{config};
    return process;
}

bool Process::SendEvent(std::shared_ptr<EventBase> event)
{
    std::lock_guard<std::mutex> lock(processMutex_);
    // EventHandler内部通过EventDispatcher自动通知所有订阅者
    // （Dump、DecomposeAnalyzer、InefficientAnalyzer、HalAnalyzer、StepInnerAnalyzer）
    EventRouter::Instance().Route(event);
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
    if (preloadPath != nullptr && !string(preloadPath).empty())
    {
        hookLibDir = preloadPath;
    }

    std::vector<string> hookLibNames{"libleaks_ascend_hal_hook.so", "libascend_mstx_hook.so",
                                     "libascend_kernel_hook.so"};

    const char *atbHomePath = std::getenv("ATB_HOME_PATH");
    if (atbHomePath == nullptr || string(atbHomePath).empty())
    {
        LOG_WARN("The environment variable ATB_HOME_PATH is not set.");
    }
    else
    {
        std::string pathStr(atbHomePath);
        std::string abi0Str = "atb/cxx_abi_0";
        std::string abi1Str = "atb/cxx_abi_1";
        if (pathStr.length() >= abi0Str.length() && pathStr.substr(pathStr.length() - abi0Str.length()) == abi0Str)
        {
            hookLibNames.push_back("libatb_abi_0_hook.so");
        }
        else if (pathStr.length() >= abi1Str.length() && pathStr.substr(pathStr.length() - abi1Str.length()) == abi1Str)
        {
            hookLibNames.push_back("libatb_abi_1_hook.so");
        }
        else
        {
            LOG_ERROR("Please set the valid environment variable ATB_HOME_PATH.");
        }
    }

    for (string &hookLib : hookLibNames)
    {
        Path hookLibPath = (Path(hookLibDir) / Path(hookLib)).Resolved();
        if (hookLibPath.ErrorOccured())
        {
            return;
        }
        if (hookLibPath.Exists())
        {
            hookLibPath.DeclarePermissionRisk();
            hookLib = hookLibPath.ToString();
            LOG_INFO("Use preload lib [%s]", hookLib.c_str());
        }
        else
        {
            LOG_ERROR("No such preload lib [%s]", hookLibPath.ToString().c_str());
        }
    }

    // 按照桩使能模式配置LD_PRELOAD环境变量
    string preloadEnv = Utility::Join(hookLibNames.cbegin(), hookLibNames.cend(), ":");
    const string envName = "LD_PRELOAD";
    auto prevLdPreEnv = getenv(envName.c_str());
    if (prevLdPreEnv && !string(prevLdPreEnv).empty())
    {
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

}  // namespace MemScope
