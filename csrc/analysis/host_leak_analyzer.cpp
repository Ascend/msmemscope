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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include "config_info.h"
#include "event_trace/event_report.h"
#include "file.h"
#include "log.h"
#include "memory_state_manager.h"
#include "trace_manager/event_trace_manager.h"

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
// TOP N上限1024:与series max-stacks=1024对齐,疑似榜/常驻子块行数均受此约束
constexpr uint64_t kTopNMax = 1024;

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

// TOP N读取:疑似榜与常驻子块共用MSMEMSCOPE_HOSTMEM_TOP_N,超1024截断
uint64_t GetTopN() { return std::min(EnvOrDefault(kTopNEnv, kTopNDefault), kTopNMax); }

// 明细CSV批量写盘缓冲:1MB堆缓冲攒行,近满时write整块刷出(代替ostream逐字段
// operator<<——百万行级明细上逐字段流式写出是写盘路径的主放大项)。
// 行长为变长(call_stack两列内联完整栈文本),近满判断按单行最坏长度(见写盘处)
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

// RFC 4180引号字段追加:双引号包裹,内部'"'转义为'""'(换行保留不转义——调用栈
// 两列帧间以'\n'分隔,与NPU dump文件Call Stack列同构);返回结束指针
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

// 置信度小节大小缩略(1024进制B/KiB/MiB/GiB,0~2位小数,尾零修剪):
// 与概览其余小节"K/M"缩写区分;未释放字节数输出精确值不缩略
std::string FormatSizeAbbrev(double value)
{
    static const char* const kUnits[] = {"B", "KiB", "MiB", "GiB"};
    std::string sign;
    double v = value;
    if (v < 0)
    {
        sign = "-";
        v = -v;
    }
    size_t unit = 0;
    while (v >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0]))
    {
        v /= 1024.0;
        ++unit;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string text(buf);
    const size_t dot = text.find('.');
    if (dot != std::string::npos)
    {
        while (!text.empty() && text.back() == '0')
        {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.')
        {
            text.pop_back();
        }
    }
    return sign + text + kUnits[unit];
}

std::string FormatSizeAbbrev(uint64_t value) { return FormatSizeAbbrev(static_cast<double>(value)); }

// 降级标注文本(不可得值统一`-`)
const char* DegradedLabel(uint8_t degraded)
{
    switch (degraded)
    {
        case CONF_DEGRADED_NO_SERIES:
            return "no_series";
        case CONF_DEGRADED_SERIES_EVICTED:
            return "series_evicted";
        case CONF_DEGRADED_INSUFFICIENT_SERIES:
            return "insufficient_series";
        case CONF_DEGRADED_NO_WARMUP_THREAD:
            return "no_warmup_thread";
        default:
            return "-";
    }
}

// 占比两位小数渲染(unfreed/alloc等;whole>0时调用)
std::string FormatPct(uint64_t part, uint64_t whole)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(part) / static_cast<double>(whole));
    return buf;
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
            LOG_WARN("fallback report skipped: analyzer lock busy >15s at destructor");
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
            LOG_WARN("fallback report: %zu window(s) still open at analyzer destructor", openWindows);
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
        LOG_WARN("analyzer cleanup aborted");
    }
}

const char* HostLeakAnalyzer::GetName() const { return "host_leak"; }

bool HostLeakAnalyzer::QueryInterimOverview(std::string& text)
{
    // 窗口状态只由STAGE事件驱动(START开窗/END闭窗)——以windows_为准,不查
    // IsTracingEnabled: stop后tracing先关、STAGE_END后到,中间时刻tracing已关
    // 而窗口仍开,快照实际可取,误判no active window误导巡检
    // 退出期逃生(同EventHandle/析构兜底):15s拿不到锁即放弃,不阻塞控制通道会话
    // 两阶段: 锁内查窗口→放锁调中间快照(钩子冻结→聚合→符号化,毫秒~百毫秒级,
    // 期间不持本锁,STAGE_END闭窗/其他巡检不被阻塞)→锁内复检窗口仍开才渲染。
    // 查与复检之间可能发生闭窗(STAGE_END已处理): 复检不通过即按无窗口提示,
    // 不渲染过期快照。两段取锁均以unique_lock RAII接管,任何返回路径都解锁
    uint64_t pid = 0;
    uint64_t stageId = 0;
    uint64_t startTs = 0;
    {
        if (!mutex_.try_lock_for(std::chrono::seconds(15)))
        {
            text = "host_leak analyzer lock busy >15s, interim snapshot skipped";
            return false;
        }
        std::unique_lock<std::timed_mutex> lock(mutex_, std::adopt_lock);
        bool found = false;
        for (const auto& window : windows_)
        {
            if (!window.second.open)
            {
                continue;
            }
            pid = window.first;
            stageId = window.second.stageId;
            startTs = window.second.startTs;
            found = true;
            break;
        }
        if (!found)
        {
            // 无开启窗口:仅提示(不做最近闭窗概览——闭窗概览已落盘leak_overview报告)
            text = "no active window";
            return false;
        }
    }
    InterimCollector ic;
    // 快照(锁外):返回false=钩子未装配/bind未就绪:渲染退化概览(仅窗口头+
    // unavailable标注),不猜零值
    ic.statsAvailable =
        EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
            .DumpHostMemInterimSnapshot(&CollectInterimStackCb, &CollectInterimSizeDistCb, &CollectInterimPreWindowCb,
                                        &CollectInterimSeriesCb, &ic.stats, &ic);
    ic.series = std::move(ic.seriesAgg.series);
    // 复检(锁内): 同一窗口(pid+stageId)仍开才渲染本次快照
    {
        if (!mutex_.try_lock_for(std::chrono::seconds(15)))
        {
            text = "host_leak analyzer lock busy >15s, interim snapshot skipped";
            return false;
        }
        std::unique_lock<std::timed_mutex> lock(mutex_, std::adopt_lock);
        for (const auto& window : windows_)
        {
            if (window.first == pid && window.second.open && window.second.stageId == stageId)
            {
                RenderInterimOverview(pid, stageId, startTs, ic, text);
                return true;
            }
        }
        text = "no active window";
        return false;
    }
}

void HostLeakAnalyzer::Subscribe()
{
    auto func = std::bind(&HostLeakAnalyzer::EventHandle, this, std::placeholders::_1, std::placeholders::_2);
    // 无逐事件流,仅窗口边界SYSTEM事件(HOST_LEAK_STAGE_START/END)
    std::vector<EventBaseType> eventList{EventBaseType::SYSTEM};
    EventDispatcher::GetInstance().Subscribe(SubscriberId::HOST_LEAKS_ANALYZER, eventList,
                                             EventDispatcher::Priority::High, func, GetName());
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
        LOG_WARN("EventHandle: analyzer lock busy >15s, event skipped (subtype=%d)",
                 static_cast<int>(event->eventSubType));
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
    // 节拍快照序列(置信度因子数据源): 与栈统计同次闭窗拉取
    SeriesCollector seriesCollector;
    report.DumpHostMemUnfreedSeries(&CollectSeriesCb, &seriesCollector);
    ws.series = std::move(seriesCollector.series);
    const bool summaryMode = GetConfig().hostLeakMode == static_cast<uint8_t>(HostLeakMode::SUMMARY);
    if (!summaryMode)
    {
        report.DumpHostMemLiveBlocks(&CollectLiveBlockCb, &ws.blocks);
    }
    WriteWindowReport(it->first, ws, false);
}

void HostLeakAnalyzer::CollectStackStatsCb(void* ctx, uint64_t stackId, uint64_t allocCount, uint64_t allocBytes,
                                           uint64_t freedCount, uint64_t freedBytes, uint64_t unfreedCount,
                                           uint64_t unfreedBytes, uint64_t maxBlockSize, uint64_t maxAllocTsNs,
                                           uint64_t freedLifetimeSumNs, uint64_t liveAgeSumNs, const char* frameDesc,
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
    row.maxAllocTsNs = maxAllocTsNs;
    row.freedLifetimeSumNs = freedLifetimeSumNs;
    row.liveAgeSumNs = liveAgeSumNs;
    if (frameDesc != nullptr && len > 0)
    {
        row.frameDesc.assign(frameDesc, len);
    }
    rows->push_back(std::move(row));
}

void HostLeakAnalyzer::WriteConfidenceEntry(std::ostream& out, const PerStackResult& r, const StackRow* row, size_t idx,
                                            bool resident)
{
    // 统计列数据源=候选栈闭窗行(row为空=防御,按0渲染,不应发生)
    const uint64_t unfreedBytes = row != nullptr ? row->unfreedBytes : 0;
    const uint64_t unfreedCount = row != nullptr ? row->unfreedCount : 0;
    const uint64_t allocCount = row != nullptr ? row->allocCount : 0;
    const uint64_t allocBytes = row != nullptr ? row->allocBytes : 0;
    const uint64_t freedCount = row != nullptr ? row->freedCount : 0;
    const uint64_t freedBytes = row != nullptr ? row->freedBytes : 0;

    // 状态文本: 常驻子块[resident/判据]; 疑似榜suspected_leak/growth_watch,
    // 周转形态(满足占比判据但尾部仍涨)追加/turnover
    std::string status;
    if (resident)
    {
        status = (r.residentCriterion == CONF_RESIDENT_EARLY) ? "resident/early" : "resident/turnover";
    }
    else
    {
        status = (r.status == CONF_STATUS_SUSPECTED) ? "suspected_leak" : "growth_watch";
        if (r.turnoverMark)
        {
            status += "/turnover";
        }
    }

    // 因子文本: 每因子=名+1空格+值两位小数,因子间2空格分组; 值恒紧随名
    // (无右对齐垫宽,读时不会与相邻因子粘连); 序列降级时Growth/Pattern输出`-`;
    // 常驻子块省略Pattern(不参与常驻判定)
    const bool degraded = r.degraded != CONF_DEGRADED_NONE;
    char factorBuf[192];
    int n = 0;
    auto appendFactor = [&](const char* name, double value, bool unavailable)
    {
        if (unavailable)
        {
            n += std::snprintf(factorBuf + n, sizeof(factorBuf) - n, "%s -  ", name);
        }
        else
        {
            n += std::snprintf(factorBuf + n, sizeof(factorBuf) - n, "%s %.2f  ", name, value);
        }
    };
    appendFactor("Growth", r.g, degraded);
    appendFactor("Release", r.r, false);
    appendFactor("Lifetime", r.s, false);
    if (!resident)
    {
        appendFactor("Pattern", r.p, degraded);
    }
    appendFactor("Scale", r.e, false);

    // 因子段尾恒2空格,直接衔接unfreed/alloc
    out << "  " << std::setw(2) << idx << ". [" << status << "] LSI " << std::lround(r.lsi) << "  " << factorBuf
        << "unfreed/alloc " << ((allocBytes > 0) ? FormatPct(unfreedBytes, allocBytes) : std::string("-")) << "\n";
    out << "     unfreed " << unfreedBytes << "B(" << unfreedCount << " blocks) | alloc " << allocCount << "x"
        << FormatSizeAbbrev(allocCount > 0 ? allocBytes / allocCount : 0) << ", freed " << freedCount << "x"
        << FormatSizeAbbrev(freedCount > 0 ? freedBytes / freedCount : 0) << "\n";
    char idBuf[24];
    std::snprintf(idBuf, sizeof(idBuf), "0x%llx", static_cast<unsigned long long>(r.stackId));
    out << "     growth " << (r.growthValid ? FormatSizeAbbrev(r.growthRate) + "/beat" : "-") << "  pts "
        << (r.growthValid ? std::to_string(r.seriesPoints) : std::string("-")) << "  degraded "
        << DegradedLabel(r.degraded) << "  stack " << idBuf << "\n";
    if (r.stackId == 0)
    {
        out << "     " << kUnknownBucketLabel << "\n";
    }
    else if (row != nullptr && !row->frameDesc.empty())
    {
        // 栈文本逐行缩进(帧描述以'\n'分隔)
        std::istringstream iss(row->frameDesc);
        std::string line;
        while (std::getline(iss, line))
        {
            out << "     " << line << "\n";
        }
    }
    else
    {
        out << "     (unresolved stack " << idBuf << ")\n";
    }
}

void HostLeakAnalyzer::CollectSeriesCb(void* ctx, uint64_t stackId, uint32_t beat, uint64_t liveBytes,
                                       uint32_t liveCount, uint32_t flags)
{
    // 按栈聚合(钩子侧同栈多拍连续交付),flags bit0=槽被驱逐
    auto* collector = static_cast<SeriesCollector*>(ctx);
    size_t idx = 0;
    const auto it = collector->index.find(stackId);
    if (it == collector->index.end())
    {
        StackSeries s;
        s.stackId = stackId;
        collector->series.push_back(std::move(s));
        idx = collector->series.size() - 1;
        collector->index.emplace(stackId, idx);
    }
    else
    {
        idx = it->second;
    }
    StackSeries& s = collector->series[idx];
    if ((flags & 0x1u) != 0)
    {
        s.evicted = true;
    }
    SeriesPoint p;
    p.beat = beat;
    p.liveBytes = liveBytes;
    p.liveCount = liveCount;
    s.points.push_back(p);
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

void HostLeakAnalyzer::CollectInterimStackCb(void* ctx, uint64_t stackId, uint64_t allocCount, uint64_t allocBytes,
                                             uint64_t freedCount, uint64_t freedBytes, uint64_t unfreedCount,
                                             uint64_t unfreedBytes, uint64_t maxBlockSize, uint64_t maxAllocTsNs,
                                             uint64_t freedLifetimeSumNs, uint64_t liveAgeSumNs, const char* frameDesc,
                                             size_t len)
{
    // 与CollectStackStatsCb同构(钩子侧快照行已是"未释放量降序,stackId升序"排序,
    // 渲染直接消费;未知桶stackId=0为最后行)——仅ctx类型不同
    auto* ic = static_cast<InterimCollector*>(ctx);
    StackRow row;
    row.stackId = stackId;
    row.allocCount = allocCount;
    row.allocBytes = allocBytes;
    row.freedCount = freedCount;
    row.freedBytes = freedBytes;
    row.unfreedCount = unfreedCount;
    row.unfreedBytes = unfreedBytes;
    row.maxBlockSize = maxBlockSize;
    row.maxAllocTsNs = maxAllocTsNs;
    row.freedLifetimeSumNs = freedLifetimeSumNs;
    row.liveAgeSumNs = liveAgeSumNs;
    if (frameDesc != nullptr && len > 0)
    {
        row.frameDesc.assign(frameDesc, len);
    }
    ic->stacks.push_back(std::move(row));
}

void HostLeakAnalyzer::CollectInterimSizeDistCb(void* ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount,
                                                uint64_t blockBytes)
{
    auto* ic = static_cast<InterimCollector*>(ctx);
    SizeBucket bucket;
    bucket.rangeLow = rangeLow;
    bucket.rangeHigh = rangeHigh;
    bucket.blockCount = blockCount;
    bucket.blockBytes = blockBytes;
    ic->buckets.push_back(bucket);
}

void HostLeakAnalyzer::CollectInterimPreWindowCb(void* ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount,
                                                 uint64_t blockBytes)
{
    auto* ic = static_cast<InterimCollector*>(ctx);
    SizeBucket bucket;
    bucket.rangeLow = rangeLow;
    bucket.rangeHigh = rangeHigh;
    bucket.blockCount = blockCount;
    bucket.blockBytes = blockBytes;
    ic->preWindowBuckets.push_back(bucket);
}

void HostLeakAnalyzer::CollectInterimSeriesCb(void* ctx, uint64_t stackId, uint32_t beat, uint64_t liveBytes,
                                              uint32_t liveCount, uint32_t flags)
{
    // 与CollectSeriesCb同构(SeriesCollector模式:同栈多拍连续,索引命中即尾插)
    auto* ic = static_cast<InterimCollector*>(ctx);
    SeriesCollector& collector = ic->seriesAgg;
    size_t idx = 0;
    const auto it = collector.index.find(stackId);
    if (it == collector.index.end())
    {
        StackSeries s;
        s.stackId = stackId;
        collector.series.push_back(std::move(s));
        idx = collector.series.size() - 1;
        collector.index.emplace(stackId, idx);
    }
    else
    {
        idx = it->second;
    }
    StackSeries& s = collector.series[idx];
    if ((flags & 0x1u) != 0)
    {
        s.evicted = true;
    }
    SeriesPoint p;
    p.beat = beat;
    p.liveBytes = liveBytes;
    p.liveCount = liveCount;
    s.points.push_back(p);
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
    LOG_INFO("Host leak overview report created: %s", overviewPath.c_str());

    // ---- 数据健康度分析 ----
    out << "====== Host Leak Overview: stage=" << ws.stageId << ", pid=" << pid << " ======\n";
    // 文件头注释LSI全称与语义(报告内其余位置沿用缩写)
    out << "LSI: Leak Suspicion Index (0-100); higher LSI = more likely a genuine leak. "
           "See TOP N Leak Sites section for details.\n\n";
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

    // ---- TOP N 泄漏点(置信度研判视图) ----
    const uint64_t topN = GetTopN();
    out << "--- TOP " << topN << " Leak Sites (by LSI desc) ---\n";
    // 序列元信息: 拍数=闭窗时刻经序列锚点折算(有序列必有首拍锚点,正常路径锚点
    // 可得); 时长=窗口时长; top-k/max-stacks为节拍采集定值
    const uint64_t beatIntervalNs =
        (ws.statsAvailable && ws.stats.seriesBeatIntervalNs > 0) ? ws.stats.seriesBeatIntervalNs : 1000000000ULL;
    const uint64_t seriesStartTs = ws.statsAvailable ? ws.stats.seriesStartTsNs : 0;
    uint64_t seriesBeats = 0;
    if (seriesStartTs > 0 && ws.endTs > seriesStartTs)
    {
        seriesBeats = (ws.endTs - seriesStartTs) / beatIntervalNs + 1;
    }
    else
    {
        // 退化路径兜底(无序列或stats缺失): 按已交付序列最大拍号
        uint64_t maxDeliveredBeat = 0;
        for (const auto& s : ws.series)
        {
            for (const auto& p : s.points)
            {
                maxDeliveredBeat = std::max(maxDeliveredBeat, static_cast<uint64_t>(p.beat));
            }
        }
        seriesBeats = maxDeliveredBeat + 1;
    }
    const uint64_t windowDurSec = (ws.endTs > ws.startTs) ? (ws.endTs - ws.startTs) / 1000000000ULL : 0;
    out << "Series: top-k=256, max-stacks=1024, beats=" << seriesBeats << "(" << windowDurSec << "s, 1s/beat)\n";

    // 置信度计算: 候选栈=闭窗栈快照中unfreed>0的栈,逐栈喂入纯算法模块,
    // 产出按LSI降序; rowById供条目落盘查栈文本/统计行
    std::unordered_map<uint64_t, StackCloseStats> closeStats;
    std::unordered_map<uint64_t, const StackRow*> rowById;
    closeStats.reserve(rows.size());
    rowById.reserve(rows.size());
    for (const StackRow* row : rows)
    {
        StackCloseStats st;
        st.allocCount = row->allocCount;
        st.allocBytes = row->allocBytes;
        st.unfreedCount = row->unfreedCount;
        st.unfreedBytes = row->unfreedBytes;
        st.maxAllocTsNs = row->maxAllocTsNs;
        st.freedLifetimeSumNs = row->freedLifetimeSumNs;
        st.liveAgeSumNs = row->liveAgeSumNs;
        closeStats.emplace(row->stackId, st);
        rowById.emplace(row->stackId, row);
    }
    const bool warmupThreadFailed = ws.statsAvailable && (ws.stats.seriesFlags & 0x1u) != 0;
    const std::vector<PerStackResult> confidence =
        HostConfidence().Compute(ws.series, closeStats, ws.startTs, ws.endTs, warmupThreadFailed);

    // 分派: 疑似榜(Compute已按LSI降序)与常驻子块(按未释放字节降序,次键栈号
    // 升序保证确定性; 常驻栈不占TOPN名额)
    std::vector<const PerStackResult*> suspects;
    std::vector<const PerStackResult*> residents;
    suspects.reserve(confidence.size());
    residents.reserve(confidence.size());
    for (const auto& result : confidence)
    {
        if (result.status == CONF_STATUS_RESIDENT)
        {
            residents.push_back(&result);
        }
        else
        {
            suspects.push_back(&result);
        }
    }
    std::sort(residents.begin(), residents.end(),
              [&closeStats](const PerStackResult* a, const PerStackResult* b)
              {
                  uint64_t aBytes = 0;
                  uint64_t bBytes = 0;
                  const auto ait = closeStats.find(a->stackId);
                  const auto bit = closeStats.find(b->stackId);
                  if (ait != closeStats.end())
                  {
                      aBytes = ait->second.unfreedBytes;
                  }
                  if (bit != closeStats.end())
                  {
                      bBytes = bit->second.unfreedBytes;
                  }
                  if (aBytes != bBytes)
                  {
                      return aBytes > bBytes;
                  }
                  return a->stackId < b->stackId;
              });

    // 疑似榜(按LSI降序,名额截断); 候选栈集为空时输出原占位行
    const size_t suspectShown = std::min(suspects.size(), static_cast<size_t>(topN));
    for (size_t i = 0; i < suspectShown; ++i)
    {
        const auto rit = rowById.find(suspects[i]->stackId);
        WriteConfidenceEntry(out, *suspects[i], rit != rowById.end() ? rit->second : nullptr, i + 1, false);
    }
    if (rows.empty())
    {
        out << "(no unfreed blocks in this window)\n";
    }

    // 常驻基线子块(按未释放量降序,不占TOPN名额; 已过滤出疑似榜)
    if (!residents.empty())
    {
        uint64_t residentBytes = 0;
        for (const PerStackResult* r : residents)
        {
            const auto it = closeStats.find(r->stackId);
            if (it != closeStats.end())
            {
                residentBytes += it->second.unfreedBytes;
            }
        }
        // 子块头标注判据来源: early=G≈0+早期分配, turnover=占比低+尾部不涨
        out << "Resident baselines (" << residents.size() << " stacks, " << FormatSizeAbbrev(residentBytes)
            << "; G≈0 & early-allocated, or turnover (low unfreed/alloc & flat tail); "
               "excluded from suspect list, no TOP N slot):\n";
        for (size_t i = 0; i < residents.size(); ++i)
        {
            const auto rit = rowById.find(residents[i]->stackId);
            WriteConfidenceEntry(out, *residents[i], rit != rowById.end() ? rit->second : nullptr, i + 1, true);
        }
    }

    // 周转提示NOTE: 开窗前free(窗口外分配的释放)占窗口内总申请过半——缓存周转
    // 的全局信号,提示结合unfreed/alloc占比解读常驻分类; 统计不可得时不出NOTE
    if (ws.statsAvailable && ws.stats.preWindowFreeBytes > 0 && ws.stats.totalAllocBytes > 0 &&
        static_cast<double>(ws.stats.preWindowFreeBytes) > 0.5 * static_cast<double>(ws.stats.totalAllocBytes))
    {
        out << "NOTE: " << FormatSizeAbbrev(ws.stats.preWindowFreeBytes)
            << " pre-window allocations were freed within this window (cache turnover pattern); resident "
               "classification includes the unfreed/alloc turnover criterion (§3.1.4 conditions 4-5)\n";
    }
    out.flush();
    if (!out.good())
    {
        LOG_WARN("Host leak overview incomplete: write %s failed", overviewPath.c_str());
    }
    out.close();

    // 逐块明细CSV(仅event模式;供时间序列等后续消费方使用)。调用栈拆两列:
    // Call Stack(C)=纯C栈文本,Call Stack(Python)=py帧文本(混合栈frameDesc按
    // marker拆分,见host_mem_hooks.h MSMEMSCOPE_HOSTMEM_MIXED_STACK_MARKER;
    // 无py文本行Python列为空""——纯C栈/占位行)。RFC 4180引号字段:双引号包裹、
    // 内部'"'转义、换行保留(与NPU dump文件Call Stack列同构),逐块自含,不依赖
    // 同窗概览报告即可解析。写盘走1MB堆缓冲攒行+整块write(见kDetailBufSize注释):
    // 行长为变长,近满按单行最坏长度(固定列62 + 引号字段2×(C+Python)文本+4 +
    // 换行)判断
    if (!summaryMode && !ws.blocks.empty())
    {
        const std::string detailPath = dir + "/block_detail_" + stage + ".csv";
        std::ofstream detail(detailPath);
        if (detail.is_open())
        {
            LOG_INFO("Host leak block detail report created: %s", detailPath.c_str());
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
            // stackId→(C栈文本, py帧文本)映射: 闭窗frameDesc为混合栈文本
            // (marker分隔;未采集派生NA时py文本为"NA\n";无marker=纯C栈)。
            // 建表时按marker一次性拆分为两列文本——同一栈多块共享拆分结果,
            // 逐块O(1)查零拆分开销。缺失/未知桶按占位处理(占位行Python列为空)
            struct StackText
            {
                std::string c;   // marker前: 纯C栈文本
                std::string py;  // marker后: py帧文本(空=该行Python列为空)
            };
            std::unordered_map<uint64_t, StackText> stackText;
            stackText.reserve(ws.stacks.size());
            for (const auto& row : ws.stacks)
            {
                StackText st;
                const auto mp = row.frameDesc.find(MSMEMSCOPE_HOSTMEM_MIXED_STACK_MARKER);
                if (mp == std::string::npos)
                {
                    st.c = row.frameDesc;
                }
                else
                {
                    st.c.assign(row.frameDesc, 0, mp);
                    st.py.assign(row.frameDesc, mp + sizeof(MSMEMSCOPE_HOSTMEM_MIXED_STACK_MARKER) - 1,
                                 std::string::npos);
                }
                stackText.emplace(row.stackId, std::move(st));
            }
            detail << "addr,size,alloc_ts,Call Stack(C),Call Stack(Python)\n";
            std::vector<char> buf(kDetailBufSize);
            char* p = buf.data();
            for (const auto& block : ws.blocks)
            {
                // 两列文本:未知桶/未符号化占位(C列,Python列为空),或该栈marker
                // 拆分文本(C列=纯C栈,Python列=py帧文本)
                const std::string* cText = nullptr;
                const std::string* pyText = nullptr;
                if (block.stackId == 0)
                {
                    cText = &kUnknownBucketLabel;
                }
                else
                {
                    const auto it = stackText.find(block.stackId);
                    if (it != stackText.end() && !it->second.c.empty())
                    {
                        cText = &it->second.c;
                        pyText = it->second.py.empty() ? nullptr : &it->second.py;
                    }
                    else
                    {
                        cText = &kUnresolvedStackLabel;
                    }
                }
                // 行最坏长度:固定列(0x+16位hex 18 + size/alloc_ts各≤20 + 4分隔符)
                // + 引号字段(C 2×+2, Python 2×+2,最坏全量'"'转义翻倍) + 换行
                const size_t rowMax = 67 + 2 * (cText->size() + (pyText == nullptr ? 0 : pyText->size()));
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
                p = AppendQuotedField(p, cText->data(), cText->size());
                *p++ = ',';
                p = AppendQuotedField(p, pyText == nullptr ? "" : pyText->data(),
                                      pyText == nullptr ? 0 : pyText->size());
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

void HostLeakAnalyzer::RenderInterimOverview(uint64_t pid, uint64_t stageId, uint64_t startTs, InterimCollector& ic,
                                             std::string& text)
{
    // 中间概览=leak_overview的交互巡检形态:章节结构同闭窗报告,差异——
    //   Window行标注(interim snapshot, window open),时长基准=快照时刻snapTsNs;
    //   冻结标注(frozenSkip>0:冻结期alloc只计入统计未入块表);
    //   快照降级标注(snapshotDegraded位,仅本次快照,不污染整窗truncated);
    //   常驻子块截断≤TOP N(同MSMEMSCOPE_HOSTMEM_TOP_N;闭窗报告不截断);不写block_detail CSV。
    // 渲染经ostringstream(终端输出,不与文件报告共享句柄)
    std::ostringstream out;
    const bool statsOk = ic.statsAvailable;

    // ---- 数据健康度分析 ----
    out << "====== Host Leak Overview (interim snapshot): stage=" << stageId << ", pid=" << pid << " ======\n";
    out << "LSI: Leak Suspicion Index (0-100); higher LSI = more likely a genuine leak. "
           "See TOP N Leak Sites section for details.\n\n";
    out << "--- Data Health Analysis ---\n";
    const uint64_t snapTs = statsOk ? ic.stats.snapTsNs : 0;
    out << "Window: " << startTs;
    if (snapTs > 0)
    {
        // 时差防护: snapTs来自钩子快照时钟,与startTs(事件时间戳)可能不同源,
        // 回拨时差分为0而非下溢(对齐下方Series行的既有防护)
        const uint64_t windowDurSec = (snapTs >= startTs) ? (snapTs - startTs) / 1000000000ULL : 0;
        out << " -> " << snapTs << " (duration: " << windowDurSec << "s) (interim snapshot, window open)\n";
    }
    else
    {
        // 快照不可得(钩子未装配/查询失败):时长未知
        out << " -> unknown (interim snapshot, window open; snapshot unavailable)\n";
    }
    const bool summaryMode = GetConfig().hostLeakMode == static_cast<uint8_t>(HostLeakMode::SUMMARY);
    out << "Mode: " << (summaryMode ? "summary" : "event") << "\n";
    if (statsOk)
    {
        const MsmemscopeInterimStats& st = ic.stats;
        // Tracked派生口径与闭窗同源:totalFreed=totalAlloc−块表存活−溢出存活
        // (冻结期alloc已计入totalAlloc,其后续free落入开窗前通道——派生口径
        // 将冻结期alloc视为已释放,不变量闭合,冻结标注见下)
        out << "Tracked: " << st.totalAllocCount << " allocations / " << st.totalAllocBytes << "B allocated; "
            << st.totalFreedCount << " freed / " << st.totalFreedBytes << "B\n";
        // 冻结标注:快照冻结期到达的申请跳过块表/栈计数,
        // 只计入totalAlloc+frozenSkip;统计值含冻结期事件,块表不含——概览如实标注
        if (st.frozenSkipAllocCount > 0)
        {
            out << "Snapshot freeze: " << st.frozenSkipAllocCount << " allocations / " << st.frozenSkipAllocBytes
                << "B during snapshot not captured in block table (stats include them)\n";
        }
        uint64_t distinctStacks = 0;
        for (const auto& row : ic.stacks)
        {
            if (row.stackId != 0)
            {
                ++distinctStacks;
            }
        }
        out << "Distinct stacks: " << distinctStacks
            << " (key depth K=" << EnvOrDefault(kKeyFramesEnv, kKeyFramesDefault) << ", category semantics)\n";
        // 未归因:未知桶未释放量(归因粒度退化,账本未失真);仅>0出行
        const StackRow* unknown = nullptr;
        for (const auto& row : ic.stacks)
        {
            if (row.stackId == 0)
            {
                unknown = &row;
                break;
            }
        }
        uint64_t totalUnfreedBytes = 0;
        for (const auto& bucket : ic.buckets)
        {
            totalUnfreedBytes += bucket.blockBytes;
        }
        if (unknown != nullptr && unknown->unfreedBytes > 0)
        {
            out << "Unattributed: " << unknown->unfreedCount << " blocks / " << unknown->unfreedBytes
                << "B (unknown bucket, "
                << (totalUnfreedBytes > 0 ? unknown->unfreedBytes * 100 / totalUnfreedBytes : 0) << "%)\n";
        }
        // 整窗截断标注(与闭窗同款;bit2构成数据不完整)
        bool truncatedShown = false;
        if ((st.truncated & 0x1u) != 0)
        {
            out << "Truncated: block table full (allocations -> overflow channel)";
            truncatedShown = true;
        }
        if ((st.truncated & 0x2u) != 0)
        {
            out << (truncatedShown ? " | " : "Truncated: ")
                << "stack table (dead-stack recycling active, no reclaimable stack at full)";
            truncatedShown = true;
        }
        if ((st.truncated & 0x4u) != 0)
        {
            out << (truncatedShown ? " | " : "Truncated: ") << "overflow channel full (recording stopped at "
                << st.liveBlockCount << " live blocks)";
            truncatedShown = true;
        }
        if (truncatedShown)
        {
            if ((st.truncated & 0x4u) != 0)
            {
                out << " [window data incomplete: not a leak conclusion]";
            }
            out << "\n";
        }
        // 本次快照读取降级(区别于整窗truncated:仅影响本次快照,窗口继续记账)
        // bit0=块表分片锁获取失败(数据为前缀) bit1=栈表分片锁获取失败(归因不完整)
        // bit2=节拍序列或开窗前free分布读取失败
        if (st.snapshotDegraded != 0)
        {
            std::string degraded;
            if ((st.snapshotDegraded & 0x1u) != 0)
            {
                degraded += "block-table read (prefix data)";
            }
            if ((st.snapshotDegraded & 0x2u) != 0)
            {
                degraded += degraded.empty() ? "stack-table read (partial attribution)"
                                             : " | stack-table read (partial attribution)";
            }
            if ((st.snapshotDegraded & 0x4u) != 0)
            {
                degraded +=
                    degraded.empty() ? "series/pre-window read (partial)" : " | series/pre-window read (partial)";
            }
            out << "Snapshot degraded: " << degraded << " (this snapshot only; window continues recording)\n";
        }
        if (st.evictedStackCount > 0)
        {
            out << "Evicted: " << st.evictedStackCount << " stacks recycled (" << st.evictedAllocCount << " allocs / "
                << st.evictedAllocBytes << "B folded to unknown bucket)\n";
        }
        if (st.overflowAllocCount > 0 || st.overflowFreedCount > 0)
        {
            out << "Overflow channel: " << st.overflowAllocCount << " allocations / " << st.overflowAllocBytes
                << "B diverted; " << st.overflowFreedCount << " freed / " << st.overflowFreedBytes
                << "B (reverse-corrected)\n";
        }
        if (st.preWindowFreeCount > 0)
        {
            out << "Pre-window frees: " << st.preWindowFreeCount << " / " << st.preWindowFreeBytes
                << "B (allocated before window, not in ledger)\n";
        }
        if (st.sampleRate > 1)
        {
            out << "Sampling: 1/" << st.sampleRate << " (sampled view)\n";
        }
        const Config& config = GetConfig();
        if (config.blockSizeThreshold > 0)
        {
            out << "Size threshold: " << config.blockSizeThreshold << "B (untracked: " << st.untrackedCount
                << " allocations / " << st.untrackedBytes << "B)\n";
        }
        uint64_t unfreedStacks = 0;
        uint64_t symbolizedStacks = 0;
        for (const auto& row : ic.stacks)
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
    out << "\n";

    // ---- 总泄漏量(桶合计权威真源,与闭窗同口径) ----
    uint64_t totalUnfreedBytes = 0;
    uint64_t totalUnfreedCount = 0;
    uint64_t maxUnfreedBlock = 0;
    for (const auto& bucket : ic.buckets)
    {
        totalUnfreedBytes += bucket.blockBytes;
        totalUnfreedCount += bucket.blockCount;
    }
    for (const auto& row : ic.stacks)
    {
        maxUnfreedBlock = std::max(maxUnfreedBlock, row.maxBlockSize);
    }
    out << "--- Total Unfreed ---\n";
    out << "Total unfreed: " << totalUnfreedBytes << " bytes in " << totalUnfreedCount << " blocks (avg "
        << (totalUnfreedCount > 0 ? totalUnfreedBytes / totalUnfreedCount : 0) << "B, max " << maxUnfreedBlock
        << "B)\n";
    out << "\n";

    // ---- 泄漏块大小排布 ----
    out << "--- Unfreed Block Size Distribution ---\n";
    out << std::left << std::setw(16) << "range" << std::right << std::setw(10) << "blocks" << std::setw(12) << "bytes"
        << std::setw(9) << "% of total" << "\n";
    for (const auto& bucket : ic.buckets)
    {
        const uint64_t pct = totalUnfreedBytes > 0 ? bucket.blockBytes * 100 / totalUnfreedBytes : 0;
        out << std::left << std::setw(16) << FormatRange(bucket.rangeLow, bucket.rangeHigh) << std::right
            << std::setw(10) << bucket.blockCount << std::setw(12) << bucket.blockBytes << std::setw(9) << pct << "%\n";
    }
    if (ic.buckets.empty())
    {
        out << "(no unfreed blocks)\n";
    }
    out << "\n";

    // ---- 开窗前free大小排布(独立通道;降级位bit2或空快照=不可得) ----
    out << "--- Pre-Window Free Size Distribution ---\n";
    if (!ic.preWindowBuckets.empty())
    {
        out << std::left << std::setw(16) << "range" << std::right << std::setw(10) << "frees" << std::setw(12)
            << "bytes" << "\n";
        for (const auto& bucket : ic.preWindowBuckets)
        {
            out << std::left << std::setw(16) << FormatRange(bucket.rangeLow, bucket.rangeHigh) << std::right
                << std::setw(10) << bucket.blockCount << std::setw(12) << bucket.blockBytes << "\n";
        }
    }
    else if (!statsOk || (ic.stats.snapshotDegraded & 0x4u) != 0)
    {
        out << "(snapshot unavailable)\n";
    }
    else
    {
        out << "(no pre-window frees)\n";
    }
    out << "\n";

    // ---- TOP N 泄漏点(置信度研判视图;时间轴折算基准=快照时刻snapTs) ----
    const uint64_t topN = GetTopN();
    out << "--- TOP " << topN << " Leak Sites (by LSI desc) ---\n";
    std::vector<const StackRow*> rows;
    rows.reserve(ic.stacks.size());
    for (const auto& row : ic.stacks)
    {
        if (row.unfreedCount == 0)
        {
            continue;  // 无未释放块的栈不是泄漏点,不进入TOP
        }
        rows.push_back(&row);
    }
    // 行排序(钩子侧已排,防御性再排保证确定性):未释放量降序,相同则stackId升序
    std::sort(rows.begin(), rows.end(),
              [](const StackRow* a, const StackRow* b)
              {
                  if (a->unfreedBytes != b->unfreedBytes)
                  {
                      return a->unfreedBytes > b->unfreedBytes;
                  }
                  return a->stackId < b->stackId;
              });
    if (statsOk)
    {
        const uint64_t beatIntervalNs =
            ic.stats.seriesBeatIntervalNs > 0 ? ic.stats.seriesBeatIntervalNs : 1000000000ULL;
        const uint64_t seriesStartTs = ic.stats.seriesStartTsNs;
        uint64_t seriesBeats = 0;
        if (seriesStartTs > 0 && snapTs > seriesStartTs)
        {
            seriesBeats = (snapTs - seriesStartTs) / beatIntervalNs + 1;
        }
        else
        {
            uint64_t maxDeliveredBeat = 0;
            for (const auto& s : ic.series)
            {
                for (const auto& p : s.points)
                {
                    maxDeliveredBeat = std::max(maxDeliveredBeat, static_cast<uint64_t>(p.beat));
                }
            }
            seriesBeats = maxDeliveredBeat + 1;
        }
        const uint64_t windowDurSec = (snapTs > startTs) ? (snapTs - startTs) / 1000000000ULL : 0;
        out << "Series: top-k=256, max-stacks=1024, beats=" << seriesBeats << "(" << windowDurSec
            << "s, 1s/beat; interim snapshot at " << snapTs << ")\n";
    }
    std::unordered_map<uint64_t, StackCloseStats> closeStats;
    std::unordered_map<uint64_t, const StackRow*> rowById;
    closeStats.reserve(rows.size());
    rowById.reserve(rows.size());
    for (const StackRow* row : rows)
    {
        StackCloseStats st;
        st.allocCount = row->allocCount;
        st.allocBytes = row->allocBytes;
        st.unfreedCount = row->unfreedCount;
        st.unfreedBytes = row->unfreedBytes;
        st.maxAllocTsNs = row->maxAllocTsNs;
        st.freedLifetimeSumNs = row->freedLifetimeSumNs;
        st.liveAgeSumNs = row->liveAgeSumNs;
        closeStats.emplace(row->stackId, st);
        rowById.emplace(row->stackId, row);
    }
    const bool warmupThreadFailed = statsOk && (ic.stats.seriesFlags & 0x1u) != 0;
    const std::vector<PerStackResult> confidence =
        HostConfidence().Compute(ic.series, closeStats, startTs, snapTs, warmupThreadFailed);

    // 分派:疑似榜(LSI降序)与常驻子块(未释放量降序,次键栈号升序;不占TOPN名额)
    std::vector<const PerStackResult*> suspects;
    std::vector<const PerStackResult*> residents;
    suspects.reserve(confidence.size());
    residents.reserve(confidence.size());
    for (const auto& result : confidence)
    {
        if (result.status == CONF_STATUS_RESIDENT)
        {
            residents.push_back(&result);
        }
        else
        {
            suspects.push_back(&result);
        }
    }
    std::sort(residents.begin(), residents.end(),
              [&closeStats](const PerStackResult* a, const PerStackResult* b)
              {
                  uint64_t aBytes = 0;
                  uint64_t bBytes = 0;
                  const auto ait = closeStats.find(a->stackId);
                  const auto bit = closeStats.find(b->stackId);
                  if (ait != closeStats.end())
                  {
                      aBytes = ait->second.unfreedBytes;
                  }
                  if (bit != closeStats.end())
                  {
                      bBytes = bit->second.unfreedBytes;
                  }
                  if (aBytes != bBytes)
                  {
                      return aBytes > bBytes;
                  }
                  return a->stackId < b->stackId;
              });
    const size_t suspectShown = std::min(suspects.size(), static_cast<size_t>(topN));
    for (size_t i = 0; i < suspectShown; ++i)
    {
        const auto rit = rowById.find(suspects[i]->stackId);
        WriteConfidenceEntry(out, *suspects[i], rit != rowById.end() ? rit->second : nullptr, i + 1, false);
    }
    if (rows.empty())
    {
        out << "(no unfreed blocks in this window)\n";
    }

    // 常驻基线子块(交互巡检截断至TOP N,与疑似榜同env;闭窗报告不截断)
    if (!residents.empty())
    {
        uint64_t residentBytes = 0;
        for (const PerStackResult* r : residents)
        {
            const auto it = closeStats.find(r->stackId);
            if (it != closeStats.end())
            {
                residentBytes += it->second.unfreedBytes;
            }
        }
        const size_t residentShown = std::min(residents.size(), static_cast<size_t>(topN));
        out << "Resident baselines (" << residentShown << " shown of " << residents.size() << " stacks, "
            << FormatSizeAbbrev(residentBytes)
            << "; G≈0 & early-allocated, or turnover (low unfreed/alloc & flat tail); "
               "excluded from suspect list, no TOP N slot):\n";
        for (size_t i = 0; i < residentShown; ++i)
        {
            const auto rit = rowById.find(residents[i]->stackId);
            WriteConfidenceEntry(out, *residents[i], rit != rowById.end() ? rit->second : nullptr, i + 1, true);
        }
        if (residents.size() > residentShown)
        {
            out << "     ... and " << (residents.size() - residentShown)
                << " more resident stacks (see leak_overview report after window close)\n";
        }
    }

    // 周转提示NOTE(与闭窗同款:开窗前free占窗口内总申请过半)
    if (statsOk && ic.stats.preWindowFreeBytes > 0 && ic.stats.totalAllocBytes > 0 &&
        static_cast<double>(ic.stats.preWindowFreeBytes) > 0.5 * static_cast<double>(ic.stats.totalAllocBytes))
    {
        out << "NOTE: " << FormatSizeAbbrev(ic.stats.preWindowFreeBytes)
            << " pre-window allocations were freed within this window (cache turnover pattern); resident "
               "classification includes the unfreed/alloc turnover criterion\n";
    }
    text = out.str();
}

}  // namespace MemScope
