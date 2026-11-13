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

/* AttachController单元测试的目标进程替身(独立可执行文件):
 * 1. 可执行文件名由CMake强制为libascend_leaks_attach_target——/proc/<pid>/maps
 *    含"libascend_leaks"满足四规则校验的maps规则;
 * 2. 控制线程命名为msmemscope_ctrl——满足监听线程规则(线程名上限15字符);
 * 3. 阻塞SIGUSR1并以sigwait等待,收到后按mode连接控制端socket,以原始协议帧
 *    回放各类响应/异常场景(REGISTER异常、RESPONSE异常、对端提前关闭等)。
 * 注意: 本文件不进入memscope_test的GLOB_RECURSE(test/attach_target不在子目录
 * 列表内),避免第二个main。
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "control_channel/control_protocol.h"

namespace
{
using namespace MemScope;

constexpr const char kThreadName[] = "msmemscope_ctrl";
sigset_t g_sigset;
std::string g_mode;
int g_notifyFd = -1;

int ConnectToController()
{
    const std::string path = ControlProtocol::SocketPath(static_cast<uint64_t>(::getpid()));
    for (int i = 0; i < 400; ++i)
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
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
        {
            return fd;
        }
        ::close(fd);
        ::usleep(2000);
    }
    return -1;
}

bool ReadOneFrame(int fd, uint16_t& seqOut)
{
    std::vector<uint8_t> payload;
    uint16_t flags = 0;
    uint8_t msgType = 0;
    std::string error;
    return ControlProtocol::ReadFrame(fd, payload, seqOut, msgType, flags, error);
}

void Respond(int fd, uint16_t seq, const std::string& payload, uint16_t flags)
{
    std::string error;
    (void)ControlProtocol::SendFrame(fd, ControlProtocol::MSG_RESPONSE, flags, seq, payload, error);
}

// 会话应答循环:每个CONTROL帧按mode回一帧(或异常形态),直至对端关闭
void RunReplyLoop(int fd)
{
    for (;;)
    {
        uint16_t seq = 0;
        if (!ReadOneFrame(fd, seq))
        {
            return;
        }
        if (g_mode == "reply-ok")
        {
            Respond(fd, seq, "{\"ok\":true,\"output\":\"hello from target\\n\"}", 0);
        }
        else if (g_mode == "reply-ok-analyzer")
        {
            Respond(fd, seq, "{\"ok\":true,\"output\":\"Analyzers: analyzer_a, analyzer_b\\n\"}", 0);
        }
        else if (g_mode == "reply-fail")
        {
            Respond(fd, seq, "{\"ok\":false,\"output\":\"rejected by target\\n\"}", 0);
        }
        else if (g_mode == "reply-multi")
        {
            Respond(fd, seq, "{\"ok\":true,\"output\":\"part1 \"}", ControlProtocol::FLAG_CONT);
            Respond(fd, seq, "{\"ok\":true,\"output\":\"part2\"}", 0);
        }
        else if (g_mode == "reply-badjson")
        {
            Respond(fd, seq, "not-json", 0);
        }
        else if (g_mode == "reply-wrongtype")
        {
            std::string error;
            (void)ControlProtocol::SendFrame(fd, ControlProtocol::MSG_CONTROL, 0, seq, "{}", error);
        }
        else if (g_mode == "reply-wrongseq")
        {
            Respond(fd, static_cast<uint16_t>(seq + 1), "{\"ok\":true,\"output\":\"x\"}", 0);
        }
        else if (g_mode == "close-after-control")
        {
            ::close(fd);
            return;
        }
        else if (g_mode == "reply-once-silent")
        {
            // 首帧(交互模式下的display analyzer预取)正常应答,之后保持连接但不再应答
            Respond(fd, seq, "{\"ok\":true,\"output\":\"hello from target\\n\"}", 0);
            for (;;)
            {
                if (!ReadOneFrame(fd, seq))
                {
                    return;
                }
            }
        }
        else if (g_mode == "reply-never")
        {
            // 全程不应答(等待被打断用例)
            for (;;)
            {
                if (!ReadOneFrame(fd, seq))
                {
                    return;
                }
            }
        }
        else
        {
            Respond(fd, seq, "{\"ok\":true,\"output\":\"ok\\n\"}", 0);
        }
    }
}

void* ControlThread(void*)
{
    (void)pthread_setname_np(pthread_self(), kThreadName);
    // 就绪通知先于sigwait: Test侧确认线程名已生效后才启动控制端
    (void)!::write(STDOUT_FILENO, "ready\n", 6);
    int signo = 0;
    if (sigwait(&g_sigset, &signo) != 0)
    {
        return nullptr;
    }
    if (g_mode == "no-connect")
    {
        return nullptr;
    }
    if (g_mode == "delegate-connect")
    {
        // 连接交给测试进程(用于SO_PEERCRED凭证不匹配场景)
        if (g_notifyFd >= 0)
        {
            (void)!::write(g_notifyFd, "c", 1);
        }
        ::usleep(2000000);
        return nullptr;
    }
    int fd = ConnectToController();
    if (fd < 0)
    {
        return nullptr;
    }
    std::string error;
    if (g_mode == "connect-close")
    {
        ::close(fd);
        return nullptr;
    }
    if (g_mode == "register-wrongtype")
    {
        (void)ControlProtocol::SendFrame(fd, ControlProtocol::MSG_CONTROL, 0, 0, "{}", error);
        ::usleep(200000);
        ::close(fd);
        return nullptr;
    }
    if (g_mode == "register-badjson")
    {
        (void)ControlProtocol::SendFrame(fd, ControlProtocol::MSG_REGISTER, 0, 0, "not-json", error);
        ::usleep(200000);
        ::close(fd);
        return nullptr;
    }
    if (g_mode == "register-badproto")
    {
        (void)ControlProtocol::SendFrame(fd, ControlProtocol::MSG_REGISTER, 0, 0, "{\"pid\":1,\"proto\":99}", error);
        ::usleep(200000);
        ::close(fd);
        return nullptr;
    }
    const std::string reg = "{\"pid\":" + std::to_string(static_cast<long long>(::getpid())) + ",\"proto\":1}";
    (void)ControlProtocol::SendFrame(fd, ControlProtocol::MSG_REGISTER, 0, 0, reg, error);
    if (g_mode == "register-ok")
    {
        // 控制端不发控制字(help/exit/非法控制字):阻塞读至对端关闭
        uint16_t seq = 0;
        while (ReadOneFrame(fd, seq))
        {
        }
        ::close(fd);
        return nullptr;
    }
    RunReplyLoop(fd);
    ::close(fd);
    return nullptr;
}
}  // namespace

int main(int argc, char** argv)
{
    g_mode = argc > 1 ? argv[1] : "reply-ok";
    if (argc > 2)
    {
        g_notifyFd = std::atoi(argv[2]);
    }
    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &g_sigset, nullptr) != 0)
    {
        return 2;
    }
    if (g_mode == "no-thread")
    {
        (void)!::write(STDOUT_FILENO, "ready\n", 6);
        for (;;)
        {
            ::pause();
        }
    }
    pthread_t tid;
    if (pthread_create(&tid, nullptr, &ControlThread, nullptr) != 0)
    {
        return 2;
    }
    pthread_join(tid, nullptr);
    return 0;
}
