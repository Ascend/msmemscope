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

/* 控制通道测试: 帧编解码(ControlProtocol)/ControlEvent/订阅者名枚举
 * (GetSubscriberNames)/6分析器GetName唯一性/补全词表(CompletionTable)/
 * 行编辑器输入序列注入(LineEditor)。
 * 注意: 测试进程内已存在业务侧ControlChannel静态实例(其ctor订阅CONTROL),
 * GetSubscriberNames结果含既有订阅者——探针用例只断言唯一名字与相对顺序,
 * 不做全量列表断言。
 */
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

#include "control_channel/control_protocol.h"
#include "line_editor.h"
#include "event_dispatcher.h"
#include "event.h"

// 6个分析器GetName唯一性
#include "leak_analyzer.h"
#include "health_analyzer.h"
#include "dump.h"
#include "decompose_analyzer.h"
#include "inefficient_analyzer.h"
#include "host_leak_analyzer.h"

namespace
{

TEST(ControlProtocolTest, socket_path_format)
{
    EXPECT_EQ(MemScope::ControlProtocol::SocketPath(12345), "/tmp/msmemscope_socket_12345");
    EXPECT_EQ(MemScope::ControlProtocol::SocketPath(0), "/tmp/msmemscope_socket_0");
    // 与默认模板一致:Linux抽象路径命名(uid差异不在本层,由AttachController校验)
    EXPECT_NE(MemScope::ControlProtocol::SocketPath(12345).find("/tmp/msmemscope_socket_"),
              std::string::npos);
}

// 帧编解码往返: 头字节序(小端)/类型/flags/seq/载荷往返
TEST(ControlProtocolTest, frame_roundtrip)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    const std::string payload = "{\"cmd\":\"start\"}";  // 长15(实现写len=15,勿误计)
    std::string err;
    ASSERT_TRUE(MemScope::ControlProtocol::SendFrame(fds[0], MemScope::ControlProtocol::MSG_CONTROL, 0x0002,
                                                     0x1234, payload, err)) << err;

    // 头字节级校验: [version=1][type=0x02][flags 02 00][seq LE 34 12][len LE 0F 00 00 00]
    // MSG_PEEK内窥不消费:校验原始字节后帧仍留在socket,供后续ReadFrame解析
    // (直接recv会消费帧,致ReadFrame缺数据永久阻塞)
    uint8_t raw[64];
    const ssize_t frameLen = static_cast<ssize_t>(10 + payload.size());
    const ssize_t n = ::recv(fds[1], raw, static_cast<size_t>(frameLen), MSG_PEEK);
    ASSERT_EQ(n, frameLen) << "frame must arrive in full (payload len " << payload.size() << ")";
    EXPECT_EQ(raw[0], 1u);
    EXPECT_EQ(raw[1], 0x02u);
    EXPECT_EQ(raw[2], 0x02u);
    EXPECT_EQ(raw[3], 0x00u);
    EXPECT_EQ(raw[4], 0x34u);
    EXPECT_EQ(raw[5], 0x12u);
    EXPECT_EQ(raw[6], payload.size());
    EXPECT_EQ(raw[7], 0u);
    EXPECT_EQ(raw[8], 0u);
    EXPECT_EQ(raw[9], 0u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(raw + 10), payload.size()), payload);

    // ReadFrame解析出各字段
    std::vector<uint8_t> payloadOut;
    uint16_t seq = 0;
    uint8_t msgType = 0;
    uint16_t flags = 0;
    ASSERT_TRUE(MemScope::ControlProtocol::ReadFrame(fds[1], payloadOut, seq, msgType, flags, err)) << err;
    EXPECT_EQ(msgType, MemScope::ControlProtocol::MSG_CONTROL);
    EXPECT_EQ(flags, 0x0002u);
    EXPECT_EQ(seq, 0x1234u);
    EXPECT_EQ(payloadOut.size(), payload.size());
    EXPECT_EQ(std::string(payloadOut.begin(), payloadOut.end()), payload);

    close(fds[0]);
    close(fds[1]);
}

// REGISTER/RESPONSE分片标志/空载荷往返
TEST(ControlProtocolTest, frame_roundtrip_register_response_empty)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    std::string err;

    // REGISTER: seq固定0, 空载荷
    ASSERT_TRUE(MemScope::ControlProtocol::SendFrame(fds[0], MemScope::ControlProtocol::MSG_REGISTER, 0, 0, "", err))
        << err;
    std::vector<uint8_t> payload;
    uint16_t seq = 0;
    uint8_t msgType = 0;
    uint16_t flags = 0;
    ASSERT_TRUE(MemScope::ControlProtocol::ReadFrame(fds[1], payload, seq, msgType, flags, err)) << err;
    EXPECT_EQ(msgType, MemScope::ControlProtocol::MSG_REGISTER);
    EXPECT_EQ(seq, 0u);
    EXPECT_TRUE(payload.empty());

    // RESPONSE: FLAG_CONT置位(分片非末帧)
    ASSERT_TRUE(MemScope::ControlProtocol::SendFrame(fds[0], MemScope::ControlProtocol::MSG_RESPONSE,
                                                     MemScope::ControlProtocol::FLAG_CONT, 7, "out", err))
        << err;
    ASSERT_TRUE(MemScope::ControlProtocol::ReadFrame(fds[1], payload, seq, msgType, flags, err)) << err;
    EXPECT_EQ(msgType, MemScope::ControlProtocol::MSG_RESPONSE);
    EXPECT_EQ(flags, static_cast<uint16_t>(MemScope::ControlProtocol::FLAG_CONT));
    EXPECT_EQ(seq, 7u);
    EXPECT_EQ(std::string(payload.begin(), payload.end()), "out");

    close(fds[0]);
    close(fds[1]);
}

// 超长载荷: 发送侧拒绝(不落socket)
TEST(ControlProtocolTest, frame_oversize_payload_rejected)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    std::string err;
    const std::string big(MemScope::ControlProtocol::MAX_PAYLOAD_LEN + 1, 'x');
    EXPECT_FALSE(MemScope::ControlProtocol::SendFrame(fds[0], MemScope::ControlProtocol::MSG_CONTROL, 0, 0, big, err));
    EXPECT_EQ(err, "frame payload exceeds limit");
    close(fds[0]);
    close(fds[1]);
}

// 非法版本: 头部version!=1→拒绝(收侧)
TEST(ControlProtocolTest, frame_bad_version_rejected)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    uint8_t header[10] = {0};
    header[0] = 2;  // 非法版本
    header[1] = MemScope::ControlProtocol::MSG_CONTROL;
    ASSERT_EQ(::write(fds[0], header, sizeof(header)), static_cast<ssize_t>(sizeof(header)));
    std::vector<uint8_t> payload;
    uint16_t seq = 0;
    uint8_t msgType = 0;
    uint16_t flags = 0;
    std::string err;
    EXPECT_FALSE(MemScope::ControlProtocol::ReadFrame(fds[1], payload, seq, msgType, flags, err));
    EXPECT_EQ(err, "unsupported protocol version");
    close(fds[0]);
    close(fds[1]);
}

// 超限载荷长度: 头部len>64KB→拒绝(收侧,不读载荷)
TEST(ControlProtocolTest, frame_payload_too_large_rejected)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    uint8_t header[10] = {0};
    header[0] = MemScope::ControlProtocol::PROTO_VERSION;
    header[1] = MemScope::ControlProtocol::MSG_CONTROL;
    const uint32_t tooBig = static_cast<uint32_t>(MemScope::ControlProtocol::MAX_PAYLOAD_LEN) + 1;
    for (size_t i = 0; i < 4; ++i)
    {
        header[6 + i] = static_cast<uint8_t>((tooBig >> (8 * i)) & 0xFF);
    }
    ASSERT_EQ(::write(fds[0], header, sizeof(header)), static_cast<ssize_t>(sizeof(header)));
    std::vector<uint8_t> payload;
    uint16_t seq = 0;
    uint8_t msgType = 0;
    uint16_t flags = 0;
    std::string err;
    EXPECT_FALSE(MemScope::ControlProtocol::ReadFrame(fds[1], payload, seq, msgType, flags, err));
    EXPECT_EQ(err, "payload too large");
    close(fds[0]);
    close(fds[1]);
}

// 对端关闭: 头部读取中途EOF→"peer closed"
TEST(ControlProtocolTest, frame_peer_closed_reported)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    ::close(fds[0]);  // 对端已关
    std::vector<uint8_t> payload;
    uint16_t seq = 0;
    uint8_t msgType = 0;
    uint16_t flags = 0;
    std::string err;
    EXPECT_FALSE(MemScope::ControlProtocol::ReadFrame(fds[1], payload, seq, msgType, flags, err));
    EXPECT_EQ(err, "peer closed");
    close(fds[1]);
}

// 控制字白名单(IsValidControlWord): 单token/display二级/display memory三级/
// host_leak summary/set config(其后参数不参与校验)
TEST(ControlProtocolTest, control_word_validation)
{
    using namespace MemScope::ControlProtocol;
    EXPECT_TRUE(IsValidControlWord({"start"}));
    EXPECT_TRUE(IsValidControlWord({"stop"}));
    EXPECT_TRUE(IsValidControlWord({"step"}));
    EXPECT_FALSE(IsValidControlWord({}));
    EXPECT_FALSE(IsValidControlWord({"start", "x"}));
    EXPECT_FALSE(IsValidControlWord({"set"}));  // set单token非动词

    EXPECT_TRUE(IsValidControlWord({"display", "hook"}));
    EXPECT_TRUE(IsValidControlWord({"display", "analyzer"}));
    EXPECT_TRUE(IsValidControlWord({"display", "config"}));
    EXPECT_TRUE(IsValidControlWord({"display", "memory"}));
    EXPECT_TRUE(IsValidControlWord({"display", "host_leak"}));
    EXPECT_FALSE(IsValidControlWord({"display", "bogus"}));
    EXPECT_FALSE(IsValidControlWord({"display", "memory", "bogus"}));

    EXPECT_TRUE(IsValidControlWord({"display", "memory", "summary"}));
    EXPECT_TRUE(IsValidControlWord({"display", "memory", "block"}));
    EXPECT_TRUE(IsValidControlWord({"display", "host_leak", "summary"}));
    EXPECT_FALSE(IsValidControlWord({"display", "host_leak", "block"}));
    // display memory block的--pool/--TOPN参数由handler解析,白名单只校验主干2级词(参数放行)
    EXPECT_TRUE(IsValidControlWord({"display", "memory", "block", "--pool", "hal"}));

    EXPECT_TRUE(IsValidControlWord({"set", "config"}));
    EXPECT_TRUE(IsValidControlWord({"set", "config", "--analysis", "leak"}));
    EXPECT_TRUE(IsValidControlWord({"set", "config", "--bogus"}));  // 参数不参与白名单校验(由set config解析)
    EXPECT_FALSE(IsValidControlWord({"set", "bogus"}));
    EXPECT_FALSE(IsValidControlWord({"set", "bogus", "--x"}));  // 未知二级词拒绝(参数再多也不行)
}

// ControlEvent: 构造即置eventType=CONTROL,字段默认空
TEST(ControlEventTest, event_type_and_fields)
{
    MemScope::ControlEvent ev;
    EXPECT_EQ(ev.eventType, MemScope::EventBaseType::CONTROL);
    EXPECT_TRUE(ev.cmd.empty());
    EXPECT_TRUE(ev.param.empty());
    EXPECT_TRUE(ev.output.empty());
    // 枚举追加不破坏既有序号: CONTROL紧邻INVALID
    EXPECT_EQ(static_cast<int>(MemScope::EventBaseType::CONTROL) + 1,
              static_cast<int>(MemScope::EventBaseType::INVALID));
}

// GetSubscriberNames: 按首次注册顺序去重(同一名字订阅多种事件类型只出现一次);
// 探针用HAL_ANALYZER id(废弃占位,生产无订阅者,UnSubscribe只删自身)
TEST(EventDispatcherTest, subscriber_names_dedup_and_order)
{
    MemScope::EventDispatcher& dispatcher = MemScope::EventDispatcher::GetInstance();
    auto handler = [](std::shared_ptr<MemScope::EventBase>&, MemScope::MemoryState*) {};
    dispatcher.Subscribe(MemScope::SubscriberId::HAL_ANALYZER, {MemScope::EventBaseType::SYSTEM, MemScope::EventBaseType::CONTROL},
                         MemScope::EventDispatcher::Priority::Low, handler, "ut_probe_a");
    dispatcher.Subscribe(MemScope::SubscriberId::HAL_ANALYZER, {MemScope::EventBaseType::CONTROL},
                         MemScope::EventDispatcher::Priority::Low, handler, "ut_probe_a");
    dispatcher.Subscribe(MemScope::SubscriberId::HAL_ANALYZER, {MemScope::EventBaseType::SYSTEM},
                         MemScope::EventDispatcher::Priority::Low, handler, "ut_probe_b");

    const std::vector<std::string> names = dispatcher.GetSubscriberNames();
    size_t idxA = names.size();
    size_t idxB = names.size();
    size_t appearA = 0;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (names[i] == "ut_probe_a")
        {
            ++appearA;
            if (idxA == names.size())
            {
                idxA = i;
            }
        }
        if (names[i] == "ut_probe_b")
        {
            idxB = i;
        }
    }
    EXPECT_EQ(appearA, 1u) << "duplicate name must be deduplicated";
    EXPECT_LT(idxA, idxB) << "first-registration order preserved";
    EXPECT_NE(idxB, names.size());

    dispatcher.UnSubscribe(MemScope::SubscriberId::HAL_ANALYZER);
}

// 6个分析器GetName: 非空且两两不同(display analyzer枚举数据源)
TEST(EventDispatcherTest, analyzer_getnames_distinct)
{
    std::vector<std::string> names = {MemScope::LeakAnalyzer::GetInstance().GetName(),
                                      MemScope::HealthAnalyzer::GetInstance().GetName(),
                                      MemScope::Dump::GetInstance().GetName(),
                                      MemScope::DecomposeAnalyzer::GetInstance().GetName(),
                                      MemScope::InefficientAnalyzer::GetInstance().GetName(),
                                      MemScope::HostLeakAnalyzer::GetInstance().GetName()};
    std::set<std::string> unique(names.begin(), names.end());
    ASSERT_EQ(names.size(), 6u);
    EXPECT_EQ(unique.size(), 6u) << "GetName must be unique across analyzers";
    for (const auto& n : names)
    {
        EXPECT_FALSE(n.empty());
    }
    // 与"control_channel"不冲突(控制通道订阅者名)
    EXPECT_EQ(unique.count("control_channel"), 0u);
}

// CompletionTable: 一级动词(空行/部分输入/完整动词)
TEST(CompletionTableTest, first_level_verbs)
{
    const std::vector<std::string> all = MemScope::CompletionTable::Complete("");
    ASSERT_EQ(all.size(), 7u);
    EXPECT_EQ(all, std::vector<std::string>({"start", "stop", "step", "display", "set", "exit", "help"}));

    EXPECT_EQ(MemScope::CompletionTable::Complete("s"), std::vector<std::string>({"start", "stop", "step", "set"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("st"), std::vector<std::string>({"start", "stop", "step"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("di"), std::vector<std::string>({"display"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("ex"), std::vector<std::string>({"exit"}));
    EXPECT_TRUE(MemScope::CompletionTable::Complete("exit").empty());  // 完整动词后无补全
    EXPECT_TRUE(MemScope::CompletionTable::Complete("bogus").empty());
    EXPECT_TRUE(MemScope::CompletionTable::Complete("start ").empty());
}

// CompletionTable: display层级(二级词/部分输入/memory/host_leak/analyzer动态)
TEST(CompletionTableTest, display_levels)
{
    const std::vector<std::string> displayWords = {"hook", "analyzer", "config", "memory", "host_leak"};
    EXPECT_EQ(MemScope::CompletionTable::Complete("display "), displayWords);
    EXPECT_EQ(MemScope::CompletionTable::Complete("display m"), std::vector<std::string>({"memory"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("display h"), std::vector<std::string>({"hook", "host_leak"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("display ho"), std::vector<std::string>({"hook", "host_leak"}));
    EXPECT_TRUE(MemScope::CompletionTable::Complete("display hook ").empty());  // hook无下级词
    EXPECT_TRUE(MemScope::CompletionTable::Complete("display config ").empty());

    EXPECT_EQ(MemScope::CompletionTable::Complete("display memory "), std::vector<std::string>({"summary", "block"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("display memory s"), std::vector<std::string>({"summary"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("display memory b"), std::vector<std::string>({"block"}));

    // host_leak仅summary(白名单约束)
    EXPECT_EQ(MemScope::CompletionTable::Complete("display host_leak "), std::vector<std::string>({"summary"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("display host_leak s"), std::vector<std::string>({"summary"}));
}

// CompletionTable: display memory block(--pool/--TOPN/池名部分输入)
TEST(CompletionTableTest, memory_block_options)
{
    const std::vector<std::string> opts = {"--pool", "--TOPN"};
    const std::vector<std::string> pools = {"host", "hal", "pta", "pta_workspace", "atb", "mindspore"};
    EXPECT_EQ(MemScope::CompletionTable::Complete("display memory block "), opts);
    EXPECT_EQ(MemScope::CompletionTable::Complete("display memory block --p"), std::vector<std::string>({"--pool"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("display memory block --T"), std::vector<std::string>({"--TOPN"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("display memory block --pool "), pools);
    EXPECT_EQ(MemScope::CompletionTable::Complete("display memory block --pool h"),
              std::vector<std::string>({"host", "hal"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("display memory block --pool p"),
              std::vector<std::string>({"pta", "pta_workspace"}));
    EXPECT_TRUE(MemScope::CompletionTable::Complete("display memory block --pool hal ").size() <= pools.size());
    EXPECT_TRUE(MemScope::CompletionTable::Complete("display memory block --TOPN 2").empty());
}

// CompletionTable: set config(二级词/配置选项子集)
TEST(CompletionTableTest, set_config_levels)
{
    EXPECT_EQ(MemScope::CompletionTable::Complete("set "), std::vector<std::string>({"config"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("set c"), std::vector<std::string>({"config"}));
    const std::vector<std::string> opts = {"--analysis", "--host-leak-mode", "--block-size-threshold",
                                           "--call-stack"};
    EXPECT_EQ(MemScope::CompletionTable::Complete("set config "), opts);
    EXPECT_EQ(MemScope::CompletionTable::Complete("set config --h"),
              std::vector<std::string>({"--host-leak-mode"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("set config --b"),
              std::vector<std::string>({"--block-size-threshold"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("set config --a"), std::vector<std::string>({"--analysis"}));
    EXPECT_TRUE(MemScope::CompletionTable::Complete("set config --analysis leak").empty());
}

// CompletionTable: 动态analyzer名(懒加载provider;SetDynamicProvider(nullptr)清除)
TEST(CompletionTableTest, dynamic_analyzer_provider)
{
    MemScope::CompletionTable::SetDynamicProvider([]()
                                                  { return std::vector<std::string>{"leak", "health", "host_leak"}; });
    EXPECT_EQ(MemScope::CompletionTable::Complete("display analyzer "),
              std::vector<std::string>({"leak", "health", "host_leak"}));
    EXPECT_EQ(MemScope::CompletionTable::Complete("display analyzer h"),
              std::vector<std::string>({"health", "host_leak"}));
    MemScope::CompletionTable::SetDynamicProvider(nullptr);
    EXPECT_TRUE(MemScope::CompletionTable::Complete("display analyzer ").empty());
}

// LineEditor: 注入式输入序列(非tty路径不经raw mode)。
// 脚本=字符序列+"\t"补全+"\r"提交;write流捕获回显
class LineEditorScriptedTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        script_.clear();
        writes_.clear();
        MemScope::LineEditor::SetIoForTest(
            [this]() -> int
            {
                if (pos_ >= script_.size())
                {
                    return -1;  // 序列耗尽:EOF
                }
                return static_cast<int>(script_[pos_++]);
            },
            [this](const std::string& t) { writes_ += t; });
    }

    void TearDown() override
    {
        MemScope::LineEditor::SetIoForTest([]() { return -1; }, [](const std::string&) {});
    }

    void Type(const std::string& chars) { script_ += chars; }

    std::string script_;
    std::string writes_;
    size_t pos_ = 0;
};

// 唯一候选tab补全: "sta"+tab→"start",回车提交
TEST_F(LineEditorScriptedTest, tab_completes_unique_candidate)
{
    Type("sta\t\r");
    std::string line;
    EXPECT_TRUE(MemScope::LineEditor::ReadLine(line));
    EXPECT_EQ(line, "start");
    // 重绘序列: 回车清行+提示符+补全文本
    EXPECT_NE(writes_.find("\r\x1b[K"), std::string::npos);
    EXPECT_NE(writes_.find("start"), std::string::npos);
}

// 退格与再补全: "sto"+退格→"st","a"+tab→"start"
TEST_F(LineEditorScriptedTest, backspace_then_tab)
{
    Type(std::string("sto") + static_cast<char>(0x7f) + "a\t\r");
    std::string line;
    EXPECT_TRUE(MemScope::LineEditor::ReadLine(line));
    EXPECT_EQ(line, "start");
}

// 多候选tab: 列示候选+保持输入("s"+tab→列出start/stop/step/set,行不变)
TEST_F(LineEditorScriptedTest, multi_candidate_lists_and_keeps_input)
{
    Type("s\t\r");
    std::string line;
    EXPECT_TRUE(MemScope::LineEditor::ReadLine(line));
    EXPECT_EQ(line, "s");
    EXPECT_NE(writes_.find("start  stop  step  set  "), std::string::npos);
}

// 多候选公共前缀扩展: "display m"+tab→"display memory"(memory为唯一候选)
TEST_F(LineEditorScriptedTest, prefix_extend_and_complete)
{
    Type("display m\t\r");
    std::string line;
    EXPECT_TRUE(MemScope::LineEditor::ReadLine(line));
    EXPECT_EQ(line, "display memory");
}

// 提示符态Ctrl-C:中断退出(返回false+interrupted置位,行清空);后续输入不再消费
TEST_F(LineEditorScriptedTest, ctrl_c_interrupts_line)
{
    Type(std::string("ab") + static_cast<char>(0x03) + "cd\r");
    std::string line;
    bool interrupted = false;
    EXPECT_FALSE(MemScope::LineEditor::ReadLine(line, &interrupted));
    EXPECT_TRUE(interrupted);
    EXPECT_TRUE(line.empty());
    EXPECT_NE(writes_.find("^C"), std::string::npos);
}

// 立即EOF(空行+EOF): 返回false(退出交互)
TEST_F(LineEditorScriptedTest, immediate_eof_exits)
{
    std::string line = "dirty";
    EXPECT_FALSE(MemScope::LineEditor::ReadLine(line));
    EXPECT_TRUE(line.empty());
}

// 输入中途EOF: 提交当前行
TEST_F(LineEditorScriptedTest, mid_input_eof_commits)
{
    Type("ab");
    std::string line;
    EXPECT_TRUE(MemScope::LineEditor::ReadLine(line));
    EXPECT_EQ(line, "ab");
}

// Ctrl-D空行=EOF(与EOF同语义)
TEST_F(LineEditorScriptedTest, ctrl_d_on_empty_line_eof)
{
    Type(std::string() + static_cast<char>(0x04));
    std::string line;
    EXPECT_FALSE(MemScope::LineEditor::ReadLine(line));
    EXPECT_TRUE(line.empty());
}

// 行长上限4096: 超长输入截断+蜂鸣
TEST_F(LineEditorScriptedTest, line_length_capped)
{
    Type(std::string(4100, 'a'));
    std::string line;
    EXPECT_TRUE(MemScope::LineEditor::ReadLine(line));
    EXPECT_EQ(line.size(), 4096u);
    EXPECT_NE(writes_.find("\a"), std::string::npos);  // 蜂鸣
}

// 回车提交即返回(line内容保持)
TEST_F(LineEditorScriptedTest, enter_commits)
{
    Type("step\r");
    std::string line;
    EXPECT_TRUE(MemScope::LineEditor::ReadLine(line));
    EXPECT_EQ(line, "step");
}

}  // namespace
