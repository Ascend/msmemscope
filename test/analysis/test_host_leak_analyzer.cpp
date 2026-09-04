/* -------------------------------------------------------------------------
 * This file is part of the MindStudio project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
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

/* HostLeakAnalyzer单元测试(单账本+闭窗快照重构,2026-08-29)
 * 直接驱动EventHandle注入STAGE开闭窗系统事件,经EventReport测试缝注入假svc表
 * (SetHostMemSvcForTest):闭窗时分析器经dump_*拉取的快照数据全部由假表提供。
 * 验证:窗口状态机只由STAGE事件驱动、per-pid隔离、event/summary两种模式的
 * 输出件(leak_overview_<stage>.txt + block_detail_<stage>.csv)、概览报告
 * 各章节内容(数据健康度/总泄漏量/大小排布/TOP N)与诚实性标注(截断/采样/
 * 阈值/未知桶/未符号化)。
 * 注:本测试进程无钩子so(bind不执行),但测试缝注入假svc后闭窗快照全部可拉取;
 * 未注入(置nullptr)时statsAvailable=false,概览以"Snapshot: unavailable"标注
 */
#include <gtest/gtest.h>
#include <gtest/internal/gtest-port.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "bit_field.h"
#include "config_info.h"
#include "event.h"
#include "file.h"
#include "securec.h"
#include "event_trace/event_report.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#define private public
#include "host_leak_analyzer.h"
#undef private

using namespace MemScope;

namespace
{
// 使能host-leak分析所需的最小配置(analysis=host-leaks;event模式默认)
void SetHostLeakConfig(uint8_t hostLeakMode = static_cast<uint8_t>(HostLeakMode::EVENT))
{
    Config config{};
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::HOST_LEAK_ANALYSIS));
    config.analysisType = analysisBit.getValue();
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    config.eventType = eventBit.getValue();
    config.hostLeakMode = hostLeakMode;
    config.stepList.stepCount = 0;
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);
    ConfigManager::Instance().SetConfig(config);
}

std::shared_ptr<SystemEvent> CreateStageStart(uint64_t pid, uint64_t stageId, uint64_t timestamp)
{
    auto event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = EventSubType::HOST_LEAK_STAGE_START;
    event->pid = pid;
    event->timestamp = timestamp;
    event->name = std::to_string(stageId);
    return event;
}

std::shared_ptr<SystemEvent> CreateStageEnd(uint64_t pid, uint64_t stageId, uint64_t timestamp)
{
    auto event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = EventSubType::HOST_LEAK_STAGE_END;
    event->pid = pid;
    event->timestamp = timestamp;
    event->name = std::to_string(stageId);
    return event;
}

void Dispatch(std::shared_ptr<EventBase> event)
{
    HostLeakAnalyzer::GetInstance().EventHandle(event, nullptr);
}

bool FileExists(const std::string& path)
{
    std::ifstream f(path);
    return f.is_open();
}

std::string ReadAllText(const std::string& path)
{
    std::string text;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
    {
        text += line;
        text += '\n';
    }
    return text;
}

const std::string REPORT_DIR = "./testmsmemscope/host_leak";  // 工程目录直下,无pid子目录

void RemoveReportFiles(const std::string& dir, const std::string& stage)
{
    std::remove((dir + "/leak_overview_" + stage + ".txt").c_str());
    std::remove((dir + "/block_detail_" + stage + ".csv").c_str());
}

// ---------------------------------------------------------------------------
// 假svc表:闭窗快照的全部数据源。测试用例在开窗后填充g_fakeSvcData,闭窗时
// (HandleStageEnd)分析器经EventReport测试缝拉取的即为此处数据
// ---------------------------------------------------------------------------

struct FakeStackRow
{
    uint64_t stackId = 0;
    uint64_t allocCount = 0;
    uint64_t allocBytes = 0;
    uint64_t freedCount = 0;
    uint64_t freedBytes = 0;
    uint64_t unfreedCount = 0;
    uint64_t unfreedBytes = 0;
    uint64_t maxBlockSize = 0;
    std::string frameDesc;  // 闭窗符号化文本('\n'分隔;空=未符号化/未知桶)
};

struct FakeBucket
{
    uint64_t rangeLow = 0;
    uint64_t rangeHigh = 0;
    uint64_t blockCount = 0;
    uint64_t blockBytes = 0;
};

struct FakeBlock
{
    uint64_t addr = 0;
    uint64_t size = 0;
    uint64_t allocTs = 0;
    uint64_t stackId = 0;
};

struct FakeSvcData
{
    MsmemscopeHostmemStats stats{};
    std::vector<FakeStackRow> stacks;
    std::vector<FakeBucket> buckets;
    std::vector<FakeBucket> preWindowBuckets;  // dump_pre_window_distribution投影
    std::vector<FakeBlock> blocks;
};

FakeSvcData g_fakeSvcData;

void FakeSvcSetEnabled(int)
{
}

void FakeSvcGetStats(MsmemscopeHostmemStats* stats)
{
    if (stats != nullptr)
    {
        *stats = g_fakeSvcData.stats;
    }
}

void FakeSvcDumpLiveBlocks(void (*emit)(void* ctx, uint64_t addr, uint64_t size, uint64_t allocTs, uint64_t stackId),
                           void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    for (const auto& b : g_fakeSvcData.blocks)
    {
        emit(ctx, b.addr, b.size, b.allocTs, b.stackId);
    }
}

void FakeSvcDumpStackStats(void (*emit)(void* ctx, uint64_t stackId, uint64_t allocCount, uint64_t allocBytes,
                                        uint64_t freedCount, uint64_t freedBytes, uint64_t unfreedCount,
                                        uint64_t unfreedBytes, uint64_t maxBlockSize, const char* frameDesc,
                                        size_t len), void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    for (const auto& r : g_fakeSvcData.stacks)
    {
        emit(ctx, r.stackId, r.allocCount, r.allocBytes, r.freedCount, r.freedBytes, r.unfreedCount, r.unfreedBytes,
             r.maxBlockSize, r.frameDesc.data(), r.frameDesc.size());
    }
}

void FakeSvcDumpSizeDist(void (*emit)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount,
                                      uint64_t blockBytes), void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    for (const auto& b : g_fakeSvcData.buckets)
    {
        emit(ctx, b.rangeLow, b.rangeHigh, b.blockCount, b.blockBytes);
    }
}

void FakeSvcDumpPreWindowDist(void (*emit)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount,
                                           uint64_t blockBytes), void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    for (const auto& b : g_fakeSvcData.preWindowBuckets)
    {
        emit(ctx, b.rangeLow, b.rangeHigh, b.blockCount, b.blockBytes);
    }
}

const MsmemscopeHostmemSvc g_fakeSvc = {FakeSvcSetEnabled, FakeSvcGetStats, FakeSvcDumpLiveBlocks,
                                        FakeSvcDumpStackStats, FakeSvcDumpSizeDist, FakeSvcDumpPreWindowDist};

EventReport& FakeReport()
{
    return EventReport::Instance(MemScopeCommType::SHARED_MEMORY);
}

void ResetFakeSvcData()
{
    g_fakeSvcData = FakeSvcData{};
}
}  // namespace

class HostLeakAnalyzerTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        SetHostLeakConfig();
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("./testmsmemscope");
        // 注入假svc表:闭窗拉取的全部快照数据来自g_fakeSvcData(测试进程无钩子so)
        FakeReport().SetHostMemSvcForTest(&g_fakeSvc);
        ResetFakeSvcData();
        // 清理单例跨用例残留状态
        HostLeakAnalyzer::GetInstance().windows_.clear();
    }

    void TearDown() override
    {
        FakeReport().SetHostMemSvcForTest(nullptr);  // 恢复未装配态
        ConfigManager::Instance().SetConfig(Config{});
        HostLeakAnalyzer::GetInstance().windows_.clear();
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("");
        // 清理各用例产出的报告文件(用例内未显式清理的兜底;stage 1~10为主用例)
        for (int stage = 1; stage <= 10; ++stage)
        {
            RemoveReportFiles(REPORT_DIR, std::to_string(stage));
        }
        rmdir(REPORT_DIR.c_str());
        rmdir("./testmsmemscope");
    }
};

// UT-A1: 重复STAGE_START被忽略——不清空在途窗口数据(钩子set_enabled幂等,
// 重复START为异常序列,防御性处理)
TEST_F(HostLeakAnalyzerTest, double_start_ignored)
{
    const uint64_t pid = 1234;
    Dispatch(CreateStageStart(pid, 1, 100));
    Dispatch(CreateStageStart(pid, 2, 120));  // 无效:窗口已开
    Dispatch(CreateStageEnd(pid, 1, 200));

    const auto& ws = HostLeakAnalyzer::GetInstance().windows_.at(pid);
    EXPECT_EQ(ws.stageId, 1u);  // 仍为原窗口
    EXPECT_FALSE(ws.open);
}

// UT-A2: 未开窗的STAGE_END被忽略,不崩溃不出报告
TEST_F(HostLeakAnalyzerTest, end_without_start_noop)
{
    const uint64_t pid = 1234;
    Dispatch(CreateStageEnd(pid, 1, 100));
    Dispatch(CreateStageEnd(pid, 1, 110));  // 重复END
    EXPECT_TRUE(HostLeakAnalyzer::GetInstance().windows_.empty());
    EXPECT_FALSE(FileExists(REPORT_DIR + "/leak_overview_1.txt"));
}

// UT-A3: per-pid隔离——pid A的窗口状态不受pid B事件影响(防御性设计)
TEST_F(HostLeakAnalyzerTest, per_pid_isolation)
{
    Dispatch(CreateStageStart(1234, 1, 100));
    Dispatch(CreateStageStart(5678, 7, 110));
    Dispatch(CreateStageEnd(5678, 7, 200));

    auto& windows = HostLeakAnalyzer::GetInstance().windows_;
    EXPECT_EQ(windows.size(), 2u);
    EXPECT_TRUE(windows.at(1234).open);  // pid 1234窗口不受5678闭窗影响
    EXPECT_FALSE(windows.at(5678).open);

    // 收尾:关闭1234窗口(避免析构兜底路径出报告干扰其他用例)
    Dispatch(CreateStageEnd(1234, 1, 300));
}

// UT-A4: event模式黄金报告——闭窗快照(stats+栈统计+大小排布+逐块明细)经假svc
// 注入,验证leak_overview_1.txt各章节内容与block_detail_1.csv(块大小降序、
// 地址列0x+16位hex)
TEST_F(HostLeakAnalyzerTest, golden_event_mode_report)
{
    const uint64_t pid = 1234;
    // stats: 全局合计(钩子原子计数器口径,闭窗冻结值)
    g_fakeSvcData.stats.liveBlockCount = 2;
    g_fakeSvcData.stats.totalAllocCount = 3;
    g_fakeSvcData.stats.totalAllocBytes = 300;
    g_fakeSvcData.stats.totalFreedCount = 1;
    g_fakeSvcData.stats.totalFreedBytes = 100;
    g_fakeSvcData.stats.sampleRate = 1;
    g_fakeSvcData.stats.truncated = 0;
    // 栈统计:栈5两块存活(100+200B),栈6全释放(不进入TOP)
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 2;
    stack5.allocBytes = 300;
    stack5.unfreedCount = 2;
    stack5.unfreedBytes = 300;
    stack5.maxBlockSize = 200;
    stack5.frameDesc = "main\nfoo() [0x1]\n";
    FakeStackRow stack6;
    stack6.stackId = 6;
    stack6.allocCount = 1;
    stack6.allocBytes = 100;
    stack6.freedCount = 1;
    stack6.freedBytes = 100;
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.stacks.push_back(stack6);
    // 大小排布桶(钩子默认桶界256/1K/4K/32K/256K/1M):存活块100B→[0,256),
    // 200B→[256,1K)
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 1, 100});
    g_fakeSvcData.buckets.push_back(FakeBucket{256, 1024, 1, 200});
    g_fakeSvcData.buckets.push_back(FakeBucket{1024, 4096, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{4096, 32768, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{32768, 262144, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{262144, 1048576, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{1048576, UINT64_MAX, 0, 0});
    // 逐块明细(block_detail数据源,event模式)
    g_fakeSvcData.blocks.push_back(FakeBlock{0x1000, 100, 1100, 5});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x2000, 200, 1200, 5});

    Dispatch(CreateStageStart(pid, 1, 1000));
    Dispatch(CreateStageEnd(pid, 1, 2000));

    // 概览报告各章节
    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_1.txt");
    // 头部与窗口
    EXPECT_NE(text.find("====== Host Leak Overview: stage=1, pid=1234 ======"), std::string::npos);
    EXPECT_NE(text.find("Window: 1000 -> 2000 (duration: 0s)"), std::string::npos);
    EXPECT_NE(text.find("Mode: event"), std::string::npos);
    // 数据健康度:全局合计/唯一栈数/符号化
    EXPECT_NE(text.find("Tracked: 3 allocations / 300B allocated; 1 freed / 100B"), std::string::npos);
    EXPECT_NE(text.find("Distinct stacks: 2 (key depth K=20, category semantics)"), std::string::npos);
    EXPECT_NE(text.find("Symbolized: 1/1 stacks (unresolved: 0)"), std::string::npos);
    // 总泄漏量:桶合计为权威真源
    EXPECT_NE(text.find("Total unfreed: 300 bytes in 2 blocks (avg 150B, max 200B)"), std::string::npos);
    // 大小排布:首桶下界0渲染为"0"(非"0M"),1K缩写
    EXPECT_NE(text.find("[0, 256)"), std::string::npos);
    EXPECT_NE(text.find("[256, 1K)"), std::string::npos);
    // TOP:仅栈5(未释放>0),栈6全释放不出行
    EXPECT_NE(text.find("1. 300B unfreed (2 blocks, avg 150B) | alloc 2x300B, freed 0x0B"), std::string::npos);
    EXPECT_NE(text.find("   main"), std::string::npos);
    EXPECT_NE(text.find("   foo() [0x1]"), std::string::npos);
    EXPECT_EQ(text.find("stack 6"), std::string::npos);
    // 无截断/采样/阈值标注
    EXPECT_EQ(text.find("Truncated:"), std::string::npos);
    EXPECT_EQ(text.find("Sampling:"), std::string::npos);
    EXPECT_EQ(text.find("Size threshold:"), std::string::npos);

    // block_detail:表头+2行,块大小降序(0x2000的200B在前),地址列0x+16位hex,
    // call_stack列内联完整栈文本(RFC 4180引号字段,帧间换行保留;frameDesc尾换行
    // 保留在引号内,故多行结构)
    const std::string csv = ReadAllText(REPORT_DIR + "/block_detail_1.csv");
    const std::string expectCsv =
        "addr,size,alloc_ts,Call Stack(C)\n"
        "0x0000000000002000,200,1200,\"main\n"
        "foo() [0x1]\n"
        "\"\n"
        "0x0000000000001000,100,1100,\"main\n"
        "foo() [0x1]\n"
        "\"\n";
    EXPECT_EQ(csv, expectCsv);

    RemoveReportFiles(REPORT_DIR, "1");
}

// UT-A4b: block_detail call_stack列边界——引号转义(RFC 4180 '"'→'""')与占位
// (未知桶stackId=0、有归因键但未符号化frameDesc空)
TEST_F(HostLeakAnalyzerTest, block_detail_call_stack_escaping_and_placeholders)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.liveBlockCount = 4;
    g_fakeSvcData.stats.totalAllocCount = 4;
    g_fakeSvcData.stats.totalAllocBytes = 400;
    // 栈8:frameDesc含双引号(符号名带引号,如printf("fmt")),验证RFC 4180转义
    FakeStackRow stack8;
    stack8.stackId = 8;
    stack8.allocCount = 1;
    stack8.allocBytes = 100;
    stack8.unfreedCount = 1;
    stack8.unfreedBytes = 100;
    stack8.maxBlockSize = 100;
    stack8.frameDesc = "main\nprintf(\"x\")\n";
    // 栈9:有归因键但符号化失败(frameDesc空)→(unresolved stack)占位
    FakeStackRow stack9;
    stack9.stackId = 9;
    stack9.allocCount = 1;
    stack9.allocBytes = 100;
    stack9.unfreedCount = 1;
    stack9.unfreedBytes = 100;
    stack9.maxBlockSize = 100;
    g_fakeSvcData.stacks.push_back(stack8);
    g_fakeSvcData.stacks.push_back(stack9);
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 4, 400});
    // 全部100B→按地址升序;栈8块验证转义,栈9块验证未解析占位,stackId=0块验证未知桶占位
    g_fakeSvcData.blocks.push_back(FakeBlock{0x3000, 100, 1300, 8});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x4000, 100, 1400, 9});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x5000, 100, 1500, 0});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x6000, 100, 1600, 8});

    Dispatch(CreateStageStart(pid, 4, 1000));
    Dispatch(CreateStageEnd(pid, 4, 2000));

    const std::string csv = ReadAllText(REPORT_DIR + "/block_detail_4.csv");
    EXPECT_NE(csv.find("addr,size,alloc_ts,Call Stack(C)"), std::string::npos);
    // 引号转义:帧内'"'→'""'(printf("x")帧;frameDesc尾换行保留在引号内)
    EXPECT_NE(csv.find("\"main\nprintf(\"\"x\"\")\n\""), std::string::npos);
    // 占位:未知桶与未解析(引号包裹)
    EXPECT_NE(csv.find("\"(unknown bucket: unattributed blocks)\""), std::string::npos);
    EXPECT_NE(csv.find("\"(unresolved stack)\""), std::string::npos);
    // 未知桶与未解析栈的块仍在明细中(自含性,不丢块)
    EXPECT_NE(csv.find("0x0000000000004000,100,1400,\"(unresolved stack)\""), std::string::npos);
    EXPECT_NE(csv.find("0x0000000000005000,100,1500,\"(unknown bucket: unattributed blocks)\""), std::string::npos);

    RemoveReportFiles(REPORT_DIR, "4");
}

// UT-A5: summary模式——仅概览报告,无逐块明细
TEST_F(HostLeakAnalyzerTest, summary_mode_suppresses_block_detail)
{
    SetHostLeakConfig(static_cast<uint8_t>(HostLeakMode::SUMMARY));
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.liveBlockCount = 1;
    g_fakeSvcData.stats.totalAllocCount = 1;
    g_fakeSvcData.stats.totalAllocBytes = 4096;
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 1;
    stack5.allocBytes = 4096;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 4096;
    stack5.maxBlockSize = 4096;
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.buckets.push_back(FakeBucket{4096, 16384, 1, 4096});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x1000, 4096, 110, 5});

    Dispatch(CreateStageStart(pid, 2, 100));
    Dispatch(CreateStageEnd(pid, 2, 200));

    EXPECT_TRUE(FileExists(REPORT_DIR + "/leak_overview_2.txt"));
    EXPECT_NE(ReadAllText(REPORT_DIR + "/leak_overview_2.txt").find("Mode: summary"), std::string::npos);
    EXPECT_FALSE(FileExists(REPORT_DIR + "/block_detail_2.csv"));
    RemoveReportFiles(REPORT_DIR, "2");
}

// UT-A6: 空窗口不出报告(闭窗快照无任何数据)
TEST_F(HostLeakAnalyzerTest, empty_window_no_report)
{
    const uint64_t pid = 1234;
    Dispatch(CreateStageStart(pid, 3, 100));
    Dispatch(CreateStageEnd(pid, 3, 200));
    EXPECT_FALSE(FileExists(REPORT_DIR + "/leak_overview_3.txt"));
    EXPECT_FALSE(FileExists(REPORT_DIR + "/block_detail_3.csv"));
}

// UT-A7: 诚实性标注——截断(块表bit0转溢出通道+栈表bit1+溢出通道bit2整窗截断)/
// 显式采样/块阈值未追踪/未知桶(未归因)/未符号化栈,全部在概览健康度章节如实呈现
TEST_F(HostLeakAnalyzerTest, health_annotations)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.liveBlockCount = 5;
    g_fakeSvcData.stats.totalAllocCount = 4;
    g_fakeSvcData.stats.totalAllocBytes = 4000;
    g_fakeSvcData.stats.totalFreedCount = 1;
    g_fakeSvcData.stats.totalFreedBytes = 1000;
    g_fakeSvcData.stats.untrackedCount = 2;
    g_fakeSvcData.stats.untrackedBytes = 64;
    g_fakeSvcData.stats.sampleRate = 4;
    g_fakeSvcData.stats.truncated = 0x7;  // 块表+栈表+溢出通道均截断
    // 死栈淘汰(栈表满回收):本窗口淘汰10栈/折叠300次申请12000B(已并入未知桶行)
    g_fakeSvcData.stats.evictedStackCount = 10;
    g_fakeSvcData.stats.evictedAllocCount = 300;
    g_fakeSvcData.stats.evictedAllocBytes = 12000;
    // 栈5:未释放2000B,已符号化;栈0:未知桶1000B(frameDesc空)
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 2;
    stack5.allocBytes = 2000;
    stack5.unfreedCount = 2;
    stack5.unfreedBytes = 2000;
    stack5.maxBlockSize = 1000;
    stack5.frameDesc = "sym_a\n";
    FakeStackRow unknown;
    unknown.stackId = 0;
    unknown.allocCount = 1;
    unknown.allocBytes = 1000;
    unknown.unfreedCount = 1;
    unknown.unfreedBytes = 1000;
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.stacks.push_back(unknown);
    g_fakeSvcData.buckets.push_back(FakeBucket{1024, 4096, 3, 3000});
    g_fakeSvcData.buckets.push_back(FakeBucket{4096, 16384, 0, 0});

    // 块阈值>0:概览"Size threshold"行按config快照,未追踪计数取stats
    Config config = MemScope::GetConfig();
    config.blockSizeThreshold = 128;
    ConfigManager::Instance().SetConfig(config);

    Dispatch(CreateStageStart(pid, 4, 100));
    Dispatch(CreateStageEnd(pid, 4, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_4.txt");
    // 截断标注:bit0(块表满→转溢出通道)+bit1(栈表,死栈回收无法腾位)+bit2(溢出
    // 通道,记录停于5块)+完整度警示
    EXPECT_NE(text.find("Truncated: block table full (allocations -> overflow channel) | "
                        "stack table (dead-stack recycling active, no reclaimable stack at full) | "
                        "overflow channel full (recording stopped at 5 live blocks)"),
              std::string::npos);
    EXPECT_NE(text.find("[window data incomplete: not a leak conclusion]"), std::string::npos);
    // 死栈淘汰行(Evicted>0时出行)
    EXPECT_NE(text.find("Evicted: 10 stacks recycled (300 allocs / 12000B folded to unknown bucket)"),
              std::string::npos);
    // 采样视图
    EXPECT_NE(text.find("Sampling: 1/4 (sampled view)"), std::string::npos);
    // 块阈值+未追踪
    EXPECT_NE(text.find("Size threshold: 128B (untracked: 2 allocations / 64B)"), std::string::npos);
    // 未归因(未知桶行):1000/3000=33%
    EXPECT_NE(text.find("Unattributed: 1 blocks / 1000B (unknown bucket, 33%)"), std::string::npos);
    // 未知桶TOP行使用设计内标签;栈5已符号化1/1
    EXPECT_NE(text.find("(unknown bucket: unattributed blocks)"), std::string::npos);
    EXPECT_NE(text.find("Symbolized: 1/1 stacks (unresolved: 0)"), std::string::npos);

    RemoveReportFiles(REPORT_DIR, "4");
}

// UT-A8: 未符号化栈占位——栈快照有未释放行但frameDesc为空(unresolved)
TEST_F(HostLeakAnalyzerTest, unresolved_stack_placeholder)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.totalAllocCount = 1;
    g_fakeSvcData.stats.totalAllocBytes = 4096;
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 1;
    stack5.allocBytes = 4096;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 4096;
    stack5.maxBlockSize = 4096;  // frameDesc留空:未符号化
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.buckets.push_back(FakeBucket{4096, 16384, 1, 4096});

    Dispatch(CreateStageStart(pid, 5, 100));
    Dispatch(CreateStageEnd(pid, 5, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_5.txt");
    EXPECT_NE(text.find("(unresolved stack 5)"), std::string::npos);
    EXPECT_NE(text.find("Symbolized: 0/1 stacks (unresolved: 1)"), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "5");
}

// UT-A9: 快照不可得——测试缝置nullptr(等效钩子未装配),闭窗statsAvailable=false,
// 概览以"Snapshot: unavailable"标注而非猜测零值;栈快照不可得时无TOP数据
TEST_F(HostLeakAnalyzerTest, snapshot_unavailable_annotation)
{
    const uint64_t pid = 1234;
    FakeReport().SetHostMemSvcForTest(nullptr);  // 等效无钩子环境

    // 直接构造窗口状态并触发报告(白盒:验证stats不可得时的概览标注)
    auto& ws = HostLeakAnalyzer::GetInstance().windows_[pid];
    ws.open = false;
    ws.stageId = 6;
    ws.startTs = 100;
    ws.endTs = 200;
    ws.statsAvailable = false;
    HostLeakAnalyzer::StackRow row;
    row.stackId = 5;
    row.unfreedCount = 1;
    row.unfreedBytes = 4096;
    row.maxBlockSize = 4096;
    ws.stacks.push_back(row);
    // 桶合计为总泄漏量真源:stats不可得仅影响健康度章节,总泄漏量仍可呈现
    HostLeakAnalyzer::SizeBucket bucket;
    bucket.rangeLow = 4096;
    bucket.rangeHigh = 16384;
    bucket.blockCount = 1;
    bucket.blockBytes = 4096;
    ws.buckets.push_back(bucket);
    HostLeakAnalyzer::GetInstance().WriteWindowReport(pid, ws, false);

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_6.txt");
    EXPECT_NE(text.find("Snapshot: unavailable (host hook not bound / query failed)"), std::string::npos);
    EXPECT_NE(text.find("Total unfreed: 4096 bytes in 1 blocks (avg 4096B, max 4096B)"), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "6");
}

// UT-A10: TOP N环境变量——MSMEMSCOPE_HOSTMEM_TOP_N限制TOP行数
TEST_F(HostLeakAnalyzerTest, top_n_env_override)
{
    const uint64_t pid = 1234;
    setenv("MSMEMSCOPE_HOSTMEM_TOP_N", "1", 1);
    g_fakeSvcData.stats.totalAllocCount = 3;
    g_fakeSvcData.stats.totalAllocBytes = 3000;
    FakeStackRow big;
    big.stackId = 1;
    big.allocCount = 2;
    big.allocBytes = 2000;
    big.unfreedCount = 2;
    big.unfreedBytes = 2000;
    big.maxBlockSize = 1000;
    big.frameDesc = "big\n";
    FakeStackRow small;
    small.stackId = 2;
    small.allocCount = 1;
    small.allocBytes = 1000;
    small.unfreedCount = 1;
    small.unfreedBytes = 1000;
    small.maxBlockSize = 1000;
    small.frameDesc = "small\n";
    g_fakeSvcData.stacks.push_back(big);
    g_fakeSvcData.stacks.push_back(small);
    g_fakeSvcData.buckets.push_back(FakeBucket{1024, 4096, 3, 3000});

    Dispatch(CreateStageStart(pid, 7, 100));
    Dispatch(CreateStageEnd(pid, 7, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_7.txt");
    EXPECT_NE(text.find("--- TOP 1 Leak Sites (by unfreed bytes) ---"), std::string::npos);
    EXPECT_NE(text.find("1. 2000B unfreed"), std::string::npos);
    EXPECT_EQ(text.find("2. 1000B unfreed"), std::string::npos) << "TOP N=1 must cap rows";
    unsetenv("MSMEMSCOPE_HOSTMEM_TOP_N");
    RemoveReportFiles(REPORT_DIR, "7");
}

// UT-A11: 大小排布桶边界格式——桶下界0→"0",K/M缩写,末桶"+inf",% of total
TEST_F(HostLeakAnalyzerTest, size_distribution_boundary_format)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.totalAllocCount = 2;
    g_fakeSvcData.stats.totalAllocBytes = 1048;
    // 桶:64B→[0,256) 1块;1K→[256,1K) 1块;1M→[1M,+inf) 1块(总计1M+1088)
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 1, 64});
    g_fakeSvcData.buckets.push_back(FakeBucket{256, 1024, 1, 1024});
    g_fakeSvcData.buckets.push_back(FakeBucket{1024, 4096, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{4096, 32768, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{32768, 262144, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{262144, 1048576, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{1048576, UINT64_MAX, 1, 1048576});
    FakeStackRow stack1;
    stack1.stackId = 1;
    stack1.unfreedCount = 3;
    stack1.unfreedBytes = 1049664;  // 64+1024+1048576
    stack1.maxBlockSize = 1048576;
    g_fakeSvcData.stacks.push_back(stack1);

    Dispatch(CreateStageStart(pid, 8, 100));
    Dispatch(CreateStageEnd(pid, 8, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_8.txt");
    EXPECT_NE(text.find("[0, 256)"), std::string::npos);
    EXPECT_NE(text.find("[256, 1K)"), std::string::npos);
    EXPECT_NE(text.find("[1M, +inf)"), std::string::npos);
    EXPECT_EQ(text.find("[0M"), std::string::npos) << "zero bound must not render as 0M";
    // 1M桶份额:1048576/1049664=99%
    EXPECT_NE(text.find("99%"), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "8");
}

// UT-A12: 键深K环境变量——MSMEMSCOPE_HOSTMEM_STACK_KEY_FRAMES显示在健康度章节
TEST_F(HostLeakAnalyzerTest, key_frames_env_override)
{
    const uint64_t pid = 1234;
    setenv("MSMEMSCOPE_HOSTMEM_STACK_KEY_FRAMES", "5", 1);
    g_fakeSvcData.stats.totalAllocCount = 1;
    g_fakeSvcData.stats.totalAllocBytes = 100;
    FakeStackRow stack1;
    stack1.stackId = 1;
    stack1.allocCount = 1;
    stack1.allocBytes = 100;
    stack1.unfreedCount = 1;
    stack1.unfreedBytes = 100;
    stack1.maxBlockSize = 100;
    stack1.frameDesc = "f\n";
    g_fakeSvcData.stacks.push_back(stack1);
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 128, 1, 100});

    Dispatch(CreateStageStart(pid, 9, 100));
    Dispatch(CreateStageEnd(pid, 9, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_9.txt");
    EXPECT_NE(text.find("Distinct stacks: 1 (key depth K=5, category semantics)"), std::string::npos);
    unsetenv("MSMEMSCOPE_HOSTMEM_STACK_KEY_FRAMES");
    RemoveReportFiles(REPORT_DIR, "9");
}

// UT-A13: 多窗口隔离——窗口1的报告不泄漏到窗口2,窗口2从零记账
TEST_F(HostLeakAnalyzerTest, multi_window_isolation)
{
    const uint64_t pid = 1234;
    // 窗口1:栈5泄漏
    g_fakeSvcData.stats.totalAllocCount = 1;
    g_fakeSvcData.stats.totalAllocBytes = 4096;
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 1;
    stack5.allocBytes = 4096;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 4096;
    stack5.maxBlockSize = 4096;
    stack5.frameDesc = "win1\n";
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.buckets.push_back(FakeBucket{4096, 16384, 1, 4096});
    Dispatch(CreateStageStart(pid, 1, 100));
    Dispatch(CreateStageEnd(pid, 1, 200));
    ASSERT_TRUE(FileExists(REPORT_DIR + "/leak_overview_1.txt"));

    // 窗口2:全新记账,窗口1的栈5不出现
    ResetFakeSvcData();
    g_fakeSvcData.stats.totalAllocCount = 1;
    g_fakeSvcData.stats.totalAllocBytes = 2048;
    FakeStackRow stack6;
    stack6.stackId = 6;
    stack6.allocCount = 1;
    stack6.allocBytes = 2048;
    stack6.unfreedCount = 1;
    stack6.unfreedBytes = 2048;
    stack6.maxBlockSize = 2048;
    stack6.frameDesc = "win2\n";
    g_fakeSvcData.stacks.push_back(stack6);
    g_fakeSvcData.buckets.push_back(FakeBucket{1024, 4096, 1, 2048});
    Dispatch(CreateStageStart(pid, 2, 300));
    Dispatch(CreateStageEnd(pid, 2, 400));

    const auto& ws = HostLeakAnalyzer::GetInstance().windows_.at(pid);
    EXPECT_EQ(ws.stageId, 2u);
    EXPECT_EQ(ws.stacks.size(), 1u);  // 仅栈6
    EXPECT_EQ(ws.stacks[0].stackId, 6u);
    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_2.txt");
    EXPECT_NE(text.find("1. 2048B unfreed"), std::string::npos);
    EXPECT_NE(text.find("win2"), std::string::npos) << "window2 own stack must render in its report";
    EXPECT_EQ(text.find("win1"), std::string::npos) << "window1 stack leaked into window2 report";
    RemoveReportFiles(REPORT_DIR, "1");
    RemoveReportFiles(REPORT_DIR, "2");
}
