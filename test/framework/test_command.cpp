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

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <gtest/gtest.h>

#include "command.h"
#include "config_info.h"

namespace {

// Launch分支最终由DoLaunch调用_exit失败退出；该开关置位时改用std::exit，
// 以便正常落盘gcov数据（仅本用例的子进程会置位）
bool g_flushGcovOnExit = false;

std::string MakeTempPath()
{
    char tmpl[] = "/tmp/msms_command_ut_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        return std::string();
    }
    close(fd);
    return std::string(tmpl);
}

// 在子进程中执行body并捕获stdout，子进程自行exit（不碰测试框架状态）
std::string RunInChildAndCapture(const std::function<void()>& body, int* exitCode = nullptr)
{
    const std::string path = MakeTempPath();
    if (path.empty()) {
        return std::string();
    }
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        body();
        std::exit(0);  // 正常退出以落盘gcov
    }
    if (pid < 0) {
        unlink(path.c_str());
        return std::string();
    }
    int status = 0;
    waitpid(pid, &status, 0);
    std::ifstream in(path);
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    unlink(path.c_str());
    EXPECT_TRUE(WIFEXITED(status));
    if (exitCode != nullptr) {
        *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return text;
}

}  // namespace

// 覆盖默认的Launch分支：不存在的可执行文件导致execvpe失败，子进程以1退出；
// Launch内部_exit由本文件的覆盖函数转为std::exit以落盘gcov
TEST(CommandTest, exec_launch_branch_missing_binary)
{
    MemScope::UserCommand userCommand{};
    userCommand.cmd = {"ut_definitely_missing_binary_msmemscope"};

    int exitCode = 0;
    std::string out = RunInChildAndCapture([&userCommand]() {
        g_flushGcovOnExit = true;
        MemScope::Command command(userCommand);
        command.Exec();
    }, &exitCode);

    // execvpe失败后进程必须以非0退出，且不应进入对比或attach流程
    EXPECT_EQ(exitCode, 1);
    EXPECT_EQ(out.find("Memory comparison failed!"), std::string::npos);
    EXPECT_EQ(out.find("does not exist"), std::string::npos);
}

// 仅本测试使用的_exit拦截：开关关闭时保持与_exit完全一致的语义
extern "C" void _exit(int status)
{
    if (g_flushGcovOnExit) {
        std::exit(status);
    }
    std::_Exit(status);
}
