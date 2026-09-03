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

/* libmsmemscope_host_mem_hook.so单元测试
 *
 * 未preload钩子的直接运行(父进程默认filter也会枚举到HostMemHookChild.*)时
 * dlsym必失败,子套件fixture SetUp统一GTEST_SKIP,保持父进程全量回归不受影响。
 */
#include <gtest/gtest.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "host_mem_hooks/host_mem_hooks.h"

namespace
{
constexpr const char* CHILD_ENV_MARKER = "MSMEMSCOPE_HOSTMEM_UT_CHILD";
constexpr const char* HOOK_SO_NAME = "libmsmemscope_host_mem_hook.so";
constexpr const char* HOOK_SO_ENV = "MSMEMSCOPE_HOSTMEM_HOOK_SO";
constexpr const char* LEAKS_SO_NAME = "libascend_leaks.so";  // 钩子so的DT_NEEDED依赖
constexpr const char* CHILD_EXE_NAME = "memscope_hostmem_child_test";  // 子套件专用二进制
constexpr const char* EXIT_GATE_PROBE_FILE = "hostmem_exit_gate_probe";  // UT-H9孙进程atexit探针输出
constexpr const char* FORK_PROBE_FILE = "hostmem_fork_probe";  // UT-H13孙进程fork语义探针输出
constexpr int CHILD_TIMEOUT_MS = 120000;     // 子进程整体超时(全套子用例)
constexpr int GRANDCHILD_TIMEOUT_MS = 90000; // 孙进程超时(栈表满单用例,含600洪峰)
constexpr int WAIT_EVENT_TIMEOUT_MS = 5000;  // 单事件等待超时

// ---------------------------------------------------------------------------
// 子进程侧:记录型api表与驱动辅助
// ---------------------------------------------------------------------------

struct RecStage
{
    bool isStart;
    uint64_t timestamp;
    uint64_t stageId;  // 与report_stage回调签名同宽(uint64)
};

std::mutex g_recMtx;
std::vector<RecStage> g_recStages;

// get_params快照源(开窗时钩子读取): 栈深度/块阈值/采样率倒数(默认1=不采样)
std::atomic<uint32_t> g_paramDepth{16};
std::atomic<uint64_t> g_paramThreshold{0};
std::atomic<uint32_t> g_paramSampleRate{1};
std::atomic<int> g_suppressFlag{0};  // is_suppressed回调返回值

void CbReportStage(int isStart, uint64_t timestamp, uint64_t stageId)
{
    std::lock_guard<std::mutex> lock(g_recMtx);
    g_recStages.push_back(RecStage{isStart != 0, timestamp, stageId});
}

int CbIsSuppressed(void)
{
    return g_suppressFlag.load(std::memory_order_relaxed);
}

void CbGetParams(MsmemscopeHostmemParams* params)
{
    if (params == nullptr)
    {
        return;
    }
    params->stackDepth = g_paramDepth.load(std::memory_order_relaxed);
    params->sampleRate = g_paramSampleRate.load(std::memory_order_relaxed);
    params->blockThreshold = g_paramThreshold.load(std::memory_order_relaxed);
}

// 阈值RAII守卫:用例退出路径(含ASSERT失败中断)统一复位,避免泄漏到后续用例
struct ThresholdGuard
{
    explicit ThresholdGuard(uint64_t value) { g_paramThreshold.store(value); }
    ~ThresholdGuard() { g_paramThreshold.store(0); }
    ThresholdGuard(const ThresholdGuard&) = delete;
    ThresholdGuard& operator=(const ThresholdGuard&) = delete;
};

// 采样率RAII守卫(开窗快照前设置,退出复位为1=不采样)
struct SampleRateGuard
{
    explicit SampleRateGuard(uint32_t value) { g_paramSampleRate.store(value); }
    ~SampleRateGuard() { g_paramSampleRate.store(1); }
    SampleRateGuard(const SampleRateGuard&) = delete;
    SampleRateGuard& operator=(const SampleRateGuard&) = delete;
};

// dlsym取得钩子bind入口并注册记录表;父进程(未preload)返回nullptr
const MsmemscopeHostmemSvc* BindHookApi()
{
    using BindFn = const MsmemscopeHostmemSvc* (*)(const MsmemscopeHostmemApi*);
    auto bindFn = reinterpret_cast<BindFn>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_bind"));
    if (bindFn == nullptr)
    {
        return nullptr;
    }
    MsmemscopeHostmemApi api{};
    api.report_stage = CbReportStage;
    api.is_suppressed = CbIsSuppressed;
    api.get_params = CbGetParams;
    return bindFn(&api);
}

// 等待谓词成立(轮询;记账为同步路径,等待仅用于STAGE事件等异步信号)
bool WaitForPred(const std::function<bool()>& pred, int timeoutMs = WAIT_EVENT_TIMEOUT_MS)
{
    for (int i = 0; i < timeoutMs / 10; ++i)
    {
        {
            std::lock_guard<std::mutex> lock(g_recMtx);
            if (pred())
            {
                return true;
            }
        }
        usleep(10000);
    }
    return false;
}

// 等待最近的STAGE_END到达(闭窗由set_enabled调用线程同步发出,通常立即满足)
bool WaitForStageEnd()
{
    return WaitForPred([]()
                       {
                           return !g_recStages.empty() && !g_recStages.back().isStart;
                       });
}

// 窗口复位(用例间隔离):闭窗并等待closing位清零;窗口已关时set_enabled(0)为幂等no-op
void ResetWindowState(const MsmemscopeHostmemSvc* svc)
{
    auto getStatus = reinterpret_cast<int (*)(void)>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_get_status"));
    svc->set_enabled(0);
    if (getStatus == nullptr)
    {
        usleep(100000);
        return;
    }
    for (int i = 0; i < 300; ++i)  // 最多3s
    {
        if ((getStatus() & 0x4) == 0)
        {
            break;
        }
        usleep(10000);
    }
}

void ClearRecords()
{
    std::lock_guard<std::mutex> lock(g_recMtx);
    g_recStages.clear();
}

// 未归因计数(诊断导出,跨窗口累计;用例自行差分)
uint64_t GetUnattributedCount()
{
    auto fn = reinterpret_cast<uint64_t (*)(void)>(
        dlsym(RTLD_DEFAULT, "msmemscope_hostmem_get_unattributed_count"));
    return fn != nullptr ? fn() : 0;
}

// dump_live_blocks收集回调(emit含allocTs,block_detail CSV同口径)
struct DumpCollector
{
    struct Item
    {
        uint64_t addr;
        uint64_t size;
        uint64_t allocTs;
        uint64_t stackId;
    };
    std::vector<Item> items;
};

void CollectDumpItem(void* ctx, uint64_t addr, uint64_t size, uint64_t allocTs, uint64_t stackId)
{
    static_cast<DumpCollector*>(ctx)->items.push_back(DumpCollector::Item{addr, size, allocTs, stackId});
}

// dump_stack_stats收集回调(闭窗per-stack聚合快照)
struct StackStatCollector
{
    struct Row
    {
        uint64_t stackId;
        uint64_t allocCount;
        uint64_t allocBytes;
        uint64_t freedCount;
        uint64_t freedBytes;
        uint64_t unfreedCount;
        uint64_t unfreedBytes;
        uint64_t maxBlockSize;
        std::string frameDesc;  // 闭窗符号化文本;stackId=0为未知桶行(空文本)
    };
    std::vector<Row> rows;
};

void CollectStackStat(void* ctx, uint64_t stackId, uint64_t allocCount, uint64_t allocBytes, uint64_t freedCount,
                      uint64_t freedBytes, uint64_t unfreedCount, uint64_t unfreedBytes, uint64_t maxBlockSize,
                      const char* frameDesc, size_t len)
{
    auto* collector = static_cast<StackStatCollector*>(ctx);
    StackStatCollector::Row row;
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
    collector->rows.push_back(std::move(row));
}

// dump_size_distribution收集回调(闭窗大小排布桶)
struct BucketCollector
{
    struct Bucket
    {
        uint64_t rangeLow;
        uint64_t rangeHigh;
        uint64_t blockCount;
        uint64_t blockBytes;
    };
    std::vector<Bucket> buckets;
};

void CollectBucket(void* ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount, uint64_t blockBytes)
{
    auto* collector = static_cast<BucketCollector*>(ctx);
    collector->buckets.push_back(BucketCollector::Bucket{rangeLow, rangeHigh, blockCount, blockBytes});
}
}  // namespace

// ---------------------------------------------------------------------------
// 子进程用例(LD_PRELOAD钩子 + 自注册api表)
// ---------------------------------------------------------------------------

// main边界探针:本constructor运行于ld.so静态初始化阶段(先于宿主main),抓取此刻的
// 钩子状态位快照;父进程(未preload)dlsym失败,保持-1,用例自跳过。
// 门控:主测试二进制内嵌kernel_hooks的dlsym桩(g_funcMocks的构造晚于init_array,
// constructor期间调用桩即访问未构造map→段错误),且父进程本就不preload钩子、
// 探针无意义——仅子进程环境(CHILD_ENV_MARKER由SpawnHookedChild装配)执行探测
static int g_statusAtInit = -1;
__attribute__((constructor)) static void ProbeStatusAtInit()
{
    if (std::getenv(CHILD_ENV_MARKER) == nullptr)
    {
        return;  // 主测试二进制:桩环境+无钩子,探针跳过
    }
    using StatusFn = int (*)(void);
    auto statusFn = reinterpret_cast<StatusFn>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_get_status"));
    if (statusFn != nullptr)
    {
        g_statusAtInit = statusFn();
    }
}

// 子套件fixture:未preload钩子时(父进程全量回归同样枚举到本套件)dlsym必失败,
// SetUp统一GTEST_SKIP(gtest的Test::Run对IsSkipped用例不再执行TestBody)。
// 注:GTEST_SKIP()展开为"return <void表达式>",仅可用于void上下文(用例体/SetUp),
// 不能封装进返回指针的普通辅助函数(否则报void value not ignored),故无BindOrSkip。
class HostMemHookChild : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (dlsym(RTLD_DEFAULT, "msmemscope_hostmem_bind") == nullptr)
        {
            GTEST_SKIP() << "hook so not preloaded (child suite runs via HostMemHookTest.LaunchChildSuite)";
        }
    }
};

// UT-H0: 宿主main边界门控——静态初始化阶段(ld.so期)mainStarted位必为0,
// main内(测试体运行时)必为1。若trampoline拦截失效(签名不匹配/dlsym失败),
// main内位为0本用例立即变红,防止门控静默失效退化为"全进程不记账"
TEST_F(HostMemHookChild, main_gate)
{
    auto getStatus = reinterpret_cast<int (*)(void)>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_get_status"));
    ASSERT_NE(getStatus, nullptr);
    ASSERT_NE(g_statusAtInit, -1) << "status probe not installed";
    EXPECT_EQ(g_statusAtInit & 0x10, 0) << "main flag must be unset during static init";
    EXPECT_NE(getStatus() & 0x10, 0) << "main flag must be set once host main entered";
}

// UT-H1: bind握手契约——null api/缺必备回调返回nullptr;完整api返回SVC表;状态位bit0=已bind
TEST_F(HostMemHookChild, bind_contract)
{
    using BindFn = const MsmemscopeHostmemSvc* (*)(const MsmemscopeHostmemApi*);
    auto bindFn = reinterpret_cast<BindFn>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_bind"));
    ASSERT_NE(bindFn, nullptr);  // SetUp已保证钩子符号可寻,失败即为真异常

    EXPECT_EQ(bindFn(nullptr), nullptr);

    MsmemscopeHostmemApi incomplete{};
    incomplete.report_stage = nullptr;  // 缺必备回调(窗口边界事件是唯一窗口驱动)
    incomplete.is_suppressed = CbIsSuppressed;
    incomplete.get_params = CbGetParams;
    EXPECT_EQ(bindFn(&incomplete), nullptr);

    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    EXPECT_NE(svc->set_enabled, nullptr);
    EXPECT_NE(svc->get_stats, nullptr);
    EXPECT_NE(svc->dump_live_blocks, nullptr);
    EXPECT_NE(svc->dump_stack_stats, nullptr);
    EXPECT_NE(svc->dump_size_distribution, nullptr);
    EXPECT_NE(svc->dump_pre_window_distribution, nullptr);

    auto getStatus = reinterpret_cast<int (*)(void)>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_get_status"));
    ASSERT_NE(getStatus, nullptr);
    EXPECT_NE(getStatus() & 0x1, 0) << "bound flag not set";
}

// UT-H2: 窗口生命周期——set_enabled(1)同步发STAGE_START且幂等;set_enabled(0)排空后
// 发STAGE_END且幂等;首个窗口stageId=1
TEST_F(HostMemHookChild, window_lifecycle_and_stage_events)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();

    svc->set_enabled(1);
    // STAGE_START由调用线程同步发出,set_enabled返回时已记录
    {
        std::lock_guard<std::mutex> lock(g_recMtx);
        ASSERT_EQ(g_recStages.size(), 1u);
        EXPECT_TRUE(g_recStages[0].isStart);
        EXPECT_EQ(g_recStages[0].stageId, 1u);  // 本套件首个开窗用例
    }
    svc->set_enabled(1);  // 幂等:窗口已开,无第二发
    usleep(50000);
    {
        std::lock_guard<std::mutex> lock(g_recMtx);
        EXPECT_EQ(g_recStages.size(), 1u);
    }

    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd()) << "STAGE_END not reported";
    {
        std::lock_guard<std::mutex> lock(g_recMtx);
        EXPECT_EQ(g_recStages.back().stageId, 1u);
    }
    svc->set_enabled(0);  // 幂等:窗口已关
    usleep(50000);
    {
        std::lock_guard<std::mutex> lock(g_recMtx);
        EXPECT_EQ(g_recStages.size(), 2u);  // 仍为一始一终
    }
}

// UT-H3: 闭窗快照聚合——同一调用点两次分配归栈同一StackEntry;释放一块后计数
// 回扣;闭窗dump_stack_stats精确聚合(alloc=2/freed=1/unfreed=1,字节口径同步),
// 存活栈统一符号化(帧文本首行锚点+栈底trampoline的机械帧过滤回归),块表投影
// 冻结(闭窗后free不再出表)
TEST_F(HostMemHookChild, close_window_snapshot_aggregation)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(4096);  // 排除框架分配噪声,保证计数确定性

    svc->set_enabled(1);
    // 同一调用点两次分配(循环体同一行):归栈同一StackEntry,块表两条目
    void* blocks[2] = {nullptr, nullptr};
    for (int i = 0; i < 2; ++i)
    {
        blocks[i] = malloc(12345);
        ASSERT_NE(blocks[i], nullptr);
    }
    const uint64_t keepAddr = reinterpret_cast<uint64_t>(blocks[1]);

    // 释放一块:块表删除+释放计数+栈计数回扣(malloc/free记账均为同步路径)
    free(blocks[0]);
    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.liveBlockCount, 1u);
    EXPECT_EQ(stats.totalAllocCount, 2u);
    EXPECT_EQ(stats.totalAllocBytes, 24690u);
    EXPECT_EQ(stats.totalFreedCount, 1u);
    EXPECT_EQ(stats.totalFreedBytes, 12345u);
    EXPECT_EQ(stats.sampleRate, 1u);
    EXPECT_EQ(stats.truncated, 0u);

    // 闭窗:per-stack精确聚合(真源=块表遍历),符号化文本先于STAGE_END就绪
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd()) << "STAGE_END not reported";

    StackStatCollector sc;
    svc->dump_stack_stats(CollectStackStat, &sc);
    ASSERT_EQ(sc.rows.size(), 2u) << "expect 1 real stack + 1 unknown-bucket row";  // 未知桶行恒在
    const StackStatCollector::Row* row = nullptr;
    for (const auto& r : sc.rows)
    {
        if (r.stackId != 0)
        {
            row = &r;
        }
    }
    ASSERT_NE(row, nullptr) << "real stack row missing";
    EXPECT_EQ(row->allocCount, 2u);
    EXPECT_EQ(row->allocBytes, 24690u);
    EXPECT_EQ(row->freedCount, 1u) << "freed = alloc - unfreed derivation";
    EXPECT_EQ(row->freedBytes, 12345u);
    EXPECT_EQ(row->unfreedCount, 1u);
    EXPECT_EQ(row->unfreedBytes, 12345u);
    EXPECT_EQ(row->maxBlockSize, 12345u);
    EXPECT_FALSE(row->frameDesc.empty()) << "live stack text must be symbolized at close";

    // 帧过滤回归(钩子自身帧动态过滤):首行应为劫持入口帧(分配接口锚点)——静态
    // 跳帧计数随编译内联/架构/glibc版本漂移时曾漏出多帧头部机械帧(aarch64实测
    // 曾漏2帧)。过滤为头部连续段剔除,栈尾不过滤:主线程回栈尾部天然存在宿主main
    // 边界trampoline帧(宿主main经__libc_start_main进入的必经帧),它是真实调用链
    // 组成部分,剔除会使main与__libc_start_main之间断链、回溯不清晰,设计选择
    // 保留。故主线程栈中本so帧恰两处:首行锚点+栈底trampoline;第二帧必须位于
    // 栈底(其后仅剩进程启动边界帧),出现在中部(尤其紧跟锚点)才是头部过滤泄漏
    // 的机械帧
    const std::string hookSoName = "libmsmemscope_host_mem_hook.so";
    const size_t firstLineEnd = row->frameDesc.find('\n');
    EXPECT_NE(row->frameDesc.substr(0, firstLineEnd).find(hookSoName), std::string::npos)
        << "first frame must be the hook entry frame";
    size_t hookFrames = 0;
    size_t lastHookPos = std::string::npos;
    for (size_t pos = 0; (pos = row->frameDesc.find(hookSoName, pos)) != std::string::npos; ++pos)
    {
        ++hookFrames;
        lastHookPos = pos;
    }
    EXPECT_EQ(hookFrames, 2u) << "hook so frames: expect head anchor + bottom main-gate trampoline only";
    ASSERT_NE(lastHookPos, std::string::npos);
    size_t lastHookLine = 0;  // 最后一个本so帧的行号(0起)
    size_t totalLines = 1;
    for (size_t i = 0; i < row->frameDesc.size(); ++i)
    {
        if (row->frameDesc[i] != '\n')
        {
            continue;
        }
        ++totalLines;
        if (i < lastHookPos)
        {
            ++lastHookLine;
        }
    }
    // trampoline下方仅剩启动边界帧(__libc_start_call_main/__libc_start_main/
    // _start,glibc版本差异1~4帧,含尾部换行余量取5)
    EXPECT_LE(totalLines - lastHookLine, 5u)
        << "hook frame between head anchor and bottom trampoline is machinery leak";

    // 块表投影:存活块=保留块,allocTs>0,stackId==闭窗聚合行
    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    ASSERT_EQ(collector.items.size(), 1u);
    EXPECT_EQ(collector.items[0].addr, keepAddr);
    EXPECT_EQ(collector.items[0].size, 12345u);
    EXPECT_GT(collector.items[0].allocTs, 0u);
    EXPECT_EQ(collector.items[0].stackId, row->stackId);

    // 闭窗后释放保留块:记账门控已关,冻结表不受影响
    free(blocks[1]);
    DumpCollector frozenCollector;
    svc->dump_live_blocks(CollectDumpItem, &frozenCollector);
    EXPECT_EQ(frozenCollector.items.size(), 1u);
}

// UT-H4: malloc族覆盖——calloc总量/posix_memalign/aligned_alloc记账;realloc迁移
// 语义(FREE旧+MALLOC新,无论原地扩或迁移);闭窗块表投影尺寸集完整、旧块出表
TEST_F(HostMemHookChild, malloc_family_coverage)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(1024);  // 过滤框架噪声,放行全部族分配(3000/2048/4096/1024/4096)

    svc->set_enabled(1);

    // calloc: 记账size=总量
    void* c = calloc(3, 1000);
    ASSERT_NE(c, nullptr);
    const uint64_t callocAddr = reinterpret_cast<uint64_t>(c);

    // posix_memalign / aligned_alloc
    void* pm = nullptr;
    ASSERT_EQ(posix_memalign(&pm, 64, 2048), 0);
    ASSERT_NE(pm, nullptr);
    const uint64_t pmAddr = reinterpret_cast<uint64_t>(pm);
    void* aa = aligned_alloc(64, 4096);
    ASSERT_NE(aa, nullptr);
    const uint64_t aaAddr = reinterpret_cast<uint64_t>(aa);

    // realloc: 无论原地扩或迁移,旧块(1024B)释放记账+新块(4096B)分配记账
    void* r = malloc(1024);
    ASSERT_NE(r, nullptr);
    const uint64_t oldAddr = reinterpret_cast<uint64_t>(r);
    void* r2 = realloc(r, 4096);
    ASSERT_NE(r2, nullptr);
    const uint64_t newAddr = reinterpret_cast<uint64_t>(r2);

    // 记账计数: 5次申请(c/pm/aa/旧r/新r),1次释放(旧r)
    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.totalAllocCount, 5u);
    EXPECT_EQ(stats.totalAllocBytes, 3000u + 2048u + 4096u + 1024u + 4096u);
    EXPECT_EQ(stats.totalFreedCount, 1u);
    EXPECT_EQ(stats.totalFreedBytes, 1024u);
    EXPECT_EQ(stats.liveBlockCount, 4u);

    // 块表投影: 4块,尺寸集{3000,2048,4096,4096},旧1024块已出表
    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    ASSERT_EQ(collector.items.size(), 4u);
    bool hasCalloc = false;
    bool hasPm = false;
    bool hasAa = false;
    bool hasNew = false;
    for (const auto& item : collector.items)
    {
        EXPECT_GT(item.allocTs, 0u);
        EXPECT_NE(item.stackId, 0u) << "family allocations must register stacks";
        if (item.addr == callocAddr)
        {
            hasCalloc = true;
            EXPECT_EQ(item.size, 3000u);
        }
        if (item.addr == pmAddr)
        {
            hasPm = true;
            EXPECT_EQ(item.size, 2048u);
        }
        if (item.addr == aaAddr)
        {
            hasAa = true;
            EXPECT_EQ(item.size, 4096u);
        }
        if (item.addr == newAddr)
        {
            hasNew = true;
            EXPECT_EQ(item.size, 4096u);
        }
        EXPECT_NE(item.addr, oldAddr) << "realloc old block must leave the table";
    }
    EXPECT_TRUE(hasCalloc);
    EXPECT_TRUE(hasPm);
    EXPECT_TRUE(hasAa);
    EXPECT_TRUE(hasNew);

    free(c);
    free(pm);
    free(aa);
    free(r2);
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());
}

// UT-H5: 块大小阈值——get_params快照blockThreshold,size<N的分配不记账
// (untracked计数),≥N照常记账;未记账块free进开窗前通道(两账本均未命中)
TEST_F(HostMemHookChild, block_size_threshold_filter)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(4096);

    svc->set_enabled(1);  // 开窗时经get_params取阈值快照
    void* small = malloc(100);   // < 4096:未追踪(不记账)
    void* large = malloc(8192);  // >= 4096:记账
    ASSERT_NE(small, nullptr);
    ASSERT_NE(large, nullptr);
    const uint64_t largeAddr = reinterpret_cast<uint64_t>(large);

    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.totalAllocCount, 1u) << "below-threshold alloc must not be accounted";
    EXPECT_EQ(stats.totalAllocBytes, 8192u);
    EXPECT_EQ(stats.untrackedCount, 1u);
    EXPECT_EQ(stats.untrackedBytes, 100u);
    EXPECT_EQ(stats.liveBlockCount, 1u);

    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    ASSERT_EQ(collector.items.size(), 1u);
    EXPECT_EQ(collector.items[0].addr, largeAddr);
    EXPECT_EQ(collector.items[0].size, 8192u);

    free(small);  // 未记账块free→开窗前通道(两账本均未命中)
    free(large);
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());
}

// UT-H6: is_suppressed回调——采集库抑制窗口内(返回1)的分配不记账(块表无记录);
// 已记账块在抑制窗口内free不处理(块表不删除、释放计数不增)。抑制全程持有防
// 地址复用陷阱:被抑制块在断言前释放后,后续同尺寸malloc经glibc unsorted精确
// 匹配/top合并极可能复用同一地址,正常分配会伪装成"抑制失效"
TEST_F(HostMemHookChild, suppression_callback_respected)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();

    // 过滤框架分配噪声(含dump回调自身vector扩容),保证投影确定性;
    // 须先于开窗——阈值由开窗时get_params快照,晚设对本窗口无效
    ThresholdGuard thresholdGuard(4096);
    svc->set_enabled(1);

    // 分配侧:抑制窗口内的分配不记账(块表亦无记录)
    g_suppressFlag.store(1);
    void* suppressed = malloc(8192);
    ASSERT_NE(suppressed, nullptr);
    const uint64_t suppressedAddr = reinterpret_cast<uint64_t>(suppressed);

    // 正常块:suppressed持有期间分配,地址必然不同(防御性断言钉住该不变式)
    g_suppressFlag.store(0);
    void* recorded = malloc(8192);
    ASSERT_NE(recorded, nullptr);
    const uint64_t recordedAddr = reinterpret_cast<uint64_t>(recorded);
    ASSERT_NE(recordedAddr, suppressedAddr) << "allocator handed out an in-use address";

    // 释放侧:已记账块在抑制窗口内free不处理(块表不删除)
    g_suppressFlag.store(1);
    free(recorded);
    g_suppressFlag.store(0);

    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.totalAllocCount, 1u) << "suppressed alloc must not be accounted";
    EXPECT_EQ(stats.totalFreedCount, 0u) << "suppressed free must not be processed";
    EXPECT_EQ(stats.liveBlockCount, 1u) << "recorded block must survive suppressed free";

    // 块表投影:仅recorded在表,suppressed块无记录
    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    ASSERT_EQ(collector.items.size(), 1u);
    EXPECT_EQ(collector.items[0].addr, recordedAddr);

    free(suppressed);  // 孤儿free(两账本均未命中)→开窗前通道;此后无分配,无复用干扰
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());
}

// UT-H7: 孤儿FREE进开窗前通道——开窗前分配的块(不在块表/溢出账本)在窗口内free
// 不产生释放记账(不并入totalFreed),但独立通道统计次数/总量(大小经malloc_usable_size
// 近似);free(NULL)无崩溃
TEST_F(HostMemHookChild, orphan_free_silent)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();

    void* preWindow = malloc(777);  // 窗口外分配,未记账
    ASSERT_NE(preWindow, nullptr);
    const uint64_t orphanAddr = reinterpret_cast<uint64_t>(preWindow);

    svc->set_enabled(1);
    free(nullptr);  // 不崩溃
    free(preWindow);  // 孤儿free:两账本均未命中→开窗前通道(不并入totalFreed)

    void* probe = malloc(8192);
    ASSERT_NE(probe, nullptr);
    const uint64_t probeAddr = reinterpret_cast<uint64_t>(probe);

    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.totalAllocCount, 1u) << "only probe accounted";
    EXPECT_EQ(stats.totalFreedCount, 0u) << "orphan free must not merge into totalFreed";
    EXPECT_EQ(stats.preWindowFreeCount, 1u) << "orphan free must land in pre-window channel";
    EXPECT_GE(stats.preWindowFreeBytes, 777u) << "pre-window size = usable size of orphan block";
    EXPECT_EQ(stats.liveBlockCount, 1u);

    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());
    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    ASSERT_EQ(collector.items.size(), 1u);
    EXPECT_EQ(collector.items[0].addr, probeAddr);
    EXPECT_NE(collector.items[0].addr, orphanAddr) << "orphan block must never enter the table";

    // 开窗前通道闭窗快照:孤儿free按大小落桶(范围含777B,即[0,1K)桶)
    BucketCollector preDist;
    ASSERT_NE(svc->dump_pre_window_distribution, nullptr);
    svc->dump_pre_window_distribution(CollectBucket, &preDist);
    uint64_t preCount = 0;
    uint64_t preBytes = 0;
    for (const auto& b : preDist.buckets)
    {
        preCount += b.blockCount;
        preBytes += b.blockBytes;
    }
    EXPECT_EQ(preCount, 1u) << "pre-window dist must carry the orphan free";
    EXPECT_GE(preBytes, 777u);

    free(probe);
}

// UT-H8: 统计与全量投影——get_stats零截断/存活块计数与闭窗冻结值一致,
// dump_live_blocks如实投影(含allocTs);关窗后表冻结仍可dump
TEST_F(HostMemHookChild, stats_and_live_block_dump)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(4096);

    svc->set_enabled(1);
    void* keep1 = malloc(5555);
    void* keep2 = malloc(6666);
    void* tmp = malloc(7777);
    ASSERT_NE(keep1, nullptr);
    ASSERT_NE(keep2, nullptr);
    ASSERT_NE(tmp, nullptr);
    const uint64_t keep1Addr = reinterpret_cast<uint64_t>(keep1);
    const uint64_t keep2Addr = reinterpret_cast<uint64_t>(keep2);
    free(tmp);

    // 记账同步完成,直接读统计(开启态为实时计数器)
    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.liveBlockCount, 2u);
    EXPECT_EQ(stats.totalAllocCount, 3u);
    EXPECT_EQ(stats.totalAllocBytes, 5555u + 6666u + 7777u);
    EXPECT_EQ(stats.totalFreedCount, 1u);
    EXPECT_EQ(stats.totalFreedBytes, 7777u);
    EXPECT_EQ(stats.untrackedCount, 0u);
    EXPECT_EQ(stats.sampleRate, 1u);
    EXPECT_EQ(stats.truncated, 0u);

    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    ASSERT_EQ(collector.items.size(), 2u);
    bool hasKeep1 = false;
    bool hasKeep2 = false;
    for (const auto& item : collector.items)
    {
        EXPECT_GT(item.allocTs, 0u);
        EXPECT_NE(item.stackId, 0u);
        if (item.addr == keep1Addr)
        {
            hasKeep1 = true;
            EXPECT_EQ(item.size, 5555u);
        }
        if (item.addr == keep2Addr)
        {
            hasKeep2 = true;
            EXPECT_EQ(item.size, 6666u);
        }
    }
    EXPECT_TRUE(hasKeep1);
    EXPECT_TRUE(hasKeep2);

    // 关窗后表冻结,统计与投影仍完整(分析器闭窗拉快照依赖此语义)
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());
    MsmemscopeHostmemStats frozen{};
    svc->get_stats(&frozen);
    EXPECT_EQ(frozen.liveBlockCount, 2u);
    EXPECT_EQ(frozen.totalAllocCount, 3u);
    EXPECT_EQ(frozen.totalFreedCount, 1u);
    DumpCollector frozenCollector;
    svc->dump_live_blocks(CollectDumpItem, &frozenCollector);
    EXPECT_EQ(frozenCollector.items.size(), 2u);

    free(keep1);
    free(keep2);
}

// UT-H9: exit门控——孙进程显式调exit(42),atexit探针在退出序列内读钩子状态位:
// ①0x20位(退出标志)必须已置位——exit拦截先于atexit处理器生效,置位后退出序列内的
//   分配/释放全部直转真函数(不再记账,退出期堆环境不可信的防护);
// ②退出码42经真exit透传(拦截器不得吞掉atexit/退出码语义);
// ③atexit序列正常执行(探针文件存在)。探针用原生write(非stdio)避免与gtest流缓冲纠缠
TEST_F(HostMemHookChild, exit_gate)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);  // 关窗态fork,孙进程侧变量最小

    unlink(EXIT_GATE_PROBE_FILE);  // 清理上次残留

    fflush(stdout);  // fork前冲刷,防孙进程exit时重复flush父进程的gtest缓冲
    fflush(stderr);
    const pid_t grandchild = fork();
    ASSERT_GE(grandchild, 0) << "fork failed";
    if (grandchild == 0)
    {
        // 孙进程:atexit探针(退出序列内读状态位写文件)后显式exit——经PLT解析到
        // 钩子so的exit拦截器(退出标志置位→转真exit→atexit跑本探针)。
        // 探针路径为文件级常量(无捕获lambda方可注册为atexit回调)
        atexit([]()
        {
            using StatusFn = int (*)(void);
            auto statusFn =
                reinterpret_cast<StatusFn>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_get_status"));
            const int flags = statusFn != nullptr ? statusFn() : -1;
            const int fd = open(EXIT_GATE_PROBE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0)
            {
                char buf[32];
                const int n = snprintf(buf, sizeof(buf), "%d\n", flags);
                if (n > 0)
                {
                    ssize_t wr = write(fd, buf, static_cast<size_t>(n));
                    (void)wr;
                }
                close(fd);
            }
        });
        exit(42);
    }

    int st = 0;
    ASSERT_EQ(waitpid(grandchild, &st, 0), grandchild);
    ASSERT_TRUE(WIFEXITED(st)) << "grandchild terminated abnormally";
    EXPECT_EQ(WEXITSTATUS(st), 42) << "exit code must pass through the real exit";

    const int fd = open(EXIT_GATE_PROBE_FILE, O_RDONLY);
    ASSERT_GE(fd, 0) << "atexit probe file missing (atexit handlers must still run)";
    char buf[32] = {0};
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    unlink(EXIT_GATE_PROBE_FILE);
    ASSERT_GT(n, static_cast<ssize_t>(0)) << "probe file empty";
    const int flags = atoi(buf);
    ASSERT_NE(flags, -1) << "hook get_status unreachable in atexit context";
    EXPECT_NE(flags & 0x20, 0) << "exiting flag (bit 0x20) must be set inside atexit handler";
    EXPECT_NE(flags & 0x1, 0) << "bound flag must persist (sanity)";
}

// UT-H13: fork语义——fork后代不监控。开窗态fork,孙进程内:①bit6(fork后代)置位;
// ②set_enabled(1)被拒(窗口位bit1不置位——fork后代一切开窗被拒);③closing位为0
// (fork发生于父进程关窗排空中时,孙进程atexit闭窗等待不挂死的前提);孙进程内
// malloc走真函数零记账(生产闸门已关,天然无事件)。父进程不受fork影响:窗口仍开、
// 无fork标记,闭窗后STAGE_END正常送达(父进程零丢失)。孙进程结果经探针文件回传
// (原生write防与gtest流缓冲纠缠;_exit绕过atexit与析构,孙进程侧零副作用)
TEST_F(HostMemHookChild, fork_descendant_not_monitored)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();

    svc->set_enabled(1);
    auto getStatus = reinterpret_cast<int (*)(void)>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_get_status"));
    ASSERT_NE(getStatus, nullptr);
    ASSERT_NE(getStatus() & 0x2, 0) << "window must be open before fork";

    unlink(FORK_PROBE_FILE);
    fflush(stdout);  // fork前冲刷,防孙进程exit时重复flush父进程的gtest缓冲
    fflush(stderr);
    const pid_t grandchild = fork();
    ASSERT_GE(grandchild, 0) << "fork failed";
    if (grandchild == 0)
    {
        // 孙进程: 开窗尝试(必须被拒)后分配一次(零记账),状态位经探针回传
        svc->set_enabled(1);
        void* p = malloc(7777);
        const int childFlags = getStatus();
        const int fd = open(FORK_PROBE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            char buf[32];
            const int n = snprintf(buf, sizeof(buf), "%d\n", childFlags);
            if (n > 0)
            {
                ssize_t wr = write(fd, buf, static_cast<size_t>(n));
                (void)wr;
            }
            close(fd);
        }
        free(p);
        _exit(0);  // 原生退出,不经atexit/析构序列
    }

    int st = 0;
    ASSERT_EQ(waitpid(grandchild, &st, 0), grandchild);
    ASSERT_TRUE(WIFEXITED(st)) << "grandchild terminated abnormally";
    EXPECT_EQ(WEXITSTATUS(st), 0);

    const int fd = open(FORK_PROBE_FILE, O_RDONLY);
    ASSERT_GE(fd, 0) << "fork probe file missing";
    char buf[32] = {0};
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    unlink(FORK_PROBE_FILE);
    ASSERT_GT(n, static_cast<ssize_t>(0)) << "probe file empty";
    const int childFlags = atoi(buf);
    EXPECT_NE(childFlags & 0x40, 0) << "forked flag (bit 0x40) must be set in fork descendant";
    EXPECT_EQ(childFlags & 0x2, 0) << "window open must be rejected in fork descendant";
    EXPECT_EQ(childFlags & 0x4, 0) << "closing flag must be cleared in fork descendant";

    // 父进程侧:窗口跨fork存活、无fork标记,闭窗路径完整(STAGE_END送达=排空+派发正常)
    const int parentFlags = getStatus();
    EXPECT_NE(parentFlags & 0x2, 0) << "parent window must survive fork";
    EXPECT_EQ(parentFlags & 0x40, 0) << "parent must not carry forked flag";
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd()) << "parent STAGE_END not reported after fork";
}

// 洪流用例的调用点生成器:每个模板实例化含独立的malloc调用点(独立去重键),
// 递归深度上限600(实例化按语法展开,须以显式特化阻断链条,运行时分支拦不住)
template <int Depth>
void FloodDistinctSites(int count, std::vector<void*>& keep)
{
    if (Depth >= count)
    {
        return;
    }
    keep.push_back(malloc(64));
    FloodDistinctSites<Depth + 1>(count, keep);
}

