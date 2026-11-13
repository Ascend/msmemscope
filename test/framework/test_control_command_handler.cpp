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

/* 控制字命令处理器测试(ControlCommandHandler):
 * 白名单校验(派发内HandleDispatch+执行侧Execute防御)、start/stop/step状态机、
 * display hook/analyzer/config/memory summary/block/host_leak summary、
 * set config解析(门控/子集更新/非法参数)。
 * 注意: 本进程为单例进程,EventTraceManager/MemoryStateManager/ConfigManager
 * 状态跨用例存在——用例自归一状态(SETUP置NOT_IN_TRACING),断言只做结构性校验
 * (与单例实际状态对比,不做绝对内容假设)。
 */
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config_info.h"
#include "event.h"
// 状态注入用例需在fork子进程内清零MemoryStateManager的进程级累加字段
// (其他用例对用量/峰值做绝对断言,主进程不得重置)。沿用test_memory_state_manager
// 的private->public惯用法读取私有累加器;必须前置于event_dispatcher.h(其间接
// 包含memory_state_manager.h,头文件守卫会使之后的宏设置失效)。
// 其所需标准库头已在上方先行包含
#define private public
#include "memory_state_manager.h"
#undef private
#include "event_dispatcher.h"
#include "event_report.h"
#include "trace_manager/event_trace_manager.h"
#include "control_channel/control_command_handler.h"
#include "analysis/host_leak_analyzer.h"

using namespace MemScope;

namespace
{

// 供TEST_F体与fork子进程lambda共用的构造器(类静态成员在lambda内的访问依赖
// CWG1202,显式下沉为命名空间函数更稳妥)
std::shared_ptr<ControlEvent> MakeControlEvent(const std::string& cmd, const std::string& param = "")
{
    auto ev = std::make_shared<ControlEvent>();
    ev->cmd = cmd;
    ev->param = param;
    return ev;
}

class ControlCommandHandlerTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        // 归一采集状态(其他用例可能留下IN_TRACING)
        EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    }

    void TearDown() override
    {
        EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    }

    // cmd=控制字(含空格,如"display memory block"),param=命令参数(如"--pool hal --TOPN 5")
    static std::shared_ptr<ControlEvent> MakeEvent(const std::string& cmd, const std::string& param = "")
    {
        return MakeControlEvent(cmd, param);
    }

    static void Execute(std::shared_ptr<ControlEvent>& ev)
    {
        ControlCommandHandler::GetInstance().Execute(ev);
    }
};

// 白名单校验: 派发内仅登记(HandleDispatch),执行侧Execute防御性兜底(两处同一校验)
TEST_F(ControlCommandHandlerTest, invalid_word_rejected_in_dispatch_and_execute)
{
    // 派发内:非法词直接回执,不执行
    auto ev = MakeEvent("frobnicate");
    std::shared_ptr<EventBase> base = ev;
    ControlCommandHandler::GetInstance().HandleDispatch(base, nullptr);
    EXPECT_EQ(ev->output, "invalid control word: frobnicate");
    EXPECT_FALSE(ev->ok);

    // 执行侧:合法词根+未知词
    ev = MakeEvent("display bogus");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid control word: display bogus");
    EXPECT_FALSE(ev->ok);
    // 合法动词+多余token→整体非法(白名单按完整token序列校验)
    ev = MakeEvent("start extra");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid control word: start extra");
    EXPECT_FALSE(ev->ok);
    // 纯动词表外的display二级词
    ev = MakeEvent("display memory bogus");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid control word: display memory bogus");
    EXPECT_FALSE(ev->ok);
    // 白名单允许但缺下级词(display memory/display host_leak):ok=false(单发退出码1)
    ev = MakeEvent("display memory");
    Execute(ev);
    EXPECT_NE(ev->output.find("invalid control word: display memory"), std::string::npos);
    EXPECT_FALSE(ev->ok);
}

// start/stop/step状态机(初态NOT_IN_TRACING;start成功后置IN_TRACING,
// 由TearDown归一恢复)
TEST_F(ControlCommandHandlerTest, start_stop_step_state_machine)
{
    // 未追踪:stop/step均拒绝
    auto ev = MakeEvent("stop");
    Execute(ev);
    EXPECT_EQ(ev->output, "not in tracing");

    ev = MakeEvent("step");
    Execute(ev);
    EXPECT_EQ(ev->output, "step ignored: not in tracing (start first)");

    // start成功→采集态;重复start拒绝(业务状态回执,ok保持true)
    ev = MakeEvent("start");
    Execute(ev);
    EXPECT_EQ(ev->output, "tracing started");
    EXPECT_TRUE(EventTraceManager::Instance().IsTracingEnabled());
    EXPECT_TRUE(ev->ok);

    ev = MakeEvent("start");
    Execute(ev);
    EXPECT_EQ(ev->output, "tracing already in progress");
    EXPECT_TRUE(ev->ok);
}

// display hook: 测试进程未装配任何钩子→两行not loaded(扫描/proc/self/maps)
TEST_F(ControlCommandHandlerTest, display_hook_reports_not_loaded)
{
    auto ev = MakeEvent("display hook");
    Execute(ev);
    EXPECT_EQ(ev->output, "Host hook: not loaded\nNPU hooks: not loaded");
}

// display analyzer: 前缀+自身(control_channel)过滤+与GetSubscriberNames同源拼接
TEST_F(ControlCommandHandlerTest, display_analyzer_excludes_control_channel)
{
    auto ev = MakeEvent("display analyzer");
    Execute(ev);
    EXPECT_NE(ev->output.find("Analyzers: "), std::string::npos);
    EXPECT_EQ(ev->output.find("control_channel"), std::string::npos);

    // 与GetSubscriberNames同源(过滤control_channel后逗号拼接,顺序一致)
    std::string expected = "Analyzers: ";
    bool first = true;
    for (const std::string& name : EventDispatcher::GetInstance().GetSubscriberNames())
    {
        if (name == "control_channel")
        {
            continue;
        }
        if (!first)
        {
            expected += ",";
        }
        expected += name;
        first = false;
    }
    EXPECT_EQ(ev->output, expected);
}

// display config: 8个键全部输出(值随单例配置变化,只校验键结构)
TEST_F(ControlCommandHandlerTest, display_config_keys_present)
{
    auto ev = MakeEvent("display config");
    Execute(ev);
    const char* keys[] = {"analysis: ",   "host_leak_mode: ",   "block_size_threshold: ",
                          "sample_rate: ", "collect_mode: ",     "call_stack: ",
                          "output_dir: ",  "log_level: "};
    for (const char* key : keys)
    {
        EXPECT_NE(ev->output.find(key), std::string::npos) << "missing key: " << key;
    }
}

// display memory summary: 行内容与MemoryStateManager当前状态一致
// (设备行=GetUsedDeviceList;host pinned/tensor=峰值>0才展示)
TEST_F(ControlCommandHandlerTest, display_memory_summary_reflects_manager)
{
    auto ev = MakeEvent("display memory summary");
    Execute(ev);

    MemoryStateManager& msm = MemoryStateManager::GetInstance();
    const std::vector<int32_t> devices = msm.GetUsedDeviceList();
    if (devices.empty())
    {
        EXPECT_NE(ev->output.find("(no device data collected yet)"), std::string::npos);
    }
    else
    {
        EXPECT_EQ(ev->output.find("(no device data collected yet)"), std::string::npos);
    }
    for (int32_t dev : devices)
    {
        EXPECT_NE(ev->output.find("Device " + std::to_string(dev) + ":"), std::string::npos);
    }
    // 每行都带hal current/peak(有设备行时必然出现)
    if (!devices.empty())
    {
        EXPECT_NE(ev->output.find(" hal_current="), std::string::npos);
        EXPECT_NE(ev->output.find(" hal_peak="), std::string::npos);
    }
    if (msm.GetHostPinnedPeak() > 0)
    {
        EXPECT_NE(ev->output.find("Host pinned:"), std::string::npos);
    }
    if (msm.GetHostTensorPeak() > 0)
    {
        EXPECT_NE(ev->output.find("Host tensor:"), std::string::npos);
    }
}

// display memory block: 参数校验(确定性)+表头/池名/TOPN格式(与活块数无关)
TEST_F(ControlCommandHandlerTest, display_memory_block_validation_and_header)
{
    // 非法参数路径(纯解析,确定性):ok=false(单发退出码1)
    auto ev = MakeEvent("display memory block", "--TOPN 0");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid --TOPN value (range 1..100): 0");
    EXPECT_FALSE(ev->ok);
    ev = MakeEvent("display memory block", "--TOPN 101");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid --TOPN value (range 1..100): 101");
    EXPECT_FALSE(ev->ok);
    ev = MakeEvent("display memory block", "--TOPN abc");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid --TOPN value (range 1..100): abc");
    EXPECT_FALSE(ev->ok);
    ev = MakeEvent("display memory block", "--pool bogus");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid pool name: bogus (expect host/hal/pta/pta_workspace/atb/mindspore)");
    EXPECT_FALSE(ev->ok);
    ev = MakeEvent("display memory block", "--wat 1");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid option: --wat (expect --pool <name> [--TOPN <N>])");
    EXPECT_FALSE(ev->ok);

    // 合法参数: 表头固定,块数随单例状态(不做绝对断言)
    ev = MakeEvent("display memory block", "--pool hal --TOPN 5");
    Execute(ev);
    EXPECT_NE(ev->output.find("Top 5 live blocks by size (of "), std::string::npos);
    EXPECT_NE(ev->output.find(", pool=hal):"), std::string::npos);
    EXPECT_NE(ev->output.find("addr\t\t\tsize\talloc_ts\tallocation_id"), std::string::npos);
    EXPECT_TRUE(ev->ok);

    // key=value形式
    ev = MakeEvent("display memory block", "--pool=pta_workspace --TOPN=3");
    Execute(ev);
    EXPECT_NE(ev->output.find("Top 3 live blocks by size"), std::string::npos);
    EXPECT_NE(ev->output.find("pool=pta_workspace"), std::string::npos);
    EXPECT_TRUE(ev->ok);

    // 缺省:全部池,TOPN=10
    ev = MakeEvent("display memory block");
    Execute(ev);
    EXPECT_NE(ev->output.find("Top 10 live blocks by size"), std::string::npos);
    EXPECT_NE(ev->output.find("pool=all"), std::string::npos);
    EXPECT_TRUE(ev->ok);
}

// set config: stop-only门控/参数解析/子集更新后恢复
TEST_F(ControlCommandHandlerTest, set_config_gate_and_parse)
{
    // 门控:窗口开着拒绝(ok=false,单发退出码1)
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);
    auto ev = MakeEvent("set config", "--analysis leak");
    Execute(ev);
    EXPECT_EQ(ev->output, "set config rejected: tracing in progress (stop first)");
    EXPECT_FALSE(ev->ok);
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);

    // 无参数
    ev = MakeEvent("set config");
    Execute(ev);
    EXPECT_NE(ev->output.find("set config: no options given"), std::string::npos);
    EXPECT_FALSE(ev->ok);

    // 未知选项/非选项参数
    ev = MakeEvent("set config", "--bogus 1");
    Execute(ev);
    EXPECT_EQ(ev->output, "unknown option: --bogus");
    EXPECT_FALSE(ev->ok);
    ev = MakeEvent("set config", "leak");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid argument: leak (expect --key[=value])");
    EXPECT_FALSE(ev->ok);

    // 各选项非法值
    ev = MakeEvent("set config", "--analysis 42");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid analysis token: 42 (expect leaks/decompose/inefficient/oom[:K]/host-leaks/none)");
    EXPECT_FALSE(ev->ok);
    ev = MakeEvent("set config", "--analysis host-leaks,leaks");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid analysis: host-leaks is mutually exclusive with leaks/decompose/inefficient/oom");
    EXPECT_FALSE(ev->ok);
    ev = MakeEvent("set config", "--host-leak-mode fast");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid --host-leak-mode value: fast (expect event/summary)");
    EXPECT_FALSE(ev->ok);
    // --collect-mode仅CLI拉起时生效:set config不支持(unknown option)
    ev = MakeEvent("set config", "--collect-mode now");
    Execute(ev);
    EXPECT_EQ(ev->output, "unknown option: --collect-mode");
    EXPECT_FALSE(ev->ok);
    ev = MakeEvent("set config", "--call-stack bogus");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid --call-stack value: bogus (expect c[:depth],python[:depth])");
    EXPECT_FALSE(ev->ok);
    ev = MakeEvent("set config", "--block-size-threshold abc");
    Execute(ev);
    EXPECT_EQ(ev->output, "invalid --block-size-threshold value: abc");
    EXPECT_FALSE(ev->ok);

    // 成功路径: 多选项子集更新,未涉及字段全量保留
    const Config orig = ConfigManager::Instance().GetConfig();
    ev = MakeEvent("set config", "--block-size-threshold 7777 --host-leak-mode summary "
                                 "--call-stack c:10,python --analysis oom:5");
    Execute(ev);
    EXPECT_EQ(ev->output, "config updated (applied at next start)");
    EXPECT_TRUE(ev->ok);
    const Config& cfg = ConfigManager::Instance().GetConfig();
    EXPECT_EQ(static_cast<uint64_t>(cfg.blockSizeThreshold), 7777ULL);
    EXPECT_EQ(cfg.hostLeakMode, static_cast<uint8_t>(HostLeakMode::SUMMARY));
    EXPECT_TRUE(cfg.enableCStack);
    EXPECT_EQ(static_cast<uint64_t>(cfg.cStackDepth), 10ULL);
    EXPECT_TRUE(cfg.enablePyStack);
    EXPECT_EQ(static_cast<uint64_t>(cfg.pyStackDepth), 50ULL);  // 缺省深度
    EXPECT_NE(cfg.analysisType & (1u << static_cast<uint8_t>(AnalysisType::OOM_ANALYSIS)), 0u);
    EXPECT_EQ(static_cast<uint64_t>(cfg.oomTopK), 5ULL);

    // 恢复原配置(SetConfig副作用与其他用例一致)
    ConfigManager::Instance().SetConfig(orig);
    EXPECT_EQ(static_cast<uint64_t>(ConfigManager::Instance().GetConfig().blockSizeThreshold),
              static_cast<uint64_t>(orig.blockSizeThreshold));
}

// display host_leak summary: 窗口开着→中间概览(interim标注);关着→no active window。
// 窗口状态属分析器单例(跨用例持久),用同源QueryInterimOverview先判定再断言
TEST_F(ControlCommandHandlerTest, display_host_leak_summary_passes_through_analyzer)
{
    std::string direct;
    const bool windowOpen = HostLeakAnalyzer::GetInstance().QueryInterimOverview(direct);

    auto ev = MakeEvent("display host_leak summary");
    Execute(ev);
    EXPECT_FALSE(ev->output.empty());
    if (windowOpen)
    {
        EXPECT_NE(ev->output.find("(interim snapshot, window open"), std::string::npos);
    }
    else
    {
        EXPECT_EQ(ev->output, "no active window");
    }
}

// ---------------- 状态注入类展示命令(fork隔离) ----------------
// MemoryStateManager为进程级单例,其他用例对用量/峰值做绝对断言;
// 状态注入用例在fork子进程内执行(COW隔离),父进程只断言子进程回写的输出文本。

std::string MakeTempPath()
{
    char tmpl[] = "/tmp/msmemscope_cmd_ut_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0)
    {
        ::close(fd);
    }
    return std::string(tmpl);
}

// fork子进程内重置MemoryStateManager(COW隔离,主进程状态不变):删除全部活块并清零
// 进程级累加器,使注入后的绝对断言不受其他用例残留状态影响(hal/host累计与峰值、
// 池级占用、整卡/本进程用量缓存均跨用例持久)
void ResetMemoryStateInChild()
{
    MemoryStateManager& msm = MemoryStateManager::GetInstance();
    const std::vector<std::pair<PoolType, MemoryStateKey>> keys = msm.GetAllStateKeys();
    for (const auto& keyPair : keys)
    {
        msm.DeteleState(keyPair.first, keyPair.second);
    }
    msm.halUsed_.clear();
    msm.halPeak_.clear();
    msm.hostUsed_ = 0;
    msm.hostPeak_ = 0;
    msm.hostTensorTotal_ = 0;
    msm.hostTensorPeak_ = 0;
    msm.poolUsage_.clear();
    msm.deviceUsedCache_.fill(-1);
    msm.processUsedCache_.fill(-1);
}

// 子进程执行上限：命令处理路径均应在秒级完成；超时强制回收，
// 避免被测代码回归(fork后继承他人持有的锁等)时无界 waitpid 拖死整个 gtest 进程
constexpr int kChildTimeoutMs = 30000;
constexpr int kChildPollStepMs = 50;

void WaitChildExitBounded(pid_t pid, int* status)
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

std::string RunInChild(const std::function<std::string()>& body)
{
    const std::string path = MakeTempPath();
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0)
        {
            ::dup2(fd, STDOUT_FILENO);
            ::close(fd);
        }
        const std::string text = body();
        (void)!::write(STDOUT_FILENO, text.data(), text.size());
        std::exit(0);  // 正常退出以落盘gcov
    }
    if (pid < 0)
    {
        return std::string();
    }
    int status = 0;
    WaitChildExitBounded(pid, &status);
    std::ifstream in(path);
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ::unlink(path.c_str());
    EXPECT_TRUE(WIFEXITED(status));
    return text;
}

void AddLiveEvent(EventBaseType type, PoolType pool, int32_t dev, uint64_t addr, int64_t size, bool pinned = false)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = type;
    event->poolType = pool;
    event->device = dev;
    event->addr = addr;
    event->size = size;
    event->pid = static_cast<uint64_t>(::getpid());
    event->isPinned = pinned;
    MemoryStateManager::GetInstance().AddEvent(event);
}

// display memory summary: 设备行四种单位/unknown分支 + 锁页/tensor行 + 四类generic池名
TEST_F(ControlCommandHandlerTest, display_memory_summary_formats_units_and_pools)
{
    const std::string out = RunInChild(
        []()
        {
            ResetMemoryStateInChild();
            MemoryStateManager& msm = MemoryStateManager::GetInstance();
            msm.UpdateDeviceUsedCache(0, 3LL * 1024 * 1024 * 1024);  // GB
            msm.UpdateDeviceUsedCache(1, 5LL * 1024 * 1024);         // MB
            msm.UpdateProcessUsedCache(1, 2048);                     // KB
            msm.UpdateDeviceUsedCache(2, -1);                        // unknown
            msm.UpdateProcessUsedCache(2, 512);                      // B
            AddLiveEvent(EventBaseType::MALLOC, PoolType::HAL, 0, 0xA000, 2LL * 1024 * 1024 * 1024);
            AddLiveEvent(EventBaseType::MALLOC, PoolType::HOST, 0, 0xB000, 3LL * 1024 * 1024, true);
            AddLiveEvent(EventBaseType::MALLOC, PoolType::HOST, 0, 0xC000, 5LL * 1024, false);
            AddLiveEvent(EventBaseType::MALLOC, PoolType::PTA_CACHING, 5, 0xD000, 100);
            AddLiveEvent(EventBaseType::MALLOC, PoolType::PTA_WORKSPACE, 6, 0xE000, 200);
            AddLiveEvent(EventBaseType::MALLOC, PoolType::MINDSPORE, 7, 0xF000, 300);
            AddLiveEvent(EventBaseType::MALLOC, PoolType::ATB, 8, 0x10000, 400);
            auto ev = MakeControlEvent("display memory summary");
            ControlCommandHandler::GetInstance().Execute(ev);
            return ev->output;
        });
    EXPECT_NE(out.find("device_used=3.00 GB"), std::string::npos) << out;
    EXPECT_NE(out.find("process_used=unknown"), std::string::npos) << out;
    EXPECT_NE(out.find("device_used=5.00 MB"), std::string::npos) << out;
    EXPECT_NE(out.find("process_used=2.00 KB"), std::string::npos) << out;
    EXPECT_NE(out.find("device_used=unknown"), std::string::npos) << out;
    EXPECT_NE(out.find("process_used=512 B"), std::string::npos) << out;
    EXPECT_NE(out.find("hal_current=2.00 GB"), std::string::npos) << out;
    EXPECT_NE(out.find("Host pinned: current=3.00 MB peak=3.00 MB"), std::string::npos) << out;
    EXPECT_NE(out.find("Host tensor: current=5.00 KB peak=5.00 KB"), std::string::npos) << out;
    EXPECT_NE(out.find("Pool pta:\n  Device 5: current=100 B peak=100 B"), std::string::npos) << out;
    EXPECT_NE(out.find("Pool pta_workspace:\n  Device 6: current=200 B peak=200 B"), std::string::npos) << out;
    EXPECT_NE(out.find("Pool mindspore:\n  Device 7: current=300 B peak=300 B"), std::string::npos) << out;
    EXPECT_NE(out.find("Pool atb:\n  Device 8: current=400 B peak=400 B"), std::string::npos) << out;
}

// display memory block: 有活块时按size降序打印(地址/size/时间戳/id),TOPN截断
TEST_F(ControlCommandHandlerTest, display_memory_block_populated_sorted_and_truncated)
{
    const std::string out = RunInChild(
        []()
        {
            ResetMemoryStateInChild();
            // 0x2222最大(8GB)→0x1111其次(7GB)→0x3333最小(6GB,被TOPN 2截断)
            AddLiveEvent(EventBaseType::MALLOC, PoolType::HAL, 3, 0x2222, 8LL * 1024 * 1024 * 1024);
            AddLiveEvent(EventBaseType::MALLOC, PoolType::HAL, 3, 0x1111, 7LL * 1024 * 1024 * 1024);
            AddLiveEvent(EventBaseType::MALLOC, PoolType::HAL, 3, 0x3333, 6LL * 1024 * 1024 * 1024);
            auto ev = MakeControlEvent("display memory block", "--pool hal --TOPN 2");
            ControlCommandHandler::GetInstance().Execute(ev);
            return ev->output;
        });
    const size_t pos2222 = out.find("0x0000000000002222");
    const size_t pos1111 = out.find("0x0000000000001111");
    EXPECT_NE(pos2222, std::string::npos) << out;
    EXPECT_NE(pos1111, std::string::npos) << out;
    EXPECT_LT(pos2222, pos1111) << out;                                  // 降序
    EXPECT_EQ(out.find("0x0000000000003333"), std::string::npos) << out;  // TOPN截断
    EXPECT_NE(out.find("Top 2 live blocks by size (of 3 live, pool=hal):"), std::string::npos) << out;
}

// display memory block: --pool各合法池名(host/pta/atb/mindspore)均可解析
TEST_F(ControlCommandHandlerTest, display_memory_block_accepts_all_pool_names)
{
    for (const char* pool : {"host", "hal", "pta", "pta_workspace", "atb", "mindspore"})
    {
        auto ev = MakeEvent("display memory block", std::string("--pool ") + pool + " --TOPN 1");
        Execute(ev);
        EXPECT_TRUE(ev->ok) << pool << ": " << ev->output;
        EXPECT_NE(ev->output.find(std::string("pool=") + pool), std::string::npos) << ev->output;
    }
}

// display memory block: --pool缺值(值以--开头/无后续token)
TEST_F(ControlCommandHandlerTest, display_memory_block_missing_pool_value)
{
    auto ev = MakeEvent("display memory block", "--pool");
    Execute(ev);
    EXPECT_EQ(ev->output, "missing value for --pool (option value cannot start with --)");
    EXPECT_FALSE(ev->ok);

    ev = MakeEvent("display memory block", "--pool --TOPN 5");
    Execute(ev);
    EXPECT_EQ(ev->output, "missing value for --pool (option value cannot start with --)");
    EXPECT_FALSE(ev->ok);
}

// display config: analysis名字串(含none/单项/逗号拼接)与call_stack组合
TEST_F(ControlCommandHandlerTest, display_config_analysis_names_and_call_stack)
{
    const Config orig = ConfigManager::Instance().GetConfig();
    const std::pair<const char*, const char*> analysisCases[] = {
        {"--analysis none", "analysis: none"},
        {"--analysis decompose", "analysis: decompose"},
        {"--analysis inefficient", "analysis: inefficient"},
        {"--analysis host-leaks", "analysis: host-leaks"},
        {"--analysis leaks,oom:5", "analysis: leaks,oom"},
    };
    for (const auto& item : analysisCases)
    {
        auto setEv = MakeEvent("set config", item.first);
        Execute(setEv);
        ASSERT_TRUE(setEv->ok) << item.first << ": " << setEv->output;
        auto showEv = MakeEvent("display config");
        Execute(showEv);
        EXPECT_NE(showEv->output.find(item.second), std::string::npos) << item.first << ": " << showEv->output;
    }

    const std::pair<const char*, const char*> callStackCases[] = {
        {"--call-stack none", "call_stack: none"},
        {"--call-stack c", "call_stack: c:50"},
        {"--call-stack python:30", "call_stack: python:30"},
        {"--call-stack c:10,python:20", "call_stack: c:10,python:20"},
    };
    for (const auto& item : callStackCases)
    {
        // 先以none清零c/python两个开关:ParseCallStack只置位值中出现的分量、
        // 不重置未出现的分量,否则上一轮的c残留会混入本轮"裸python"用例
        auto resetEv = MakeEvent("set config", "--call-stack none");
        Execute(resetEv);
        ASSERT_TRUE(resetEv->ok) << resetEv->output;
        auto setEv = MakeEvent("set config", item.first);
        Execute(setEv);
        ASSERT_TRUE(setEv->ok) << item.first << ": " << setEv->output;
        auto showEv = MakeEvent("display config");
        Execute(showEv);
        EXPECT_NE(showEv->output.find(item.second), std::string::npos) << item.first << ": " << showEv->output;
    }

    // 恢复原配置并同步分析器订阅(UpdateAnalysisType副作用)
    ConfigManager::Instance().SetConfig(orig);
    EventReport::Instance(MemScopeCommType::SHARED_MEMORY).UpdateAnalysisType();
}

// set config: 取值边界(空/超界/溢出/空值)、key=value形式、host-leak-mode event
TEST_F(ControlCommandHandlerTest, set_config_value_edge_cases)
{
    const Config orig = ConfigManager::Instance().GetConfig();
    struct BadCase
    {
        const char* param;
        const char* expect;
    };
    const BadCase badCases[] = {
        {"--call-stack c:", "invalid --call-stack value: c: (expect c[:depth],python[:depth])"},
        {"--call-stack c:0", "invalid --call-stack value: c:0 (expect c[:depth],python[:depth])"},
        {"--call-stack c:1001", "invalid --call-stack value: c:1001 (expect c[:depth],python[:depth])"},
        {"--call-stack python:", "invalid --call-stack value: python: (expect c[:depth],python[:depth])"},
        {"--analysis oom:", "invalid oom top-K (range 1..1000): oom:"},
        {"--analysis oom:1001", "invalid oom top-K (range 1..1000): oom:1001"},
        {"--analysis oom:abc", "invalid oom top-K (range 1..1000): oom:abc"},
        {"--block-size-threshold=", "invalid --block-size-threshold value: "},
        {"--block-size-threshold 99999999999999999999999",
         "invalid --block-size-threshold value: 99999999999999999999999"},
        {"--analysis", "missing value for --analysis"},
    };
    for (const auto& item : badCases)
    {
        auto ev = MakeEvent("set config", item.param);
        Execute(ev);
        EXPECT_EQ(ev->output, item.expect) << item.param;
        EXPECT_FALSE(ev->ok) << item.param;
    }

    // key=value形式 + host-leak-mode event成功路径
    auto ev = MakeEvent("set config", "--block-size-threshold=1234 --host-leak-mode event");
    Execute(ev);
    EXPECT_TRUE(ev->ok) << ev->output;
    const Config& cfg = ConfigManager::Instance().GetConfig();
    EXPECT_EQ(static_cast<uint64_t>(cfg.blockSizeThreshold), 1234ULL);
    EXPECT_EQ(cfg.hostLeakMode, static_cast<uint8_t>(HostLeakMode::EVENT));

    ConfigManager::Instance().SetConfig(orig);
    EventReport::Instance(MemScopeCommType::SHARED_MEMORY).UpdateAnalysisType();
}

// start→step→stop: stop清理路径与step计数回显
TEST_F(ControlCommandHandlerTest, start_step_stop_sequence)
{
    auto ev = MakeEvent("start");
    Execute(ev);
    EXPECT_EQ(ev->output, "tracing started");

    ev = MakeEvent("step");
    Execute(ev);
    EXPECT_EQ(ev->output.rfind("step ", 0), 0u) << ev->output;
    EXPECT_TRUE(ev->ok) << ev->output;

    ev = MakeEvent("stop");
    Execute(ev);
    EXPECT_EQ(ev->output, "tracing stopped");
}

// EventHandle(分发回调)委托HandleDispatch: 合法词仅登记不执行(非法词回执由command用例覆盖)
TEST_F(ControlCommandHandlerTest, event_handle_delegates_to_dispatch)
{
    auto valid = MakeEvent("start");
    std::shared_ptr<EventBase> validBase = valid;
    ControlCommandHandler::GetInstance().EventHandle(validBase, nullptr);
    EXPECT_TRUE(valid->output.empty());
    EXPECT_TRUE(valid->ok);
}

}  // namespace
