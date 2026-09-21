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

#include "line_editor.h"

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <deque>

namespace MemScope
{

namespace
{
constexpr size_t kMaxLineLen = 4096;
std::string g_prompt = "msmemscope> ";  // 会话提示符(AttachController按pid注入)
constexpr int kIdlePollMs = 200;        // 空闲tick:对端存活探针轮询周期
constexpr int kPeerGoneCode = -2;       // 对端断联(区别于EOF/错误-1)
constexpr size_t kMaxHistory = 3;       // 历史上限(只保留下发成功命令,最新在前)
constexpr int kEscapeTimeoutMs = 100;   // ESC序列续读超时(裸ESC判定,不阻塞等待)
std::deque<std::string> g_history;      // 会话级历史(AddHistory写入,上下键浏览)

std::vector<std::string> g_dynamicAnalyzers;  // 懒加载缓存(AttachController注入provider时填充)

const char* const kVerbs[] = {"start", "stop", "step", "display", "set", "exit", "help"};
const char* const kDisplayWords[] = {"hook", "analyzer", "config", "memory", "host_leak"};
const char* const kMemoryWords[] = {"summary", "block"};
const char* const kHostLeakWords[] = {"summary"};  // host_leak仅summary(白名单约束)
const char* const kBlockWords[] = {"--pool", "--TOPN"};
const char* const kPoolNames[] = {"host", "hal", "pta", "pta_workspace", "atb", "mindspore"};
const char* const kSetWords[] = {"config"};
const char* const kSetConfigWords[] = {"--analysis", "--host-leak-mode", "--block-size-threshold", "--call-stack"};

std::vector<std::string> Words(const char* const* words, size_t count)
{
    std::vector<std::string> result;
    for (size_t i = 0; i < count; ++i)
    {
        result.emplace_back(words[i]);
    }
    return result;
}
}  // namespace

std::vector<std::string> CompletionTable::Filter(const std::vector<std::string>& words, const std::string& prefix)
{
    std::vector<std::string> result;
    for (const std::string& w : words)
    {
        if (w.compare(0, prefix.size(), prefix) == 0)
        {
            result.push_back(w);
        }
    }
    return result;
}

void CompletionTable::SetDynamicProvider(Provider provider)
{
    if (provider)
    {
        g_dynamicAnalyzers = provider();
    }
    else
    {
        g_dynamicAnalyzers.clear();
    }
}

std::vector<std::string> CompletionTable::Complete(const std::string& line)
{
    // 分词(忽略空token):末尾未完成token=补全前缀,其余=上下文层级
    size_t lastSpace = line.find_last_of(' ');
    const std::string prefix = lastSpace == std::string::npos ? line : line.substr(lastSpace + 1);
    std::vector<std::string> tokens;
    {
        size_t pos = 0;
        while (pos <= line.size())
        {
            const size_t end = line.find(' ', pos);
            const size_t len = (end == std::string::npos ? line.size() : end) - pos;
            if (len > 0)
            {
                tokens.push_back(line.substr(pos, len));
            }
            if (end == std::string::npos)
            {
                break;
            }
            pos = end + 1;
        }
    }
    if (tokens.empty())
    {
        return Filter(Words(kVerbs, sizeof(kVerbs) / sizeof(kVerbs[0])), prefix);
    }
    const std::string& head = tokens[0];
    if (head == "display")
    {
        if (tokens.size() == 1)
        {
            return Filter(Words(kDisplayWords, sizeof(kDisplayWords) / sizeof(kDisplayWords[0])), prefix);
        }
        if (tokens[1] == "memory")
        {
            if (tokens.size() == 2)
            {
                return Filter(Words(kMemoryWords, sizeof(kMemoryWords) / sizeof(kMemoryWords[0])), prefix);
            }
            if (tokens.size() >= 3 && tokens[2] == "block")
            {
                if (tokens.size() == 3)
                {
                    return Filter(Words(kBlockWords, sizeof(kBlockWords) / sizeof(kBlockWords[0])), prefix);
                }
                if (tokens[3] == "--pool")
                {
                    // 末token=池名(可能部分):完整池名→无下级词;部分输入→按前缀过滤
                    if (tokens.size() >= 5)
                    {
                        for (size_t i = 0; i < sizeof(kPoolNames) / sizeof(kPoolNames[0]); ++i)
                        {
                            if (tokens[4] == kPoolNames[i])
                            {
                                return {};
                            }
                        }
                    }
                    return Filter(Words(kPoolNames, sizeof(kPoolNames) / sizeof(kPoolNames[0])), prefix);
                }
                // "--p"/"--T"等部分输入
                return Filter(Words(kBlockWords, sizeof(kBlockWords) / sizeof(kBlockWords[0])), prefix);
            }
            // "summary"部分/完整输入
            return Filter(Words(kMemoryWords, sizeof(kMemoryWords) / sizeof(kMemoryWords[0])), prefix);
        }
        if (tokens[1] == "host_leak")
        {
            return Filter(Words(kHostLeakWords, sizeof(kHostLeakWords) / sizeof(kHostLeakWords[0])), prefix);
        }
        if (tokens[1] == "analyzer")
        {
            return Filter(g_dynamicAnalyzers, prefix);
        }
        // 二级词部分输入("display m"/"display h"):按前缀过滤;完整二级词后无下级词→空
        if (tokens.size() == 2)
        {
            for (size_t i = 0; i < sizeof(kDisplayWords) / sizeof(kDisplayWords[0]); ++i)
            {
                if (tokens[1] == kDisplayWords[i])
                {
                    return {};
                }
            }
            return Filter(Words(kDisplayWords, sizeof(kDisplayWords) / sizeof(kDisplayWords[0])), prefix);
        }
        return {};  // hook/config 无下级词
    }
    if (head == "set")
    {
        if (tokens.size() == 1)
        {
            return Filter(Words(kSetWords, sizeof(kSetWords) / sizeof(kSetWords[0])), prefix);
        }
        if (tokens[1] == "config")
        {
            return Filter(Words(kSetConfigWords, sizeof(kSetConfigWords) / sizeof(kSetConfigWords[0])), prefix);
        }
        // 二级词部分输入("set c"):按前缀过滤
        if (tokens.size() == 2)
        {
            return Filter(Words(kSetWords, sizeof(kSetWords) / sizeof(kSetWords[0])), prefix);
        }
        return {};
    }
    // 一级动词部分输入("s"/"st"/"di"):按前缀过滤动词表;完整动词后无补全
    if (tokens.size() == 1)
    {
        for (size_t i = 0; i < sizeof(kVerbs) / sizeof(kVerbs[0]); ++i)
        {
            if (head == kVerbs[i])
            {
                return {};
            }
        }
        return Filter(Words(kVerbs, sizeof(kVerbs) / sizeof(kVerbs[0])), prefix);
    }
    return {};
}

namespace
{
// 退出信号置位检测(EINTR重试止步条件):AttachController安装SIGINT/SIGTERM
// handler后注入;未注入(测试/非控制端)默认恒false——EINTR一律重试
std::function<bool()>& GetQuitFlagFn()
{
    static std::function<bool()> fn = []() { return false; };
    return fn;
}

// 对端存活探针(空闲tick调用):AttachController注入;未注入(测试/非控制端)
// 默认恒true——空闲tick无操作
std::function<bool()>& GetPeerAliveFn()
{
    static std::function<bool()> fn = []() { return true; };
    return fn;
}

// 默认IO:stdin轮询+单字符读/stdout写(测试注入替换)。
// 每tick(200ms)轮询stdin并检查对端存活探针:探针返回false(对端断联)→
// 返回-2交ReadLine重绘当前行(提示由探针打印);EINTR语义与原有阻塞读一致
// (退出信号置位→按EOF收尾,其他信号→重试)
int DefaultReadChar()
{
    char c = 0;
    for (;;)
    {
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, kIdlePollMs);
        if (pr > 0 && (pfd.revents & POLLIN) != 0)
        {
            const ssize_t n = ::read(STDIN_FILENO, &c, 1);
            if (n == 1)
            {
                return static_cast<unsigned char>(c);
            }
            // 读EOF(0)/错误:按原语义收尾(EINTR且非退出信号→重试)
            if (n < 0 && errno == EINTR && !GetQuitFlagFn()())
            {
                continue;
            }
            return -1;
        }
        if (pr == 0)
        {
            // 空闲tick:对端断联→-2(ReadLine重绘当前行后继续),否则继续等输入
            if (!GetPeerAliveFn()())
            {
                return kPeerGoneCode;
            }
            continue;
        }
        // poll错误: EINTR且非退出信号→重试;其他→EOF语义
        if (pr < 0 && errno == EINTR && !GetQuitFlagFn()())
        {
            continue;
        }
        return -1;
    }
}

void DefaultWriteStr(const std::string& text) { (void)::write(STDOUT_FILENO, text.data(), text.size()); }

// ESC序列续读(短超时kEscapeTimeoutMs):返回字符;-1=超时/EOF/错误(序列中止,
// 整体丢弃)。独立于DefaultReadChar:后者空闲tick无限等待,裸ESC(无后续字节)
// 走它会把输入挂住。
int DefaultReadEscapeChar()
{
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, kEscapeTimeoutMs);
    if (pr > 0 && (pfd.revents & POLLIN) != 0)
    {
        char c = 0;
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n == 1)
        {
            return static_cast<unsigned char>(c);
        }
    }
    return -1;
}

// 输入流/输出流(测试缝;未注入时指向默认实现)
std::function<int()>& GetReadCharFn()
{
    static std::function<int()> fn = &DefaultReadChar;
    return fn;
}

std::function<void(const std::string&)>& GetWriteStrFn()
{
    static std::function<void(const std::string&)> fn = &DefaultWriteStr;
    return fn;
}

// ESC续读流(测试缝;未注入时指向默认实现)
std::function<int()>& GetEscapeReadFn()
{
    static std::function<int()> fn = &DefaultReadEscapeChar;
    return fn;
}

struct TermiosGuard
{
    bool active = false;  // raw模式已生效:析构需恢复原termios
    struct termios orig;

    ~TermiosGuard()
    {
        if (active)
        {
            (void)::tcsetattr(STDIN_FILENO, TCSANOW, &orig);
        }
    }
};
}  // namespace

void LineEditor::SetPrompt(const std::string& prompt)
{
    if (!prompt.empty())
    {
        g_prompt = prompt;
    }
}

void LineEditor::SetIoForTest(ReadCharFn readFn, WriteStrFn writeFn)
{
    if (readFn)
    {
        GetReadCharFn() = std::move(readFn);
    }
    if (writeFn)
    {
        GetWriteStrFn() = std::move(writeFn);
    }
}

void LineEditor::SetEscapeReaderForTest(ReadEscapeFn fn)
{
    if (fn)
    {
        GetEscapeReadFn() = std::move(fn);
    }
    else
    {
        GetEscapeReadFn() = &DefaultReadEscapeChar;
    }
}

void LineEditor::ClearHistoryLine() { g_history.clear(); }

void LineEditor::AddHistory(const std::string& cmd)
{
    if (cmd.empty())
    {
        return;
    }
    // 全表去重(命中先删除,保证只留一份);插入队首;超上限淘汰最旧
    for (auto it = g_history.begin(); it != g_history.end(); ++it)
    {
        if (*it == cmd)
        {
            g_history.erase(it);
            break;
        }
    }
    g_history.push_front(cmd);
    if (g_history.size() > kMaxHistory)
    {
        g_history.pop_back();
    }
}

void LineEditor::SetQuitFlagProvider(QuitFlagFn provider)
{
    if (provider)
    {
        GetQuitFlagFn() = std::move(provider);
    }
    else
    {
        GetQuitFlagFn() = []() { return false; };
    }
}

void LineEditor::SetPeerAliveProvider(PeerAliveFn provider)
{
    if (provider)
    {
        GetPeerAliveFn() = std::move(provider);
    }
    else
    {
        GetPeerAliveFn() = []() { return true; };
    }
}

void LineEditor::RedrawLine(const std::string& line, size_t cursor, const WriteStrFn& write)
{
    // 整行重绘:回退行首+清行+重写提示符与输入,再按绝对列定位光标
    // (提示符与输入均为ASCII,显示列=字符数;列号=promptLen+cursor+1,1基)
    write("\r\x1b[K");
    write(g_prompt);
    write(line);
    write("\x1b[" + std::to_string(g_prompt.size() + cursor + 1) + "G");
}

bool LineEditor::TryComplete(std::string& line, size_t& cursor, const WriteStrFn& write)
{
    const std::vector<std::string> candidates = CompletionTable::Complete(line);
    if (candidates.empty())
    {
        write("\a");  // 无可补全:蜂鸣
        return false;
    }
    const size_t lastSpace = line.find_last_of(' ');
    const std::string prefix = lastSpace == std::string::npos ? line : line.substr(lastSpace + 1);

    if (candidates.size() == 1)
    {
        // 唯一候选:替换最后一个token
        line = lastSpace == std::string::npos ? candidates[0] : line.substr(0, lastSpace + 1) + candidates[0];
        cursor = line.size();  // 补全后光标置行尾
        RedrawLine(line, cursor, write);
        return true;
    }
    // 多候选:计算公共前缀,比当前输入长则扩展,否则列示候选
    std::string common = candidates[0];
    for (size_t i = 1; i < candidates.size(); ++i)
    {
        size_t j = 0;
        while (j < common.size() && j < candidates[i].size() && common[j] == candidates[i][j])
        {
            ++j;
        }
        common.resize(j);
    }
    if (common.size() > prefix.size())
    {
        line = lastSpace == std::string::npos ? common : line.substr(0, lastSpace + 1) + common;
        cursor = line.size();
        RedrawLine(line, cursor, write);
        return true;
    }
    // 列示候选(每行一个)+ 重绘当前输入(无文本变化,光标保持)
    write("\r\n");
    for (const std::string& c : candidates)
    {
        write(c + "  ");
    }
    write("\r\n");
    RedrawLine(line, cursor, write);
    return false;
}

bool LineEditor::ReadLine(std::string& line, bool* interrupted)
{
    if (interrupted != nullptr)
    {
        *interrupted = false;
    }
    std::function<int()>& readChar = GetReadCharFn();
    std::function<void(const std::string&)>& write = GetWriteStrFn();
    std::function<int()>& escapeRead = GetEscapeReadFn();

    // 仅tty路径进入raw mode;测试/非tty由调用方绕过
    TermiosGuard guard;
    bool isTty = ::isatty(STDIN_FILENO) != 0;
    if (isTty)
    {
        struct termios raw;
        if (::tcgetattr(STDIN_FILENO, &guard.orig) == 0)
        {
            raw = guard.orig;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
            {
                guard.active = true;  // 析构时恢复
            }
        }
    }

    line.clear();
    // 行内光标(字符偏移0..len)与历史浏览状态(每次调用从编辑态开始:
    // 进入历史前保存草稿,下键越过最新一条恢复草稿)
    size_t cursor = 0;
    std::string draft;
    size_t histIndex = 0;
    bool inHistory = false;
    write(g_prompt);
    for (;;)
    {
        const int c = readChar();
        if (c == kPeerGoneCode)
        {
            // 对端断联提示已由探针打印:重绘当前行恢复输入编辑,继续等待
            RedrawLine(line, cursor, write);
            continue;
        }
        if (c < 0)
        {
            if (line.empty())
            {
                write("\r\n");
                return false;  // 立即EOF:退出交互
            }
            write("\r\n");
            return true;  // 输入中途EOF:提交当前行
        }
        if (c == '\r' || c == '\n')
        {
            write("\r\n");
            return true;
        }
        if (c == 0x7f || c == 0x08)  // 退格:删除光标前一字符
        {
            if (cursor > 0)
            {
                line.erase(cursor - 1, 1);
                --cursor;
                RedrawLine(line, cursor, write);
            }
            continue;
        }
        if (c == '\t')  // 补全
        {
            TryComplete(line, cursor, write);
            continue;
        }
        if (c == 0x1b)  // ESC:方向键序列(CSI/SS3)或裸ESC(无绑定,丢弃)
        {
            const int b2 = escapeRead();  // 续读(短超时,超时/EOF=-1)
            if (b2 == '[' || b2 == 'O')
            {
                const int b3 = escapeRead();
                if (b3 == 'A' || b3 == 'B' || b3 == 'C' || b3 == 'D')
                {
                    // 上/下=历史浏览;左/右=光标移动
                    if (b3 == 'A')  // 上:进入历史取最新,或向更旧移动(到底停)
                    {
                        if (g_history.empty())
                        {
                            write("\a");
                        }
                        else if (!inHistory)
                        {
                            draft = line;
                            histIndex = 0;
                            inHistory = true;
                            line = g_history[0];
                            cursor = line.size();
                            RedrawLine(line, cursor, write);
                        }
                        else if (histIndex + 1 < g_history.size())
                        {
                            ++histIndex;
                            line = g_history[histIndex];
                            cursor = line.size();
                            RedrawLine(line, cursor, write);
                        }
                        else
                        {
                            write("\a");  // 已到最旧:蜂鸣,不循环
                        }
                    }
                    else if (b3 == 'B')  // 下:向新移动,越过最新一条恢复草稿
                    {
                        if (!inHistory)
                        {
                            write("\a");  // 编辑态无下可切
                        }
                        else if (histIndex > 0)
                        {
                            --histIndex;  // 向新移动(0=最新)
                            line = g_history[histIndex];
                            cursor = line.size();
                            RedrawLine(line, cursor, write);
                        }
                        else
                        {
                            inHistory = false;
                            line = draft;
                            cursor = line.size();
                            RedrawLine(line, cursor, write);
                        }
                    }
                    else if (b3 == 'C')  // 右:光标右移(越界clamp)
                    {
                        if (cursor < line.size())
                        {
                            ++cursor;
                            RedrawLine(line, cursor, write);
                        }
                    }
                    else  // 'D' 左:光标左移(越界clamp)
                    {
                        if (cursor > 0)
                        {
                            --cursor;
                            RedrawLine(line, cursor, write);
                        }
                    }
                    continue;
                }
                // 其他CSI/SS3序列(Delete/Home等):消费到终结字节(0x40~0x7e)后整体丢弃
                int bc = b3;
                while (bc >= 0 && (bc < 0x40 || bc > 0x7e))
                {
                    bc = escapeRead();
                }
            }
            continue;  // 裸ESC/未知序列:整体丢弃
        }
        if (c == 0x03)  // 提示符态Ctrl-C:中断退出(exit清理路径,退出130)
        {
            write("^C\r\n");
            line.clear();
            cursor = 0;
            if (interrupted != nullptr)
            {
                *interrupted = true;
            }
            return false;
        }
        if (c == 0x04)  // Ctrl-D:行空=EOF
        {
            if (line.empty())
            {
                write("\r\n");
                return false;
            }
            continue;
        }
        if (c < 0x20 || c > 0x7e)  // 其他控制字符:忽略
        {
            continue;
        }
        if (line.size() >= kMaxLineLen)
        {
            write("\a");
            continue;
        }
        line.insert(cursor, 1, static_cast<char>(c));  // 光标处插入
        ++cursor;
        RedrawLine(line, cursor, write);
    }
}

}  // namespace MemScope
