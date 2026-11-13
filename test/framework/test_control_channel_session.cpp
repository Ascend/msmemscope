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

/* 业务侧控制通道(ControlChannel)端到端测试:
 * 测试进程自身即目标进程(静态构造已启动监听线程)。测试扮演控制端:
 * bind目标pid的socket路径→kill(SIGUSR1)唤醒→accept→校验REGISTER→
 * 收发CONTROL/RESPONSE→验证会话生命周期(EOF/非法帧回IDLE后可再次建联)。
 * 覆盖: IDLE无socket空转、僵尸socket连接失败回IDLE、REGISTER内容、
 * 命令执行与RESPONSE(ok位/seq回带)、非CONTROL消息/非法JSON/空cmd/坏版本帧
 * 的断会话路径、会话中重复SIGUSR1不重复唤醒、会话结束后重新建联。
 */
#include <gtest/gtest.h>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "config_info.h"
#include "control_channel/control_channel.h"
#include "control_channel/control_protocol.h"
#include "trace_manager/event_trace_manager.h"
#include "nlohmann/json.hpp"

using namespace MemScope;

namespace
{
constexpr int kWaitTimeoutMs = 5000;

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

bool ReadFrameAs(int fd, std::vector<uint8_t>& payload, uint16_t& seq, uint8_t& msgType, uint16_t& flags)
{
    struct pollfd pfd = {fd, POLLIN, 0};
    if (::poll(&pfd, 1, kWaitTimeoutMs) <= 0)
    {
        return false;
    }
    std::string error;
    return ControlProtocol::ReadFrame(fd, payload, seq, msgType, flags, error);
}

bool ReadRegister(int fd, nlohmann::json& reg)
{
    std::vector<uint8_t> payload;
    uint16_t seq = 0;
    uint8_t msgType = 0;
    uint16_t flags = 0;
    if (!ReadFrameAs(fd, payload, seq, msgType, flags) || msgType != ControlProtocol::MSG_REGISTER)
    {
        return false;
    }
    reg = nlohmann::json::parse(payload.begin(), payload.end());
    return true;
}

bool SendControl(int fd, uint16_t seq, const std::string& cmd, const std::string& param)
{
    nlohmann::json req;
    req["cmd"] = cmd;
    req["param"] = param;
    std::string error;
    return ControlProtocol::SendFrame(fd, ControlProtocol::MSG_CONTROL, 0, seq, req.dump(), error);
}

// 收RESPONSE单帧,返回ok位与output
bool ReadResponse(int fd, uint16_t expectedSeq, bool& ok, std::string& output)
{
    ok = false;
    output.clear();
    std::vector<uint8_t> payload;
    uint16_t seq = 0;
    uint8_t msgType = 0;
    uint16_t flags = 0;
    if (!ReadFrameAs(fd, payload, seq, msgType, flags) || msgType != ControlProtocol::MSG_RESPONSE || seq != expectedSeq)
    {
        return false;
    }
    const nlohmann::json parsed = nlohmann::json::parse(payload.begin(), payload.end());
    ok = parsed.value("ok", false);
    output = parsed.value("output", "");
    return true;
}

bool WaitPeerClosed(int fd)
{
    struct pollfd pfd = {fd, POLLIN, 0};
    if (::poll(&pfd, 1, kWaitTimeoutMs) <= 0)
    {
        return false;
    }
    char byte = 0;
    return ::recv(fd, &byte, 1, 0) == 0;
}

class ControlChannelSessionTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        if (!ControlChannel::GetInstance().IsEnabled())
        {
            GTEST_SKIP() << "control channel disabled (MSMEMSCOPE_DISABLE_CONTROL_CHANNEL)";
        }
        EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
        path_ = ControlProtocol::SocketPath(static_cast<uint64_t>(::getpid()));
        ::unlink(path_.c_str());
    }

    void TearDown() override
    {
        if (session_ >= 0)
        {
            ::close(session_);
            session_ = -1;
        }
        if (listen_ >= 0)
        {
            ::close(listen_);
            listen_ = -1;
        }
        ::unlink(path_.c_str());
        EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    }

    // 唤醒并accept:上一用例会话收尾期间g_chActive可能仍为1(SIGUSR1被吞),
    // 以短超时重试——监听线程回IDLE后下一次kill即可建联
    bool WakeAndAccept()
    {
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            ::kill(::getpid(), SIGUSR1);
            struct pollfd pfd = {listen_, POLLIN, 0};
            if (::poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN) != 0)
            {
                session_ = ::accept(listen_, nullptr, nullptr);
                return session_ >= 0;
            }
        }
        return false;
    }

    // 建联:bind+listen→SIGUSR1→accept→读REGISTER
    bool EstablishSession()
    {
        listen_ = BindListen(path_);
        if (listen_ < 0)
        {
            return false;
        }
        if (!WakeAndAccept())
        {
            return false;
        }
        nlohmann::json reg;
        return ReadRegister(session_, reg) && reg.value("proto", 0) == ControlProtocol::PROTO_VERSION;
    }

    std::string path_;
    int listen_ = -1;
    int session_ = -1;
};

// SIGUSR1空转与僵尸socket连接失败都不应破坏监听循环：随后仍可正常建联
TEST_F(ControlChannelSessionTest, spurious_wake_and_refused_connect_keep_listener_alive)
{
    // 无socket文件时SIGUSR1唤醒一次
    ::kill(::getpid(), SIGUSR1);
    ::usleep(200000);

    // 保留socket文件但关闭监听fd，连接被拒后再唤醒一次
    const int probe = BindListen(path_);
    ASSERT_GE(probe, 0);
    ::close(probe);
    ::kill(::getpid(), SIGUSR1);
    ::usleep(300000);

    ASSERT_TRUE(EstablishSession());
}

// REGISTER内容(pid/proto) + 命令执行与RESPONSE(seq回带/ok位) + 会话中重复SIGUSR1
TEST_F(ControlChannelSessionTest, command_roundtrip_and_response_fields)
{
    ASSERT_TRUE(EstablishSession());

    bool ok = false;
    std::string output;
    ASSERT_TRUE(SendControl(session_, 7, "display config", ""));
    ASSERT_TRUE(ReadResponse(session_, 7, ok, output));
    EXPECT_TRUE(ok);
    EXPECT_NE(output.find("analysis: "), std::string::npos) << output;

    // 会话中再次SIGUSR1: g_chActive非0,处理器直接返回(不重复唤醒)
    ::kill(::getpid(), SIGUSR1);

    ASSERT_TRUE(SendControl(session_, 8, "start", ""));
    ASSERT_TRUE(ReadResponse(session_, 8, ok, output));
    EXPECT_TRUE(ok);
    EXPECT_EQ(output, "tracing started");

    ASSERT_TRUE(SendControl(session_, 9, "stop", ""));
    ASSERT_TRUE(ReadResponse(session_, 9, ok, output));
    EXPECT_TRUE(ok);
    EXPECT_EQ(output, "tracing stopped");
}

// 业务拒绝(ok=false)同样回传: set config非法选项
TEST_F(ControlChannelSessionTest, command_business_reject_returns_ok_false)
{
    ASSERT_TRUE(EstablishSession());
    bool ok = true;
    std::string output;
    ASSERT_TRUE(SendControl(session_, 21, "set config", "--bogus 1"));
    ASSERT_TRUE(ReadResponse(session_, 21, ok, output));
    EXPECT_FALSE(ok);
    EXPECT_NE(output.find("unknown option: --bogus"), std::string::npos) << output;
}

// 超长响应按预算分片: output超出单帧预算(64KB-256B)时切分,
// 非末帧置FLAG_CONT,末帧清零;控制端逐帧拼接得到完整响应
TEST_F(ControlChannelSessionTest, large_response_is_chunked_by_budget)
{
    ASSERT_TRUE(EstablishSession());
    // 超长非法控制字被原样回显: output = "invalid control word: " + cmd
    // 65300字符使output(65322B)略超分片预算65280B,触发一次按预算切分
    const std::string hugeCmd(65300, 'a');
    nlohmann::json req;
    req["cmd"] = hugeCmd;
    req["param"] = "";
    const std::string dump = req.dump();
    ASSERT_LE(dump.size(), ControlProtocol::MAX_PAYLOAD_LEN);
    std::string error;
    ASSERT_TRUE(ControlProtocol::SendFrame(session_, ControlProtocol::MSG_CONTROL, 0, 31, dump, error));

    std::string merged;
    int frames = 0;
    for (;;)
    {
        std::vector<uint8_t> payload;
        uint16_t seq = 0;
        uint8_t msgType = 0;
        uint16_t flags = 0;
        ASSERT_TRUE(ReadFrameAs(session_, payload, seq, msgType, flags));
        ASSERT_EQ(msgType, ControlProtocol::MSG_RESPONSE);
        ASSERT_EQ(seq, 31);
        const nlohmann::json parsed = nlohmann::json::parse(payload.begin(), payload.end());
        EXPECT_FALSE(parsed.value("ok", true));
        merged += parsed.value("output", "");
        ++frames;
        if ((flags & ControlProtocol::FLAG_CONT) == 0)
        {
            break;
        }
    }
    EXPECT_GE(frames, 2);
    EXPECT_EQ(merged, "invalid control word: " + hugeCmd);
}

// 转义放大超限: 控制字符(\x01)在JSON中放大为6字节,整段序列化超64KB,
// 分片长度减半重试,且该帧退化为非末帧(原为末帧)
TEST_F(ControlChannelSessionTest, escaped_response_halves_chunk)
{
    ASSERT_TRUE(EstablishSession());
    // 10918个\x01: 入帧payload=22+6*10918=65530B仍在64KB内,
    // 回显序列化=22+6*10918+33=65563B超出上限,只能减半
    const std::string hugeCmd(10918, '\x01');
    nlohmann::json req;
    req["cmd"] = hugeCmd;
    req["param"] = "";
    const std::string dump = req.dump();
    ASSERT_LE(dump.size(), ControlProtocol::MAX_PAYLOAD_LEN);
    std::string error;
    ASSERT_TRUE(ControlProtocol::SendFrame(session_, ControlProtocol::MSG_CONTROL, 0, 32, dump, error));

    std::string merged;
    int frames = 0;
    for (;;)
    {
        std::vector<uint8_t> payload;
        uint16_t seq = 0;
        uint8_t msgType = 0;
        uint16_t flags = 0;
        ASSERT_TRUE(ReadFrameAs(session_, payload, seq, msgType, flags));
        ASSERT_EQ(msgType, ControlProtocol::MSG_RESPONSE);
        ASSERT_EQ(seq, 32);
        ASSERT_LE(payload.size(), ControlProtocol::MAX_PAYLOAD_LEN);
        const nlohmann::json parsed = nlohmann::json::parse(payload.begin(), payload.end());
        EXPECT_FALSE(parsed.value("ok", true));
        merged += parsed.value("output", "");
        ++frames;
        if ((flags & ControlProtocol::FLAG_CONT) == 0)
        {
            break;
        }
    }
    EXPECT_GE(frames, 2);
    EXPECT_EQ(merged, "invalid control word: " + hugeCmd);
}

// 非CONTROL消息: 会话断开(不静默吞),监听线程回IDLE
TEST_F(ControlChannelSessionTest, session_dropped_on_non_control_message)
{
    ASSERT_TRUE(EstablishSession());
    std::string error;
    ASSERT_TRUE(ControlProtocol::SendFrame(session_, ControlProtocol::MSG_RESPONSE, 0, 0, "{}", error));
    EXPECT_TRUE(WaitPeerClosed(session_));
    ::close(session_);
    session_ = -1;
}

// 非法JSON载荷: 会话断开
TEST_F(ControlChannelSessionTest, session_dropped_on_invalid_json)
{
    ASSERT_TRUE(EstablishSession());
    std::string error;
    ASSERT_TRUE(ControlProtocol::SendFrame(session_, ControlProtocol::MSG_CONTROL, 0, 0, "not-json", error));
    EXPECT_TRUE(WaitPeerClosed(session_));
    ::close(session_);
    session_ = -1;
}

// 空cmd: 会话断开
TEST_F(ControlChannelSessionTest, session_dropped_on_empty_cmd)
{
    ASSERT_TRUE(EstablishSession());
    std::string error;
    ASSERT_TRUE(ControlProtocol::SendFrame(session_, ControlProtocol::MSG_CONTROL, 0, 0, "{}", error));
    EXPECT_TRUE(WaitPeerClosed(session_));
    ::close(session_);
    session_ = -1;
}

// 帧版本不支持: ReadFrame失败 → 会话断开
TEST_F(ControlChannelSessionTest, session_dropped_on_bad_frame_version)
{
    ASSERT_TRUE(EstablishSession());
    uint8_t header[ControlProtocol::HEADER_LEN] = {0};
    header[0] = 2;                              // 非法版本
    header[1] = ControlProtocol::MSG_CONTROL;   // 类型正确
    ASSERT_EQ(::send(session_, header, sizeof(header), MSG_NOSIGNAL), static_cast<ssize_t>(sizeof(header)));
    EXPECT_TRUE(WaitPeerClosed(session_));
    ::close(session_);
    session_ = -1;
}

// 对端直接关闭(EOF): 会话断开;再次唤醒可重新建联
TEST_F(ControlChannelSessionTest, session_dropped_on_eof_then_reconnect)
{
    ASSERT_TRUE(EstablishSession());
    ::close(session_);
    session_ = -1;
    ::usleep(200000);  // 等待监听线程感知EOF并回IDLE

    ASSERT_TRUE(WakeAndAccept());
    nlohmann::json reg;
    ASSERT_TRUE(ReadRegister(session_, reg));
    EXPECT_EQ(reg.value("pid", 0ULL), static_cast<uint64_t>(::getpid()));
    EXPECT_EQ(reg.value("proto", 0), ControlProtocol::PROTO_VERSION);
}

}  // namespace
