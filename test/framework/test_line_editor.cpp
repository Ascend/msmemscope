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

#include <gtest/gtest.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "line_editor.h"

using namespace MemScope;

namespace
{
constexpr const char* kDefaultPrompt = "msmemscope> ";
// 子进程标记:默认IO(DefaultReadChar/DefaultWriteStr/DefaultReadEscapeChar)只能在新进程内测得,
// 父用例负责fork+exec自身并带上本标记,子进程内专测命中后才执行
constexpr const char* kChildEnv = "MSMEMSCOPE_LINE_EDITOR_CHILD";
constexpr int kChildTimeoutMs = 20000;

bool IsChildProcess()
{
    const char* value = std::getenv(kChildEnv);
    return value != nullptr && std::string(value) == "1";
}

size_t CountOf(const std::string& text, const std::string& token)
{
    if (token.empty())
    {
        return 0;
    }
    size_t count = 0;
    size_t pos = text.find(token);
    while (pos != std::string::npos)
    {
        ++count;
        pos = text.find(token, pos + token.size());
    }
    return count;
}

// 脚本化输入流:按预置序列逐字符返回,耗尽后返回-1(EOF)
class ScriptedInput
{
   public:
    void Feed(std::initializer_list<int> chars)
    {
        chars_.assign(chars.begin(), chars.end());
        pos_ = 0;
    }
    int Read() { return pos_ < chars_.size() ? chars_[pos_++] : -1; }

   private:
    std::vector<int> chars_;
    size_t pos_ = 0;
};

class ScriptedOutput
{
   public:
    void Write(const std::string& text) { out_ += text; }
    const std::string& Str() const { return out_; }
    void Clear() { out_.clear(); }

   private:
    std::string out_;
};

std::string SelfExePath()
{
    char path[4096] = {0};
    const ssize_t len = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0)
    {
        return "";
    }
    return std::string(path, static_cast<size_t>(len));
}

std::string ReadAllFd(int fd)
{
    std::string content;
    char buffer[4096];
    ssize_t len = 0;
    while ((len = ::read(fd, buffer, sizeof(buffer))) > 0)
    {
        content.append(buffer, static_cast<size_t>(len));
    }
    return content;
}

// 等待子进程退出(带超时):超时发SIGKILL回收,避免用例挂死
bool WaitChildExit(pid_t pid, int timeoutMs, int* status)
{
    constexpr int kPollStepMs = 50;
    for (int waited = 0; waited < timeoutMs; waited += kPollStepMs)
    {
        const pid_t ret = ::waitpid(pid, status, WNOHANG);
        if (ret == pid)
        {
            return true;
        }
        ::usleep(kPollStepMs * 1000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, status, 0);
    return false;
}

// fork+exec自身只跑指定子用例(全新进程=默认IO静态缝未被注入)
pid_t SpawnChild(const std::string& filter, int stdinFd, int stdoutFd)
{
    const std::string exePath = SelfExePath();
    const std::string filterArg = "--gtest_filter=" + filter;
    pid_t pid = ::fork();
    if (pid != 0)
    {
        return pid;
    }
    (void)::dup2(stdinFd, STDIN_FILENO);
    (void)::dup2(stdoutFd, STDOUT_FILENO);
    ::setenv(kChildEnv, "1", 1);
    ::execl(exePath.c_str(), exePath.c_str(), filterArg.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
}

// 用例自归一:每个用例注入自己的脚本流/提示符/历史/提供者,结束后恢复全局默认
class LineEditorTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        LineEditor::ClearHistoryLine();
        CompletionTable::SetDynamicProvider(nullptr);
        LineEditor::SetPrompt(kDefaultPrompt);
        LineEditor::SetQuitFlagProvider(nullptr);
        LineEditor::SetPeerAliveProvider(nullptr);

        input_ = std::make_shared<ScriptedInput>();
        output_ = std::make_shared<ScriptedOutput>();
        escape_ = std::make_shared<ScriptedInput>();

        auto input = input_;
        auto output = output_;
        auto escape = escape_;
        LineEditor::SetIoForTest([input]() { return input->Read(); },
                                 [output](const std::string& text) { output->Write(text); });
        LineEditor::SetEscapeReaderForTest([escape]() { return escape->Read(); });
    }

    void TearDown() override
    {
        LineEditor::ClearHistoryLine();
        LineEditor::SetPrompt(kDefaultPrompt);
        LineEditor::SetQuitFlagProvider(nullptr);
        LineEditor::SetPeerAliveProvider(nullptr);
        CompletionTable::SetDynamicProvider(nullptr);
        // ESC续读缝支持恢复默认;读/写缝无默认恢复接口,后续用例均会重新注入
        LineEditor::SetEscapeReaderForTest(nullptr);
    }

    bool ReadScriptedLine(std::string& line, bool* interrupted = nullptr)
    {
        return LineEditor::ReadLine(line, interrupted);
    }

    std::shared_ptr<ScriptedInput> input_;
    std::shared_ptr<ScriptedOutput> output_;
    std::shared_ptr<ScriptedInput> escape_;
};

// 补全表叶子层级:无下级词时返回空(display hook/config、set config后多了token、未知动词多token)
TEST_F(LineEditorTest, completion_leaf_contexts_return_empty)
{
    EXPECT_TRUE(CompletionTable::Complete("display hook x").empty());
    EXPECT_TRUE(CompletionTable::Complete("set bogus x").empty());
    EXPECT_TRUE(CompletionTable::Complete("bogus x").empty());

    // 对照:非叶子层级仍有候选,证明上面的空结果来自叶子分支而非输入不匹配
    EXPECT_EQ(CompletionTable::Complete("display h"), (std::vector<std::string>{"hook", "host_leak"}));
    EXPECT_EQ(CompletionTable::Complete("se"), (std::vector<std::string>{"set"}));
}

// SetPrompt:空串被忽略(保留上一提示符),非空串生效并在后续读取时输出
TEST_F(LineEditorTest, set_prompt_ignores_empty_and_applies_non_empty)
{
    LineEditor::SetPrompt("");
    input_->Feed({'a', '\n'});
    std::string line;
    ASSERT_TRUE(ReadScriptedLine(line));
    EXPECT_EQ(line, "a");
    EXPECT_NE(output_->Str().find(kDefaultPrompt), std::string::npos);

    LineEditor::SetPrompt("msmemscope[42]> ");
    output_->Clear();
    input_->Feed({'b', '\n'});
    ASSERT_TRUE(ReadScriptedLine(line));
    EXPECT_EQ(line, "b");
    EXPECT_NE(output_->Str().find("msmemscope[42]> "), std::string::npos);
}

// 补全无候选:蜂鸣且输入保持原样
TEST_F(LineEditorTest, complete_without_candidates_beeps)
{
    input_->Feed({'b', 'o', 'g', 'u', 's', '\t', '\n'});
    std::string line;
    ASSERT_TRUE(ReadScriptedLine(line));
    EXPECT_EQ(line, "bogus");
    EXPECT_EQ(CountOf(output_->Str(), "\a"), 1u);
}

// 多候选公共前缀长于当前输入:扩展公共前缀("display h"→"display ho")
TEST_F(LineEditorTest, complete_extends_common_prefix)
{
    input_->Feed({'d', 'i', 's', 'p', 'l', 'a', 'y', ' ', 'h', '\t', '\n'});
    std::string line;
    ASSERT_TRUE(ReadScriptedLine(line));
    EXPECT_EQ(line, "display ho");
    EXPECT_NE(output_->Str().find("display ho"), std::string::npos);
}

// 对端断联码(-2):重绘当前行后继续等待输入,已输入内容保留
TEST_F(LineEditorTest, peer_gone_redraws_and_continues)
{
    input_->Feed({-2, 'a', '\n'});
    std::string line;
    ASSERT_TRUE(ReadScriptedLine(line));
    EXPECT_EQ(line, "a");
    // 初始提示符写一次,断联重绘一次,字符插入重绘一次
    EXPECT_GE(CountOf(output_->Str(), "\r\x1b[K"), 2u);
}

// 编辑态按上/下键:下键无历史可切→蜂鸣
TEST_F(LineEditorTest, down_arrow_in_edit_state_beeps)
{
    input_->Feed({0x1b, '\n'});
    escape_->Feed({'[', 'B'});
    std::string line;
    ASSERT_TRUE(ReadScriptedLine(line));
    EXPECT_TRUE(line.empty());
    EXPECT_NE(output_->Str().find("\a"), std::string::npos);
}

// Ctrl-D:行非空时忽略(不提交不退出),后续字符继续编辑
TEST_F(LineEditorTest, ctrl_d_with_non_empty_line_is_ignored)
{
    input_->Feed({'a', 0x04, 'b', '\n'});
    std::string line;
    ASSERT_TRUE(ReadScriptedLine(line));
    EXPECT_EQ(line, "ab");
}

// 其他控制字符(<0x20与>0x7e)一律忽略,不进入输入
TEST_F(LineEditorTest, other_control_chars_are_ignored)
{
    input_->Feed({0x01, 0x80, 'x', '\n'});
    std::string line;
    ASSERT_TRUE(ReadScriptedLine(line));
    EXPECT_EQ(line, "x");
}

}  // namespace

// ==================== 默认IO子进程用例 ====================
// 默认读写/ESC续读/空闲tick都是函数内静态缝,一旦被注入无法在本进程恢复默认,
// 故经fork+exec新进程验证;子用例仅在父用例置位环境标记时执行,单独跑整套时跳过。

namespace
{
TEST(LineEditorDefaultIoChild, read_stdin_file_submits_on_eof)
{
    if (!IsChildProcess())
    {
        GTEST_SKIP() << "only driven by LineEditorDefaultIo launcher";
    }
    // stdin为普通文件:读到EOF时行非空→提交当前行
    std::string line;
    bool interrupted = true;
    const bool ok = LineEditor::ReadLine(line, &interrupted);
    EXPECT_TRUE(ok);
    EXPECT_EQ(line, "start");
    EXPECT_FALSE(interrupted);
}

TEST(LineEditorDefaultIoChild, escape_eof_aborts_sequence)
{
    if (!IsChildProcess())
    {
        GTEST_SKIP() << "only driven by LineEditorDefaultIo launcher";
    }
    // ESC '[' 后EOF:续读返回-1,序列整体丢弃;随后读到EOF且行为空,按EOF退出
    std::string line;
    bool interrupted = true;
    const bool ok = LineEditor::ReadLine(line, &interrupted);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(line.empty());
    EXPECT_FALSE(interrupted);
}

TEST(LineEditorDefaultIoChild, peer_gone_tick_redraws_and_continues)
{
    if (!IsChildProcess())
    {
        GTEST_SKIP() << "only driven by LineEditorDefaultIo launcher";
    }
    auto calls = std::make_shared<int>(0);
    LineEditor::SetPeerAliveProvider([calls]() { return ++(*calls) > 1; });

    // READY标记:父进程据此确认子进程已进入ReadLine,再等待空闲tick后投递输入
    const char ready[] = "READY\n";
    (void)::write(STDOUT_FILENO, ready, sizeof(ready) - 1);

    std::string line;
    bool interrupted = false;
    // 首个空闲tick探针返回断联→重绘继续;父进程随后写入回车结束本行
    const bool ok = LineEditor::ReadLine(line, &interrupted);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(line.empty());
    EXPECT_GE(*calls, 1);
}

TEST(LineEditorDefaultIoChild, poll_eintr_retries_then_quit)
{
    if (!IsChildProcess())
    {
        GTEST_SKIP() << "only driven by LineEditorDefaultIo launcher";
    }
    // 无数据的管道使poll阻塞;周期SIGALRM(不带SA_RESTART)打断poll
    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipeFds), 0);
    ASSERT_GE(::dup2(pipeFds[0], STDIN_FILENO), 0);

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // 关键:无SA_RESTART, poll被信号打断返回EINTR
    ASSERT_EQ(::sigaction(SIGALRM, &action, nullptr), 0);

    struct itimerval timer;
    std::memset(&timer, 0, sizeof(timer));
    timer.it_interval.tv_usec = 20 * 1000;
    timer.it_value.tv_usec = 20 * 1000;
    ASSERT_EQ(::setitimer(ITIMER_REAL, &timer, nullptr), 0);

    auto quitCalls = std::make_shared<int>(0);
    LineEditor::SetQuitFlagProvider([quitCalls]() { return ++(*quitCalls) >= 3; });

    std::string line;
    bool interrupted = true;
    // 前两次EINTR按非退出信号重试,第三次退出信号置位→按EOF收尾
    const bool ok = LineEditor::ReadLine(line, &interrupted);

    struct itimerval off;
    std::memset(&off, 0, sizeof(off));
    ASSERT_EQ(::setitimer(ITIMER_REAL, &off, nullptr), 0);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(interrupted);
    EXPECT_TRUE(line.empty());
    EXPECT_GE(*quitCalls, 3);
}
}  // namespace

namespace
{
class LineEditorDefaultIoLauncher : public ::testing::Test
{
   protected:
    void TearDown() override
    {
        // 断言失败会提前返回，跳过后续的投递/回收步骤：此处兜底强杀，避免孤儿
        // 子进程继承的stderr拖住CI管道（正常路径下WaitAndCollect已回收并置-1）
        if (child_ > 0)
        {
            int status = 0;
            ::kill(child_, SIGKILL);
            ::waitpid(child_, &status, 0);
            child_ = -1;
        }
        if (stdinFile_ != nullptr)
        {
            std::fclose(stdinFile_);
            stdinFile_ = nullptr;
        }
        if (!stdinPath_.empty())
        {
            ::unlink(stdinPath_.c_str());
            stdinPath_.clear();
        }
        if (stdoutFd_ >= 0)
        {
            ::close(stdoutFd_);
            stdoutFd_ = -1;
        }
        if (!stdoutPath_.empty())
        {
            ::unlink(stdoutPath_.c_str());
            stdoutPath_.clear();
        }
        if (pipeFds_[0] >= 0)
        {
            ::close(pipeFds_[0]);
            pipeFds_[0] = -1;
        }
        if (pipeFds_[1] >= 0)
        {
            ::close(pipeFds_[1]);
            pipeFds_[1] = -1;
        }
    }

    // 创建临时文件并返回可读fd(内容由调用方写入)
    int MakeTempFile(std::string& path)
    {
        char tmpl[] = "/tmp/msms_line_editor_XXXXXX";
        int fd = ::mkstemp(tmpl);
        if (fd >= 0)
        {
            path.assign(tmpl);
        }
        return fd;
    }

    // 先等子进程退出再收集其stdout,避免读到半截输出
    std::string WaitAndCollect(pid_t pid, bool* finished, int* status)
    {
        *finished = WaitChildExit(pid, kChildTimeoutMs, status);
        child_ = -1;  // WaitChildExit两条路径(正常退出/超时强杀)均已完成回收
        std::string output;
        if (stdoutFd_ >= 0)
        {
            (void)::lseek(stdoutFd_, 0, SEEK_SET);
            output = ReadAllFd(stdoutFd_);
        }
        return output;
    }

    void AssertChildPassed(bool finished, int status, const std::string& output)
    {
        ASSERT_TRUE(finished) << "child timeout, output:\n" << output;
        ASSERT_TRUE(WIFEXITED(status)) << "child exited abnormally, output:\n" << output;
        EXPECT_EQ(WEXITSTATUS(status), 0) << output;
        EXPECT_NE(output.find("[  PASSED  ] 1 test"), std::string::npos) << output;
    }

    std::FILE* stdinFile_ = nullptr;
    std::string stdinPath_;
    int stdoutFd_ = -1;
    std::string stdoutPath_;
    int pipeFds_[2] = {-1, -1};
    // 已spawn但尚未回收的子进程：断言失败提前返回时由TearDown强杀回收
    pid_t child_ = -1;
};

// 默认读:普通文件输入"start"(无换行),EOF时提交;默认写:stdout输出提示符
TEST_F(LineEditorDefaultIoLauncher, default_read_and_write)
{
    if (IsChildProcess())
    {
        GTEST_SKIP();
    }
    int stdinFd = MakeTempFile(stdinPath_);
    ASSERT_GE(stdinFd, 0);
    stdinFile_ = ::fdopen(stdinFd, "r");
    ASSERT_NE(stdinFile_, nullptr);
    ASSERT_EQ(::write(::fileno(stdinFile_), "start", 5), 5);
    ASSERT_EQ(::lseek(::fileno(stdinFile_), 0, SEEK_SET), 0);

    stdoutFd_ = MakeTempFile(stdoutPath_);
    ASSERT_GE(stdoutFd_, 0);

    const pid_t pid = SpawnChild("LineEditorDefaultIoChild.read_stdin_file_submits_on_eof",
                                 ::fileno(stdinFile_), stdoutFd_);
    ASSERT_GT(pid, 0);
    child_ = pid;  // 记录子进程：后续断言失败提前返回时由TearDown强杀
    bool finished = false;
    int status = 0;
    const std::string output = WaitAndCollect(pid, &finished, &status);
    AssertChildPassed(finished, status, output);
    EXPECT_NE(output.find(kDefaultPrompt), std::string::npos) << output;
}

// 默认ESC续读:输入ESC'['后EOF,续读返回-1,序列中止;主读EOF空行退出
TEST_F(LineEditorDefaultIoLauncher, default_escape_reader_eof)
{
    if (IsChildProcess())
    {
        GTEST_SKIP();
    }
    int stdinFd = MakeTempFile(stdinPath_);
    ASSERT_GE(stdinFd, 0);
    stdinFile_ = ::fdopen(stdinFd, "r");
    ASSERT_NE(stdinFile_, nullptr);
    const char escapeSeq[] = {0x1b, '['};
    ASSERT_EQ(::write(::fileno(stdinFile_), escapeSeq, sizeof(escapeSeq)), static_cast<ssize_t>(sizeof(escapeSeq)));
    ASSERT_EQ(::lseek(::fileno(stdinFile_), 0, SEEK_SET), 0);

    stdoutFd_ = MakeTempFile(stdoutPath_);
    ASSERT_GE(stdoutFd_, 0);

    const pid_t pid = SpawnChild("LineEditorDefaultIoChild.escape_eof_aborts_sequence",
                                 ::fileno(stdinFile_), stdoutFd_);
    ASSERT_GT(pid, 0);
    child_ = pid;  // 记录子进程：后续断言失败提前返回时由TearDown强杀
    bool finished = false;
    int status = 0;
    const std::string output = WaitAndCollect(pid, &finished, &status);
    AssertChildPassed(finished, status, output);
    EXPECT_NE(output.find(kDefaultPrompt), std::string::npos) << output;
}

// 默认读+对端探针:无数据管道触发空闲tick,探针返回断联→重绘继续;父进程随后写入回车
TEST_F(LineEditorDefaultIoLauncher, default_peer_gone_tick)
{
    if (IsChildProcess())
    {
        GTEST_SKIP();
    }
    ASSERT_EQ(::pipe(pipeFds_), 0);
    stdoutFd_ = MakeTempFile(stdoutPath_);
    ASSERT_GE(stdoutFd_, 0);

    const pid_t pid = SpawnChild("LineEditorDefaultIoChild.peer_gone_tick_redraws_and_continues",
                                 pipeFds_[0], stdoutFd_);
    ASSERT_GT(pid, 0);
    child_ = pid;  // 记录子进程：后续断言失败提前返回时由TearDown强杀
    ::close(pipeFds_[0]);
    pipeFds_[0] = -1;

    // 先等子进程写出READY(已进入ReadLine),再等它写出断联重绘标记"\r\x1b[K"
    // (首个空闲tick触发探针后由ReadLine重绘),随后才投递回车。
    // 不能按固定时长等待:子进程若被调度延迟,回车会先于空闲tick到达,
    // 探针不被调用即结束本行,子用例的EXPECT_GE(*calls,1)随之失败
    bool ready = false;
    for (int waited = 0; waited < 5000 && !ready; waited += 20)
    {
        ::usleep(20 * 1000);
        (void)::lseek(stdoutFd_, 0, SEEK_SET);
        ready = ReadAllFd(stdoutFd_).find("READY") != std::string::npos;
    }
    ASSERT_TRUE(ready);
    bool redrawn = false;
    for (int waited = 0; waited < 10000 && !redrawn; waited += 20)
    {
        ::usleep(20 * 1000);
        (void)::lseek(stdoutFd_, 0, SEEK_SET);
        redrawn = ReadAllFd(stdoutFd_).find("\r\x1b[K") != std::string::npos;
    }
    ASSERT_TRUE(redrawn);
    ASSERT_EQ(::write(pipeFds_[1], "\n", 1), 1);
    ::close(pipeFds_[1]);
    pipeFds_[1] = -1;

    bool finished = false;
    int status = 0;
    const std::string output = WaitAndCollect(pid, &finished, &status);
    AssertChildPassed(finished, status, output);
    EXPECT_NE(output.find("\r\x1b[K"), std::string::npos) << output;
}

// 默认读+退出信号:周期SIGALRM打断poll,EINTR非退出信号重试,置位后按EOF收尾
TEST_F(LineEditorDefaultIoLauncher, default_poll_eintr)
{
    if (IsChildProcess())
    {
        GTEST_SKIP();
    }
    ASSERT_EQ(::pipe(pipeFds_), 0);
    stdoutFd_ = MakeTempFile(stdoutPath_);
    ASSERT_GE(stdoutFd_, 0);

    const pid_t pid = SpawnChild("LineEditorDefaultIoChild.poll_eintr_retries_then_quit",
                                 pipeFds_[0], stdoutFd_);
    ASSERT_GT(pid, 0);
    child_ = pid;  // 记录子进程：后续断言失败提前返回时由TearDown强杀
    bool finished = false;
    int status = 0;
    const std::string output = WaitAndCollect(pid, &finished, &status);
    AssertChildPassed(finished, status, output);
}
}  // namespace
