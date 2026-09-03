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

#include "host_leak_analyzer.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "config_info.h"
#include "event_trace/event_report.h"
#include "file.h"
#include "log.h"
#include "memory_state_manager.h"

namespace MemScope
{

namespace
{
// stageId经SystemEvent::name承载(std::to_string(stageId));解析失败按0处理
uint64_t ParseStageId(const std::string& name)
{
    try
    {
        return std::stoull(name);
    }
    catch (...)
    {
        return 0;
    }
}

// stackId=0的栈串占位:0号是未知桶的语义位(栈表超限/栈层失败块照常记账的归宿),
// 与"栈文本丢失"严格区分——前者是设计内口径,后者是未符号化残余
const std::string kUnknownBucketLabel = "(unknown bucket: unattributed blocks)";
// stackId≠0但符号化文本缺失的占位(登记PC缺失/闭窗符号化失败的残余路径);
// 与未知桶占位严格区分——该栈有归因键但文本不可得
const std::string kUnresolvedStackLabel = "(unresolved stack)";

// 概览报告可配项(默认与钩子侧一致):TOP N泄漏点个数与记账键深K。
// 桶界由钩子侧MSMEMSCOPE_HOSTMEM_SIZE_BUCKETS决定,dump_size_distribution
// 直接交付桶行,分析器无需读该配置
constexpr const char* kTopNEnv = "MSMEMSCOPE_HOSTMEM_TOP_N";
constexpr const char* kKeyFramesEnv = "MSMEMSCOPE_HOSTMEM_STACK_KEY_FRAMES";
constexpr uint64_t kTopNDefault = 10;
constexpr uint64_t kKeyFramesDefault = 20;

uint64_t EnvOrDefault(const char* name, uint64_t def)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
    {
        return def;
    }
    try
    {
        const uint64_t v = std::stoull(value);
        return v == 0 ? def : v;  // 0视为未配置
    }
    catch (...)
    {
        return def;
    }
}

// 明细CSV批量写盘缓冲:1MB堆缓冲攒行,近满时write整块刷出(代替ostream逐字段
// operator<<——百万行级明细上逐字段流式写出是写盘路径的主放大项)。
// 行长为变长(call_stack列内联完整栈文本),近满判断按单行最坏长度(见写盘处)
constexpr size_t kDetailBufSize = 1u << 20;

// 0x%016llx等价手写(地址列与Uint64ToHexString逐字节一致:0x前缀+16位小写零填充);
// snprintf逐行百万级调用(含string临时对象)是明细写盘的隐藏热点
char* AppendHexAddr(char* p, uint64_t value)
{
    static const char kHex[] = "0123456789abcdef";
    *p++ = '0';
    *p++ = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        *p++ = kHex[(value >> shift) & 0xf];
    }
    return p;
}

// 十进制无符号追加(明细列size/alloc_ts)
char* AppendU64(char* p, uint64_t value)
{
    char tmp[20];
    int n = 0;
    do
    {
        tmp[n++] = static_cast<char>('0' + static_cast<char>(value % 10));
        value /= 10;
    } while (value != 0);
    while (n > 0)
    {
        *p++ = tmp[--n];
    }
    return p;
}

// RFC 4180引号字段追加:双引号包裹,内部'"'转义为'""'(换行保留不转义——call_stack
// 列帧间以'\n'分隔,与NPU dump文件Call Stack(C)列同构);返回结束指针
char* AppendQuotedField(char* p, const char* text, size_t len)
{
    *p++ = '"';
    for (size_t i = 0; i < len; ++i)
    {
        if (text[i] == '"')
        {
            *p++ = '"';
        }
        *p++ = text[i];
    }
    *p++ = '"';
    return p;
}

// 大小范围文本:字节值1024整倍→K/M缩写(128→"128",1024→"1K",1048576→"1M"),
// 0为特例(0%2^20==0会误入M分支)——首桶下界固定为0,须渲染为"0"而非"0M"
std::string FormatRangeBound(uint64_t value)
{
    if (value == 0)
    {
        return "0";
    }
    if (value % 1048576ull == 0)
    {
        return std::to_string(value / 1048576ull) + "M";
    }
    if (value % 1024ull == 0)
    {
        return std::to_string(value / 1024ull) + "K";
    }
    return std::to_string(value);
}

std::string FormatRange(uint64_t low, uint64_t high)
{
    std::string range = "[" + FormatRangeBound(low) + ", ";
    if (high == UINT64_MAX)
    {
        range += "+inf)";
    }
    else
    {
        range += FormatRangeBound(high) + ")";
    }
    return range;
}
}  // namespace

HostLeakAnalyzer& HostLeakAnalyzer::GetInstance()
{
    // 确保依赖的单例先于本分析器构造(MSM内部触发EventDispatcher/FileWriteManager,
    // MSM构造还锁定FileCreateManager的projectDir_),利用C++静态对象析构逆序规则,
    // 使析构报告中的GetProjectDir/LOG宏安全
    MemoryStateManager::GetInstance();
    Utility::Log::GetLog();
    static HostLeakAnalyzer analyzer;
    return analyzer;
}

HostLeakAnalyzer::HostLeakAnalyzer() : createPid_(static_cast<uint64_t>(getpid())) { Subscribe(); }

HostLeakAnalyzer::~HostLeakAnalyzer()
{
    // fork子进程守卫:本单例构造于父进程,随COW继承进子进程——子进程不监控,窗口
    // 状态是父进程的陈旧快照,出报告只会把父进程数据写进父进程锁定的工程目录;
    // 且子进程内mutex_可能被fork瞬间存活的父上报线程持有,加锁即挂死子进程退出。
    // 直接跳过全部清理:进程将退出内存随进程回收;订阅表残留在子进程自身的
    // EventDispatcher内,子进程无上报线程、退出路径无人再派发事件,悬空无后果
    if (createPid_ != static_cast<uint64_t>(getpid()))
    {
        return;
    }
    UnSubscribe();
    // 析构兜底:正常闭窗由EventReport的退出闭窗路径(CloseHostMemWindowAtExit)在
    // 本析构前完成并出报告,残留open窗口=闭窗STAGE_END未送达的异常路径。dump_*
    // 仅窗口关闭态可调(ABI约束,热路径未冻结),此处只能取get_stats尽力而为值出
    // 退化概览(标注窗口未正常关闭/快照不完整);stats亦不可得时仅打点。
    try
    {
        // 退出期逃生:析构兜底同样不永久阻塞——若退出期analyzer锁被死锁线程永久
        // 占住,兜底报告会再次挂起进程退出。15s拿不到锁即放弃(进程将退出,
        // 内存随进程回收),退出路径不再有任何无界等待
        if (!mutex_.try_lock_for(std::chrono::seconds(15)))
        {
            fprintf(stderr,
                    "[msmemscope] host leak [pid=%llu] fallback report skipped: analyzer lock busy >15s at "
                    "destructor\n",
                    static_cast<unsigned long long>(getpid()));
            return;
        }
        std::lock_guard<std::timed_mutex> lock(mutex_, std::adopt_lock);
        // 兜底打点:残留open窗口数=闭窗STAGE_END未送达数。正常关闭(含stop()/config
        // 变更/退出handler)后此数应为0;非0即与钩子侧闭窗打点互证,定位派发链断点
        size_t openWindows = 0;
        for (auto& window : windows_)
        {
            if (window.second.open)
            {
                ++openWindows;
            }
        }
        if (openWindows != 0)
        {
            fprintf(stderr,
                    "[msmemscope] host leak [pid=%llu] fallback report: %zu window(s) still open at analyzer "
                    "destructor\n",
                    static_cast<unsigned long long>(getpid()), openWindows);
        }
        for (auto& window : windows_)
        {
            if (!window.second.open)
            {
                continue;  // 已闭窗的报告在STAGE_END处理时已输出
            }
            // 尽力取stats(EventReport可能已析构/钩子未装配,查询失败按无数据出退化报告)
            window.second.statsAvailable =
                EventReport::Instance(MemScopeCommType::SHARED_MEMORY).GetHostMemStats(window.second.stats);
            window.second.endTs = 0;  // 结束时刻未知
            window.second.open = false;
            WriteWindowReport(window.first, window.second, true);
        }
    }
    catch (...)
    {
        // 临终处理阶段部分对象可能已析构,异常必须吞掉防std::terminate
        fprintf(stderr, "[msmemscope] host leak [pid=%llu] analyzer cleanup aborted\n",
                static_cast<unsigned long long>(getpid()));
    }
}

void HostLeakAnalyzer::Subscribe()
{
    auto func = std::bind(&HostLeakAnalyzer::EventHandle, this, std::placeholders::_1, std::placeholders::_2);
    // 无逐事件流,仅窗口边界SYSTEM事件(HOST_LEAK_STAGE_START/END)
    std::vector<EventBaseType> eventList{EventBaseType::SYSTEM};
    EventDispatcher::GetInstance().Subscribe(SubscriberId::HOST_LEAKS_ANALYZER, eventList,
                                             EventDispatcher::Priority::High, func);
}

void HostLeakAnalyzer::UnSubscribe() const
{
    EventDispatcher::GetInstance().UnSubscribe(SubscriberId::HOST_LEAKS_ANALYZER);
}

void HostLeakAnalyzer::EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state)
{
    (void)state;
    if (event == nullptr || event->eventType != EventBaseType::SYSTEM)
    {
        return;
    }
    // 退出期逃生:dispatch链持锁调用本handler,任何上游持锁死锁都会让此处永久
    // 阻塞→上报线程挂起→进程退出挂起(实测竞态)。try_lock_for(15s)超时跳过
    // 本次处理(STAGE事件丢失,由~HostLeakAnalyzer析构兜底),进程得以退出;
    // 正常路径锁竞争毫秒级,15s不可达。
    if (!mutex_.try_lock_for(std::chrono::seconds(15)))
    {
        fprintf(stderr,
                "[msmemscope] host leak [pid=%llu] EventHandle: analyzer lock busy >15s, event skipped "
                "(subtype=%d)\n",
                static_cast<unsigned long long>(getpid()), static_cast<int>(event->eventSubType));
        return;
    }
    std::lock_guard<std::timed_mutex> lock(mutex_, std::adopt_lock);
    // 窗口状态只由STAGE事件驱动;其余SYSTEM子类型(TRACE_START等)与本分析器无关
    if (event->eventSubType == EventSubType::HOST_LEAK_STAGE_START)
    {
        HandleStageStart(event);
    }
    else if (event->eventSubType == EventSubType::HOST_LEAK_STAGE_END)
    {
        HandleStageEnd(event);
    }
}

void HostLeakAnalyzer::HandleStageStart(std::shared_ptr<EventBase>& event)
{
    // 状态机:OPEN状态收到START为无效序列(钩子set_enabled幂等,不会重复发),
    // 忽略以防清空在途窗口数据
    auto it = windows_.find(event->pid);
    if (it != windows_.end() && it->second.open)
    {
        LOG_WARN("Host leak stage start ignored: window %llu already open",
                 static_cast<unsigned long long>(it->second.stageId));
        return;
    }
    // 开窗即整体清零(钩子侧表清零延迟到下次开启,分析器侧同款语义——
    // 上一窗口未出报告(异常路径)的数据不泄漏到新窗口)
    WindowState& ws = windows_[event->pid];
    ws = WindowState{};
    ws.open = true;
    ws.stageId = ParseStageId(event->name);
    ws.startTs = event->timestamp;
}

void HostLeakAnalyzer::HandleStageEnd(std::shared_ptr<EventBase>& event)
{
    auto it = windows_.find(event->pid);
    if (it == windows_.end() || !it->second.open)
    {
        return;  // 未开窗的END(重复/异常序),忽略
    }
    WindowState& ws = it->second;
    ws.endTs = event->timestamp;
    ws.open = false;
    // 闭窗拉快照(窗口关闭态,钩子侧聚合/符号化已同步完成):stats+栈统计+大小排布
    // 三种桥接查询并行独立;event模式额外拉逐块明细(block_detail数据源)。
    // dump_*返回false(bind未就绪/EventReport已析构)时对应快照保持空,报告如实
    // 以"Snapshot: unavailable"标注,不猜测零值
    EventReport& report = EventReport::Instance(MemScopeCommType::SHARED_MEMORY);
    ws.statsAvailable = report.GetHostMemStats(ws.stats);
    report.DumpHostMemStackStats(&CollectStackStatsCb, &ws.stacks);
    report.DumpHostMemSizeDist(&CollectSizeDistCb, &ws.buckets);
    report.DumpHostMemPreWindowDist(&CollectSizeDistCb, &ws.preWindowBuckets);
    const bool summaryMode = GetConfig().hostLeakMode == static_cast<uint8_t>(HostLeakMode::SUMMARY);
    if (!summaryMode)
    {
        report.DumpHostMemLiveBlocks(&CollectLiveBlockCb, &ws.blocks);
    }
    WriteWindowReport(it->first, ws, false);
}

void HostLeakAnalyzer::CollectStackStatsCb(void* ctx, uint64_t stackId, uint64_t allocCount, uint64_t allocBytes,
                                           uint64_t freedCount, uint64_t freedBytes, uint64_t unfreedCount,
                                           uint64_t unfreedBytes, uint64_t maxBlockSize, const char* frameDesc,
                                           size_t len)
{
    auto* rows = static_cast<std::vector<StackRow>*>(ctx);
    StackRow row;
    row.stackId = stackId;
    row.allocCount = allocCount;
    row.allocBytes = allocBytes;
    row.freedCount = freedCount;
    row.freedBytes = freedBytes;
    row.unfreedCount = unfreedCount;
    row.unfreedBytes = unfreedBytes;
    row.maxBlockSize = maxBlockSize;
    if (frameDesc != nullptr && len > 0)
    {
        row.frameDesc.assign(frameDesc, len);
    }
    rows->push_back(std::move(row));
}

void HostLeakAnalyzer::CollectSizeDistCb(void* ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount,
                                         uint64_t blockBytes)
{
    auto* buckets = static_cast<std::vector<SizeBucket>*>(ctx);
    SizeBucket bucket;
    bucket.rangeLow = rangeLow;
    bucket.rangeHigh = rangeHigh;
    bucket.blockCount = blockCount;
    bucket.blockBytes = blockBytes;
    buckets->push_back(bucket);
}

void HostLeakAnalyzer::CollectLiveBlockCb(void* ctx, uint64_t addr, uint64_t size, uint64_t allocTs, uint64_t stackId)
{
    auto* blocks = static_cast<std::vector<LiveBlock>*>(ctx);
    LiveBlock block;
    block.addr = addr;
    block.size = size;
    block.allocTs = allocTs;
    block.stackId = stackId;
    blocks->push_back(block);
}

void HostLeakAnalyzer::WriteWindowReport(uint64_t pid, WindowState& ws, bool atExit)
{
    // 空窗口不出报告(开窗后无任何记账/统计,常见于极短区间或纯配置探测);
    // 析构兜底且stats亦不可得时同样无处可写。开窗前free独立通道(窗口外分配释放)
    // 亦计入有数据判定:窗口内可能无记账申请而只有开窗前free(缓存老化场景)
    const bool anyData =
        !ws.stacks.empty() || !ws.buckets.empty() || !ws.blocks.empty() || !ws.preWindowBuckets.empty() ||
        (ws.statsAvailable &&
         (ws.stats.totalAllocCount > 0 || ws.stats.untrackedCount > 0 || ws.stats.preWindowFreeCount > 0));
    if (!anyData)
    {
        return;
    }

    const Config& config = GetConfig();
    const bool summaryMode = config.hostLeakMode == static_cast<uint8_t>(HostLeakMode::SUMMARY);

    // TOP候选行:来自闭窗栈快照(每栈一行,含未知桶)。栈快照缺失(合成测试仅注入
    // 块明细的退化场景)时按stackId聚合逐块明细回推——alloc/freed计数不可得置0,
    // 报告如实呈现未释放量;正常bind路径栈快照恒齐备,该回推不触发
    std::vector<StackRow> synthesized;
    if (ws.stacks.empty() && !ws.blocks.empty())
    {
        std::unordered_map<uint64_t, size_t> index;  // stackId→synthesized下标
        for (const auto& block : ws.blocks)
        {
            auto idx = index.find(block.stackId);
            if (idx == index.end())
            {
                index.emplace(block.stackId, synthesized.size());
                StackRow row;
                row.stackId = block.stackId;
                row.unfreedCount = 1;
                row.unfreedBytes = block.size;
                row.maxBlockSize = block.size;
                synthesized.push_back(row);
            }
            else
            {
                StackRow& row = synthesized[idx->second];
                row.unfreedCount += 1;
                row.unfreedBytes += block.size;
                row.maxBlockSize = std::max(row.maxBlockSize, block.size);
            }
        }
    }
    std::vector<const StackRow*> rows;
    rows.reserve(ws.stacks.size() + synthesized.size());
    for (const auto& row : ws.stacks)
    {
        if (row.unfreedCount == 0)
        {
            continue;  // 无未释放块的栈不是泄漏点,不进入TOP
        }
        rows.push_back(&row);
    }
    for (const auto& row : synthesized)
    {
        rows.push_back(&row);
    }
    // 行排序:未释放量降序,相同则stackId升序(报告确定性)
    std::sort(rows.begin(), rows.end(),
              [](const StackRow* a, const StackRow* b)
              {
                  if (a->unfreedBytes != b->unfreedBytes)
                  {
                      return a->unfreedBytes > b->unfreedBytes;
                  }
                  return a->stackId < b->stackId;
              });
    // 未知桶行(未归因证据:栈表超限/栈层失败块照常记账的归宿)
    const StackRow* unknown = nullptr;
    for (const StackRow* row : rows)
    {
        if (row->stackId == 0)
        {
            unknown = row;
            break;
        }
    }

    // 总泄漏量:闭窗块表遍历的桶合计为权威真源(每存活块恰落入一个桶,含未知桶);
    // 桶快照缺失(退化场景)时以逐块明细回推
    uint64_t totalUnfreedBytes = 0;
    uint64_t totalUnfreedCount = 0;
    uint64_t maxUnfreedBlock = 0;
    if (!ws.buckets.empty())
    {
        for (const auto& bucket : ws.buckets)
        {
            totalUnfreedBytes += bucket.blockBytes;
            totalUnfreedCount += bucket.blockCount;
        }
    }
    else
    {
        for (const auto& block : ws.blocks)
        {
            totalUnfreedBytes += block.size;
            totalUnfreedCount += 1;
            maxUnfreedBlock = std::max(maxUnfreedBlock, block.size);
        }
    }
    for (const auto& row : ws.stacks)
    {
        maxUnfreedBlock = std::max(maxUnfreedBlock, row.maxBlockSize);
    }

    // 输出目录:<output>/msmemscope_<pid>_<date>_ascend/host_leak/(MakeDir递归建链;
    // 上级工程目录名已含pid,不再单列pid子目录)
    const std::string dir = Utility::FileCreateManager::GetInstance(config.outputDir).GetProjectDir() + "/host_leak";
    if (!Utility::MakeDir(dir))
    {
        LOG_WARN("Host leak report aborted: cannot create dir %s", dir.c_str());
        return;
    }
    const std::string stage = std::to_string(ws.stageId);
    const std::string overviewPath = dir + "/leak_overview_" + stage + ".txt";
    std::ofstream out(overviewPath);
    if (!out.is_open())
    {
        LOG_WARN("Host leak report aborted: cannot open %s", overviewPath.c_str());
        return;
    }

    // ---- 数据健康度分析 ----
    out << "====== Host Leak Overview: stage=" << ws.stageId << ", pid=" << pid << " ======\n\n";
    out << "--- Data Health Analysis ---\n";
    out << "Window: " << ws.startTs;
    if (ws.endTs > 0)
    {
        out << " -> " << ws.endTs << " (duration: " << (ws.endTs - ws.startTs) / 1000000000ULL << "s)\n";
    }
    else
    {
        // 析构兜底路径:结束时刻未知,闭窗快照不可得
        out << " -> unknown (process exit before window closed";
        if (atExit)
        {
            out << "; snapshot incomplete";
        }
        out << ")\n";
    }
    out << "Mode: " << (summaryMode ? "summary" : "event") << "\n";
    if (ws.statsAvailable)
    {
        // 全局计数(钩子原子合计,精确):本窗口内经记账门控的申请/释放累计
        out << "Tracked: " << ws.stats.totalAllocCount << " allocations / " << ws.stats.totalAllocBytes
            << "B allocated; " << ws.stats.totalFreedCount << " freed / " << ws.stats.totalFreedBytes << "B\n";
        // 唯一栈数(除未知桶stackId=0):键深K=前K帧相同的"类"语义
        uint64_t distinctStacks = 0;
        for (const auto& row : ws.stacks)
        {
            if (row.stackId != 0)
            {
                ++distinctStacks;
            }
        }
        if (ws.stacks.empty())
        {
            for (const auto& row : synthesized)
            {
                if (row.stackId != 0)
                {
                    ++distinctStacks;
                }
            }
        }
        out << "Distinct stacks: " << distinctStacks
            << " (key depth K=" << EnvOrDefault(kKeyFramesEnv, kKeyFramesDefault) << ", category semantics)\n";
        // 未归因:未知桶未释放量——栈表超限/栈层失败转未知桶照常记账,账本未失真,
        // 仅归因粒度退化;仅>0时出行
        if (unknown != nullptr && unknown->unfreedBytes > 0)
        {
            out << "Unattributed: " << unknown->unfreedCount << " blocks / " << unknown->unfreedBytes
                << "B (unknown bucket, "
                << (totalUnfreedBytes > 0 ? unknown->unfreedBytes * 100 / totalUnfreedBytes : 0) << "%)\n";
        }
        // 整窗显式截断标注(诚实性契约):bit0=块表触顶→申请转溢出通道照常记账,仅归因
        // 粒度退化(总量口径不变);bit1=栈表触顶且死栈回收无法腾位(回收已激活,全活表
        // 无供给)→新键转未知桶照常记账;bit2=溢出通道也触顶→记账停止(窗口为截断点前
        // 的完整前缀)。仅bit2构成数据不完整。死栈淘汰详情见下方Evicted行
        bool truncatedShown = false;
        if ((ws.stats.truncated & 0x1u) != 0)
        {
            out << "Truncated: block table full (allocations -> overflow channel)";
            truncatedShown = true;
        }
        if ((ws.stats.truncated & 0x2u) != 0)
        {
            out << (truncatedShown ? " | " : "Truncated: ")
                << "stack table (dead-stack recycling active, no reclaimable stack at full)";
            truncatedShown = true;
        }
        if ((ws.stats.truncated & 0x4u) != 0)
        {
            out << (truncatedShown ? " | " : "Truncated: ") << "overflow channel full (recording stopped at "
                << ws.stats.liveBlockCount << " live blocks)";
            truncatedShown = true;
        }
        if (truncatedShown)
        {
            if ((ws.stats.truncated & 0x4u) != 0)
            {
                out << " [window data incomplete: not a leak conclusion]";
            }
            out << "\n";
        }
        // 死栈淘汰(栈表满时回收,见EvictDeadStackLocked):被淘汰条目计数与折叠的
        // 申请计数/字节(折叠已并入未知桶行,行求和==全局合计的闭合关系保持,
        // 诚实性零损失);仅>0出行
        if (ws.stats.evictedStackCount > 0)
        {
            out << "Evicted: " << ws.stats.evictedStackCount << " stacks recycled (" << ws.stats.evictedAllocCount
                << " allocs / " << ws.stats.evictedAllocBytes << "B folded to unknown bucket)\n";
        }
        // 溢出通道(块表满降级):转出申请与逆向修正释放均已并入Tracked合计(合计口径
        // 不变);块表未触顶时全零,仅>0出行
        if (ws.stats.overflowAllocCount > 0 || ws.stats.overflowFreedCount > 0)
        {
            out << "Overflow channel: " << ws.stats.overflowAllocCount << " allocations / "
                << ws.stats.overflowAllocBytes << "B diverted; " << ws.stats.overflowFreedCount << " freed / "
                << ws.stats.overflowFreedBytes << "B (reverse-corrected)\n";
        }
        // 开窗前free(窗口外分配,独立通道,不并入Tracked):缓存老化/开窗前残块释放场景分析
        if (ws.stats.preWindowFreeCount > 0)
        {
            out << "Pre-window frees: " << ws.stats.preWindowFreeCount << " / " << ws.stats.preWindowFreeBytes
                << "B (allocated before window, not in ledger)\n";
        }
        // 显式采样视图(采样率倒数>1才标注;1=全量无标注)
        if (ws.stats.sampleRate > 1)
        {
            out << "Sampling: 1/" << ws.stats.sampleRate << " (sampled view)\n";
        }
        // 块阈值(仅>0标注;0=全量无标注),未追踪(size<阈值)分配合计
        if (config.blockSizeThreshold > 0)
        {
            out << "Size threshold: " << config.blockSizeThreshold << "B (untracked: " << ws.stats.untrackedCount
                << " allocations / " << ws.stats.untrackedBytes << "B)\n";
        }
        // 符号化健康:有未释放块的实栈(排除未知桶)中,符号化文本可得的比例;
        // 未符号化=登记PC缺失/闭窗符号化失败的残余路径
        uint64_t unfreedStacks = 0;
        uint64_t symbolizedStacks = 0;
        for (const auto& row : ws.stacks)
        {
            if (row.stackId == 0 || row.unfreedCount == 0)
            {
                continue;
            }
            unfreedStacks += 1;
            if (!row.frameDesc.empty())
            {
                symbolizedStacks += 1;
            }
        }
        out << "Symbolized: " << symbolizedStacks << "/" << unfreedStacks
            << " stacks (unresolved: " << (unfreedStacks - symbolizedStacks) << ")\n";
    }
    else
    {
        out << "Snapshot: unavailable (host hook not bound / query failed)\n";
    }
    if (atExit)
    {
        // 析构兜底:闭窗聚合未发生,逐栈/逐块/大小排布均不可得,概览仅统计列
        out << "Note: window closed at process exit without snapshot; unfreed details unavailable\n";
    }
    out << "\n";

    // ---- 总泄漏量 ----
    out << "--- Total Unfreed ---\n";
    out << "Total unfreed: " << totalUnfreedBytes << " bytes in " << totalUnfreedCount << " blocks (avg "
        << (totalUnfreedCount > 0 ? totalUnfreedBytes / totalUnfreedCount : 0) << "B, max " << maxUnfreedBlock
        << "B)\n";
    out << "\n";

    // ---- 泄漏块大小排布 ----
    out << "--- Unfreed Block Size Distribution ---\n";
    out << std::left << std::setw(16) << "range" << std::right << std::setw(10) << "blocks" << std::setw(12) << "bytes"
        << std::setw(9) << "% of total" << "\n";
    for (const auto& bucket : ws.buckets)
    {
        const uint64_t pct = totalUnfreedBytes > 0 ? bucket.blockBytes * 100 / totalUnfreedBytes : 0;
        out << std::left << std::setw(16) << FormatRange(bucket.rangeLow, bucket.rangeHigh) << std::right
            << std::setw(10) << bucket.blockCount << std::setw(12) << bucket.blockBytes << std::setw(9) << pct << "%\n";
    }
    if (ws.buckets.empty())
    {
        out << "(no unfreed blocks)\n";
    }
    out << "\n";

    // ---- 开窗前free大小排布 ----
    // 窗口外分配(开窗前申请/记账被跳过)在本窗口内释放的事件按大小归桶
    // (dump_pre_window_distribution投影,大小经malloc_usable_size近似,解析失败为0);
    // 独立通道,不并入总泄漏量——仅窗口关闭态可得
    out << "--- Pre-Window Free Size Distribution ---\n";
    if (!ws.preWindowBuckets.empty())
    {
        out << std::left << std::setw(16) << "range" << std::right << std::setw(10) << "frees" << std::setw(12)
            << "bytes" << "\n";
        for (const auto& bucket : ws.preWindowBuckets)
        {
            out << std::left << std::setw(16) << FormatRange(bucket.rangeLow, bucket.rangeHigh) << std::right
                << std::setw(10) << bucket.blockCount << std::setw(12) << bucket.blockBytes << "\n";
        }
    }
    else if (atExit || !ws.statsAvailable)
    {
        out << "(snapshot unavailable)\n";
    }
    else
    {
        out << "(no pre-window frees)\n";
    }
    out << "\n";

    // ---- TOP N 泄漏点 ----
    const uint64_t topN = EnvOrDefault(kTopNEnv, kTopNDefault);
    out << "--- TOP " << topN << " Leak Sites (by unfreed bytes) ---\n";
    int rank = 0;
    for (const StackRow* row : rows)
    {
        if (rank >= static_cast<int>(topN))
        {
            break;
        }
        rank += 1;
        out << rank << ". " << row->unfreedBytes << "B unfreed (" << row->unfreedCount << " blocks, avg "
            << (row->unfreedCount > 0 ? row->unfreedBytes / row->unfreedCount : 0) << "B) | alloc " << row->allocCount
            << "x" << row->allocBytes << "B, freed " << row->freedCount << "x" << row->freedBytes << "B\n";
        if (row->stackId == 0)
        {
            out << "   " << kUnknownBucketLabel << "\n";
        }
        else if (!row->frameDesc.empty())
        {
            // 栈文本逐行缩进(帧描述以'\n'分隔,缩进保持层级可读)
            std::istringstream iss(row->frameDesc);
            std::string line;
            while (std::getline(iss, line))
            {
                out << "   " << line << "\n";
            }
        }
        else
        {
            out << "   (unresolved stack " << row->stackId << ")\n";
        }
    }
    if (rank == 0)
    {
        out << "(no unfreed blocks in this window)\n";
    }
    out.flush();
    if (!out.good())
    {
        LOG_WARN("Host leak overview incomplete: write %s failed", overviewPath.c_str());
    }
    out.close();

    // 逐块明细CSV(仅event模式;供时间序列等后续消费方使用)。call_stack列内联
    // 完整符号化调用栈文本(RFC 4180引号字段:双引号包裹、内部'"'转义、换行保留,
    // 与NPU dump文件Call Stack(C)列同构),逐块自含,不依赖同窗概览报告即可解析。
    // 写盘走1MB堆缓冲攒行+整块write(见kDetailBufSize注释):行长为变长,近满按
    // 单行最坏长度(固定列61 + 引号字段2×栈文本+2 + 换行)判断
    if (!summaryMode && !ws.blocks.empty())
    {
        const std::string detailPath = dir + "/block_detail_" + stage + ".csv";
        std::ofstream detail(detailPath);
        if (detail.is_open())
        {
            // 块明细排序:块大小降序(泄漏定位优先看大块),相同大小按地址升序保证确定性
            std::sort(ws.blocks.begin(), ws.blocks.end(),
                      [](const LiveBlock& a, const LiveBlock& b)
                      {
                          if (a.size != b.size)
                          {
                              return a.size > b.size;
                          }
                          return a.addr < b.addr;
                      });
            // stackId→栈文本映射(闭窗符号化产物frameDesc,'\n'分隔帧描述;缺失/未知桶
            // 按占位处理)。同一栈多块共享同一文本,映射一次建表,逐块O(1)查
            std::unordered_map<uint64_t, const std::string*> stackText;
            stackText.reserve(ws.stacks.size());
            for (const auto& row : ws.stacks)
            {
                stackText.emplace(row.stackId, &row.frameDesc);
            }
            detail << "addr,size,alloc_ts,Call Stack(C)\n";
            std::vector<char> buf(kDetailBufSize);
            char* p = buf.data();
            for (const auto& block : ws.blocks)
            {
                // call_stack列文本:未知桶/未符号化占位,或该栈frameDesc原样内联
                const std::string* text = nullptr;
                if (block.stackId == 0)
                {
                    text = &kUnknownBucketLabel;
                }
                else
                {
                    const auto it = stackText.find(block.stackId);
                    text = (it != stackText.end() && !it->second->empty()) ? it->second : &kUnresolvedStackLabel;
                }
                // 行最坏长度:固定列(0x+16位hex 18 + size/alloc_ts各≤20 + 3分隔符)
                // + 引号字段(最坏全量'"'转义翻倍 + 2引号) + 换行
                const size_t rowMax = 61 + 2 * text->size() + 3;
                if (static_cast<size_t>(buf.data() + buf.size() - p) < rowMax)
                {
                    detail.write(buf.data(), static_cast<std::streamsize>(p - buf.data()));
                    p = buf.data();
                }
                p = AppendHexAddr(p, block.addr);
                *p++ = ',';
                p = AppendU64(p, block.size);
                *p++ = ',';
                p = AppendU64(p, block.allocTs);
                *p++ = ',';
                p = AppendQuotedField(p, text->data(), text->size());
                *p++ = '\n';
            }
            if (p > buf.data())
            {
                detail.write(buf.data(), static_cast<std::streamsize>(p - buf.data()));
            }
            detail.flush();
            if (!detail.good())
            {
                LOG_WARN("Host leak block detail incomplete: write %s failed", detailPath.c_str());
            }
            detail.close();
        }
        else
        {
            LOG_WARN("Host leak block detail aborted: cannot open %s", detailPath.c_str());
        }
    }

    LOG_INFO("Host leak report generated: %s", overviewPath.c_str());
}

}  // namespace MemScope
