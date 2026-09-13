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

#ifndef ATTACH_CONTROLLER_H
#define ATTACH_CONTROLLER_H

#include <cstdint>
#include <string>

namespace MemScope
{

// 响应等待结束原因:单发按退出码语义(2=5min无响应/130=Ctrl-C中断)处理,
// 交互按会话保留语义(打断/超时均回到提示符,会话不退出)
enum class WaitResult
{
    Done,
    Error,
    Interrupted,
    Timeout
};

/*
 * 进程外控制通道控制端: 四规则校验(任一失败报错且不发信号)→socket文件
 * 生命周期(僵尸重建/live拒绝)→kill(SIGUSR1)→accept+SO_PEERCRED→读REGISTER
 * 校验版本→unlink socket→交互循环(或-c单发)。退出统一close+unlink残留;
 * SIGINT/SIGTERM→清理+退出130。
 */
class AttachController
{
   public:
    // 控制端入口:singleCommand非空=单发模式(发一帧收响应即退),否则交互
    static void Run(uint64_t pid, const std::string& singleCommand);

    // 测试缝:accept超时覆盖(ms)
    static void SetTimeoutMsForTest(uint64_t ms);

   private:
    AttachController(uint64_t pid, const std::string& singleCommand) : pid_(pid), singleCommand_(singleCommand) {}
    int RunImpl();  // 返回进程退出码

    // 四规则校验(全部通过才kill)
    bool CheckPidAlive() const;
    bool CheckSameUid() const;
    bool CheckMaps() const;
    bool CheckThreadName() const;
    // socket生命周期
    bool PrepareSocket();  // 已存在live→拒绝;僵尸→unlink重建;bind+listen
    bool WaitAndAccept();  // kill(SIGUSR1)→accept(poll超时)→SO_PEERCRED
    bool ReadRegister();   // 读REGISTER帧+版本校验
    // 会话
    bool SendControl(const std::string& cmd, const std::string& param, bool* cmdOk = nullptr,
                     WaitResult* waitResult = nullptr, std::string* responseOut = nullptr);
    // cmdOk输出RESPONSE的ok位(业务侧拒绝时false);waitResult输出等待结束原因;
    // responseOut非空=调用方需解析响应(累积返回,不边收边显;仅display analyzer等有界响应走此路径)
    bool CheckPeerGone();    // 对端断联检测:非阻塞poll+MSG_PEEK内窥确认EOF(不消费数据)
    void RunInteractive();   // 交互循环(tty→LineEditor,非tty→getline)
    int RunSingleCommand();  // -c单发:校验+握手→发一帧→收响应→退出(0成功/1失败)
    void Cleanup() const;    // close会话fd+unlink socket残留

    // 拆分控制字/参数:"display memory block --pool hal"→("display memory block","--pool hal")
    static void SplitCmdParam(const std::string& line, std::string& cmd, std::string& param);

    uint64_t pid_;
    std::string singleCommand_;
    int listenFd_ = -1;
    int sessionFd_ = -1;
    uint16_t seq_ = 0;            // 单调递增(回绕不复用,uint16回绕即断会话)
    uint64_t timeoutMs_ = 30000;  // accept等待超时(env可调,测试缝覆盖)
    bool peerGone_ = false;       // 对端已退出并已提示:会话续存,仅exit/help可用
};

}  // namespace MemScope

#endif
