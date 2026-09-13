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

#include <memory>
#include <string>
#include <vector>

#include "config_info.h"
#include "event.h"
#include "event_dispatcher.h"
#include "memory_state_manager.h"
#include "trace_manager/event_trace_manager.h"
#include "control_channel/control_command_handler.h"
#include "analysis/host_leak_analyzer.h"

using namespace MemScope;

namespace
{

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
        auto ev = std::make_shared<ControlEvent>();
        ev->cmd = cmd;
        ev->param = param;
        return ev;
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

}  // namespace
