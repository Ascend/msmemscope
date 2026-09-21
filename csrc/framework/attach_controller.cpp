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

// SO_PEERCRED/struct ucred为Linux GNU扩展:须在首个系统头包含前定义
#define _GNU_SOURCE

#include "attach_controller.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "control_channel/control_protocol.h"
#include "line_editor.h"
#include "nlohmann/json.hpp"
#include "utility/log.h"
#include "utility/utils.h"

namespace MemScope
{

namespace
{
// SIGINT/SIGTERM退出标志(async-signal-safe写,主循环poll检查)
volatile sig_atomic_t g_exitRequested = 0;
// 最近一次退出信号编号(区分SIGINT/SIGTERM:等待态SIGINT=打断会话保留,
// SIGTERM任意时刻=退出路径;0=无信号)
volatile sig_atomic_t g_sigCaught = 0;

void OnSigQuit(int signo)
{
    g_exitRequested = 1;
    g_sigCaught = signo;
}

constexpr uint64_t kDefaultTimeoutMs = 30000;
// 响应等待:1s粒度轮询(30s提示与打断检查同源);30s打印等待提示;300s无响应兜底打断
constexpr int kWaitTickMs = 1000;
constexpr int kWaitHintMs = 30000;
constexpr int kWaitAbortMs = 300000;
// 对端退出提示:断联即时打印一次;其后非exit/help控制字均回显该提示(会话续存)
constexpr char kTargetExitedMsg[] = "target process exited";

// help本地命令:打印控制字清单与说明(交互与-c单发共用,不发帧)
void PrintHelp()
{
    std::cout << "  start                   enable tracing (same as msmemscope.start())" << std::endl;
    std::cout << "  stop                    disable tracing (same as msmemscope.stop())" << std::endl;
    std::cout << "  step                    mark a step (same as msmemscope.step())" << std::endl;
    std::cout << "  display hook            show hook .so categories loaded in target" << std::endl;
    std::cout << "  display analyzer        list analyzers registered in target" << std::endl;
    std::cout << "  display config          show effective configuration" << std::endl;
    std::cout << "  display memory summary  show memory overview (devices/hal/pools with peaks)" << std::endl;
    std::cout << "  display memory block    show top-N largest live blocks (--pool p [--TOPN N])" << std::endl;
    std::cout << "  display host_leak summary  show interim leak overview (window open only)" << std::endl;
    std::cout << "  set config <args>       update configuration (CLI-style args, stop state only)" << std::endl;
    std::cout << "  exit                    detach and exit" << std::endl;
}

// 刷新tab补全analyzer缓存(建联预取与display analyzer回显成功后调用):
// 解析回显"Analyzers: <a,b,c>"列表注入CompletionTable动态提供者
void RefreshAnalyzerCache(const std::string& response)
{
    const std::string marker = "Analyzers: ";
    std::vector<std::string> names;
    size_t pos = response.find(marker);
    if (pos != std::string::npos)
    {
        const std::string list = response.substr(pos + marker.size());
        for (const std::string& name : Utility::SplitString(list, ","))
        {
            std::string trimmed = name;
            while (!trimmed.empty() && trimmed.front() == ' ')
            {
                trimmed.erase(trimmed.begin());
            }
            if (!trimmed.empty())
            {
                names.push_back(trimmed);
            }
        }
    }
    CompletionTable::SetDynamicProvider([names]() { return names; });
}

// 测试缝:非0时优先于env/默认值(UT注入)
uint64_t g_testTimeoutMs = 0;

// 会话清理RAII:任何退出路径统一close+unlink(cleanup由调用方传入,
// 规避匿名命名空间类访问AttachController私有成员的权限问题)
class SessionGuard
{
   public:
    explicit SessionGuard(std::function<void()> cleanup) : cleanup_(std::move(cleanup)) {}
    ~SessionGuard() { cleanup_(); }

   private:
    std::function<void()> cleanup_;
};

std::string ProcPath(uint64_t pid, const char* suffix) { return "/proc/" + std::to_string(pid) + suffix; }

bool ReadProcFileToString(const std::string& path, std::string& out)
{
    std::ifstream in(path);
    if (!in)
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}
}  // namespace

void AttachController::SetTimeoutMsForTest(uint64_t ms) { g_testTimeoutMs = ms; }

// 超时解析:测试缝→MSMEMSCOPE_ATTACH_TIMEOUT_MS env(正整数)→默认30s
static uint64_t ResolveTimeoutMs()
{
    if (g_testTimeoutMs != 0)
    {
        return g_testTimeoutMs;
    }
    const char* env = std::getenv(ControlProtocol::ENV_ATTACH_TIMEOUT_MS);
    if (env == nullptr || *env == '\0')
    {
        return kDefaultTimeoutMs;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(env, &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || v == 0)
    {
        return kDefaultTimeoutMs;
    }
    return static_cast<uint64_t>(v);
}

void AttachController::Run(uint64_t pid, const std::string& singleCommand)
{
    AttachController controller(pid, singleCommand);
    controller.timeoutMs_ = ResolveTimeoutMs();
    // SIGINT/SIGTERM:清理后退出130(handler只置位,主循环负责清理)
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &OnSigQuit;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)::sigaction(SIGINT, &sa, nullptr);
    (void)::sigaction(SIGTERM, &sa, nullptr);
    // 退出信号置位检测注入line_editor:交互读EINTR时据此区分退出信号/其他信号
    LineEditor::SetQuitFlagProvider([]() { return g_exitRequested != 0; });
    // 会话提示符(交互回显含目标pid)
    LineEditor::SetPrompt("msmemscope[" + std::to_string(pid) + "]> ");

    std::exit(controller.RunImpl());
}

int AttachController::RunImpl()
{
    SessionGuard guard([this] { Cleanup(); });

    // ---- 四规则校验(任一失败报错且不发信号) ----
    if (!CheckPidAlive())
    {
        std::cout << "[msmemscope] Error: process " << pid_ << " does not exist" << std::endl;
        return 1;
    }
    if (!CheckSameUid())
    {
        std::cout << "[msmemscope] Error: process " << pid_ << " is owned by another user (permission denied)"
                  << std::endl;
        return 1;
    }
    if (!CheckMaps())
    {
        std::cout << "[msmemscope] Error: process " << pid_ << " does not have libascend_leaks.so loaded "
                  << "(unsupported process)" << std::endl;
        return 1;
    }
    if (!CheckThreadName())
    {
        std::cout << "[msmemscope] Error: process " << pid_
                  << " has no msmemscope_ctrl listener thread (old version or SIGUSR1 overridden by another "
                     "tool)"
                  << std::endl;
        return 1;
    }

    // ---- socket生命周期 ----
    if (!PrepareSocket())
    {
        return g_exitRequested != 0 ? 130 : 1;
    }
    if (!WaitAndAccept())
    {
        return g_exitRequested != 0 ? 130 : 1;
    }
    if (!ReadRegister())
    {
        return 1;
    }
    // REGISTER收到即删socket文件(会话期间不再有新控制端)
    ::unlink(ControlProtocol::SocketPath(pid_).c_str());

    if (!singleCommand_.empty())
    {
        return RunSingleCommand();
    }
    else
    {
        RunInteractive();
    }
    // SIGINT/SIGTERM(sigaction置位):清理后退出130(与头注释一致);正常exit/EOF=0
    return g_exitRequested != 0 ? 130 : 0;
}

bool AttachController::CheckPidAlive() const { return ::access(ProcPath(pid_, "").c_str(), F_OK) == 0; }

bool AttachController::CheckSameUid() const
{
    struct stat st;
    if (::stat(ProcPath(pid_, "").c_str(), &st) != 0)
    {
        return false;
    }
    // uid相同视为合法；当前进程为root用户时可附加任意用户进程
    return st.st_uid == ::getuid() || ::geteuid() == 0;
}

bool AttachController::CheckMaps() const
{
    std::string maps;
    if (!ReadProcFileToString(ProcPath(pid_, "/maps"), maps))
    {
        return false;
    }
    return maps.find("libascend_leaks") != std::string::npos;
}

bool AttachController::CheckThreadName() const
{
    const std::string taskDir = ProcPath(pid_, "/task");
    DIR* dir = ::opendir(taskDir.c_str());
    if (dir == nullptr)
    {
        return false;
    }
    bool found = false;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        const std::string commPath = taskDir + "/" + entry->d_name + "/comm";
        std::string comm;
        if (!ReadProcFileToString(commPath, comm))
        {
            continue;
        }
        // comm含尾部换行
        while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r'))
        {
            comm.pop_back();
        }
        if (comm == "msmemscope_ctrl")
        {
            found = true;
            break;
        }
    }
    ::closedir(dir);
    return found;
}

bool AttachController::PrepareSocket()
{
    const std::string sockPath = ControlProtocol::SocketPath(pid_);
    struct stat st;
    if (::lstat(sockPath.c_str(), &st) == 0)
    {
        // 已有socket:尝试connect判活
        int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (probe < 0)
        {
            std::cout << "[msmemscope] Error: socket() failed: " << std::strerror(errno) << std::endl;
            return false;
        }
        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
        const int cr = ::connect(probe, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        ::close(probe);
        if (cr == 0)
        {
            std::cout << "[msmemscope] Error: process " << pid_
                      << " already attached by another controller (live session in progress)" << std::endl;
            return false;
        }
        if (errno != ECONNREFUSED)
        {
            std::cout << "[msmemscope] Error: socket " << sockPath << " probe failed: " << std::strerror(errno)
                      << std::endl;
            return false;
        }
        // 僵尸socket(对端已退出):unlink重建
        if (::unlink(sockPath.c_str()) != 0)
        {
            std::cout << "[msmemscope] Error: stale socket unlink failed: " << std::strerror(errno) << std::endl;
            return false;
        }
    }
    else if (errno != ENOENT)
    {
        std::cout << "[msmemscope] Error: lstat socket failed: " << std::strerror(errno) << std::endl;
        return false;
    }

    // bind(umask限制为0600)+listen
    const mode_t oldUmask = ::umask(0077);
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0)
    {
        ::umask(oldUmask);
        std::cout << "[msmemscope] Error: socket() failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    const int br = ::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    ::umask(oldUmask);
    if (br != 0)
    {
        std::cout << "[msmemscope] Error: bind " << sockPath << " failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    if (::listen(listenFd_, 1) != 0)
    {
        std::cout << "[msmemscope] Error: listen failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool AttachController::WaitAndAccept()
{
    // 校验已通过才发信号;SIGUSR1唤醒业务侧监听线程发起connect
    if (::kill(static_cast<pid_t>(pid_), SIGUSR1) != 0)
    {
        std::cout << "[msmemscope] Error: kill(SIGUSR1) failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    // accept等待(poll超时;超时提示语覆盖already attached/SIGUSR1被覆盖场景)
    struct pollfd pfd = {listenFd_, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, static_cast<int>(timeoutMs_));
    if (pr < 0 && errno == EINTR)
    {
        // 等待期信号(SIGINT/SIGTERM):不报超时误导,退出码由RunImpl按g_exitRequested裁决
        return false;
    }
    if (pr <= 0)
    {
        std::cout << "[msmemscope] Error: attach timed out after " << timeoutMs_
                  << "ms (process already attached, or SIGUSR1 overridden by another tool)" << std::endl;
        return false;
    }
    sessionFd_ = ::accept(listenFd_, nullptr, nullptr);
    if (sessionFd_ < 0)
    {
        std::cout << "[msmemscope] Error: accept failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    // SO_PEERCRED:确认连接方确实是目标进程
    struct ucred cred;
    socklen_t credLen = sizeof(cred);
    if (::getsockopt(sessionFd_, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0)
    {
        std::cout << "[msmemscope] Error: SO_PEERCRED failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    if (cred.pid != static_cast<int32_t>(pid_) || cred.uid != ::getuid())
    {
        std::cout << "[msmemscope] Error: peer credential mismatch (pid=" << cred.pid << " uid=" << cred.uid
                  << "), attach aborted" << std::endl;
        return false;
    }
    return true;
}

bool AttachController::ReadRegister()
{
    std::vector<uint8_t> payload;
    uint16_t seq = 0;
    uint8_t msgType = 0;
    uint16_t flags = 0;
    std::string error;
    if (!ControlProtocol::ReadFrame(sessionFd_, payload, seq, msgType, flags, error))
    {
        std::cout << "[msmemscope] Error: REGISTER read failed: " << error << std::endl;
        return false;
    }
    if (msgType != ControlProtocol::MSG_REGISTER)
    {
        std::cout << "[msmemscope] Error: expected REGISTER, got msg_type=" << static_cast<uint32_t>(msgType)
                  << std::endl;
        return false;
    }
    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(payload.begin(), payload.end());
        if (parsed.value("proto", 0) != ControlProtocol::PROTO_VERSION)
        {
            std::cout << "[msmemscope] Error: incompatible protocol version" << std::endl;
            return false;
        }
    }
    catch (const std::exception&)
    {
        std::cout << "[msmemscope] Error: REGISTER payload is not valid JSON (" << payload.size() << " bytes)"
                  << std::endl;
        // 诊断转储:帧头(msgType)解析正确但载荷解析失败,转储实际字节定位错位来源
        std::cout << "  payload hex:";
        char hexBuf[8];
        for (size_t i = 0; i < payload.size() && i < 128; ++i)
        {
            std::snprintf(hexBuf, sizeof(hexBuf), " %02x", payload[i]);
            std::cout << hexBuf;
        }
        std::cout << std::endl;
        return false;
    }
    return true;
}

bool AttachController::SendControl(const std::string& cmd, const std::string& param, bool* cmdOk,
                                   WaitResult* waitResult, std::string* responseOut)
{
    if (waitResult != nullptr)
    {
        *waitResult = WaitResult::Error;  // 默认失败(协议/对端退出)
    }
    nlohmann::json req;
    req["cmd"] = cmd;
    req["param"] = param;
    std::string error;
    if (!ControlProtocol::SendFrame(sessionFd_, ControlProtocol::MSG_CONTROL, 0, seq_, req.dump(), error))
    {
        std::cout << "[msmemscope] Error: control frame send failed: " << error << std::endl;
        return false;
    }

    // 收RESPONSE(1s粒度轮询;30s提示/打断检查同源):CONT位分片逐帧处理——
    // responseOut为空=边收边显(响应理论无上限,不整体组包,单帧缓冲≤64KB);
    // responseOut非空=逐帧累积(仅display analyzer等有界响应:调用方需解析)
    bool ok = false;
    uint64_t elapsedMs = 0;
    uint64_t nextHintMs = static_cast<uint64_t>(kWaitHintMs);
    for (;;)
    {
        struct pollfd pfd = {sessionFd_, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, kWaitTickMs);
        if (pr > 0 && (pfd.revents & POLLIN) != 0)
        {
            std::vector<uint8_t> payload;
            uint16_t respSeq = 0;
            uint8_t msgType = 0;
            uint16_t flags = 0;
            if (!ControlProtocol::ReadFrame(sessionFd_, payload, respSeq, msgType, flags, error))
            {
                std::cout << "[msmemscope] Error: response read failed: " << error << " (target process exited?)"
                          << std::endl;
                return false;
            }
            if (msgType != ControlProtocol::MSG_RESPONSE || respSeq != seq_)
            {
                std::cout << "[msmemscope] Error: protocol violation (msg_type=" << static_cast<uint32_t>(msgType)
                          << " seq=" << respSeq << ")" << std::endl;
                return false;
            }
            try
            {
                const nlohmann::json parsed = nlohmann::json::parse(payload.begin(), payload.end());
                ok = parsed.value("ok", false);
                const std::string part = parsed.value("output", "");
                if (responseOut == nullptr)
                {
                    std::cout << part;  // 边收边显:每收一帧即回显(64KB边界可能断行,属预期)
                }
                else
                {
                    responseOut->append(part);
                }
            }
            catch (const std::exception&)
            {
                std::cout << "[msmemscope] Error: response payload is not valid JSON" << std::endl;
                return false;
            }
            if ((flags & ControlProtocol::FLAG_CONT) == 0)
            {
                break;  // 末帧
            }
            continue;
        }
        if (pr < 0 && errno == EINTR)
        {
            // 等待态SIGINT(Ctrl-C):打断等待(SIGTERM走下方g_exitRequested退出路径)
            if (g_sigCaught == SIGINT)
            {
                if (waitResult != nullptr)
                {
                    *waitResult = WaitResult::Interrupted;
                }
                return false;
            }
            if (g_exitRequested != 0)
            {
                return false;  // SIGTERM等:退出路径(退出码由RunImpl裁决)
            }
            continue;  // 其他信号:重试
        }
        if (pr < 0 || (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
        {
            std::cout << "[msmemscope] Error: target process exited during control exchange" << std::endl;
            return false;
        }
        // pr==0:一个tick超时——提示/兜底打断检查
        elapsedMs += static_cast<uint64_t>(kWaitTickMs);
        if (elapsedMs >= nextHintMs)
        {
            std::cout << "still waiting for response (" << (elapsedMs / 1000) << "s elapsed), Ctrl-C to abort"
                      << std::endl;
            nextHintMs += static_cast<uint64_t>(kWaitHintMs);
        }
        if (elapsedMs >= static_cast<uint64_t>(kWaitAbortMs))
        {
            if (waitResult != nullptr)
            {
                *waitResult = WaitResult::Timeout;  // 5min无响应兜底打断
            }
            return false;
        }
    }
    ++seq_;
    if (seq_ == 0)
    {
        std::cout << "[msmemscope] Error: sequence space exhausted, detaching" << std::endl;
        return false;
    }
    if (responseOut == nullptr)
    {
        std::cout << std::endl;  // 边收边显路径:补行尾换行(原整体回显语义)
    }
    if (cmdOk != nullptr)
    {
        *cmdOk = ok;  // 业务侧拒绝(false)与传输失败(return false)区分
    }
    if (waitResult != nullptr)
    {
        *waitResult = WaitResult::Done;
    }
    return true;
}

bool AttachController::CheckPeerGone()
{
    if (sessionFd_ < 0)
    {
        return false;
    }
    // 非阻塞poll+MSG_PEEK内窥确认EOF:不消费会话数据;空闲期无在途帧
    // (RESPONSE已在SendControl内逐帧消费完),可读却读不到字节=对端已close
    struct pollfd pfd = {sessionFd_, POLLIN, 0};
    if (::poll(&pfd, 1, 0) <= 0)
    {
        return false;  // 无事件/被信号打断:视为存活
    }
    if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
    {
        return true;
    }
    char probe = 0;
    const ssize_t n = ::recv(sessionFd_, &probe, 1, MSG_PEEK);
    return n == 0;
}

int AttachController::RunSingleCommand()
{
    // 本地命令不发帧:help打印控制字清单退出0;exit直接退出0
    if (singleCommand_ == "help")
    {
        PrintHelp();
        return 0;
    }
    if (singleCommand_ == "exit" || singleCommand_ == "quit")
    {
        return 0;
    }
    std::string cmd;
    std::string param;
    SplitCmdParam(singleCommand_, cmd, param);
    const std::vector<std::string> tokens = Utility::SplitString(singleCommand_, " ");
    if (!ControlProtocol::IsValidControlWord(tokens))
    {
        std::cout << "[msmemscope] Error: invalid control word: " << singleCommand_ << std::endl;
        return 1;
    }
    bool cmdOk = false;
    WaitResult waitResult = WaitResult::Error;
    if (!SendControl(cmd, param, &cmdOk, &waitResult))
    {
        // 等待结束原因→退出码:Ctrl-C打断=130;5min无响应=2;传输失败=1
        if (waitResult == WaitResult::Interrupted)
        {
            return 130;
        }
        if (waitResult == WaitResult::Timeout)
        {
            std::cout << "[msmemscope] Error: no response for 300s, aborted waiting" << std::endl;
            return 2;
        }
        return 1;
    }
    return cmdOk ? 0 : 1;
}

void AttachController::RunInteractive()
{
    std::cout << "[msmemscope] Target check passed (pid=" << pid_ << ", uid=" << ::getuid()
              << ", libascend_leaks.so loaded, control channel ready)." << std::endl;
    std::cout << "[msmemscope] Attached to pid " << pid_ << ". Type 'help' for control words, 'exit' to detach."
              << std::endl;

    // 预取analyzer名(tab补全display analyzer用):建联后查一次并缓存;
    // display analyzer响应有界,走累积路径(responseOut)供解析
    {
        std::string response;
        if (SendControl("display analyzer", "", nullptr, nullptr, &response))
        {
            RefreshAnalyzerCache(response);
        }
    }

    const bool isTty = ::isatty(STDIN_FILENO) != 0;
    // 对端存活探针(LineEditor空闲tick调用):断联即打印提示一次,并触发
    // ReadLine重绘当前行(提示已打印,恢复输入编辑);已提示后回归纯stdin等待,
    // 后续非exit/help控制字由主循环门控回显同一提示
    LineEditor::SetPeerAliveProvider(
        [this]()
        {
            if (peerGone_)
            {
                return true;  // 已提示过:保持输入等待
            }
            if (CheckPeerGone())
            {
                peerGone_ = true;
                std::cout << kTargetExitedMsg << std::endl;
                return false;  // 触发ReadLine重绘
            }
            return true;
        });
    for (;;)
    {
        if (g_exitRequested != 0)
        {
            std::cout << "Interrupted" << std::endl;
            return;
        }
        std::string line;
        bool gotLine = false;
        bool interrupted = false;
        if (isTty)
        {
            gotLine = LineEditor::ReadLine(line, &interrupted);
        }
        else
        {
            gotLine = static_cast<bool>(std::getline(std::cin, line));
        }
        if (interrupted)
        {
            // 提示符态Ctrl-C:退出清理路径(退出码130由RunImpl按g_exitRequested裁决)
            g_exitRequested = 1;
            std::cout << "Interrupted" << std::endl;
            return;
        }
        if (!gotLine)
        {
            // EOF(Ctrl-D/输入结束):正常detach;信号打断的读(无SA_RESTART,EINTR→
            // EOF路径):置位标志已见,打印提示后按信号退出码收尾
            if (g_exitRequested != 0)
            {
                std::cout << "Interrupted" << std::endl;
            }
            return;
        }
        // 去尾空白
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }
        if (line == "exit" || line == "quit")
        {
            std::cout << "[msmemscope] Detached." << std::endl;
            return;
        }
        if (line == "help")
        {
            PrintHelp();
            continue;
        }
        // 对端已退出:非exit/help控制字不发帧,每次回显同一提示(exit/help已在上方处理)
        if (CheckPeerGone())
        {
            peerGone_ = true;
            std::cout << kTargetExitedMsg << std::endl;
            continue;
        }
        const std::vector<std::string> tokens = Utility::SplitString(line, " ");
        if (!ControlProtocol::IsValidControlWord(tokens))
        {
            std::cout << "invalid control word: " << line << std::endl;
            continue;
        }
        std::string cmd;
        std::string param;
        SplitCmdParam(line, cmd, param);
        bool cmdOk = false;
        WaitResult waitResult = WaitResult::Error;
        // display analyzer回显需解析刷新补全缓存:走累积路径(响应有界);
        // 其余控制字边收边显(响应理论无上限,避免整体组包)
        std::string analyzerResp;
        const bool isAnalyzer = (cmd == "display analyzer");
        if (!SendControl(cmd, param, &cmdOk, &waitResult, isAnalyzer ? &analyzerResp : nullptr))
        {
            // 等待态Ctrl-C:打断本次等待回到提示符,会话保留(清退出标志+残余输入)
            if (waitResult == WaitResult::Interrupted)
            {
                g_exitRequested = 0;
                g_sigCaught = 0;
                (void)::tcflush(STDIN_FILENO, TCIFLUSH);
                std::cout << "wait interrupted (Ctrl-C), session kept" << std::endl;
                continue;
            }
            if (waitResult == WaitResult::Timeout)
            {
                std::cout << "no response for 300s, aborted waiting (session kept)" << std::endl;
                continue;
            }
            // 传输失败但已确认对端断联:转已退出状态继续交互(仅exit/help可用),
            // 不因命令下发而报错退出;协议错误等仍按原行为收尾
            if (CheckPeerGone())
            {
                if (!peerGone_)
                {
                    peerGone_ = true;
                    std::cout << kTargetExitedMsg << std::endl;
                }
                continue;
            }
            return;  // 会话已断(协议错误)
        }
        if (isAnalyzer)
        {
            // 回显即一次实时查询:结果顺手刷新补全缓存(缓存新鲜度=最近一次回显)
            std::cout << analyzerResp << std::endl;
            if (cmdOk)
            {
                RefreshAnalyzerCache(analyzerResp);
            }
        }
        // 历史只保留下发成功(ok=true且完整往返)的命令:业务拒绝(ok=false)、
        // 等待被打断/超时/断联(SendControl失败)、未知控制字/本地命令均不入
        if (cmdOk)
        {
            LineEditor::AddHistory(line);
        }
    }
}

void AttachController::SplitCmdParam(const std::string& line, std::string& cmd, std::string& param)
{
    const std::vector<std::string> tokens = Utility::SplitString(line, " ");
    if (tokens.empty())
    {
        cmd.clear();
        param.clear();
        return;
    }
    if (tokens.size() == 1)
    {
        cmd = tokens[0];
        param.clear();
        return;
    }
    // 白名单语法:display <w> [<w>] / set config;cmd=语法主干,其余=param
    if (tokens[0] == "display")
    {
        cmd = "display " + tokens[1];
        if (tokens.size() >= 3 && (tokens[1] == "memory" || tokens[1] == "host_leak"))
        {
            cmd += " " + tokens[2];
        }
    }
    else if (tokens[0] == "set")
    {
        cmd = "set config";
    }
    else
    {
        cmd = tokens[0];
    }
    param.clear();
    size_t take = 1;  // tokens中属于cmd的个数
    if (tokens[0] == "display")
    {
        take = tokens.size() >= 3 && (tokens[1] == "memory" || tokens[1] == "host_leak") ? 3 : 2;
    }
    else if (tokens[0] == "set")
    {
        take = 2;
    }
    for (size_t i = take; i < tokens.size(); ++i)
    {
        if (!param.empty())
        {
            param += " ";
        }
        param += tokens[i];
    }
}

void AttachController::Cleanup() const
{
    if (sessionFd_ >= 0)
    {
        ::close(sessionFd_);
    }
    if (listenFd_ >= 0)
    {
        ::close(listenFd_);
    }
    // 残留socket防御性unlink(正常路径REGISTER后已删)
    const std::string sockPath = ControlProtocol::SocketPath(pid_);
    if (::access(sockPath.c_str(), F_OK) == 0)
    {
        ::unlink(sockPath.c_str());
    }
}

}  // namespace MemScope
