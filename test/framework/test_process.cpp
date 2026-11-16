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

#include <fcntl.h>
#include <linux/limits.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "securec.h"
#define private public
#include "process.h"
#undef private
#include "client_parser.h"
#include "bit_field.h"
using namespace MemScope;

// 大部分单例config在此初始化，config相关修改请修改此处。
void setConfig(Config &config)
{
    BitField<decltype(config.eventType)> eventBit;
    BitField<decltype(config.levelType)> levelBit;
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    analysisBit.setBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS));
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    config.analysisType = analysisBit.getValue();
    config.eventType = eventBit.getValue();
    config.levelType = levelBit.getValue();
    config.enableCStack = true;
    config.enablePyStack = true;
    config.stepList.stepCount = 0;
    config.dataFormat = 0;
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);
}

TEST(Process, process_setpreloadenv_without_atb_expect_success)
{
    unsetenv("ATB_HOME_PATH");
    unsetenv("LD_PRELOAD");  // 清理环境继承的 LD_PRELOAD（SetPreloadEnv 语义为追加，不清理则期望值失真）
    setenv("LD_PRELOAD_PATH", "/lib64/", 1);
    Config config;
    Utility::FileCreateManager::GetInstance("testmsmemscope");
    Process process;
    process.SetPreloadEnv();
    char *env = getenv("LD_PRELOAD");
    std::string hooksSo = "libleaks_ascend_hal_hook.so:"
                          "libascend_mstx_hook.so:libascend_kernel_hook.so";
    EXPECT_EQ(std::string(env), hooksSo);
    setenv("LD_PRELOAD", "test.so", 1);
    process.SetPreloadEnv();
    env = getenv("LD_PRELOAD");
    EXPECT_EQ(std::string(env), hooksSo + ":test.so");
    unsetenv("LD_PRELOAD");
}

TEST(Process, process_setpreloadenv_with_atb_abi_0_expect_success)
{
    unsetenv("LD_PRELOAD");  // 清理环境继承的 LD_PRELOAD（SetPreloadEnv 语义为追加，不清理则期望值失真）
    setenv("ATB_HOME_PATH", "/usr/local/Ascend/nnal/atb/latest/atb/cxx_abi_0", 1);
    setenv("LD_PRELOAD_PATH", "/lib64/", 1);
    Config config;
    Utility::FileCreateManager::GetInstance("testmsmemscope");
    Process process;
    process.SetPreloadEnv();
    char *env = getenv("LD_PRELOAD");
    std::string hooksSo = "libleaks_ascend_hal_hook.so:"
                          "libascend_mstx_hook.so:libascend_kernel_hook.so:libatb_abi_0_hook.so";
    EXPECT_EQ(std::string(env), hooksSo);
    setenv("LD_PRELOAD", "test.so", 1);
    process.SetPreloadEnv();
    env = getenv("LD_PRELOAD");
    EXPECT_EQ(std::string(env), hooksSo + ":test.so");
    unsetenv("LD_PRELOAD");
}

TEST(Process, process_setpreloadenv_with_atb_abi_1_expect_success)
{
    unsetenv("LD_PRELOAD");  // 清理环境继承的 LD_PRELOAD（SetPreloadEnv 语义为追加，不清理则期望值失真）
    setenv("ATB_HOME_PATH", "/usr/local/Ascend/nnal/atb/latest/atb/cxx_abi_1", 1);
    setenv("LD_PRELOAD_PATH", "/lib64/", 1);
    Config config;
    Utility::FileCreateManager::GetInstance("testmsmemscope");
    Process process;
    process.SetPreloadEnv();
    char *env = getenv("LD_PRELOAD");
    std::string hooksSo = "libleaks_ascend_hal_hook.so:"
                          "libascend_mstx_hook.so:libascend_kernel_hook.so:libatb_abi_1_hook.so";
    EXPECT_EQ(std::string(env), hooksSo);
    setenv("LD_PRELOAD", "test.so", 1);
    process.SetPreloadEnv();
    env = getenv("LD_PRELOAD");
    EXPECT_EQ(std::string(env), hooksSo + ":test.so");
    unsetenv("LD_PRELOAD");
}

TEST(Process, do_record_handler_except_success)
{
    std::shared_ptr<MemoryEvent> event1 = std::make_shared<MemoryEvent>();
    event1->eventType = EventBaseType::MALLOC;
    event1->eventSubType = EventSubType::PTA_CACHING;
    event1->id = 1;
    event1->device = 0;
    event1->size = 512;
    event1->addr = 12345;
    event1->used = 512;
    
    std::shared_ptr<MstxEvent> event2 = std::make_shared<MstxEvent>();
    event2->eventType = EventBaseType::MSTX;
    event2->eventSubType = EventSubType::MSTX_RANGE_START;
    event2->rangeId = 0;
    event2->stepId = 1;
    event2->streamId = 123;

    std::shared_ptr<KernelLaunchEvent> event3 = std::make_shared<KernelLaunchEvent>();
    event3->eventType = EventBaseType::KERNEL_LAUNCH;
    event3->eventSubType = EventSubType::KERNEL_LAUNCH;

    std::shared_ptr<SystemEvent> event4 = std::make_shared<SystemEvent>();
    event4->eventType = EventBaseType::SYSTEM;
    event4->eventSubType = EventSubType::ACL_INIT;

    std::shared_ptr<OpLaunchEvent> event5 = std::make_shared<OpLaunchEvent>();
    event5->eventType = EventBaseType::OP_LAUNCH;
    event5->eventSubType = EventSubType::ATEN_START;
    event5->name = "X";

    std::shared_ptr<MemoryEvent> event6 = std::make_shared<MemoryEvent>();
    event6->eventType = EventBaseType::MALLOC;
    event6->eventSubType = EventSubType::PTA_WORKSPACE;
    event6->id = 1;
    event6->device = 0;
    event6->size = 512;
    event6->addr = 12347;
    event6->used = 512;

    Config config;
    setConfig(config);
    Process::GetInstance().SendEvent(event1);
    Process::GetInstance().SendEvent(event2);
    Process::GetInstance().SendEvent(event3);
    Process::GetInstance().SendEvent(event4);
    Process::GetInstance().SendEvent(event5);
    Process::GetInstance().SendEvent(event6);
}

// ============================================================================
// 覆盖率补充用例：目标 framework/process.cpp 未覆盖行(ExecCmd构造、SetPreloadEnv
// 的host/npu分支与getcwd失败早退、Launch/DoLaunch)。
// SetPreloadEnv 读写进程级环境变量，全部用例经 ScopedEnv RAII 恢复原值；
// Launch 会替换/终止进程映像，在 fork 子进程内验证。
// ============================================================================
namespace
{
// 环境变量RAII：析构时恢复用例开始前的值(存在则恢复内容，不存在则unset)，
// 避免 LD_PRELOAD/MSMEMSCOPE_API_ENV/ATB_HOME_PATH 等跨用例污染
class ScopedEnv
{
   public:
    explicit ScopedEnv(const char *name) : name_(name)
    {
        const char *value = std::getenv(name);
        if (value != nullptr)
        {
            hasValue_ = true;
            value_ = value;
        }
    }
    ~ScopedEnv()
    {
        if (hasValue_)
        {
            (void)::setenv(name_.c_str(), value_.c_str(), 1);
        }
        else
        {
            (void)::unsetenv(name_.c_str());
        }
    }
    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

   private:
    std::string name_;
    std::string value_;
    bool hasValue_{false};
};

std::string MakeTempDir(const char *tag)
{
    char tmpl[128] = {0};
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/msmemscope_process_ut_%s_XXXXXX", tag);
    char *dir = ::mkdtemp(tmpl);
    return dir == nullptr ? std::string() : std::string(dir);
}

std::string JoinPath(const std::string &dir, const char *name)
{
    return dir + "/" + name;
}

void TouchFile(const std::string &path)
{
    std::ofstream ofs(path);
    ofs << "stub";
}

struct ChildRunResult
{
    int status{-1};
    std::string output;
};

// 子进程执行上限：Launch/ExecCmd路径均应在秒级完成；超时强制回收，
// 避免被测代码回归(fork后继承他人持有的锁等)时无界 waitpid 拖死整个 gtest 进程
constexpr int kChildTimeoutMs = 30000;
constexpr int kChildPollStepMs = 50;

void WaitChildExitBounded(pid_t pid, int *status)
{
    for (int waitedMs = 0; waitedMs < kChildTimeoutMs; waitedMs += kChildPollStepMs)
    {
        const pid_t ret = ::waitpid(pid, status, WNOHANG);
        if (ret == pid)
        {
            return;
        }
        if (ret < 0 && errno != EINTR)
        {
            return;
        }
        ::usleep(kChildPollStepMs * 1000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, status, 0);
}

// fork隔离执行：子进程stdout重定向到临时文件、stderr丢弃；body正常返回后
// std::exit(0)以落盘gcov(Launch成功路径会exec替换映像，计数无法落盘)
ChildRunResult RunInFork(const std::function<void()> &body)
{
    char tmpl[] = "/tmp/msmemscope_process_fork_XXXXXX";
    const int pathFd = ::mkstemp(tmpl);
    if (pathFd < 0)
    {
        return ChildRunResult();
    }
    ::close(pathFd);
    const std::string path(tmpl);
    ChildRunResult result;
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0)
        {
            ::dup2(fd, STDOUT_FILENO);
            ::close(fd);
        }
        const int nullFd = ::open("/dev/null", O_WRONLY);
        if (nullFd >= 0)
        {
            ::dup2(nullFd, STDERR_FILENO);
            ::close(nullFd);
        }
        body();
        std::exit(0);  // 正常退出以落盘gcov
    }
    if (pid < 0)
    {
        ::unlink(path.c_str());
        return result;
    }
    WaitChildExitBounded(pid, &result.status);
    std::ifstream in(path);
    result.output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    in.close();
    ::unlink(path.c_str());
    return result;
}

// ExecCmd存在的路径(L43-62)：path_经realpath归一，argv保留原始串且末尾为nullptr
TEST(Process, execcmd_resolves_existing_path_to_absolute)
{
    char tmpl[] = "/tmp/msmemscope_process_ut_execcmd_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    ASSERT_GE(fd, 0);
    ::close(fd);

    ExecCmd cmd(std::vector<std::string>{std::string(tmpl), "arg1", "arg2"});
    char resolved[PATH_MAX] = {0};
    ASSERT_NE(::realpath(tmpl, resolved), nullptr);
    EXPECT_EQ(cmd.ExecPath(), std::string(resolved));
    EXPECT_EQ(cmd.argc_, 3);
    ASSERT_NE(cmd.ExecArgv(), nullptr);
    EXPECT_STREQ(cmd.ExecArgv()[0], tmpl);  // argv[0]保留用户原始串，不做归一
    EXPECT_STREQ(cmd.ExecArgv()[1], "arg1");
    EXPECT_STREQ(cmd.ExecArgv()[2], "arg2");
    EXPECT_EQ(cmd.ExecArgv()[3], nullptr);

    (void)::unlink(tmpl);
}

// ExecCmd不可解析的路径(L51-54)：realpath失败时path_原样保留
TEST(Process, execcmd_keeps_unresolvable_path)
{
    const char *missing = "/tmp/msmemscope_process_ut_missing_binary";
    ExecCmd cmd(std::vector<std::string>{std::string(missing), "--flag"});
    EXPECT_EQ(cmd.ExecPath(), std::string(missing));
    EXPECT_EQ(cmd.argc_, 2);
    ASSERT_NE(cmd.ExecArgv(), nullptr);
    EXPECT_STREQ(cmd.ExecArgv()[0], missing);
    EXPECT_STREQ(cmd.ExecArgv()[1], "--flag");
    EXPECT_EQ(cmd.ExecArgv()[2], nullptr);
}

// host模式且host钩子存在(L105-135)：LD_PRELOAD装配绝对路径，并保留用户既有条目
TEST(Process, process_setpreloadenv_host_mode_with_existing_hook)
{
    ScopedEnv apiEnv("MSMEMSCOPE_API_ENV");
    ScopedEnv preloadPathEnv("LD_PRELOAD_PATH");
    ScopedEnv ldPreloadEnv("LD_PRELOAD");
    ScopedEnv atbHomeEnv("ATB_HOME_PATH");

    (void)::unsetenv("ATB_HOME_PATH");
    (void)::unsetenv("LD_PRELOAD");
    (void)::setenv("MSMEMSCOPE_API_ENV", "host", 1);
    const std::string dir = MakeTempDir("hosthook");
    ASSERT_FALSE(dir.empty());
    const std::string hook = JoinPath(dir, "libmsmemscope_host_mem_hook.so");
    TouchFile(hook);
    (void)::setenv("LD_PRELOAD_PATH", dir.c_str(), 1);

    Process::GetInstance().SetPreloadEnv();
    const char *env = std::getenv("LD_PRELOAD");
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(std::string(env), hook);

    (void)::setenv("LD_PRELOAD", "test.so", 1);
    Process::GetInstance().SetPreloadEnv();
    env = std::getenv("LD_PRELOAD");
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(std::string(env), hook + ":test.so");

    (void)::unlink(hook.c_str());
    (void)::rmdir(dir.c_str());
}

// host模式且host钩子缺失(L121-125)：保留相对库名并继续写入LD_PRELOAD
TEST(Process, process_setpreloadenv_host_mode_without_hook)
{
    ScopedEnv apiEnv("MSMEMSCOPE_API_ENV");
    ScopedEnv preloadPathEnv("LD_PRELOAD_PATH");
    ScopedEnv ldPreloadEnv("LD_PRELOAD");

    (void)::unsetenv("LD_PRELOAD");
    (void)::setenv("MSMEMSCOPE_API_ENV", "host", 1);
    const std::string dir = MakeTempDir("hosthook_empty");
    ASSERT_FALSE(dir.empty());
    (void)::setenv("LD_PRELOAD_PATH", dir.c_str(), 1);

    Process::GetInstance().SetPreloadEnv();
    const char *env = std::getenv("LD_PRELOAD");
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(std::string(env), "libmsmemscope_host_mem_hook.so");

    (void)::rmdir(dir.c_str());
}

// npu模式且三个钩子so都存在(L165-192)：LD_PRELOAD使用解析后的绝对路径拼接
TEST(Process, process_setpreloadenv_npu_mode_with_existing_hooks)
{
    ScopedEnv apiEnv("MSMEMSCOPE_API_ENV");
    ScopedEnv preloadPathEnv("LD_PRELOAD_PATH");
    ScopedEnv ldPreloadEnv("LD_PRELOAD");
    ScopedEnv atbHomeEnv("ATB_HOME_PATH");

    (void)::unsetenv("MSMEMSCOPE_API_ENV");
    (void)::unsetenv("ATB_HOME_PATH");
    (void)::unsetenv("LD_PRELOAD");
    const std::string dir = MakeTempDir("npuhooks");
    ASSERT_FALSE(dir.empty());
    const char *hookNames[] = {"libleaks_ascend_hal_hook.so", "libascend_mstx_hook.so", "libascend_kernel_hook.so"};
    std::string expected;
    for (const char *name : hookNames)
    {
        const std::string hook = JoinPath(dir, name);
        TouchFile(hook);
        expected += (expected.empty() ? "" : ":") + hook;
    }
    (void)::setenv("LD_PRELOAD_PATH", dir.c_str(), 1);

    Process::GetInstance().SetPreloadEnv();
    const char *env = std::getenv("LD_PRELOAD");
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(std::string(env), expected);

    for (const char *name : hookNames)
    {
        (void)::unlink(JoinPath(dir, name).c_str());
    }
    (void)::rmdir(dir.c_str());
}

// npu模式且ATB_HOME_PATH非法(L159-162)：不追加atb钩子，按基础三钩子装配
TEST(Process, process_setpreloadenv_invalid_atb_home_path)
{
    ScopedEnv apiEnv("MSMEMSCOPE_API_ENV");
    ScopedEnv preloadPathEnv("LD_PRELOAD_PATH");
    ScopedEnv ldPreloadEnv("LD_PRELOAD");
    ScopedEnv atbHomeEnv("ATB_HOME_PATH");

    (void)::unsetenv("MSMEMSCOPE_API_ENV");
    (void)::unsetenv("LD_PRELOAD");
    (void)::setenv("ATB_HOME_PATH", "/tmp/msmemscope_process_ut_atb/cxx_abi_9", 1);
    const std::string dir = MakeTempDir("atb_invalid");
    ASSERT_FALSE(dir.empty());
    (void)::setenv("LD_PRELOAD_PATH", dir.c_str(), 1);

    Process::GetInstance().SetPreloadEnv();
    const char *env = std::getenv("LD_PRELOAD");
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(std::string(env), "libleaks_ascend_hal_hook.so:libascend_mstx_hook.so:libascend_kernel_hook.so");

    (void)::rmdir(dir.c_str());
}

// SetPreloadEnv在getcwd失败时提前返回(L110-114/L167-171)：npu与host分支均不设置
// LD_PRELOAD(已删除cwd的进程无法解析hook路径)；fork隔离避免影响其他用例
TEST(Process, process_setpreloadenv_getcwd_failure_returns_early)
{
    const ChildRunResult result = RunInFork(
        []()
        {
            char tmpl[] = "/tmp/msmemscope_process_ut_deleted_cwd_XXXXXX";
            char *dir = ::mkdtemp(tmpl);
            if (dir == nullptr || ::chdir(dir) != 0 || ::rmdir(dir) != 0)
            {
                std::cout << "setup-failed";
                return;
            }
            (void)::unsetenv("ATB_HOME_PATH");
            (void)::unsetenv("LD_PRELOAD");
            (void)::unsetenv("MSMEMSCOPE_API_ENV");
            (void)::setenv("LD_PRELOAD_PATH", "/lib64/", 1);

            Process::GetInstance().SetPreloadEnv();
            std::cout << "npu=" << (std::getenv("LD_PRELOAD") == nullptr ? 0 : 1) << ";";

            (void)::setenv("MSMEMSCOPE_API_ENV", "host", 1);
            (void)::unsetenv("LD_PRELOAD");
            Process::GetInstance().SetPreloadEnv();
            std::cout << "host=" << (std::getenv("LD_PRELOAD") == nullptr ? 0 : 1) << std::endl;
        });
    ASSERT_TRUE(WIFEXITED(result.status)) << result.output;
    EXPECT_EQ(WEXITSTATUS(result.status), 0) << result.output;
    EXPECT_NE(result.output.find("npu=0"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("host=0"), std::string::npos) << result.output;
}

// Launch/DoLaunch(L83-90/L195-200)：正常拉起目标程序；exec失败以EXIT_FAILURE退出。
// 注：成功exec替换映像、失败路径走_exit，子进程gcov计数均无法落盘，本用例做运行期校验
TEST(Process, process_launch_executes_target_or_exits_on_failure)
{
    const ChildRunResult okResult = RunInFork(
        []()
        {
            (void)::unsetenv("LD_PRELOAD");
            (void)::unsetenv("MSMEMSCOPE_API_ENV");
            (void)::unsetenv("ATB_HOME_PATH");
            (void)::setenv("LD_PRELOAD_PATH", "/lib64/", 1);
            Process::GetInstance().Launch(std::vector<std::string>{"/bin/true"});
        });
    ASSERT_TRUE(WIFEXITED(okResult.status)) << okResult.output;
    EXPECT_EQ(WEXITSTATUS(okResult.status), 0) << okResult.output;

    const ChildRunResult failResult = RunInFork(
        []()
        {
            (void)::unsetenv("LD_PRELOAD");
            (void)::unsetenv("MSMEMSCOPE_API_ENV");
            (void)::unsetenv("ATB_HOME_PATH");
            (void)::setenv("LD_PRELOAD_PATH", "/lib64/", 1);
            Process::GetInstance().Launch(std::vector<std::string>{"/tmp/msmemscope_process_ut_missing_program"});
        });
    ASSERT_TRUE(WIFEXITED(failResult.status)) << failResult.output;
    EXPECT_EQ(WEXITSTATUS(failResult.status), EXIT_FAILURE) << failResult.output;
}
}  // namespace
