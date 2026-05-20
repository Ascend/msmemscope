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

#ifndef FRAMEWORK_PROCESS_H
#define FRAMEWORK_PROCESS_H

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include "event.h"
#include "comm_def.h"

namespace MemScope {
struct ExecCmd {
    explicit ExecCmd(std::vector<std::string> const &args);
    std::string const &ExecPath(void) const;
    char *const *ExecArgv(void) const;

private:
    std::string path_;
    int argc_;
    std::vector<char*> argv_;
    std::vector<std::string> args_;
};

/*
 * Process类主要功能：
 * 1. Launch用于拉起被检测程序进程，并对client传回的数据进行转发
 * 2. 注册分析回调函数
*/
class Process {
public:
    static Process& GetInstance(Config config);
    void Launch(const std::vector<std::string> &execParams);
    bool SendEvent(std::shared_ptr<EventBase> event);

private:
    void SetPreloadEnv();
    void DoLaunch(const ExecCmd &cmd) const;

private:
    explicit Process(const Config &config) : config_(config) {}
    Config config_;
    std::mutex processMutex_;

    static constexpr const size_t MAX_EVENT_QUEUE_LEN = 4096;
};

void EventHandler(std::shared_ptr<EventBase> event);

}

#endif