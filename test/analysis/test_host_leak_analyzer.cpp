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
 * 阈值/未知桶/未符号化)。置信度视图(节拍序列+五因子LSI+常驻子块+降级标注+
 * 周转NOTE)经UT-A4黄金报告与UT-CA1..5覆盖;纯算法因子在test_host_confidence。
 * 注:本测试进程无钩子so(bind不执行),但测试缝注入假svc后闭窗快照全部可拉取;
 * 未注入(置nullptr)时statsAvailable=false,概览以"Snapshot: unavailable"标注
 */
#include <gtest/gtest.h>
#include <gtest/internal/gtest-port.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
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
    uint64_t maxAllocTsNs = 0;        // 未释放块最新分配时刻(常驻判据条件(2))
    uint64_t freedLifetimeSumNs = 0;  // 已释放块寿命和(S因子数据源)
    uint64_t liveAgeSumNs = 0;        // 未释放块年龄和(S因子数据源)
    std::string frameDesc;  // 闭窗符号化文本('\n'分隔;空=未符号化/未知桶)
};

// 节拍快照序列行(dump_unfreed_series投影;flags bit0=槽被驱逐)
struct FakeSeriesRow
{
    uint64_t stackId = 0;
    uint32_t beat = 0;
    uint64_t liveBytes = 0;
    uint32_t liveCount = 0;
    uint32_t flags = 0;
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
    std::vector<FakeSeriesRow> series;  // dump_unfreed_series投影(置信度数据源)
    std::vector<FakeBucket> buckets;
    std::vector<FakeBucket> preWindowBuckets;  // dump_pre_window_distribution投影
    std::vector<FakeBlock> blocks;
    // 窗口中间快照(dump_interim_snapshot投影,display host_leak summary数据源):
    // 与闭窗投影共享stacks/buckets/preWindowBuckets/series(同一窗口的当前状态),
    // stats为MsmemscopeInterimStats(含frozenSkip/snapTsNs/snapshotDegraded)
    MsmemscopeInterimStats interimStats{};
};

FakeSvcData g_fakeSvcData;

// 用例钩子:非0时在中间快照回调尾部派发该pid的STAGE_END——模拟"快照交付期间
// 窗口被关闭"的竞态,覆盖QueryInterimOverview复检不通过分支。正常用例保持0
uint64_t g_closeWindowDuringInterim = 0;

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
                                        uint64_t unfreedBytes, uint64_t maxBlockSize, uint64_t maxAllocTsNs,
                                        uint64_t freedLifetimeSumNs, uint64_t liveAgeSumNs, const char* frameDesc,
                                        size_t len), void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    for (const auto& r : g_fakeSvcData.stacks)
    {
        emit(ctx, r.stackId, r.allocCount, r.allocBytes, r.freedCount, r.freedBytes, r.unfreedCount, r.unfreedBytes,
             r.maxBlockSize, r.maxAllocTsNs, r.freedLifetimeSumNs, r.liveAgeSumNs, r.frameDesc.data(),
             r.frameDesc.size());
    }
}

void FakeSvcDumpUnfreedSeries(void (*emit)(void* ctx, uint64_t stackId, uint32_t beat, uint64_t liveBytes,
                                           uint32_t liveCount, uint32_t flags), void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    for (const auto& s : g_fakeSvcData.series)
    {
        emit(ctx, s.stackId, s.beat, s.liveBytes, s.liveCount, s.flags);
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

void FakeSvcDumpInterimSnapshot(void (*emitStack)(void* ctx, uint64_t stackId, uint64_t allocCount,
                                                  uint64_t allocBytes, uint64_t freedCount, uint64_t freedBytes,
                                                  uint64_t unfreedCount, uint64_t unfreedBytes, uint64_t maxBlockSize,
                                                  uint64_t maxAllocTsNs, uint64_t freedLifetimeSumNs,
                                                  uint64_t liveAgeSumNs, const char* frameDesc, size_t len),
                                void (*emitSizeDist)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh,
                                                     uint64_t blockCount, uint64_t blockBytes),
                                void (*emitPreWindow)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh,
                                                      uint64_t blockCount, uint64_t blockBytes),
                                void (*emitSeries)(void* ctx, uint64_t stackId, uint32_t beat, uint64_t liveBytes,
                                                   uint32_t liveCount, uint32_t flags),
                                MsmemscopeInterimStats* stats, void* ctx)
{
    if (stats != nullptr)
    {
        *stats = g_fakeSvcData.interimStats;
    }
    if (emitStack != nullptr)
    {
        for (const auto& r : g_fakeSvcData.stacks)
        {
            emitStack(ctx, r.stackId, r.allocCount, r.allocBytes, r.freedCount, r.freedBytes, r.unfreedCount,
                      r.unfreedBytes, r.maxBlockSize, r.maxAllocTsNs, r.freedLifetimeSumNs, r.liveAgeSumNs,
                      r.frameDesc.data(), r.frameDesc.size());
        }
    }
    if (emitSizeDist != nullptr)
    {
        for (const auto& b : g_fakeSvcData.buckets)
        {
            emitSizeDist(ctx, b.rangeLow, b.rangeHigh, b.blockCount, b.blockBytes);
        }
    }
    if (emitPreWindow != nullptr)
    {
        for (const auto& b : g_fakeSvcData.preWindowBuckets)
        {
            emitPreWindow(ctx, b.rangeLow, b.rangeHigh, b.blockCount, b.blockBytes);
        }
    }
    if (emitSeries != nullptr)
    {
        for (const auto& s : g_fakeSvcData.series)
        {
            emitSeries(ctx, s.stackId, s.beat, s.liveBytes, s.liveCount, s.flags);
        }
    }
    // 用例钩子(见g_closeWindowDuringInterim):在快照交付尾部关闭窗口
    if (g_closeWindowDuringInterim != 0)
    {
        Dispatch(CreateStageEnd(g_closeWindowDuringInterim, 1, 200));
    }
}

const MsmemscopeHostmemSvc g_fakeSvc = {FakeSvcSetEnabled,      FakeSvcGetStats,     FakeSvcDumpLiveBlocks,
                                        FakeSvcDumpStackStats,  FakeSvcDumpUnfreedSeries, FakeSvcDumpSizeDist,
                                        FakeSvcDumpPreWindowDist, FakeSvcDumpInterimSnapshot};

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
        g_closeWindowDuringInterim = 0;  // 竞态钩子按用例启用,默认关闭
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

// UT-A4: event模式黄金报告——闭窗快照(stats+栈统计+节拍序列+大小排布+逐块明细)
// 经假svc注入,验证leak_overview_1.txt各章节内容与block_detail_1.csv(块大小降序、
// 地址列0x+16位hex)。栈5为持续增长泄漏(每拍+1000B×101拍),置信度全链路:
// G=1.0/R=1.0/S=0.5(无free样本)/P=0.0(斜率平稳)/E=1.0(101块)→LSI=82
// (0.26+0.21+0.06+0.29)×100,常驻判据双条件均不满足→suspected_leak进疑似榜
TEST_F(HostLeakAnalyzerTest, golden_event_mode_report)
{
    const uint64_t pid = 1234;
    const uint64_t ns = 1000000000ULL;
    const uint64_t windowStart = 100 * ns;  // 100s
    const uint64_t windowEnd = 200 * ns;    // 200s
    // stats: 全局合计(钩子原子计数器口径,闭窗冻结值)+序列锚点
    g_fakeSvcData.stats.liveBlockCount = 101;
    g_fakeSvcData.stats.totalAllocCount = 102;
    g_fakeSvcData.stats.totalAllocBytes = 101100;
    g_fakeSvcData.stats.totalFreedCount = 1;
    g_fakeSvcData.stats.totalFreedBytes = 100;
    g_fakeSvcData.stats.sampleRate = 1;
    g_fakeSvcData.stats.truncated = 0;
    g_fakeSvcData.stats.seriesStartTsNs = windowStart;  // 拍0对齐窗口起点
    g_fakeSvcData.stats.seriesBeatIntervalNs = ns;
    // 栈统计:栈5持续增长泄漏(101块×1000B全部未释放),栈6全释放(不进入TOP)
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 101;
    stack5.allocBytes = 101000;
    stack5.unfreedCount = 101;
    stack5.unfreedBytes = 101000;
    stack5.maxBlockSize = 1000;
    stack5.maxAllocTsNs = windowEnd - ns;  // 窗口尾部新分配(常驻判据条件(2)不满足)
    stack5.frameDesc = "main\nfoo() [0x1]\n";
    FakeStackRow stack6;
    stack6.stackId = 6;
    stack6.allocCount = 1;
    stack6.allocBytes = 100;
    stack6.freedCount = 1;
    stack6.freedBytes = 100;
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.stacks.push_back(stack6);
    // 节拍序列:栈5每拍+1000B(拍0..100,与allocBytes口径一致:live(100)=101000B)
    for (uint32_t b = 0; b <= 100; ++b)
    {
        g_fakeSvcData.series.push_back(FakeSeriesRow{5, b, 1000ULL * (b + 1), b + 1, 0});
    }
    // 大小排布桶(钩子默认桶界256/1K/4K/32K/256K/1M):1000B块落在[0,256)与[256,1K)
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 51, 51000});
    g_fakeSvcData.buckets.push_back(FakeBucket{256, 1024, 50, 50000});
    g_fakeSvcData.buckets.push_back(FakeBucket{1024, 4096, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{4096, 32768, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{32768, 262144, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{262144, 1048576, 0, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{1048576, UINT64_MAX, 0, 0});
    // 逐块明细(block_detail数据源,event模式)
    g_fakeSvcData.blocks.push_back(FakeBlock{0x1000, 100, 1100, 5});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x2000, 200, 1200, 5});

    Dispatch(CreateStageStart(pid, 1, windowStart));
    Dispatch(CreateStageEnd(pid, 1, windowEnd));

    // 概览报告各章节
    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_1.txt");
    // 头部与窗口
    EXPECT_NE(text.find("====== Host Leak Overview: stage=1, pid=1234 ======"), std::string::npos);
    EXPECT_NE(text.find("Window: 100000000000 -> 200000000000 (duration: 100s)"), std::string::npos);
    EXPECT_NE(text.find("Mode: event"), std::string::npos);
    // 数据健康度:全局合计/唯一栈数/符号化
    EXPECT_NE(text.find("Tracked: 102 allocations / 101100B allocated; 1 freed / 100B"), std::string::npos);
    EXPECT_NE(text.find("Distinct stacks: 2 (key depth K=20, category semantics)"), std::string::npos);
    EXPECT_NE(text.find("Symbolized: 1/1 stacks (unresolved: 0)"), std::string::npos);
    // 总泄漏量:桶合计为权威真源
    EXPECT_NE(text.find("Total unfreed: 101000 bytes in 101 blocks (avg 1000B, max 1000B)"), std::string::npos);
    // 大小排布:首桶下界0渲染为"0"(非"0M"),1K缩写
    EXPECT_NE(text.find("[0, 256)"), std::string::npos);
    EXPECT_NE(text.find("[256, 1K)"), std::string::npos);
    // TOP N(置信度视图):序列元信息行(拍数=窗口时长折算,锚点=窗口起点)
    EXPECT_NE(text.find("--- TOP 10 Leak Sites (by LSI desc) ---"), std::string::npos);
    EXPECT_NE(text.find("Series: top-k=256, max-stacks=1024, beats=101(100s, 1s/beat)"), std::string::npos);
    // 疑似榜条目(4行):状态/LSI/五因子/统计列/增长与降级/栈文本
    EXPECT_NE(text.find("  1. [suspected_leak] LSI 82"), std::string::npos);
    EXPECT_NE(text.find("Growth 1.00  Release 1.00  Lifetime 0.50  Pattern 0.00"), std::string::npos);
    EXPECT_NE(text.find("Scale 1.00  unfreed/alloc 1.00"), std::string::npos);
    EXPECT_NE(text.find("unfreed 101000B(101 blocks) | alloc 101x1000B, freed 0x0B"), std::string::npos);
    EXPECT_NE(text.find("growth 1000B/beat  pts 96  degraded -  stack 0x5"), std::string::npos);
    EXPECT_NE(text.find("     main"), std::string::npos);
    EXPECT_NE(text.find("     foo() [0x1]"), std::string::npos);
    // 栈6全释放不入候选集;常驻子块与周转NOTE均不出现
    EXPECT_EQ(text.find("stack 6"), std::string::npos);
    EXPECT_EQ(text.find("Resident baselines"), std::string::npos);
    EXPECT_EQ(text.find("NOTE: "), std::string::npos);
    // 无截断/采样/阈值标注
    EXPECT_EQ(text.find("Truncated:"), std::string::npos);
    EXPECT_EQ(text.find("Sampling:"), std::string::npos);
    EXPECT_EQ(text.find("Size threshold:"), std::string::npos);

    // block_detail:表头+2行,块大小降序(0x2000的200B在前),地址列0x+16位hex,
    // 调用栈两列——Call Stack(C)=纯C栈文本(RFC 4180引号字段,帧间换行保留;
    // frameDesc尾换行保留在引号内,故多行结构),Call Stack(Python)=py帧文本
    // (frameDesc无marker的纯C栈行Python列为空"")
    const std::string csv = ReadAllText(REPORT_DIR + "/block_detail_1.csv");
    const std::string expectCsv =
        "addr,size,alloc_ts,Call Stack(C),Call Stack(Python)\n"
        "0x0000000000002000,200,1200,\"main\n"
        "foo() [0x1]\n"
        "\",\"\"\n"
        "0x0000000000001000,100,1100,\"main\n"
        "foo() [0x1]\n"
        "\",\"\"\n";
    EXPECT_EQ(csv, expectCsv);

    RemoveReportFiles(REPORT_DIR, "1");
}

// UT-A4b: block_detail call_stack列边界——引号转义(RFC 4180 '"'→'""')与占位
// (未知桶stackId=0、有归因键但未符号化frameDesc空)
TEST_F(HostLeakAnalyzerTest, block_detail_call_stack_escaping_and_placeholders)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.liveBlockCount = 5;
    g_fakeSvcData.stats.totalAllocCount = 5;
    g_fakeSvcData.stats.totalAllocBytes = 500;
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
    // 栈10:混合栈文本(C栈+marker+py帧,marker文本与钩子侧组装共用同一宏,
    // 验证block_detail按marker拆两列)
    FakeStackRow stack10;
    stack10.stackId = 10;
    stack10.allocCount = 1;
    stack10.allocBytes = 100;
    stack10.unfreedCount = 1;
    stack10.unfreedBytes = 100;
    stack10.maxBlockSize = 100;
    stack10.frameDesc = std::string("main\nfoo() [0x1]\n") + MSMEMSCOPE_HOSTMEM_MIXED_STACK_MARKER +
                        "train.py(42): run()\n";
    g_fakeSvcData.stacks.push_back(stack8);
    g_fakeSvcData.stacks.push_back(stack9);
    g_fakeSvcData.stacks.push_back(stack10);
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 5, 500});
    // 全部100B→按地址升序;栈8块验证转义,栈9块验证未解析占位,stackId=0块验证未知桶占位,
    // 栈10块验证混合栈拆两列
    g_fakeSvcData.blocks.push_back(FakeBlock{0x3000, 100, 1300, 8});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x4000, 100, 1400, 9});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x5000, 100, 1500, 0});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x6000, 100, 1600, 8});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x7000, 100, 1700, 10});

    Dispatch(CreateStageStart(pid, 4, 1000));
    Dispatch(CreateStageEnd(pid, 4, 2000));

    const std::string csv = ReadAllText(REPORT_DIR + "/block_detail_4.csv");
    EXPECT_NE(csv.find("addr,size,alloc_ts,Call Stack(C),Call Stack(Python)"), std::string::npos);
    // 引号转义:帧内'"'→'""'(printf("x")帧;frameDesc尾换行保留在引号内)
    EXPECT_NE(csv.find("\"main\nprintf(\"\"x\"\")\n\""), std::string::npos);
    // 占位:未知桶与未解析(引号包裹,Python列为空"")
    EXPECT_NE(csv.find("\"(unknown bucket: unattributed blocks)\""), std::string::npos);
    EXPECT_NE(csv.find("\"(unresolved stack)\""), std::string::npos);
    // 未知桶与未解析栈的块仍在明细中(自含性,不丢块),Python列为空
    EXPECT_NE(csv.find("0x0000000000004000,100,1400,\"(unresolved stack)\",\"\""), std::string::npos);
    EXPECT_NE(csv.find("0x0000000000005000,100,1500,\"(unknown bucket: unattributed blocks)\",\"\""),
              std::string::npos);
    // 混合栈拆分:marker前→Call Stack(C),marker后→Call Stack(Python)
    EXPECT_NE(csv.find("0x0000000000007000,100,1700,\"main\n"
                       "foo() [0x1]\n"
                       "\",\"train.py(42): run()\n"
                       "\""),
              std::string::npos);

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
    stack5.maxBlockSize = 4096;
    stack5.maxAllocTsNs = 190;  // 窗口尾部(常驻条件(2)不满足,留疑似榜)
    // frameDesc留空:未符号化
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.buckets.push_back(FakeBucket{4096, 16384, 1, 4096});

    Dispatch(CreateStageStart(pid, 5, 100));
    Dispatch(CreateStageEnd(pid, 5, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_5.txt");
    EXPECT_NE(text.find("(unresolved stack 0x5)"), std::string::npos);
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
    big.maxAllocTsNs = 190;  // 窗口尾部(常驻条件(2)不满足,留疑似榜)
    big.frameDesc = "big\n";
    FakeStackRow small;
    small.stackId = 2;
    small.allocCount = 1;
    small.allocBytes = 1000;
    small.unfreedCount = 1;
    small.unfreedBytes = 1000;
    small.maxBlockSize = 1000;
    small.maxAllocTsNs = 190;
    small.frameDesc = "small\n";
    g_fakeSvcData.stacks.push_back(big);
    g_fakeSvcData.stacks.push_back(small);
    g_fakeSvcData.buckets.push_back(FakeBucket{1024, 4096, 3, 3000});

    Dispatch(CreateStageStart(pid, 7, 100));
    Dispatch(CreateStageEnd(pid, 7, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_7.txt");
    // 置信度视图:两栈均无序列(no_series降级,G/P=0,LSI由R/S/E支撑),
    // 规模2块>1块→大栈LSI更高置顶,TOP N=1仅出行大栈
    EXPECT_NE(text.find("--- TOP 1 Leak Sites (by LSI desc) ---"), std::string::npos);
    EXPECT_NE(text.find("Series: top-k=256, max-stacks=1024, beats=1(0s, 1s/beat)"), std::string::npos);
    EXPECT_NE(text.find("1. [growth_watch] LSI 34"), std::string::npos);
    EXPECT_NE(text.find("degraded no_series"), std::string::npos);
    EXPECT_NE(text.find("stack 0x1"), std::string::npos);
    EXPECT_NE(text.find("unfreed 2000B(2 blocks)"), std::string::npos);
    EXPECT_EQ(text.find("2. ["), std::string::npos) << "TOP N=1 must cap rows";
    EXPECT_EQ(text.find("unfreed 1000B"), std::string::npos) << "small stack must not render";
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
    stack6.maxAllocTsNs = 390;  // 窗口尾部(常驻条件(2)不满足,留疑似榜)
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
    EXPECT_NE(text.find("unfreed 2048B(1 blocks)"), std::string::npos);
    EXPECT_NE(text.find("win2"), std::string::npos) << "window2 own stack must render in its report";
    EXPECT_EQ(text.find("win1"), std::string::npos) << "window1 stack leaked into window2 report";
    RemoveReportFiles(REPORT_DIR, "1");
    RemoveReportFiles(REPORT_DIR, "2");
}

// UT-CA1: 闭窗经dump_unfreed_series拉取节拍序列——窗口状态携带系列(多栈分组、
// flags bit0→槽被驱逐标注)
TEST_F(HostLeakAnalyzerTest, stage_end_pulls_series)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.totalAllocCount = 2;
    g_fakeSvcData.stats.totalAllocBytes = 2000;
    g_fakeSvcData.stats.seriesStartTsNs = 100;
    g_fakeSvcData.stats.seriesBeatIntervalNs = 1000000000ULL;
    g_fakeSvcData.series.push_back(FakeSeriesRow{5, 0, 1000, 1, 0});
    g_fakeSvcData.series.push_back(FakeSeriesRow{5, 1, 2000, 1, 0});
    g_fakeSvcData.series.push_back(FakeSeriesRow{7, 0, 3000, 2, 0x1});  // 栈7槽被驱逐
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 2;
    stack5.allocBytes = 2000;
    stack5.unfreedCount = 2;
    stack5.unfreedBytes = 2000;
    stack5.maxAllocTsNs = 190;
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.buckets.push_back(FakeBucket{1024, 4096, 1, 2000});

    Dispatch(CreateStageStart(pid, 1, 100));
    Dispatch(CreateStageEnd(pid, 1, 200));

    const auto& ws = HostLeakAnalyzer::GetInstance().windows_.at(pid);
    ASSERT_EQ(ws.series.size(), 2u);  // 栈5与栈7(栈7无候选行仍保留序列)
    EXPECT_EQ(ws.series[0].stackId, 5u);
    EXPECT_FALSE(ws.series[0].evicted);
    ASSERT_EQ(ws.series[0].points.size(), 2u);
    EXPECT_EQ(ws.series[0].points[0].beat, 0u);
    EXPECT_EQ(ws.series[0].points[1].liveBytes, 2000u);
    EXPECT_EQ(ws.series[1].stackId, 7u);
    EXPECT_TRUE(ws.series[1].evicted);  // flags bit0→槽被驱逐标注
    RemoveReportFiles(REPORT_DIR, "1");
}

// UT-CA2: 预热线程创建失败(纯C进程/资源耗尽)全窗无序列——统一no_warmup_thread
// 降级,G/P=0中性,报告如实标注
TEST_F(HostLeakAnalyzerTest, warmup_thread_failed_degrade)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.totalAllocCount = 1;
    g_fakeSvcData.stats.totalAllocBytes = 4096;
    g_fakeSvcData.stats.seriesFlags = 0x1;  // bit0=预热线程创建失败
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 1;
    stack5.allocBytes = 4096;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 4096;
    stack5.maxBlockSize = 4096;
    stack5.maxAllocTsNs = 190;  // 窗口尾部(常驻条件(2)不满足,留疑似榜)
    stack5.frameDesc = "f\n";
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.buckets.push_back(FakeBucket{4096, 16384, 1, 4096});

    Dispatch(CreateStageStart(pid, 5, 100));
    Dispatch(CreateStageEnd(pid, 5, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_5.txt");
    // 降级标注no_warmup_thread覆盖全窗候选栈;条目仍出(LSI由R/S/E支撑)
    EXPECT_NE(text.find("1. [growth_watch] LSI 31"), std::string::npos);
    EXPECT_NE(text.find("degraded no_warmup_thread"), std::string::npos);
    EXPECT_NE(text.find("stack 0x5"), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "5");
}

// UT-CA3: 常驻基线子块——早期突发+G≈0栈判常驻(early)出子块(省略Pattern因子),
// 不占疑似榜名额;疑似榜只含真泄漏栈
TEST_F(HostLeakAnalyzerTest, resident_baseline_subblock)
{
    const uint64_t pid = 1234;
    const uint64_t ns = 1000000000ULL;
    const uint64_t windowStart = 100 * ns;
    const uint64_t windowEnd = 200 * ns;
    g_fakeSvcData.stats.totalAllocCount = 102;
    g_fakeSvcData.stats.totalAllocBytes = 101000 + 1048576;
    g_fakeSvcData.stats.seriesStartTsNs = windowStart;
    g_fakeSvcData.stats.seriesBeatIntervalNs = ns;
    // 栈5:窗口前1%分配的1MiB常驻池(序列平稳G≈0)→常驻early
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 1;
    stack5.allocBytes = 1048576;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 1048576;
    stack5.maxBlockSize = 1048576;
    stack5.maxAllocTsNs = windowStart + ns;  // 前1%分配
    stack5.frameDesc = "pool\n";
    g_fakeSvcData.stacks.push_back(stack5);
    for (uint32_t b = 0; b <= 100; ++b)
    {
        // 拍0..4=0(预热段), 拍5..100=1MiB平稳
        g_fakeSvcData.series.push_back(FakeSeriesRow{5, b, b < 5 ? 0 : 1048576, b < 5 ? 0 : 1, 0});
    }
    // 栈6:持续增长泄漏(每拍+1000B×101拍)→LSI=82疑似
    FakeStackRow stack6;
    stack6.stackId = 6;
    stack6.allocCount = 101;
    stack6.allocBytes = 101000;
    stack6.unfreedCount = 101;
    stack6.unfreedBytes = 101000;
    stack6.maxBlockSize = 1000;
    stack6.maxAllocTsNs = windowEnd - ns;
    stack6.frameDesc = "leak\n";
    g_fakeSvcData.stacks.push_back(stack6);
    for (uint32_t b = 0; b <= 100; ++b)
    {
        g_fakeSvcData.series.push_back(FakeSeriesRow{6, b, 1000ULL * (b + 1), b + 1, 0});
    }
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 51, 51000});
    g_fakeSvcData.buckets.push_back(FakeBucket{256, 1024, 50, 50000});
    g_fakeSvcData.buckets.push_back(FakeBucket{1048576, UINT64_MAX, 1, 1048576});

    Dispatch(CreateStageStart(pid, 3, windowStart));
    Dispatch(CreateStageEnd(pid, 3, windowEnd));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_3.txt");
    // 疑似榜:仅栈6(1号)
    EXPECT_NE(text.find("1. [suspected_leak] LSI 82"), std::string::npos);
    EXPECT_NE(text.find("Pattern 0.00"), std::string::npos);  // 疑似榜含Pattern
    // 常驻子块:栈5,省略Pattern(不参与常驻判定),单独编号
    EXPECT_NE(text.find("Resident baselines (1 stacks, 1MiB;"), std::string::npos);
    EXPECT_NE(text.find("1. [resident/early] LSI 31"), std::string::npos);
    EXPECT_NE(text.find("Lifetime 0.50  Scale 0.15"), std::string::npos);  // Lifetime后直接Scale
    EXPECT_NE(text.find("unfreed 1048576B(1 blocks)"), std::string::npos);
    // 常驻栈不进疑似榜编号(无2号)
    EXPECT_EQ(text.find("2. ["), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "3");
}

// UT-CA4: 周转NOTE——开窗前free(窗口外分配的释放)占窗口内总申请过半→NOTE行
// 提示缓存周转形态(与常驻turnover判据呼应)
TEST_F(HostLeakAnalyzerTest, cache_turnover_note)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.totalAllocCount = 10;
    g_fakeSvcData.stats.totalAllocBytes = 100000;
    g_fakeSvcData.stats.preWindowFreeCount = 5;
    g_fakeSvcData.stats.preWindowFreeBytes = 65536;  // 64KiB>0.5×100000→NOTE
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 10;
    stack5.allocBytes = 100000;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 4000;
    stack5.maxBlockSize = 4000;
    stack5.maxAllocTsNs = 190;
    stack5.frameDesc = "buf\n";
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.buckets.push_back(FakeBucket{1024, 4096, 1, 4000});

    Dispatch(CreateStageStart(pid, 6, 100));
    Dispatch(CreateStageEnd(pid, 6, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_6.txt");
    // NOTE文本按落盘规范逐字保留(含判据章节引用)
    EXPECT_NE(text.find("NOTE: 64KiB pre-window allocations were freed within this window "
                       "(cache turnover pattern); resident "
                       "classification includes the unfreed/alloc turnover criterion (§3.1.4 conditions 4-5)"),
              std::string::npos);
    RemoveReportFiles(REPORT_DIR, "6");
}

// UT-CA5: 节拍序列跨窗隔离——STAGE_START清零窗口状态,上一窗序列不进入本窗置信度
TEST_F(HostLeakAnalyzerTest, series_window_isolation)
{
    const uint64_t pid = 1234;
    // 窗口1:栈5带增长序列
    g_fakeSvcData.stats.totalAllocCount = 101;
    g_fakeSvcData.stats.totalAllocBytes = 101000;
    g_fakeSvcData.stats.seriesStartTsNs = 100;
    g_fakeSvcData.stats.seriesBeatIntervalNs = 1000000000ULL;
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 101;
    stack5.allocBytes = 101000;
    stack5.unfreedCount = 101;
    stack5.unfreedBytes = 101000;
    stack5.maxBlockSize = 1000;
    stack5.maxAllocTsNs = 190;
    g_fakeSvcData.stacks.push_back(stack5);
    for (uint32_t b = 0; b <= 100; ++b)
    {
        g_fakeSvcData.series.push_back(FakeSeriesRow{5, b, 1000ULL * (b + 1), b + 1, 0});
    }
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 51, 51000});
    g_fakeSvcData.buckets.push_back(FakeBucket{256, 1024, 50, 50000});
    Dispatch(CreateStageStart(pid, 1, 100));
    Dispatch(CreateStageEnd(pid, 1, 200));
    ASSERT_TRUE(FileExists(REPORT_DIR + "/leak_overview_1.txt"));

    // 窗口2:全新记账且无序列交付——栈5'须为no_series降级(不得沿用窗口1的序列)
    ResetFakeSvcData();
    g_fakeSvcData.stats.totalAllocCount = 101;
    g_fakeSvcData.stats.totalAllocBytes = 101000;
    g_fakeSvcData.stats.seriesStartTsNs = 300;
    g_fakeSvcData.stats.seriesBeatIntervalNs = 1000000000ULL;
    FakeStackRow stack5b;
    stack5b.stackId = 5;
    stack5b.allocCount = 101;
    stack5b.allocBytes = 101000;
    stack5b.unfreedCount = 101;
    stack5b.unfreedBytes = 101000;
    stack5b.maxBlockSize = 1000;
    stack5b.maxAllocTsNs = 390;
    g_fakeSvcData.stacks.push_back(stack5b);
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 51, 51000});
    g_fakeSvcData.buckets.push_back(FakeBucket{256, 1024, 50, 50000});
    Dispatch(CreateStageStart(pid, 2, 300));
    Dispatch(CreateStageEnd(pid, 2, 400));

    const auto& ws = HostLeakAnalyzer::GetInstance().windows_.at(pid);
    EXPECT_TRUE(ws.series.empty());  // 开窗清零,序列不跨窗
    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_2.txt");
    // 无序列→G/P=0中性,LSI由R/S/E支撑(56=100×(0.21R+0.12S+0.29E),
    // R=1.0/S=0.5/E=1.0);若窗口1序列泄漏,此处会得G=1→LSI 82
    EXPECT_NE(text.find("1. [growth_watch] LSI 56"), std::string::npos);
    EXPECT_NE(text.find("degraded no_series"), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "1");
    RemoveReportFiles(REPORT_DIR, "2");
}

// UT-IA1: 窗口关着→"no active window"(false);不查IsTracingEnabled(窗口状态只由
// STAGE事件驱动,分析器windows_是唯一真相)
TEST_F(HostLeakAnalyzerTest, interim_no_window)
{
    std::string text;
    EXPECT_FALSE(HostLeakAnalyzer::GetInstance().QueryInterimOverview(text));
    EXPECT_EQ(text, "no active window");
}

// UT-IA2: 窗口开着→中间快照渲染(display host_leak summary)。构造复用UT-CA3
// (栈5=常驻early 1MiB平稳 + 栈6=持续增长LSI 82),interimStats带frozenSkip与
// snapTsNs。验证:Window行interim标注/冻结标注/疑似榜/常驻子块(interim截断文案
// "shown of")/拍数按snapTs折算(51拍)/不写报告文件不写CSV
TEST_F(HostLeakAnalyzerTest, interim_snapshot_golden)
{
    const uint64_t pid = 1234;
    const uint64_t ns = 1000000000ULL;
    const uint64_t windowStart = 100 * ns;
    const uint64_t snapTs = 150 * ns;  // 快照时刻(窗口中途)
    // interim stats: 全局合计(派生口径)+冻结跳过计数+快照时刻+序列锚点
    MsmemscopeInterimStats& st = g_fakeSvcData.interimStats;
    st.liveBlockCount = 102;
    st.totalAllocCount = 102 + 3;  // 冻结期3次alloc计入totalAlloc
    st.totalAllocBytes = 101000 + 1048576 + 330;
    st.totalFreedCount = 2;
    st.totalFreedBytes = 330;
    st.frozenSkipAllocCount = 3;
    st.frozenSkipAllocBytes = 330;
    st.snapTsNs = snapTs;
    st.seriesStartTsNs = windowStart;
    st.seriesBeatIntervalNs = ns;
    st.sampleRate = 1;
    // 栈5:窗口前1%分配的1MiB常驻池(序列平稳G≈0)→常驻early(同UT-CA3构造)
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 1;
    stack5.allocBytes = 1048576;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 1048576;
    stack5.maxBlockSize = 1048576;
    stack5.maxAllocTsNs = windowStart + ns;
    stack5.frameDesc = "pool\n";
    g_fakeSvcData.stacks.push_back(stack5);
    for (uint32_t b = 0; b <= 100; ++b)
    {
        g_fakeSvcData.series.push_back(FakeSeriesRow{5, b, b < 5 ? 0 : 1048576, b < 5 ? 0 : 1, 0});
    }
    // 栈6:持续增长泄漏(每拍+1000B×101拍)→LSI=82疑似
    FakeStackRow stack6;
    stack6.stackId = 6;
    stack6.allocCount = 101;
    stack6.allocBytes = 101000;
    stack6.unfreedCount = 101;
    stack6.unfreedBytes = 101000;
    stack6.maxBlockSize = 1000;
    stack6.maxAllocTsNs = snapTs - ns;
    stack6.frameDesc = "leak\n";
    g_fakeSvcData.stacks.push_back(stack6);
    for (uint32_t b = 0; b <= 100; ++b)
    {
        g_fakeSvcData.series.push_back(FakeSeriesRow{6, b, 1000ULL * (b + 1), b + 1, 0});
    }
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 51, 51000});
    g_fakeSvcData.buckets.push_back(FakeBucket{256, 1024, 50, 50000});
    g_fakeSvcData.buckets.push_back(FakeBucket{1048576, UINT64_MAX, 1, 1048576});

    Dispatch(CreateStageStart(pid, 3, windowStart));
    std::string text;
    ASSERT_TRUE(HostLeakAnalyzer::GetInstance().QueryInterimOverview(text));

    // Window行interim标注(时长=快照时刻-窗口起点,50s)
    EXPECT_NE(text.find("(interim snapshot, window open)"), std::string::npos);
    EXPECT_NE(text.find("duration: 50s"), std::string::npos);
    // 冻结标注(冻结期alloc只统计不入块表)
    EXPECT_NE(text.find("Snapshot freeze: 3 allocations / 330B during snapshot not captured in block table "
                       "(stats include them)"),
              std::string::npos);
    // Tracked(interim派生口径)
    EXPECT_NE(text.find("Tracked: 105 allocations / 1149906B allocated; 2 freed / 330B"), std::string::npos);
    // 疑似榜与常驻子块(interim截断文案"shown of";常驻不占TOP N名额)
    EXPECT_NE(text.find("1. [suspected_leak] LSI 82"), std::string::npos);
    EXPECT_NE(text.find("Resident baselines (1 shown of 1 stacks, 1MiB;"), std::string::npos);
    EXPECT_NE(text.find("1. [resident/early] LSI 31"), std::string::npos);
    // 拍数按快照时刻折算: (150s-100s)/1s+1=51拍
    EXPECT_NE(text.find("beats=51(50s, 1s/beat; interim snapshot at "), std::string::npos);
    // 中间概览不落盘: 不写leak_overview与block_detail
    EXPECT_FALSE(FileExists(REPORT_DIR + "/leak_overview_3.txt"));
    EXPECT_EQ(text.find("addr,size,alloc_ts"), std::string::npos);
}

// UT-IA3: 窗口开着但钩子未装配(bind未执行,SetHostMemSvcForTest(nullptr))→
// 快照不可得:退化概览标注,窗口状态不被破坏(后续闭窗仍正常)
TEST_F(HostLeakAnalyzerTest, interim_snapshot_unbound)
{
    const uint64_t pid = 1234;
    FakeReport().SetHostMemSvcForTest(nullptr);  // 未装配态(与SetUp注入相对)
    Dispatch(CreateStageStart(pid, 4, 100));
    std::string text;
    ASSERT_TRUE(HostLeakAnalyzer::GetInstance().QueryInterimOverview(text));
    EXPECT_NE(text.find("Snapshot: unavailable (host hook not bound / query failed)"), std::string::npos);
    EXPECT_NE(text.find("(interim snapshot, window open; snapshot unavailable)"), std::string::npos);
    // 窗口仍开:后续闭窗拉快照出报告不受影响
    FakeReport().SetHostMemSvcForTest(&g_fakeSvc);
    Dispatch(CreateStageEnd(pid, 4, 200));
    EXPECT_FALSE(HostLeakAnalyzer::GetInstance().windows_.at(pid).open);
    RemoveReportFiles(REPORT_DIR, "4");
}

// UT-A11: 落盘文件生成信息直接打屏(9fe4b0e)——WriteWindowReport 生成
// leak_overview_*.txt 与 block_detail_*.csv 时经 std::cout 打屏(打屏=原有
// 可观测性,Log::Printf 仅落日志文件不落 stdout),捕获 stdout 断言三条打屏行
TEST_F(HostLeakAnalyzerTest, file_generation_prints_to_stdout)
{
    const uint64_t pid = 1234;
    // 最小窗口状态(白盒构造,仿 UT-A9):仅注入一块存活块——overview 总泄漏量
    // 按块明细回推、detail CSV 因 blocks 非空而生成,三条打屏行全触发
    auto& ws = HostLeakAnalyzer::GetInstance().windows_[pid];
    ws.open = false;
    ws.stageId = 10;
    ws.startTs = 100;
    ws.endTs = 200;
    ws.statsAvailable = false;
    HostLeakAnalyzer::LiveBlock block;
    block.addr = 0x1000;
    block.size = 4096;
    block.allocTs = 120;
    block.stackId = 1;
    ws.blocks.push_back(block);

    // 重定向 stdout 捕获打屏输出,调用完成后立即恢复(恢复后再断言)
    std::ostringstream captured;
    std::streambuf* oldBuf = std::cout.rdbuf(captured.rdbuf());
    HostLeakAnalyzer::GetInstance().WriteWindowReport(pid, ws, false);
    std::cout.rdbuf(oldBuf);

    const std::string out = captured.str();
    EXPECT_NE(out.find("[msmemscope] Info: Host leak overview report created: "), std::string::npos);
    EXPECT_NE(out.find("leak_overview_10.txt"), std::string::npos);
    EXPECT_NE(out.find("[msmemscope] Info: Host leak block detail report created: "), std::string::npos);
    EXPECT_NE(out.find("block_detail_10.csv"), std::string::npos);
    EXPECT_NE(out.find("[msmemscope] Info: Host leak report generated: "), std::string::npos);
    // 打屏对应的落盘文件确实生成
    EXPECT_TRUE(FileExists(REPORT_DIR + "/leak_overview_10.txt"));
    EXPECT_TRUE(FileExists(REPORT_DIR + "/block_detail_10.csv"));
    RemoveReportFiles(REPORT_DIR, "10");
}

// ===========================================================================
// 覆盖率补强用例(解析容错/退化渲染/异常写盘/析构兜底/中间概览未覆盖分支)
// 分析器为进程内单例:用例内各自构造所需状态,结束后清理环境变量、特殊目录与
// 报告文件(SetUp/TearDown兜底),不依赖其他用例的执行次序
// ===========================================================================

namespace
{
// 建单层目录(已存在视为成功);调用方需保证父目录已存在
bool EnsureDir(const std::string& path)
{
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
    {
        return false;
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// 最小可出报告的窗口状态(白盒直调WriteWindowReport):一个候选栈行+一个大小桶。
// statsAvailable=false时健康度章节走Snapshot unavailable,不影响总泄漏量章节
HostLeakAnalyzer::WindowState MakeMinimalWindow(uint64_t stageId, uint64_t startTs, uint64_t endTs)
{
    HostLeakAnalyzer::WindowState ws;
    ws.stageId = stageId;
    ws.startTs = startTs;
    ws.endTs = endTs;
    HostLeakAnalyzer::StackRow row;
    row.stackId = 5;
    row.allocCount = 1;
    row.allocBytes = 100;
    row.unfreedCount = 1;
    row.unfreedBytes = 100;
    row.maxBlockSize = 100;
    row.maxAllocTsNs = endTs > 0 ? endTs - 1 : 0;
    row.frameDesc = "minimal\n";
    ws.stacks.push_back(row);
    HostLeakAnalyzer::SizeBucket bucket;
    bucket.rangeLow = 0;
    bucket.rangeHigh = 256;
    bucket.blockCount = 1;
    bucket.blockBytes = 100;
    ws.buckets.push_back(bucket);
    return ws;
}

// 大小排布行精确文本(与WriteWindowReport/RenderInterimOverview同版式:
// 范围左对齐16宽,计数/字节右对齐10/12宽)
std::string SizeDistRow(const std::string& range, uint64_t count, uint64_t bytes)
{
    std::ostringstream row;
    row << std::left << std::setw(16) << range << std::right << std::setw(10) << count << std::setw(12) << bytes;
    return row.str();
}
}  // namespace

// COV-H1: 非法事件输入——空事件/非SYSTEM事件/SYSTEM非STAGE子类型直接返回不建窗;
// STAGE_START的stageId文本解析失败(std::stoull抛异常)按0处理,窗口照常开启
TEST_F(HostLeakAnalyzerTest, ignored_events_and_malformed_stage_name)
{
    const uint64_t pid = 1234;
    auto& analyzer = HostLeakAnalyzer::GetInstance();

    std::shared_ptr<EventBase> nullEvent;
    analyzer.EventHandle(nullEvent, nullptr);

    auto memEvent = std::make_shared<MemoryEvent>();
    memEvent->eventType = EventBaseType::MALLOC;
    std::shared_ptr<EventBase> memBase = memEvent;
    analyzer.EventHandle(memBase, nullptr);

    auto traceEvent = std::make_shared<SystemEvent>();
    traceEvent->eventType = EventBaseType::SYSTEM;
    traceEvent->eventSubType = EventSubType::TRACE_START;
    std::shared_ptr<EventBase> traceBase = traceEvent;
    analyzer.EventHandle(traceBase, nullptr);
    EXPECT_TRUE(analyzer.windows_.empty());

    auto start = CreateStageStart(pid, 1, 100);
    start->name = "not-a-number";
    Dispatch(start);
    ASSERT_EQ(analyzer.windows_.size(), 1u);
    EXPECT_EQ(analyzer.windows_.at(pid).stageId, 0u);
    EXPECT_TRUE(analyzer.windows_.at(pid).open);
    // 收尾:空窗口闭窗不出报告(by stageId=0)
    Dispatch(CreateStageEnd(pid, 0, 200));
    EXPECT_FALSE(analyzer.windows_.at(pid).open);
}

// COV-H2: 环境变量解析容错——非数字(std::stoull异常)与"0"均回落默认值
// (TOP N=10、键深K=20),报告头部按默认值渲染
TEST_F(HostLeakAnalyzerTest, env_parse_failure_and_zero_use_defaults)
{
    const uint64_t pid = 1234;
    setenv("MSMEMSCOPE_HOSTMEM_TOP_N", "not-a-number", 1);
    setenv("MSMEMSCOPE_HOSTMEM_STACK_KEY_FRAMES", "not-a-number", 1);
    g_fakeSvcData.stats.totalAllocCount = 1;
    g_fakeSvcData.stats.totalAllocBytes = 100;
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 1;
    stack5.allocBytes = 100;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 100;
    stack5.maxBlockSize = 100;
    stack5.frameDesc = "f\n";
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 1, 100});
    Dispatch(CreateStageStart(pid, 1, 100));
    Dispatch(CreateStageEnd(pid, 1, 200));
    std::string text = ReadAllText(REPORT_DIR + "/leak_overview_1.txt");
    EXPECT_NE(text.find("--- TOP 10 Leak Sites (by LSI desc) ---"), std::string::npos);
    EXPECT_NE(text.find("key depth K=20"), std::string::npos);

    // "0"视为未配置,同走默认值
    setenv("MSMEMSCOPE_HOSTMEM_TOP_N", "0", 1);
    unsetenv("MSMEMSCOPE_HOSTMEM_STACK_KEY_FRAMES");
    Dispatch(CreateStageStart(pid, 2, 300));
    Dispatch(CreateStageEnd(pid, 2, 400));
    text = ReadAllText(REPORT_DIR + "/leak_overview_2.txt");
    EXPECT_NE(text.find("--- TOP 10 Leak Sites (by LSI desc) ---"), std::string::npos);
    EXPECT_NE(text.find("key depth K=20"), std::string::npos);
    unsetenv("MSMEMSCOPE_HOSTMEM_TOP_N");
    RemoveReportFiles(REPORT_DIR, "1");
    RemoveReportFiles(REPORT_DIR, "2");
}

// COV-H3: 析构兜底形态的报告——endTs=0(结束时刻未知)+atExit=true:Window行
// 标注进程退出前未闭窗/快照不完整,另出"闭窗未发生"Note,开窗前free小节标注
// 快照不可得(不猜零值)
TEST_F(HostLeakAnalyzerTest, at_exit_report_marks_window_unknown_and_incomplete)
{
    const uint64_t pid = 1234;
    HostLeakAnalyzer::WindowState ws;
    ws.stageId = 3;
    ws.startTs = 100;
    ws.endTs = 0;
    ws.statsAvailable = true;
    ws.stats.totalAllocCount = 1;
    ws.stats.totalAllocBytes = 64;

    HostLeakAnalyzer::GetInstance().WriteWindowReport(pid, ws, true);
    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_3.txt");
    EXPECT_NE(text.find("Window: 100 -> unknown (process exit before window closed; snapshot incomplete)"),
              std::string::npos);
    EXPECT_NE(text.find("Note: window closed at process exit without snapshot; unfreed details unavailable"),
              std::string::npos);
    EXPECT_NE(text.find("(snapshot unavailable)"), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "3");
}

// COV-H4: 析构兜底报告(白盒构造独立分析器实例)——仅残留open窗口出退化报告,
// 已闭窗口跳过;析构退订了同一SubscriberId,本用例负责恢复单例订阅并验证
// 事件状态机仍可用
TEST_F(HostLeakAnalyzerTest, destructor_fallback_reports_open_window_only)
{
    g_fakeSvcData.stats.totalAllocCount = 2;
    g_fakeSvcData.stats.totalAllocBytes = 200;
    {
        HostLeakAnalyzer probe;
        HostLeakAnalyzer::WindowState openWs;
        openWs.open = true;
        openWs.stageId = 4;
        openWs.startTs = 100;
        probe.windows_[100] = openWs;
        HostLeakAnalyzer::WindowState closedWs;
        closedWs.open = false;
        closedWs.stageId = 5;
        closedWs.startTs = 50;
        closedWs.endTs = 60;
        HostLeakAnalyzer::SizeBucket bucket;
        bucket.rangeLow = 0;
        bucket.rangeHigh = 256;
        bucket.blockCount = 1;
        bucket.blockBytes = 64;
        closedWs.buckets.push_back(bucket);
        probe.windows_[200] = closedWs;
    }
    // probe析构已按SubscriberId退订(同一id即单例订阅),恢复单例订阅
    HostLeakAnalyzer::GetInstance().Subscribe();

    EXPECT_TRUE(FileExists(REPORT_DIR + "/leak_overview_4.txt"));
    EXPECT_FALSE(FileExists(REPORT_DIR + "/leak_overview_5.txt")) << "closed window must be skipped by fallback";
    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_4.txt");
    EXPECT_NE(text.find("Window: 100 -> unknown (process exit before window closed; snapshot incomplete)"),
              std::string::npos);
    // 订阅已恢复:单例仍能驱动STAGE状态机
    Dispatch(CreateStageStart(777, 6, 100));
    EXPECT_TRUE(HostLeakAnalyzer::GetInstance().windows_.at(777).open);
    Dispatch(CreateStageEnd(777, 6, 200));
    EXPECT_FALSE(HostLeakAnalyzer::GetInstance().windows_.at(777).open);
    RemoveReportFiles(REPORT_DIR, "4");
}

// COV-H5: 报告目录建链失败(MakeDir=false,工程目录被同名普通文件占位)——
// 写报告前中止:概览/明细/生成三行打屏均不出现,也无任何文件产出
TEST_F(HostLeakAnalyzerTest, report_mkdir_failure_aborts_without_file)
{
    const std::string blockedDir = "./testmsmemscope_blocked_ut_" + std::to_string(static_cast<long long>(getpid()));
    rmdir(blockedDir.c_str());  // 清理上次异常退出可能残留的目录
    {
        std::ofstream blocker(blockedDir);
        ASSERT_TRUE(blocker.is_open());
        blocker << "blocked";
    }
    Utility::FileCreateManager::GetInstance(blockedDir).SetProjectDir(blockedDir);
    HostLeakAnalyzer::WindowState ws = MakeMinimalWindow(1, 100, 200);
    std::ostringstream captured;
    std::streambuf* oldBuf = std::cout.rdbuf(captured.rdbuf());
    HostLeakAnalyzer::GetInstance().WriteWindowReport(1234, ws, false);
    std::cout.rdbuf(oldBuf);
    Utility::FileCreateManager::GetInstance(blockedDir).SetProjectDir("./testmsmemscope");

    EXPECT_EQ(captured.str().find("Host leak overview report created"), std::string::npos);
    EXPECT_EQ(captured.str().find("Host leak block detail report created"), std::string::npos);
    EXPECT_EQ(captured.str().find("Host leak report generated"), std::string::npos);
    EXPECT_FALSE(FileExists(blockedDir + "/host_leak/leak_overview_1.txt"));
    std::remove(blockedDir.c_str());
}

// COV-H6: 概览文件打开失败(报告文件路径被同名目录占位)——直接返回:
// 不打"created"/"generated",也不继续写CSV
TEST_F(HostLeakAnalyzerTest, overview_open_failure_aborts_report)
{
    ASSERT_TRUE(EnsureDir("./testmsmemscope"));
    ASSERT_TRUE(EnsureDir(REPORT_DIR));
    const std::string overviewPath = REPORT_DIR + "/leak_overview_1.txt";
    std::remove(overviewPath.c_str());
    ASSERT_TRUE(EnsureDir(overviewPath));
    HostLeakAnalyzer::WindowState ws = MakeMinimalWindow(1, 100, 200);
    std::ostringstream captured;
    std::streambuf* oldBuf = std::cout.rdbuf(captured.rdbuf());
    HostLeakAnalyzer::GetInstance().WriteWindowReport(1234, ws, false);
    std::cout.rdbuf(oldBuf);

    EXPECT_EQ(captured.str().find("Host leak overview report created"), std::string::npos);
    EXPECT_EQ(captured.str().find("Host leak report generated"), std::string::npos);
    EXPECT_EQ(rmdir(overviewPath.c_str()), 0);
}

// COV-H7: 概览写盘失败(报告文件指向/dev/full)——打开成功(created打屏),
// flush失败仅落告警不中断后续流程(generated仍打屏)
TEST_F(HostLeakAnalyzerTest, overview_write_failure_keeps_generation)
{
    if (access("/dev/full", W_OK) != 0)
    {
        GTEST_SKIP() << "/dev/full not writable in current environment";
    }
    ASSERT_TRUE(EnsureDir("./testmsmemscope"));
    ASSERT_TRUE(EnsureDir(REPORT_DIR));
    const std::string overviewPath = REPORT_DIR + "/leak_overview_1.txt";
    rmdir(overviewPath.c_str());  // 清掉上次异常残留(目录/文件/链接)
    std::remove(overviewPath.c_str());
    ASSERT_EQ(symlink("/dev/full", overviewPath.c_str()), 0);
    HostLeakAnalyzer::WindowState ws = MakeMinimalWindow(1, 100, 200);
    std::ostringstream captured;
    std::streambuf* oldBuf = std::cout.rdbuf(captured.rdbuf());
    HostLeakAnalyzer::GetInstance().WriteWindowReport(1234, ws, false);
    std::cout.rdbuf(oldBuf);

    EXPECT_NE(captured.str().find("Host leak overview report created"), std::string::npos);
    EXPECT_NE(captured.str().find("Host leak report generated"), std::string::npos);
    EXPECT_EQ(std::remove(overviewPath.c_str()), 0);
}

// COV-H8: 明细CSV打开失败(CSV路径被同名目录占位)——概览正常产出的同时
// 静默跳过明细(无明细created打屏),generated仍打屏
TEST_F(HostLeakAnalyzerTest, detail_open_failure_skips_csv)
{
    ASSERT_TRUE(EnsureDir("./testmsmemscope"));
    ASSERT_TRUE(EnsureDir(REPORT_DIR));
    const std::string detailPath = REPORT_DIR + "/block_detail_1.csv";
    std::remove(detailPath.c_str());
    ASSERT_TRUE(EnsureDir(detailPath));
    HostLeakAnalyzer::WindowState ws = MakeMinimalWindow(1, 100, 200);
    HostLeakAnalyzer::LiveBlock block;
    block.addr = 0x1000;
    block.size = 100;
    block.allocTs = 150;
    block.stackId = 5;
    ws.blocks.push_back(block);
    std::ostringstream captured;
    std::streambuf* oldBuf = std::cout.rdbuf(captured.rdbuf());
    HostLeakAnalyzer::GetInstance().WriteWindowReport(1234, ws, false);
    std::cout.rdbuf(oldBuf);

    EXPECT_NE(captured.str().find("Host leak overview report created"), std::string::npos);
    EXPECT_EQ(captured.str().find("Host leak block detail report created"), std::string::npos);
    EXPECT_NE(captured.str().find("Host leak report generated"), std::string::npos);
    EXPECT_EQ(rmdir(detailPath.c_str()), 0);
    RemoveReportFiles(REPORT_DIR, "1");
}

// COV-H9: 明细CSV写盘失败(CSV指向/dev/full)——打开与打屏成功,写失败只落
// 告警,概览生成流程走完
TEST_F(HostLeakAnalyzerTest, detail_write_failure_keeps_generation)
{
    if (access("/dev/full", W_OK) != 0)
    {
        GTEST_SKIP() << "/dev/full not writable in current environment";
    }
    ASSERT_TRUE(EnsureDir("./testmsmemscope"));
    ASSERT_TRUE(EnsureDir(REPORT_DIR));
    const std::string detailPath = REPORT_DIR + "/block_detail_1.csv";
    rmdir(detailPath.c_str());  // 清掉上次异常残留(目录/文件/链接)
    std::remove(detailPath.c_str());
    ASSERT_EQ(symlink("/dev/full", detailPath.c_str()), 0);
    HostLeakAnalyzer::WindowState ws = MakeMinimalWindow(1, 100, 200);
    HostLeakAnalyzer::LiveBlock block;
    block.addr = 0x1000;
    block.size = 100;
    block.allocTs = 150;
    block.stackId = 5;
    ws.blocks.push_back(block);
    std::ostringstream captured;
    std::streambuf* oldBuf = std::cout.rdbuf(captured.rdbuf());
    HostLeakAnalyzer::GetInstance().WriteWindowReport(1234, ws, false);
    std::cout.rdbuf(oldBuf);

    EXPECT_NE(captured.str().find("Host leak overview report created"), std::string::npos);
    EXPECT_NE(captured.str().find("Host leak block detail report created"), std::string::npos);
    EXPECT_NE(captured.str().find("Host leak report generated"), std::string::npos);
    EXPECT_EQ(std::remove(detailPath.c_str()), 0);
    RemoveReportFiles(REPORT_DIR, "1");
}

// COV-H10: CSV的1MB堆缓冲攒行+整块write——4万条明细(单行无内嵌换行)足以
// 触发中途整块刷写:行数=块数+表头,首行为地址升序的小地址块
TEST_F(HostLeakAnalyzerTest, csv_buffer_flush_spans_multiple_writes)
{
    const uint64_t pid = 1234;
    const size_t blockCount = 40000;
    g_fakeSvcData.stats.totalAllocCount = blockCount;
    g_fakeSvcData.stats.totalAllocBytes = blockCount * 100;
    FakeStackRow stack1;
    stack1.stackId = 1;
    stack1.allocCount = blockCount;
    stack1.allocBytes = blockCount * 100;
    stack1.unfreedCount = blockCount;
    stack1.unfreedBytes = blockCount * 100;
    stack1.maxBlockSize = 100;
    stack1.frameDesc = "frame";  // 无换行:CSV行数=块数+表头
    g_fakeSvcData.stacks.push_back(stack1);
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, blockCount, blockCount * 100});
    for (size_t i = 0; i < blockCount; ++i)
    {
        g_fakeSvcData.blocks.push_back(FakeBlock{0x1000 + i, 100, 1000 + i, 1});
    }
    Dispatch(CreateStageStart(pid, 1, 100));
    Dispatch(CreateStageEnd(pid, 1, 200));

    const std::string csv = ReadAllText(REPORT_DIR + "/block_detail_1.csv");
    EXPECT_GT(csv.size(), static_cast<size_t>(1u << 20)) << "csv must exceed the 1MB write buffer";
    EXPECT_EQ(static_cast<size_t>(std::count(csv.begin(), csv.end(), '\n')), blockCount + 1);
    EXPECT_NE(csv.find("0x0000000000001000,100,1000,\"frame\",\"\""), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "1");
}

// COV-H11: 栈快照缺失、仅逐块明细的退化场景——TOP候选行由块明细按stackId
// 聚合回推(未释放量/块数/最大块),唯一栈数同样由合成行回推
TEST_F(HostLeakAnalyzerTest, synthesized_rows_from_block_only_snapshot)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.totalAllocCount = 3;
    g_fakeSvcData.stats.totalAllocBytes = 600;
    g_fakeSvcData.blocks.push_back(FakeBlock{0x1000, 100, 1100, 3});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x2000, 300, 1200, 3});
    g_fakeSvcData.blocks.push_back(FakeBlock{0x3000, 200, 1300, 4});
    Dispatch(CreateStageStart(pid, 1, 100));
    Dispatch(CreateStageEnd(pid, 1, 200));

    const auto& ws = HostLeakAnalyzer::GetInstance().windows_.at(pid);
    EXPECT_TRUE(ws.stacks.empty());
    ASSERT_EQ(ws.blocks.size(), 3u);

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_1.txt");
    // 唯一栈数:栈表缺失时由逐块明细回推(2个stackId)
    EXPECT_NE(text.find("Distinct stacks: 2 (key depth K=20, category semantics)"), std::string::npos);
    // 总泄漏量:桶缺失时以块明细回推
    EXPECT_NE(text.find("Total unfreed: 600 bytes in 3 blocks (avg 200B, max 300B)"), std::string::npos);
    // 合成行按stackId聚合:stack3两块400B、stack4一块200B
    EXPECT_NE(text.find("unfreed 400B(2 blocks)"), std::string::npos);
    EXPECT_NE(text.find("stack 0x3"), std::string::npos);
    EXPECT_NE(text.find("stack 0x4"), std::string::npos);
    EXPECT_TRUE(FileExists(REPORT_DIR + "/block_detail_1.csv"));
    RemoveReportFiles(REPORT_DIR, "1");
}

// COV-H12: 溢出通道与开窗前free标注——溢出转出/逆向修正、开窗前free统计行,
// 以及开窗前free大小排布的行渲染(含范围文本与列宽)
TEST_F(HostLeakAnalyzerTest, overflow_and_prewindow_distribution_rows)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.totalAllocCount = 1;
    g_fakeSvcData.stats.totalAllocBytes = 1000;
    g_fakeSvcData.stats.overflowAllocCount = 5;
    g_fakeSvcData.stats.overflowAllocBytes = 500;
    g_fakeSvcData.stats.overflowFreedCount = 2;
    g_fakeSvcData.stats.overflowFreedBytes = 200;
    g_fakeSvcData.stats.preWindowFreeCount = 2;
    g_fakeSvcData.stats.preWindowFreeBytes = 128;
    g_fakeSvcData.stats.sampleRate = 1;
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 1;
    stack5.allocBytes = 1000;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 100;
    stack5.maxBlockSize = 100;
    stack5.frameDesc = "f\n";
    g_fakeSvcData.stacks.push_back(stack5);
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 1, 100});
    g_fakeSvcData.preWindowBuckets.push_back(FakeBucket{64, 128, 2, 128});
    Dispatch(CreateStageStart(pid, 1, 100));
    Dispatch(CreateStageEnd(pid, 1, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_1.txt");
    EXPECT_NE(text.find("Overflow channel: 5 allocations / 500B diverted; 2 freed / 200B (reverse-corrected)"),
              std::string::npos);
    EXPECT_NE(text.find("Pre-window frees: 2 / 128B (allocated before window, not in ledger)"), std::string::npos);
    EXPECT_NE(text.find("--- Pre-Window Free Size Distribution ---"), std::string::npos);
    EXPECT_NE(text.find(SizeDistRow("[64, 128)", 2, 128)), std::string::npos);
    EXPECT_EQ(text.find("(no pre-window frees)"), std::string::npos) << "non-empty buckets must render rows";
    RemoveReportFiles(REPORT_DIR, "1");
}

// COV-H13: 无候选栈占位行+序列拍数兜底——序列锚点缺失(seriesStartTsNs=0)时
// 拍数按已交付序列最大拍号回推;栈表无未释放行时TOP N节输出占位行
TEST_F(HostLeakAnalyzerTest, top_placeholder_with_series_beat_fallback)
{
    const uint64_t pid = 1234;
    g_fakeSvcData.stats.totalAllocCount = 0;
    g_fakeSvcData.stats.totalAllocBytes = 0;
    g_fakeSvcData.series.push_back(FakeSeriesRow{9, 0, 10, 1, 0});
    g_fakeSvcData.series.push_back(FakeSeriesRow{9, 3, 20, 1, 0});
    g_fakeSvcData.series.push_back(FakeSeriesRow{9, 7, 30, 1, 0});
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 0, 0});
    Dispatch(CreateStageStart(pid, 1, 100));
    Dispatch(CreateStageEnd(pid, 1, 200));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_1.txt");
    EXPECT_NE(text.find("Series: top-k=256, max-stacks=1024, beats=8(0s, 1s/beat)"), std::string::npos);
    EXPECT_NE(text.find("(no unfreed blocks in this window)"), std::string::npos);
    EXPECT_NE(text.find("Total unfreed: 0 bytes in 0 blocks (avg 0B, max 0B)"), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "1");
}

// COV-H14: 负增长斜率——每拍-1000B的回归结果走FormatSizeAbbrev负号分支,
// growth行渲染"-1000B/beat"(同时验证参与回归拍数pts)
TEST_F(HostLeakAnalyzerTest, negative_growth_rate_sign_format)
{
    const uint64_t pid = 1234;
    const uint64_t ns = 1000000000ULL;
    g_fakeSvcData.stats.totalAllocCount = 21;
    g_fakeSvcData.stats.totalAllocBytes = 20000;
    g_fakeSvcData.stats.seriesStartTsNs = 100 * ns;
    g_fakeSvcData.stats.seriesBeatIntervalNs = ns;
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 21;
    stack5.allocBytes = 20000;
    stack5.freedCount = 20;
    stack5.freedBytes = 10000;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 10000;
    stack5.maxBlockSize = 10000;
    stack5.maxAllocTsNs = 199 * ns;
    stack5.freedLifetimeSumNs = 20 * ns;
    stack5.liveAgeSumNs = 10 * ns;
    stack5.frameDesc = "dec\n";
    g_fakeSvcData.stacks.push_back(stack5);
    for (uint32_t b = 0; b <= 20; ++b)
    {
        // 每拍-1000B:回归斜率-1000B/beat(负值走FormatSizeAbbrev符号分支)
        g_fakeSvcData.series.push_back(FakeSeriesRow{5, b, 100000 - 1000ULL * b, 1, 0});
    }
    g_fakeSvcData.buckets.push_back(FakeBucket{8192, 16384, 1, 10000});
    Dispatch(CreateStageStart(pid, 1, 100 * ns));
    Dispatch(CreateStageEnd(pid, 1, 200 * ns));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_1.txt");
    EXPECT_NE(text.find("growth -1000B/beat  pts 16  degraded -  stack 0x5"), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "1");
}

// COV-H15: 周转形态——unfreed/alloc≤0.3(占比判据满足)但尾部仍以100B/beat上涨
// (cond5不满足)→疑似榜状态追加/turnover后缀
TEST_F(HostLeakAnalyzerTest, turnover_mark_suffix_in_suspect_row)
{
    const uint64_t pid = 1234;
    const uint64_t ns = 1000000000ULL;
    g_fakeSvcData.stats.totalAllocCount = 100;
    g_fakeSvcData.stats.totalAllocBytes = 100000;
    g_fakeSvcData.stats.seriesStartTsNs = 100 * ns;
    g_fakeSvcData.stats.seriesBeatIntervalNs = ns;
    FakeStackRow stack5;
    stack5.stackId = 5;
    stack5.allocCount = 100;
    stack5.allocBytes = 100000;
    stack5.unfreedCount = 1;
    stack5.unfreedBytes = 10000;  // unfreed/alloc=0.1≤0.3(占比判据满足)
    stack5.maxBlockSize = 10000;
    stack5.maxAllocTsNs = 199 * ns;  // 窗口尾部:early条件(2)不满足
    stack5.frameDesc = "buf\n";
    g_fakeSvcData.stacks.push_back(stack5);
    for (uint32_t b = 0; b <= 20; ++b)
    {
        // 全窗缓涨(β=100B/beat>0.05):尾部仍在涨→cond5不满足→turnoverMark
        g_fakeSvcData.series.push_back(FakeSeriesRow{5, b, 1000 + 100ULL * b, 1, 0});
    }
    g_fakeSvcData.buckets.push_back(FakeBucket{8192, 16384, 1, 10000});
    Dispatch(CreateStageStart(pid, 1, 100 * ns));
    Dispatch(CreateStageEnd(pid, 1, 200 * ns));

    const std::string text = ReadAllText(REPORT_DIR + "/leak_overview_1.txt");
    EXPECT_NE(text.find("1. [growth_watch/turnover]"), std::string::npos);
    EXPECT_NE(text.find("degraded -"), std::string::npos);
    RemoveReportFiles(REPORT_DIR, "1");
}

// COV-H16: 中间概览两阶段锁的边界——(a)仅存在已闭窗口:扫描跳过(continue)后
// 按无活动窗口返回;(b)快照交付期间STAGE_END闭窗:复检不通过,不渲染过期快照
TEST_F(HostLeakAnalyzerTest, interim_query_skips_closed_window_and_recheck_race)
{
    const uint64_t pid = 1234;
    auto& analyzer = HostLeakAnalyzer::GetInstance();
    // (a)窗口表只有已闭窗口
    HostLeakAnalyzer::WindowState closed;
    closed.open = false;
    closed.stageId = 1;
    closed.startTs = 10;
    closed.endTs = 20;
    analyzer.windows_[pid] = closed;
    std::string text;
    EXPECT_FALSE(analyzer.QueryInterimOverview(text));
    EXPECT_EQ(text, "no active window");

    // (b)中间快照回调内闭窗:复检失败按无活动窗口处理
    analyzer.windows_.clear();
    Dispatch(CreateStageStart(pid, 1, 100));
    g_closeWindowDuringInterim = pid;
    const bool rendered = analyzer.QueryInterimOverview(text);
    g_closeWindowDuringInterim = 0;
    EXPECT_FALSE(rendered);
    EXPECT_EQ(text, "no active window");
    EXPECT_FALSE(analyzer.windows_.at(pid).open);
}

// COV-H17: 中间快照收集回调——开窗前free桶收集(CollectInterimPreWindowCb)
// 与同栈多拍序列聚合/槽驱逐标注(CollectInterimSeriesCb):渲染侧呈现开窗前
// free行与series_evicted降级
TEST_F(HostLeakAnalyzerTest, interim_query_collects_prewindow_and_evicted_series)
{
    const uint64_t pid = 1234;
    const uint64_t ns = 1000000000ULL;
    Dispatch(CreateStageStart(pid, 1, 100 * ns));
    MsmemscopeInterimStats& st = g_fakeSvcData.interimStats;
    st.totalAllocCount = 2;
    st.totalAllocBytes = 200;
    st.snapTsNs = 150 * ns;
    st.seriesStartTsNs = 100 * ns;
    st.seriesBeatIntervalNs = ns;
    FakeStackRow row;
    row.stackId = 5;
    row.allocCount = 2;
    row.allocBytes = 200;
    row.unfreedCount = 1;
    row.unfreedBytes = 100;
    row.maxBlockSize = 100;
    row.maxAllocTsNs = 149 * ns;
    row.frameDesc = "f\n";
    g_fakeSvcData.stacks.push_back(row);
    g_fakeSvcData.buckets.push_back(FakeBucket{0, 256, 1, 100});
    g_fakeSvcData.preWindowBuckets.push_back(FakeBucket{64, 128, 2, 128});
    g_fakeSvcData.series.push_back(FakeSeriesRow{5, 0, 100, 1, 0x1});  // 槽被驱逐
    g_fakeSvcData.series.push_back(FakeSeriesRow{5, 1, 100, 1, 0});    // 同栈索引命中
    std::string text;
    ASSERT_TRUE(HostLeakAnalyzer::GetInstance().QueryInterimOverview(text));
    EXPECT_NE(text.find("--- Pre-Window Free Size Distribution ---"), std::string::npos);
    EXPECT_NE(text.find(SizeDistRow("[64, 128)", 2, 128)), std::string::npos);
    // 序列槽被驱逐:降级标注series_evicted(该栈仍为候选)
    EXPECT_NE(text.find("degraded series_evicted"), std::string::npos);
}

// COV-H18: 中间概览健康度标注全集合(白盒直调渲染)——未归因/整窗截断复合文本/
// 本次快照降级复合文本/死栈淘汰/溢出通道/开窗前free/采样/块阈值/符号化健康,
// 以及开窗前free分布行、常驻子块排序与周转NOTE(interim版本不引用章节号)
TEST_F(HostLeakAnalyzerTest, interim_render_health_annotations)
{
    const uint64_t pid = 1234;
    const uint64_t ns = 1000000000ULL;
    Config config = MemScope::GetConfig();
    config.blockSizeThreshold = 128;
    ConfigManager::Instance().SetConfig(config);

    HostLeakAnalyzer::InterimCollector ic;
    ic.statsAvailable = true;
    MsmemscopeInterimStats& st = ic.stats;
    st.liveBlockCount = 7;
    st.totalAllocCount = 10;
    st.totalAllocBytes = 100000;
    st.totalFreedCount = 2;
    st.totalFreedBytes = 200;
    st.truncated = 0x7;
    st.snapshotDegraded = 0x7;
    st.evictedStackCount = 3;
    st.evictedAllocCount = 30;
    st.evictedAllocBytes = 3000;
    st.overflowAllocCount = 5;
    st.overflowAllocBytes = 500;
    st.overflowFreedCount = 2;
    st.overflowFreedBytes = 200;
    st.preWindowFreeCount = 4;
    st.preWindowFreeBytes = 65536;
    st.sampleRate = 4;
    st.untrackedCount = 2;
    st.untrackedBytes = 64;
    st.snapTsNs = 150 * ns;
    st.seriesStartTsNs = 100 * ns;
    st.seriesBeatIntervalNs = ns;

    HostLeakAnalyzer::StackRow row5;
    row5.stackId = 5;
    row5.allocCount = 1;
    row5.allocBytes = 1000;
    row5.unfreedCount = 1;
    row5.unfreedBytes = 1000;
    row5.maxBlockSize = 1000;
    row5.maxAllocTsNs = 149 * ns;
    row5.frameDesc = "sym5\n";
    HostLeakAnalyzer::StackRow unknown;
    unknown.stackId = 0;
    unknown.allocCount = 1;
    unknown.allocBytes = 1000;
    unknown.unfreedCount = 1;
    unknown.unfreedBytes = 1000;
    unknown.maxBlockSize = 1000;
    HostLeakAnalyzer::StackRow zero;
    zero.stackId = 7;
    zero.allocCount = 1;
    zero.allocBytes = 500;
    zero.unfreedCount = 0;
    HostLeakAnalyzer::StackRow unresolved;
    unresolved.stackId = 8;
    unresolved.allocCount = 1;
    unresolved.allocBytes = 500;
    unresolved.unfreedCount = 1;
    unresolved.unfreedBytes = 500;
    unresolved.maxBlockSize = 500;
    ic.stacks.push_back(row5);
    ic.stacks.push_back(unknown);
    ic.stacks.push_back(zero);
    ic.stacks.push_back(unresolved);
    HostLeakAnalyzer::SizeBucket bucket;
    bucket.rangeLow = 0;
    bucket.rangeHigh = 256;
    bucket.blockCount = 3;
    bucket.blockBytes = 3000;
    ic.buckets.push_back(bucket);
    HostLeakAnalyzer::SizeBucket pre;
    pre.rangeLow = 1048576;
    pre.rangeHigh = UINT64_MAX;
    pre.blockCount = 4;
    pre.blockBytes = 65536;
    ic.preWindowBuckets.push_back(pre);

    std::string text;
    HostLeakAnalyzer::GetInstance().RenderInterimOverview(pid, 7, 100 * ns, ic, text);

    EXPECT_NE(text.find("Window: 100000000000 -> 150000000000 (duration: 50s) (interim snapshot, window open)"),
              std::string::npos);
    EXPECT_NE(text.find("Tracked: 10 allocations / 100000B allocated; 2 freed / 200B"), std::string::npos);
    EXPECT_NE(text.find("Distinct stacks: 3 (key depth K=20, category semantics)"), std::string::npos);
    EXPECT_NE(text.find("Unattributed: 1 blocks / 1000B (unknown bucket, 33%)"), std::string::npos);
    EXPECT_NE(text.find("Truncated: block table full (allocations -> overflow channel) | "
                        "stack table (dead-stack recycling active, no reclaimable stack at full) | "
                        "overflow channel full (recording stopped at 7 live blocks) "
                        "[window data incomplete: not a leak conclusion]"),
              std::string::npos);
    EXPECT_NE(text.find("Snapshot degraded: block-table read (prefix data) | stack-table read (partial attribution) | "
                        "series/pre-window read (partial) (this snapshot only; window continues recording)"),
              std::string::npos);
    EXPECT_NE(text.find("Evicted: 3 stacks recycled (30 allocs / 3000B folded to unknown bucket)"), std::string::npos);
    EXPECT_NE(text.find("Overflow channel: 5 allocations / 500B diverted; 2 freed / 200B (reverse-corrected)"),
              std::string::npos);
    EXPECT_NE(text.find("Pre-window frees: 4 / 65536B (allocated before window, not in ledger)"), std::string::npos);
    EXPECT_NE(text.find("Sampling: 1/4 (sampled view)"), std::string::npos);
    EXPECT_NE(text.find("Size threshold: 128B (untracked: 2 allocations / 64B)"), std::string::npos);
    EXPECT_NE(text.find("Symbolized: 1/2 stacks (unresolved: 1)"), std::string::npos);
    EXPECT_NE(text.find(SizeDistRow("[1M, +inf)", 4, 65536)), std::string::npos);
    // 常驻子块(未知桶+未符号化栈均满足early判据,总1.46KiB)
    EXPECT_NE(text.find("Resident baselines (2 shown of 2 stacks, 1.46KiB;"), std::string::npos);
    EXPECT_NE(text.find("NOTE: 64KiB pre-window allocations were freed within this window (cache turnover pattern); "
                        "resident classification includes the unfreed/alloc turnover criterion"),
              std::string::npos);
    EXPECT_EQ(text.find("§3.1.4"), std::string::npos) << "interim NOTE must not cite closed-report section ids";
    EXPECT_NE(text.find("Series: top-k=256, max-stacks=1024, beats=51(50s, 1s/beat; interim snapshot at "),
              std::string::npos);
}

// COV-H19: 中间概览退化形态(白盒直调渲染)——snapTs不可得时Window行标注快照
// 不可用;序列锚点缺失时拍数按已交付序列最大拍号回推;空桶/空开窗前free桶/
// 无候选栈分别输出占位行
TEST_F(HostLeakAnalyzerTest, interim_render_series_fallback_and_empty_rows)
{
    const uint64_t pid = 1234;
    HostLeakAnalyzer::InterimCollector ic;
    ic.statsAvailable = true;
    ic.stats.totalAllocCount = 1;
    ic.stats.totalAllocBytes = 100;
    ic.stats.snapTsNs = 0;  // 快照时刻不可得
    ic.stats.seriesStartTsNs = 0;
    ic.stats.seriesBeatIntervalNs = 0;
    StackSeries series;
    series.stackId = 9;
    SeriesPoint p0;
    p0.beat = 0;
    p0.liveBytes = 10;
    series.points.push_back(p0);
    SeriesPoint p3;
    p3.beat = 3;
    p3.liveBytes = 20;
    series.points.push_back(p3);
    SeriesPoint p7;
    p7.beat = 7;
    p7.liveBytes = 30;
    series.points.push_back(p7);
    ic.series.push_back(series);

    std::string text;
    HostLeakAnalyzer::GetInstance().RenderInterimOverview(pid, 8, 100, ic, text);
    EXPECT_NE(text.find("Window: 100 -> unknown (interim snapshot, window open; snapshot unavailable)"),
              std::string::npos);
    // 序列锚点缺失:拍数按已交付序列最大拍号回推(7+1)
    EXPECT_NE(text.find("Series: top-k=256, max-stacks=1024, beats=8(0s, 1s/beat; interim snapshot at 0)"),
              std::string::npos);
    // 无候选栈:TOP占位行;空桶:大小排布占位行;空开窗前free桶:无数据标注
    EXPECT_NE(text.find("(no unfreed blocks in this window)"), std::string::npos);
    EXPECT_NE(text.find("(no unfreed blocks)\n"), std::string::npos);
    EXPECT_NE(text.find("(no pre-window frees)"), std::string::npos);
    EXPECT_NE(text.find("Total unfreed: 0 bytes in 0 blocks (avg 0B, max 0B)"), std::string::npos);
}

// COV-H20: 中间概览常驻子块——未释放量相同走stackId次键(平局),不同走字节
// 降序分支;TOP N截断常驻子块并提示剩余数量
TEST_F(HostLeakAnalyzerTest, interim_render_resident_sort_and_topn_cap)
{
    const uint64_t pid = 1234;
    const uint64_t ns = 1000000000ULL;
    const uint64_t startTs = 100 * ns;
    const uint64_t snapTs = 150 * ns;
    auto addResident = [startTs](HostLeakAnalyzer::InterimCollector& collector, uint64_t stackId, uint64_t bytes)
    {
        HostLeakAnalyzer::StackRow row;
        row.stackId = stackId;
        row.allocCount = 1;
        row.allocBytes = bytes;
        row.unfreedCount = 1;
        row.unfreedBytes = bytes;
        row.maxBlockSize = bytes;
        row.maxAllocTsNs = startTs + ns;  // 窗口前1s(≤起始+0.2×时长)→early常驻
        row.frameDesc = "pool\n";
        collector.stacks.push_back(row);
        StackSeries series;
        series.stackId = stackId;
        for (uint32_t beat = 0; beat <= 10; ++beat)
        {
            SeriesPoint point;
            point.beat = beat;
            point.liveBytes = bytes;
            point.liveCount = 1;
            series.points.push_back(point);
        }
        collector.series.push_back(series);
    };
    auto initStats = [startTs, snapTs](HostLeakAnalyzer::InterimCollector& collector)
    {
        collector.statsAvailable = true;
        collector.stats.snapTsNs = snapTs;
        collector.stats.seriesStartTsNs = startTs;
        collector.stats.seriesBeatIntervalNs = ns;
        collector.stats.totalAllocCount = 2;
        collector.stats.totalAllocBytes = 2 * 1048576;
    };

    // 平局:两个1MiB常驻栈未释放量相同→stackId升序
    HostLeakAnalyzer::InterimCollector tieCollector;
    initStats(tieCollector);
    addResident(tieCollector, 5, 1048576);
    addResident(tieCollector, 6, 1048576);
    std::string text;
    HostLeakAnalyzer::GetInstance().RenderInterimOverview(pid, 9, startTs, tieCollector, text);
    EXPECT_NE(text.find("Resident baselines (2 shown of 2 stacks, 2MiB;"), std::string::npos);
    EXPECT_NE(text.find("stack 0x5"), std::string::npos);
    EXPECT_NE(text.find("stack 0x6"), std::string::npos);

    // TOP N=1:常驻子块截断并提示剩余数量
    setenv("MSMEMSCOPE_HOSTMEM_TOP_N", "1", 1);
    text.clear();
    HostLeakAnalyzer::GetInstance().RenderInterimOverview(pid, 9, startTs, tieCollector, text);
    EXPECT_NE(text.find("Resident baselines (1 shown of 2 stacks, 2MiB;"), std::string::npos);
    EXPECT_NE(text.find("... and 1 more resident stacks (see leak_overview report after window close)"),
              std::string::npos);
    unsetenv("MSMEMSCOPE_HOSTMEM_TOP_N");

    // 非平局:1MiB与2MiB常驻栈→字节降序比较
    HostLeakAnalyzer::InterimCollector byteCollector;
    initStats(byteCollector);
    addResident(byteCollector, 5, 1048576);
    addResident(byteCollector, 7, 2097152);
    text.clear();
    HostLeakAnalyzer::GetInstance().RenderInterimOverview(pid, 9, startTs, byteCollector, text);
    EXPECT_NE(text.find("Resident baselines (2 shown of 2 stacks, 3MiB;"), std::string::npos);
}

