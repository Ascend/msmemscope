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

/* AttachController(进程外控制通道控制端)测试:
 * AttachController::Run以std::exit收尾,故控制端在fork子进程内运行,测试进程读取
 * 子进程退出码与重定向输出做断言;目标进程由独立替身attach_target_helper扮演
 * (可执行名含libascend_leaks、控制线程名msmemscope_ctrl),按场景回放协议帧。
 * 覆盖: 四规则校验逐项失败、socket生命周期(live拒绝/僵尸重建)、accept超时、
 * SO_PEERCRED凭证校验、REGISTER各类异常、单发命令成功/业务拒绝/异常响应、
 * 交互脚本(help/display analyzer/set config/exit)、等待态SIGINT/SIGTERM、
 * 对端提前退出。环境: Linux(AF_UNIX/SO_PEERCRED/procfs)。
 */
#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "attach_controller.h"
#include "control_channel/control_protocol.h"

using namespace MemScope;

namespace
{
constexpr const char* kHelperBin = ATTACH_TARGET_HELPER_BIN;
constexpr uint64_t kNoSuchPid = 99999999ULL;
constexpr uint64_t kShortTimeoutMs = 400;

std::string SocketPathOf(pid_t pid)
{
    return ControlProtocol::SocketPath(static_cast<uint64_t>(pid));
}

std::string MakeTempFile()
{
    char tmpl[] = "/tmp/msmemscope_attach_ut_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd >= 0)
    {
        ::close(fd);
    }
    return std::string(tmpl);
}

std::string ReadWholeFile(const std::string& path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// 启动目标进程替身;返回pid(就绪信号=控制线程名已生效),失败返回-1
pid_t SpawnHelper(const std::string& mode, int notifyFd = -1)
{
    int pipefd[2];
    if (::pipe(pipefd) != 0)
    {
        return -1;
    }
    const std::string fdArg = notifyFd >= 0 ? std::to_string(notifyFd) : "";
    pid_t pid = ::fork();
    if (pid == 0)
    {
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        if (notifyFd >= 0)
        {
            ::execl(kHelperBin, kHelperBin, mode.c_str(), fdArg.c_str(), static_cast<char*>(nullptr));
        }
        else
        {
            ::execl(kHelperBin, kHelperBin, mode.c_str(), static_cast<char*>(nullptr));
        }
        ::_exit(127);
    }
    ::close(pipefd[1]);
    char buf[32] = {0};
    const ssize_t n = ::read(pipefd[0], buf, sizeof(buf) - 1);
    ::close(pipefd[0]);
    if (n <= 0)
    {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        return -1;
    }
    return pid;
}

void StopHelper(pid_t pid)
{
    if (pid <= 0)
    {
        return;
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    ::unlink(SocketPathOf(pid).c_str());
}

int BindListen(const std::string& path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 1) != 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

void CreateStaleSocket(const std::string& path)
{
    const int fd = BindListen(path);
    if (fd >= 0)
    {
        ::close(fd);  // 关闭但保留socket文件: 后续connect得到ECONNREFUSED
    }
}

int ConnectToPath(const std::string& path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

struct ControllerProc
{
    pid_t pid = -1;
    std::string outPath;
    int stdinWriteFd = -1;
};

// 控制端在fork子进程内执行(AttachController::Run不返回):
// 输出重定向到临时文件(避免管道容量阻塞);pipeStdin=交互模式脚本通道
ControllerProc StartController(uint64_t targetPid, const std::string& cmd, uint64_t seamTimeoutMs, bool pipeStdin)
{
    ControllerProc proc;
    proc.outPath = MakeTempFile();
    int inPipe[2] = {-1, -1};
    if (pipeStdin && ::pipe(inPipe) != 0)
    {
        return proc;
    }
    proc.pid = ::fork();
    if (proc.pid == 0)
    {
        const int fd = ::open(proc.outPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0)
        {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }
        if (pipeStdin)
        {
            ::dup2(inPipe[0], STDIN_FILENO);
            ::close(inPipe[0]);
            ::close(inPipe[1]);
        }
        else
        {
            const int devNull = ::open("/dev/null", O_RDONLY);
            if (devNull >= 0)
            {
                ::dup2(devNull, STDIN_FILENO);
                ::close(devNull);
            }
        }
        AttachController::SetTimeoutMsForTest(seamTimeoutMs);
        AttachController::Run(targetPid, cmd);
        ::_exit(0);  // Run以std::exit收尾,不可达
    }
    if (pipeStdin)
    {
        ::close(inPipe[0]);
        proc.stdinWriteFd = inPipe[1];
    }
    return proc;
}

int WaitController(ControllerProc& proc, std::string& output)
{
    int status = 0;
    ::waitpid(proc.pid, &status, 0);
    output = ReadWholeFile(proc.outPath);
    ::unlink(proc.outPath.c_str());
    proc.pid = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// 定向到控制端主线程(fork子进程的线程组组长,tid==pid):
// 子进程内atfork回调另拉起控制通道监听线程,进程级kill的投递线程不确定,
// 定向投递确保主线程的poll/read被EINTR打断
void KillLeaderThread(pid_t pid, int signo)
{
    (void)::syscall(SYS_tgkill, pid, pid, signo);
}

int RunSingle(uint64_t targetPid, const std::string& cmd, uint64_t seamTimeoutMs, std::string& output)
{
    ControllerProc proc = StartController(targetPid, cmd, seamTimeoutMs, false);
    return WaitController(proc, output);
}

int RunInteractive(uint64_t targetPid, const std::string& script, uint64_t seamTimeoutMs, std::string& output)
{
    ControllerProc proc = StartController(targetPid, "", seamTimeoutMs, true);
    if (!script.empty())
    {
        (void)!::write(proc.stdinWriteFd, script.data(), script.size());
    }
    ::close(proc.stdinWriteFd);
    return WaitController(proc, output);
}

class AttachControllerTest : public ::testing::Test
{
   protected:
    void TearDown() override
    {
        ::unsetenv(ControlProtocol::ENV_ATTACH_TIMEOUT_MS);
        AttachController::SetTimeoutMsForTest(0);
    }
};

// 规则1: 目标pid不存在
TEST_F(AttachControllerTest, rejects_pid_not_exist)
{
    std::string out;
    const int code = RunSingle(kNoSuchPid, "start", kShortTimeoutMs, out);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("does not exist"), std::string::npos) << out;
}

// 规则3: 目标进程未加载libascend_leaks(以测试进程的fork子进程扮演普通进程)
TEST_F(AttachControllerTest, rejects_maps_without_libascend_leaks)
{
    pid_t sleeper = ::fork();
    if (sleeper == 0)
    {
        for (;;)
        {
            ::pause();
        }
    }
    ASSERT_GT(sleeper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(sleeper), "start", kShortTimeoutMs, out);
    ::kill(sleeper, SIGKILL);
    ::waitpid(sleeper, nullptr, 0);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("does not have libascend_leaks.so loaded"), std::string::npos) << out;
}

// 规则4: maps合规但无msmemscope_ctrl监听线程
TEST_F(AttachControllerTest, rejects_missing_listener_thread)
{
    const pid_t helper = SpawnHelper("no-thread");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("has no msmemscope_ctrl listener thread"), std::string::npos) << out;
}

// socket生命周期: 已有live会话(可connect) → 拒绝,不发信号
TEST_F(AttachControllerTest, rejects_live_socket)
{
    const pid_t helper = SpawnHelper("register-ok");
    ASSERT_GT(helper, 0);
    const int liveFd = BindListen(SocketPathOf(helper));
    ASSERT_GE(liveFd, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    ::close(liveFd);
    ::unlink(SocketPathOf(helper).c_str());
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("already attached by another controller"), std::string::npos) << out;
}

// socket生命周期: 僵尸socket(ECONNREFUSED) → unlink重建;单发成功往返
TEST_F(AttachControllerTest, rebuilds_stale_socket_and_single_command_succeeds)
{
    const pid_t helper = SpawnHelper("reply-ok");
    ASSERT_GT(helper, 0);
    CreateStaleSocket(SocketPathOf(helper));
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "display memory block --pool hal --TOPN 5",
                               kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("hello from target"), std::string::npos) << out;
}

// socket生命周期: socket路径为目录 → connect失败后unlink失败(非僵尸socket)
TEST_F(AttachControllerTest, reports_stale_socket_unlink_failure)
{
    const pid_t helper = SpawnHelper("register-ok");
    ASSERT_GT(helper, 0);
    const std::string path = SocketPathOf(helper);
    ::unlink(path.c_str());
    ASSERT_EQ(::mkdir(path.c_str(), 0700), 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    ::rmdir(path.c_str());
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("stale socket unlink failed"), std::string::npos) << out;
}

// accept等待超时(目标收到SIGUSR1但不connect): 退出码1 + 超时提示带实际ms
TEST_F(AttachControllerTest, accept_timeout_reports_elapsed_ms)
{
    const pid_t helper = SpawnHelper("no-connect");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", 300, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("attach timed out after 300ms"), std::string::npos) << out;
}

// accept超时经env配置(有效正整数): 提示使用env值
TEST_F(AttachControllerTest, accept_timeout_from_env)
{
    ::setenv(ControlProtocol::ENV_ATTACH_TIMEOUT_MS, "250", 1);
    const pid_t helper = SpawnHelper("no-connect");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", 0, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("attach timed out after 250ms"), std::string::npos) << out;
}

// SO_PEERCRED: 连接方不是目标进程(测试进程代连) → 拒绝
TEST_F(AttachControllerTest, rejects_peer_credential_mismatch)
{
    int notify[2];
    ASSERT_EQ(::pipe(notify), 0);
    const pid_t helper = SpawnHelper("delegate-connect", notify[1]);
    ::close(notify[1]);
    ASSERT_GT(helper, 0);

    ControllerProc proc = StartController(static_cast<uint64_t>(helper), "start", 3000, false);
    char token = 0;
    ASSERT_EQ(::read(notify[0], &token, 1), 1);
    ::close(notify[0]);

    const int peer = ConnectToPath(SocketPathOf(helper));
    ASSERT_GE(peer, 0);
    std::string out;
    const int code = WaitController(proc, out);
    ::close(peer);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("peer credential mismatch"), std::string::npos) << out;
}

// REGISTER读取失败: 连接后立即关闭(帧未送达)
TEST_F(AttachControllerTest, register_read_failure)
{
    const pid_t helper = SpawnHelper("connect-close");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("REGISTER read failed"), std::string::npos) << out;
}

// REGISTER消息类型错误(收到CONTROL)
TEST_F(AttachControllerTest, register_wrong_message_type)
{
    const pid_t helper = SpawnHelper("register-wrongtype");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("expected REGISTER, got msg_type=2"), std::string::npos) << out;
}

// REGISTER载荷非JSON: 报错并转储十六进制字节
TEST_F(AttachControllerTest, register_payload_not_json)
{
    const pid_t helper = SpawnHelper("register-badjson");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("REGISTER payload is not valid JSON"), std::string::npos) << out;
    EXPECT_NE(out.find("payload hex:"), std::string::npos) << out;
    EXPECT_NE(out.find("6e 6f 74 2d 6a 73 6f 6e"), std::string::npos) << out;  // "not-json"
}

// REGISTER协议版本不匹配
TEST_F(AttachControllerTest, register_incompatible_proto)
{
    const pid_t helper = SpawnHelper("register-badproto");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("incompatible protocol version"), std::string::npos) << out;
}

// 单发本地命令help: 不发帧,打印控制字清单,退出0
TEST_F(AttachControllerTest, single_help_is_local_and_exits_zero)
{
    const pid_t helper = SpawnHelper("register-ok");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "help", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("display host_leak summary"), std::string::npos) << out;
    EXPECT_NE(out.find("set config <args>"), std::string::npos) << out;
}

// 单发本地命令exit/quit: 不发帧,退出0
TEST_F(AttachControllerTest, single_exit_and_quit_are_local)
{
    for (const char* cmd : {"exit", "quit"})
    {
        const pid_t helper = SpawnHelper("register-ok");
        ASSERT_GT(helper, 0);
        std::string out;
        const int code = RunSingle(static_cast<uint64_t>(helper), cmd, kShortTimeoutMs, out);
        StopHelper(helper);
        EXPECT_EQ(code, 0) << cmd << ": " << out;
    }
}

// 单发非法控制字: 本地白名单拒绝,退出1(两类词根分别覆盖SplitCmdParam的display分支与else分支)
TEST_F(AttachControllerTest, single_invalid_control_word_exits_one)
{
    for (const char* cmd : {"display memory bogus", "bogus extra"})
    {
        const pid_t helper = SpawnHelper("register-ok");
        ASSERT_GT(helper, 0);
        std::string out;
        const int code = RunSingle(static_cast<uint64_t>(helper), cmd, kShortTimeoutMs, out);
        StopHelper(helper);
        EXPECT_EQ(code, 1) << out;
        EXPECT_NE(out.find(std::string("invalid control word: ") + cmd), std::string::npos) << out;
    }
}

// 单发成功往返: 响应边收边显(streaming路径),退出0
TEST_F(AttachControllerTest, single_command_streams_response)
{
    const pid_t helper = SpawnHelper("reply-ok");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "display host_leak summary", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("hello from target"), std::string::npos) << out;
}

// 单发业务拒绝(ok=false): 传输成功但退出码1
TEST_F(AttachControllerTest, single_business_reject_exits_one)
{
    const pid_t helper = SpawnHelper("reply-fail");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "set config --analysis leaks", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1) << out;
    EXPECT_NE(out.find("rejected by target"), std::string::npos) << out;
}

// 单发多帧响应(FLAG_CONT): 逐帧拼接回显
TEST_F(AttachControllerTest, single_command_handles_cont_frames)
{
    const pid_t helper = SpawnHelper("reply-multi");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "display hook", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("part1 part2"), std::string::npos) << out;
}

// 响应载荷非JSON: 协议错误退出1
TEST_F(AttachControllerTest, single_response_not_json)
{
    const pid_t helper = SpawnHelper("reply-badjson");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("response payload is not valid JSON"), std::string::npos) << out;
}

// 响应消息类型错误: 协议违规退出1
TEST_F(AttachControllerTest, single_response_wrong_type)
{
    const pid_t helper = SpawnHelper("reply-wrongtype");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("protocol violation (msg_type=2"), std::string::npos) << out;
}

// 响应seq与请求不匹配: 协议违规退出1
TEST_F(AttachControllerTest, single_response_wrong_seq)
{
    const pid_t helper = SpawnHelper("reply-wrongseq");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_NE(out.find("protocol violation"), std::string::npos) << out;
}

// 等待响应期间对端关闭: 传输失败退出1
TEST_F(AttachControllerTest, single_peer_closed_during_wait)
{
    const pid_t helper = SpawnHelper("close-after-control");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunSingle(static_cast<uint64_t>(helper), "start", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1);
    EXPECT_TRUE(out.find("response read failed") != std::string::npos ||
                out.find("target process exited during control exchange") != std::string::npos)
        << out;
}

// 等待响应期间SIGINT: WaitResult::Interrupted → 退出码130
TEST_F(AttachControllerTest, single_wait_interrupted_by_sigint_exits_130)
{
    const pid_t helper = SpawnHelper("reply-never");
    ASSERT_GT(helper, 0);
    ControllerProc proc = StartController(static_cast<uint64_t>(helper), "start", 3000, false);
    ::usleep(600000);
    KillLeaderThread(proc.pid, SIGINT);
    std::string out;
    const int code = WaitController(proc, out);
    StopHelper(helper);
    EXPECT_EQ(code, 130) << out;
}

// 等待响应期间SIGTERM: 退出路径(非打断语义) → 退出码1
TEST_F(AttachControllerTest, single_wait_sigterm_exits_one)
{
    const pid_t helper = SpawnHelper("reply-never");
    ASSERT_GT(helper, 0);
    ControllerProc proc = StartController(static_cast<uint64_t>(helper), "start", 3000, false);
    ::usleep(600000);
    KillLeaderThread(proc.pid, SIGTERM);
    std::string out;
    const int code = WaitController(proc, out);
    StopHelper(helper);
    EXPECT_EQ(code, 1) << out;
}

// 交互脚本: help/display analyzer(累积路径+补全缓存刷新)/start/非法词/空行/exit
TEST_F(AttachControllerTest, interactive_script_roundtrip)
{
    const pid_t helper = SpawnHelper("reply-ok-analyzer");
    ASSERT_GT(helper, 0);
    const std::string script = "help\ndisplay analyzer\nstart   \nbadword extra\n\nset config --analysis leaks\nexit\n";
    std::string out;
    const int code = RunInteractive(static_cast<uint64_t>(helper), script, kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("Attached to pid"), std::string::npos) << out;
    EXPECT_NE(out.find("display memory block"), std::string::npos) << out;   // help
    EXPECT_NE(out.find("Analyzers: analyzer_a, analyzer_b"), std::string::npos) << out;
    EXPECT_NE(out.find("invalid control word: badword extra"), std::string::npos) << out;
    EXPECT_NE(out.find("[msmemscope] Detached."), std::string::npos) << out;
}

// 交互: 标准输入立即EOF(Ctrl-D) → 正常detach退出0
TEST_F(AttachControllerTest, interactive_eof_detaches)
{
    const pid_t helper = SpawnHelper("reply-ok");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunInteractive(static_cast<uint64_t>(helper), "", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 0) << out;
}

// 交互: 对端在会话中退出 → 打印提示,会话续存,exit正常退出0
TEST_F(AttachControllerTest, interactive_peer_gone_session_kept)
{
    const pid_t helper = SpawnHelper("close-after-control");
    ASSERT_GT(helper, 0);
    std::string out;
    const int code = RunInteractive(static_cast<uint64_t>(helper), "start\nexit\n", kShortTimeoutMs, out);
    StopHelper(helper);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("target process exited"), std::string::npos) << out;
    EXPECT_NE(out.find("[msmemscope] Detached."), std::string::npos) << out;
}

// 交互: 提示符态SIGINT → Interrupted,退出码130
TEST_F(AttachControllerTest, interactive_sigint_at_prompt_exits_130)
{
    const pid_t helper = SpawnHelper("reply-ok");
    ASSERT_GT(helper, 0);
    ControllerProc proc = StartController(static_cast<uint64_t>(helper), "", 3000, true);
    ::usleep(600000);  // 等待建联预取完成,控制端阻塞在stdin读取
    KillLeaderThread(proc.pid, SIGINT);
    ::usleep(300000);
    ::close(proc.stdinWriteFd);
    std::string out;
    const int code = WaitController(proc, out);
    StopHelper(helper);
    EXPECT_EQ(code, 130) << out;
    EXPECT_NE(out.find("Interrupted"), std::string::npos) << out;
}

// 交互: 等待响应时SIGINT → 打断本次等待回到提示符,会话保留,exit退出0
TEST_F(AttachControllerTest, interactive_wait_interrupted_session_kept)
{
    const pid_t helper = SpawnHelper("reply-once-silent");
    ASSERT_GT(helper, 0);
    ControllerProc proc = StartController(static_cast<uint64_t>(helper), "", 3000, true);
    const std::string line = "start\n";
    (void)!::write(proc.stdinWriteFd, line.data(), line.size());
    ::usleep(800000);  // 预取已被应答,控制端进入start响应的等待
    KillLeaderThread(proc.pid, SIGINT);
    ::usleep(500000);
    ::close(proc.stdinWriteFd);
    std::string out;
    const int code = WaitController(proc, out);
    StopHelper(helper);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("wait interrupted (Ctrl-C), session kept"), std::string::npos) << out;
}

}  // namespace
