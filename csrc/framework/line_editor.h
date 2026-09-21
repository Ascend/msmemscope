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

#ifndef LINE_EDITOR_H
#define LINE_EDITOR_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace MemScope
{

/*
 * 控制端交互CLI补全词表(与control_protocol白名单同源):
 * 静态层级词表 + 动态analyzer名提供者(懒加载:首次需要时经display analyzer
 * 请求缓存,由AttachController注入provider)。纯函数,可独立UT。
 *
 * Complete(line)返回"以最后一个token为前缀"的候选token列表:
 *   ""                       → start/stop/step/display/set/exit/help
 *   "display ..."            → hook/analyzer/config/memory/host_leak
 *   "display memory ..."     → summary/block
 *   "display memory block "  → --pool/--TOPN; "--pool " → 池名
 *   "display host_leak ..."  → summary
 *   "display analyzer ..."   → 动态analyzer名(provider)
 *   "set ..."                → config; "set config " → 配置子集选项
 *   其他 → 空
 */
class CompletionTable
{
   public:
    static std::vector<std::string> Complete(const std::string& line);

    // 动态analyzer名提供者(懒加载缓存;传nullptr清除)
    using Provider = std::function<std::vector<std::string>()>;
    static void SetDynamicProvider(Provider provider);

   private:
    static std::vector<std::string> Filter(const std::vector<std::string>& words, const std::string& prefix);
};

/*
 * 交互行编辑器(仅tty路径):termios raw mode(关ICANON/ECHO/ISIG)逻辑键读,
 * 处理可打印回显(行中插入)/退格(0x7f/0x08)/回车提交/tab补全/左右键光标移动/
 * 上下键历史浏览/Ctrl-C中断(走exit清理路径退出130)/Ctrl-D EOF,行长上限4096;
 * termios恢复RAII守卫覆盖全部退出路径。
 * 历史=下发成功(ok=true且完整往返)的控制字(AttachController在成功往返后调
 * AddHistory):全表去重,最多3条,最新在前,会话级内存不持久化;上下键成对
 * 浏览,下键越过最新一条恢复进入历史前的草稿。
 * 非tty(stdin管道/文件)不进入本类,由调用方走普通getline。
 */
class LineEditor
{
   public:
    // 读取一行(raw mode已由本函数管理):EOF返回false;Ctrl-C置*interrupted=true
    // 并返回false(提示符态退出路径);输入经补全/回显
    static bool ReadLine(std::string& line, bool* interrupted = nullptr);

    // 提示符文本(交互会话含pid,如"msmemscope[12345]> ");默认"msmemscope> "
    static void SetPrompt(const std::string& prompt);

    // 下发成功(ok=true且完整往返)的控制字入历史:全表去重,最多3条,最新在前;
    // 会话级内存不持久化;仅tty交互路径上下键浏览使用(非tty无导航,存了无害)
    static void AddHistory(const std::string& cmd);

    // 测试缝:读写/termios替换(UT注入无tty场景)
    using ReadCharFn = std::function<int()>;  // 返回-1=EOF/错误,否则字符
    using WriteStrFn = std::function<void(const std::string&)>;
    static void SetIoForTest(ReadCharFn readFn, WriteStrFn writeFn);

    // 测试缝:ESC序列续读(短超时)——方向键序列(\x1b[A等)首字节经ReadCharFn,
    // 续字节经本缝;返回字符,-1=超时/EOF/错误(序列中止)。未注入默认短超时stdin读
    using ReadEscapeFn = std::function<int()>;
    static void SetEscapeReaderForTest(ReadEscapeFn fn);

    // 测试缝:清空历史(UT用例间隔离;会话级历史跨用例残留)
    static void ClearHistoryLine();

    // 退出信号置位检测注入(控制端安装SIGINT/SIGTERM handler后调用):
    // read被打断(EINTR)时据此区分退出信号(按EOF收尾,退出码由调用方裁决)
    // 与其他信号(如SIGWINCH,重试继续读)——未注入默认恒false(EINTR一律重试)
    using QuitFlagFn = std::function<bool()>;
    static void SetQuitFlagProvider(QuitFlagFn provider);

    // 对端存活探针(控制端注入):等待输入期间空闲tick周期调用;返回false=
    // 对端断联(ReadLine重绘当前行后继续读,不中断输入);未注入默认恒true
    using PeerAliveFn = std::function<bool()>;
    static void SetPeerAliveProvider(PeerAliveFn provider);

   private:
    // 补全一次:返回是否发生了文本变化;候选打印/重绘在内部完成
    // (文本变更后光标置行尾,无变更保持原光标)
    static bool TryComplete(std::string& line, size_t& cursor, const WriteStrFn& write);
    // 整行重绘:清行重写提示符与输入,再按绝对列定位光标
    // (提示符与输入均为ASCII,显示列=字符数,列号=promptLen+cursor+1)
    static void RedrawLine(const std::string& line, size_t cursor, const WriteStrFn& write);
};

}  // namespace MemScope

#endif
