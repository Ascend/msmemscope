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

template <>
void FloodDistinctSites<600>(int, std::vector<void*>&)
{
}

// 闭窗聚合用例的调用点生成器:四相各300个独立调用点(相内每个模板实例化含独立
// malloc调用点;实例化按语法展开,同样须以显式特化阻断递归链条)
template <int Depth, int Gen>
void FloodGen(int count, std::vector<void*>& keep)
{
    if (Depth >= count)
    {
        return;
    }
    keep.push_back(malloc(64));
    FloodGen<Depth + 1, Gen>(count, keep);
}

// 函数模板只能全特化(偏特化不合法),四相各需一条阻断特化
template <>
void FloodGen<300, 0>(int, std::vector<void*>&)
{
}

template <>
void FloodGen<300, 1>(int, std::vector<void*>&)
{
}

template <>
void FloodGen<300, 2>(int, std::vector<void*>&)
{
}

template <>
void FloodGen<300, 3>(int, std::vector<void*>&)
{
}

// 锤击用例的调用点生成器:每实例化独立malloc/realloc调用点,按Depth混合三种路径
// (纯释放/重分配成功/重分配失败回插);递归深度上限300,须以显式特化阻断链条。
// 轮末由调用方释放keep中保留块——存活站点随轮次更替,死栈供给持续再生
template <int Depth, int Gen>
void HammerGen(int count, std::vector<void*>& keep)
{
    if (Depth >= count)
    {
        return;
    }
    void* p = malloc(64);
    const int mode = Depth % 3;
    if (mode == 0)
    {
        free(p);  // 纯释放:块引用dispose,站点变死栈(淘汰供给)
    }
    else if (mode == 1)
    {
        void* q = realloc(p, 128);  // 成功路径:旧块引用dispose+新块记账
        keep.push_back(q != nullptr ? q : p);  // 防御OOM:失败则原块保持有效
    }
    else
    {
        void* q = realloc(p, static_cast<size_t>(-1));  // 必失败:ReinsertBlock回插引用归块
        if (q != nullptr)
        {
            keep.push_back(q);
        }
        else
        {
            free(p);
        }
    }
    HammerGen<Depth + 1, Gen>(count, keep);
}

// 函数模板只能全特化(偏特化不合法),四线程各需一条阻断特化
template <>
void HammerGen<300, 0>(int, std::vector<void*>&)
{
}

template <>
void HammerGen<300, 1>(int, std::vector<void*>&)
{
}

template <>
void HammerGen<300, 2>(int, std::vector<void*>&)
{
}

template <>
void HammerGen<300, 3>(int, std::vector<void*>&)
{
}

// UT-H10: 洪流闭窗符号化——600个独立调用点灌入并全部保留存活,闭窗统一符号化:
// 每个存活栈(dump_live_blocks投影的stackId)在dump_stack_stats中必有非空文本,
// 逐块归栈(600块=600个不同栈),块表投影完整(记账为同步路径,无需等事件)。
// 默认栈表容量(40万)下无截断:全归栈、无未知桶
TEST_F(HostMemHookChild, stack_text_delivered_at_close)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(64);  // 过滤框架噪声,放行全部64B洪峰块

    std::vector<void*> keep;
    keep.reserve(600);  // 开窗前预留:窗口内vector扩容分配不得混入记账
    svc->set_enabled(1);
    FloodDistinctSites<0>(600, keep);

    // 记账同步完成,直接读统计
    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.totalAllocCount, 600u);
    EXPECT_EQ(stats.liveBlockCount, 600u);
    EXPECT_EQ(stats.sampleRate, 1u);
    EXPECT_EQ(stats.truncated, 0u) << "default stack capacity must not truncate 600 sites";

    // 闭窗:全部存活栈统一符号化(默认容量600站<40万上限,全归栈)
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd()) << "STAGE_END not reported after close sweep";

    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    ASSERT_EQ(collector.items.size(), 600u);
    std::set<uint64_t> liveIds;
    for (const auto& item : collector.items)
    {
        EXPECT_EQ(item.size, 64u);
        EXPECT_GT(item.allocTs, 0u);
        EXPECT_NE(item.stackId, 0u) << "no truncation at default capacity";
        liveIds.insert(item.stackId);
    }
    ASSERT_EQ(liveIds.size(), 600u) << "each distinct call site must register its own stack";

    StackStatCollector sc;
    svc->dump_stack_stats(CollectStackStat, &sc);
    uint64_t realRows = 0;
    for (const auto& row : sc.rows)
    {
        if (row.stackId == 0)
        {
            continue;  // 未知桶行(计数0)
        }
        realRows += 1;
        EXPECT_EQ(row.unfreedCount, 1u);
        EXPECT_EQ(row.unfreedBytes, 64u);
        EXPECT_FALSE(row.frameDesc.empty()) << "live stack text lost at close, stackId=" << row.stackId;
    }
    ASSERT_EQ(realRows, 600u) << "all flood stacks must be symbolized at close";

    for (void* p : keep)
    {
        free(p);
    }
}

// UT-H11: 账本自检——完整生命周期(开窗→分配/释放→闭窗)后,自检符号遍历全部分片
// 校验:栈表/块表全局计数与分片之和一致、栈条目活跃计数非负。记账路径各态转换
// 有缺陷(漏删块/计数不对称)时返回非零
TEST_F(HostMemHookChild, link_invariants_selfcheck)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    auto selfcheck = reinterpret_cast<uint64_t (*)(void)>(
        dlsym(RTLD_DEFAULT, "msmemscope_hostmem_selfcheck"));
    ASSERT_NE(selfcheck, nullptr);

    svc->set_enabled(1);
    void* keep1 = malloc(4096);
    void* keep2 = malloc(8192);
    void* tmp = malloc(4096);
    ASSERT_NE(keep1, nullptr);
    ASSERT_NE(keep2, nullptr);
    ASSERT_NE(tmp, nullptr);
    // 三态并存(活栈keep1/keep2+已释放栈tmp)下自检须通过
    free(tmp);  // 块表删除+栈计数回扣为同步路径
    EXPECT_EQ(selfcheck(), 0u) << "ledger invariants broken while window open";

    // 闭窗:块表/栈表冻结,自检仍须通过
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());
    EXPECT_EQ(selfcheck(), 0u) << "ledger invariants broken after window closed";
    free(keep1);
    free(keep2);
}

// UT-H12: 闭窗聚合不变量——四相各300个独立调用点"分配后全部释放"后,闭窗聚合
// 每栈满足 freed=alloc−unfreed 派生不变量(释放栈freed==alloc/unfreed==0),
// 存活keeper栈unfreed精确=1;大小排布桶合计=块表投影合计=总未释放量;
// 默认容量下无截断
TEST_F(HostMemHookChild, close_aggregation_invariant_under_flood)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(64);  // 过滤框架噪声(洪峰块恰64B,全部放行)

    std::vector<void*> keep;
    keep.reserve(300);  // 开窗前预留:窗口内vector扩容分配不得混入记账
    svc->set_enabled(1);

    // 四相:每相300个独立调用点各分配1块(64B)后全部释放(记账同步,无需等待)
    for (int gen = 0; gen < 4; ++gen)
    {
        switch (gen)
        {
            case 0:
                FloodGen<0, 0>(300, keep);
                break;
            case 1:
                FloodGen<0, 1>(300, keep);
                break;
            case 2:
                FloodGen<0, 2>(300, keep);
                break;
            default:
                FloodGen<0, 3>(300, keep);
                break;
        }
        for (void* p : keep)
        {
            free(p);
        }
        keep.clear();
    }

    // 常驻keeper:保留到闭窗——验证未释放聚合与符号化
    void* keeper1 = malloc(2048);
    void* keeper2 = malloc(4096);
    ASSERT_NE(keeper1, nullptr);
    ASSERT_NE(keeper2, nullptr);

    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.totalAllocCount, 1202u);  // 4×300+2
    EXPECT_EQ(stats.totalFreedCount, 1200u);
    EXPECT_EQ(stats.liveBlockCount, 2u);
    EXPECT_EQ(stats.truncated, 0u);

    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());

    // per-stack聚合:全量满足 freed=alloc−unfreed;释放栈freed==alloc;keeper栈unfreed==1
    StackStatCollector sc;
    svc->dump_stack_stats(CollectStackStat, &sc);
    uint64_t sumAlloc = 0;
    uint64_t sumFreed = 0;
    uint64_t sumUnfreed = 0;
    uint64_t keeperRows = 0;
    for (const auto& row : sc.rows)
    {
        if (row.stackId == 0)
        {
            continue;  // 未知桶行(计数0)
        }
        EXPECT_EQ(row.freedCount, row.allocCount - row.unfreedCount)
            << "freed = alloc - unfreed must hold per stack";
        EXPECT_EQ(row.freedBytes, row.allocBytes - row.unfreedBytes);
        sumAlloc += row.allocCount;
        sumFreed += row.freedCount;
        sumUnfreed += row.unfreedCount;
        if (row.unfreedCount > 0)
        {
            keeperRows += 1;
            EXPECT_EQ(row.unfreedCount, 1u);
            EXPECT_FALSE(row.frameDesc.empty()) << "keeper stack must be symbolized at close";
        }
        else
        {
            EXPECT_EQ(row.freedCount, row.allocCount) << "freed-phase stacks must be fully released";
        }
    }
    EXPECT_EQ(sumAlloc, 1202u);
    EXPECT_EQ(sumFreed, 1200u);
    EXPECT_EQ(sumUnfreed, 2u);
    EXPECT_EQ(keeperRows, 2u);

    // 大小排布:桶合计==块表投影合计==总未释放量(2048+4096)
    BucketCollector bc;
    svc->dump_size_distribution(CollectBucket, &bc);
    uint64_t bucketCount = 0;
    uint64_t bucketBytes = 0;
    for (const auto& b : bc.buckets)
    {
        bucketCount += b.blockCount;
        bucketBytes += b.blockBytes;
    }
    EXPECT_EQ(bucketCount, 2u);
    EXPECT_EQ(bucketBytes, 2048u + 4096u);
    EXPECT_EQ(bucketCount, stats.liveBlockCount) << "bucket total must equal block total";

    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    EXPECT_EQ(collector.items.size(), 2u);

    free(keeper1);
    free(keeper2);
}

// UT-H14: 显式采样——采样率倒数2(采样门控在记账之前,被跳过块对钩子完全不可见):
// ①生效采样率如实呈现在get_stats;②记账量<分配量(采样必跳过至少一块)且>0;
// ③块表投影==记账量(采样不引入块表/计数不一致);④闭窗聚合的alloc==记账量
TEST_F(HostMemHookChild, explicit_sampling_gate)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    SampleRateGuard rateGuard(2);          // 采样率倒数2
    ThresholdGuard thresholdGuard(64);     // 过滤框架噪声

    // 开窗前预留块表投影收集器容量:窗口内dump期间vector扩容的malloc
    // 也会被采样记账(与malloc(8192)调用点不同),污染"被跳过块不可见"判定
    DumpCollector collector;
    collector.items.reserve(16);
    svc->set_enabled(1);
    void* blocks[16] = {nullptr};
    for (int i = 0; i < 16; ++i)
    {
        blocks[i] = malloc(8192);
        ASSERT_NE(blocks[i], nullptr);
    }

    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.sampleRate, 2u) << "effective sample rate must be reported";
    EXPECT_GE(stats.totalAllocCount, 1u);
    EXPECT_LT(stats.totalAllocCount, 16u) << "rate=2 must skip at least one block (P(skip)=2^-16)";
    EXPECT_EQ(stats.liveBlockCount, stats.totalAllocCount) << "sampled blocks all kept live";
    EXPECT_EQ(stats.truncated, 0u);

    // 被跳过块对钩子完全不可见:块表投影==记账量
    svc->dump_live_blocks(CollectDumpItem, &collector);
    EXPECT_EQ(collector.items.size(), stats.totalAllocCount);

    // 闭窗聚合:同一调用点仅一个栈,alloc==记账量
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());
    StackStatCollector sc;
    svc->dump_stack_stats(CollectStackStat, &sc);
    uint64_t realRows = 0;
    for (const auto& row : sc.rows)
    {
        if (row.stackId == 0)
        {
            continue;
        }
        realRows += 1;
        EXPECT_EQ(row.allocCount, stats.totalAllocCount) << "sampled allocs share one stack";
    }
    EXPECT_EQ(realRows, 1u);

    for (void* p : blocks)
    {
        free(p);
    }
}

// UT-H15: 栈表满转未知桶继续记账(孙进程运行,见LaunchGrandchildUnattrCycle;
// MAX_STACKS=128→分片容量2,默认容量下洪峰打不满表)。600独立调用点全部保留
// 存活灌入(全为活栈,refs>=2无淘汰供给——存活栈永不可淘汰),表满后新站点登记
// 必失败→转未知桶(stackId=0)。
// 判据:①get_stats truncated bit1置位(栈表触顶且死栈回收无法腾位:全活表无
// 供给,淘汰采样无可淘汰者)且bit0清零(块表未截断),evicted*全零(无死栈被淘汰);
// ②未归因计数>0(每次登记失败=1块转未知桶);③块表投影600块完整,未知桶块
// (stackId=0)与正常归栈块并存;④闭窗聚合:未知桶行unfreed==未知桶块数,
// 全部行unfreed合计=600(账本未失真,仅归因粒度退化)
TEST_F(HostMemHookChild, stack_table_full_unattributed)
{
    // 仅孙进程运行(LaunchGrandchildUnattrCycle压低MAX_STACKS=128,分片容量2);
    // 全量套件下默认容量40万,600站点洪峰打不满栈表,此用例失去意义(bit1必为0)
    const char* maxStacksEnv = std::getenv("MSMEMSCOPE_HOSTMEM_MAX_STACKS");
    if (maxStacksEnv == nullptr || atoi(maxStacksEnv) != 128)
    {
        GTEST_SKIP() << "run via HostMemHookTest.LaunchGrandchildUnattrCycle (MAX_STACKS=128)";
    }
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(64);  // 放行全部64B洪峰块,过滤框架噪声

    std::vector<void*> keep;
    keep.reserve(600);  // 开窗前预留:窗口内vector扩容分配不得混入记账
    const uint64_t unattrBase = GetUnattributedCount();

    svc->set_enabled(1);
    FloodDistinctSites<0>(600, keep);

    // 记账同步完成,直接读统计(600块全部在表;栈表128容量触顶)
    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.totalAllocCount, 600u) << "all flood allocations must be accounted";
    EXPECT_EQ(stats.liveBlockCount, 600u);
    EXPECT_EQ(stats.truncated & 0x1u, 0u) << "block table must not be truncated";
    EXPECT_NE(stats.truncated & 0x2u, 0u) << "stack table must be truncated (capacity 128)";
    EXPECT_EQ(stats.evictedStackCount, 0u) << "all-alive table: no dead-stack supply, eviction must not fire";
    EXPECT_EQ(stats.evictedAllocCount, 0u) << "all-alive table: no folded allocs";
    EXPECT_EQ(stats.evictedAllocBytes, 0u) << "all-alive table: no folded bytes";
    const uint64_t unattr = GetUnattributedCount() - unattrBase;
    EXPECT_GT(unattr, 0u) << "table full with no evictable supply must count unattributed";

    // 闭窗拉快照:未知桶行+正常栈行并存,块表投影600块完整
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());

    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    ASSERT_EQ(collector.items.size(), 600u);
    uint64_t dumpUnknown = 0;
    uint64_t dumpNormal = 0;
    for (const auto& item : collector.items)
    {
        EXPECT_EQ(item.size, 64u);
        EXPECT_GT(item.allocTs, 0u);
        if (item.stackId == 0)
        {
            dumpUnknown += 1;
        }
        else
        {
            dumpNormal += 1;
        }
    }
    EXPECT_GT(dumpUnknown, 0u) << "table full must route excess sites to unknown bucket";
    EXPECT_GT(dumpNormal, 0u) << "early sites must have registered (capacity 128)";

    StackStatCollector sc;
    svc->dump_stack_stats(CollectStackStat, &sc);
    uint64_t sumUnfreed = 0;
    uint64_t unknownUnfreed = 0;
    uint64_t realRows = 0;
    for (const auto& row : sc.rows)
    {
        sumUnfreed += row.unfreedCount;
        if (row.stackId == 0)
        {
            unknownUnfreed = row.unfreedCount;
            EXPECT_TRUE(row.frameDesc.empty()) << "unknown bucket row carries no stack text";
        }
        else
        {
            realRows += 1;
        }
    }
    EXPECT_EQ(realRows, dumpNormal) << "registered stacks == normal-bucket blocks";
    EXPECT_EQ(unknownUnfreed, dumpUnknown) << "unknown bucket row must match unknown-bucket blocks";
    EXPECT_EQ(sumUnfreed, 600u) << "ledger must stay complete (only attribution granularity degrades)";

    for (void* p : keep)
    {
        free(p);
    }
}

// UT-H16: 栈表满死栈淘汰回收(孙进程运行,见LaunchGrandchildEvictCycle;
// MAX_STACKS=128→分片容量2)。第一相600独立调用点全部保留存活灌入→栈表128
// 条目触顶(余472站点转未知桶);全部释放→在表128条目全成死栈(refs回落1)。
// 第二相600新调用点(Gen0/1,与第一相站点不重叠)登记→表满触发死栈淘汰腾位。
// 判据:①evictedStackCount>0(死栈供给充足,淘汰必然发生)且≤128(每死栈至多
// 淘汰一次,第二相站点全存活不可淘汰);②evictedAllocCount==evictedStackCount
// (每站点仅1次申请)且evictedAllocBytes==64*evictedStackCount(全64B);
// ③闭窗聚合行求和(含未知桶行)==全局合计1200(折叠算术闭合,诚实性零损失);
// ④存活600块全部保留且块表投影完整,未知桶行与未知桶块精确一致;
// ⑤selfcheck==0(淘汰+dispose路径refs一致,红线)
TEST_F(HostMemHookChild, stack_eviction_recycles_dead_stacks)
{
    const char* maxStacksEnv = std::getenv("MSMEMSCOPE_HOSTMEM_MAX_STACKS");
    if (maxStacksEnv == nullptr || atoi(maxStacksEnv) != 128)
    {
        GTEST_SKIP() << "run via HostMemHookTest.LaunchGrandchildEvictCycle (MAX_STACKS=128)";
    }
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(64);  // 放行全部64B洪峰块,过滤框架噪声
    auto selfcheck = reinterpret_cast<uint64_t (*)(void)>(
        dlsym(RTLD_DEFAULT, "msmemscope_hostmem_selfcheck"));
    ASSERT_NE(selfcheck, nullptr);

    std::vector<void*> keep1;
    keep1.reserve(600);
    std::vector<void*> keep2;
    keep2.reserve(600);

    svc->set_enabled(1);
    // 第一相:600独立站点全存活灌入→栈表128条目触顶,余472转未知桶
    FloodDistinctSites<0>(600, keep1);
    // 全部释放:在表128条目全成死栈(refs回落1),未知桶块照常出表
    for (void* p : keep1)
    {
        free(p);
    }
    keep1.clear();
    // 第二相:600新站点(Gen0/1)登记→表满死栈淘汰腾位
    FloodGen<0, 0>(300, keep2);
    FloodGen<0, 1>(300, keep2);

    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.totalAllocCount, 1200u);
    EXPECT_EQ(stats.liveBlockCount, 600u) << "phase-2 blocks all kept live";
    EXPECT_GT(stats.evictedStackCount, 0u) << "dead stacks must be recycled under capacity pressure";
    EXPECT_LE(stats.evictedStackCount, 128u) << "each dead stack evicted at most once";
    EXPECT_EQ(stats.evictedAllocCount, stats.evictedStackCount) << "one alloc per evicted site";
    EXPECT_EQ(stats.evictedAllocBytes, stats.evictedStackCount * 64u) << "all blocks are 64B";
    EXPECT_EQ(selfcheck(), 0u) << "refs invariant broken after eviction while open";

    // 闭窗拉快照:折叠算术闭合 + 块表投影完整
    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());

    StackStatCollector sc;
    svc->dump_stack_stats(CollectStackStat, &sc);
    uint64_t sumAlloc = 0;
    uint64_t sumUnfreed = 0;
    uint64_t unknownUnfreed = 0;
    for (const auto& row : sc.rows)
    {
        sumAlloc += row.allocCount;
        sumUnfreed += row.unfreedCount;
        if (row.stackId == 0)
        {
            unknownUnfreed = row.unfreedCount;
        }
    }
    EXPECT_EQ(sumAlloc, 1200u) << "row-sum (incl. unknown bucket) must equal global total after folding";
    EXPECT_EQ(sumUnfreed, 600u) << "live blocks must all be accounted after eviction";

    DumpCollector collector;
    svc->dump_live_blocks(CollectDumpItem, &collector);
    ASSERT_EQ(collector.items.size(), 600u);
    uint64_t dumpUnknown = 0;
    for (const auto& item : collector.items)
    {
        EXPECT_EQ(item.size, 64u);
        EXPECT_GT(item.allocTs, 0u);
        if (item.stackId == 0)
        {
            dumpUnknown += 1;
        }
    }
    EXPECT_EQ(unknownUnfreed, dumpUnknown) << "unknown bucket row must match unknown-bucket blocks after eviction";
    EXPECT_EQ(selfcheck(), 0u) << "refs invariant broken after close sweep with eviction";

    for (void* p : keep2)
    {
        free(p);
    }
}

// UT-H17: 引用计数refs各路径不变量(默认容量运行,无淘汰干扰)——脚本化逐路径
// 验证refs增减与selfcheck红线:①新站点登记+②快路径命中(同调用点二次malloc,
// 在途lookup+1后dispose);③块释放(块引用dispose);④realloc成功(旧块引用
// dispose+新块记账);⑤realloc失败(ReinsertBlock回插,引用归块不增不减);
// ⑥realloc(p,0)释放路径(引用dispose)。每步后selfcheck==0(refs==0双重dispose
// 绊线/refs==1且liveBytes!=0计数不一致绊线),闭窗后账本精确:5申请/3释放/2存活
TEST_F(HostMemHookChild, refs_path_invariants)
{
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(64);
    auto selfcheck = reinterpret_cast<uint64_t (*)(void)>(
        dlsym(RTLD_DEFAULT, "msmemscope_hostmem_selfcheck"));
    ASSERT_NE(selfcheck, nullptr);

    svc->set_enabled(1);
    // ① 新站点登记 + ② 同调用点快路径命中
    void* a = malloc(64);
    ASSERT_NE(a, nullptr);
    void* b = malloc(64);  // 同调用点:快路径命中,同一StackEntry
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(selfcheck(), 0u);
    // ③ 块释放:块引用dispose
    free(a);
    EXPECT_EQ(selfcheck(), 0u);
    // ④ realloc成功:旧块引用dispose+新块记账(新调用点)
    void* c = realloc(b, 128);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(selfcheck(), 0u);
    // ⑤ realloc失败:ReinsertBlock回插,引用归块(p保持有效)
    void* d = malloc(64);
    ASSERT_NE(d, nullptr);
    void* f = realloc(d, static_cast<size_t>(-1));  // 必然失败(glibc ENOMEM)
    EXPECT_EQ(f, nullptr);
    EXPECT_EQ(selfcheck(), 0u);
    // ⑥ realloc(p,0):释放路径,引用dispose(glibc返回NULL并释放p)
    void* e = malloc(64);
    ASSERT_NE(e, nullptr);
    void* z = realloc(e, 0);
    EXPECT_EQ(z, nullptr) << "glibc realloc(p,0) frees p and returns NULL";
    EXPECT_EQ(selfcheck(), 0u);

    // 账本精确:5申请(a/b/新c/d/e) 3释放(a/旧b/e) 2存活(c/d)
    MsmemscopeHostmemStats stats{};
    svc->get_stats(&stats);
    EXPECT_EQ(stats.totalAllocCount, 5u);
    EXPECT_EQ(stats.totalFreedCount, 3u);
    EXPECT_EQ(stats.liveBlockCount, 2u);
    EXPECT_EQ(selfcheck(), 0u);

    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());
    EXPECT_EQ(selfcheck(), 0u) << "refs invariant broken after close";
    free(c);
    free(d);
}

// UT-H18: 并发锤击下refs红线(孙进程运行,见LaunchGrandchildEvictCycle;
// MAX_STACKS=128)。4线程各持独立调用点集(Gen0-3),每线程20轮,每轮300站点
// 执行malloc/纯free/realloc成功/realloc失败回插混合路径,轮末释放制造死栈
// 供给——并发淘汰(判读refs==1)与在途lookup/块引用增减竞争,是"双重dispose→
// refs提前归1→淘汰后UAF"的真悬垂检测器。判据:并发结束后selfcheck==0(红线),
// 闭窗后selfcheck==0,账本一致(未释放=申请-释放)
TEST_F(HostMemHookChild, hammer_refs_integrity)
{
    const char* maxStacksEnv = std::getenv("MSMEMSCOPE_HOSTMEM_MAX_STACKS");
    if (maxStacksEnv == nullptr || atoi(maxStacksEnv) != 128)
    {
        GTEST_SKIP() << "run via HostMemHookTest.LaunchGrandchildEvictCycle (MAX_STACKS=128)";
    }
    const MsmemscopeHostmemSvc* svc = BindHookApi();
    ASSERT_NE(svc, nullptr);
    ResetWindowState(svc);
    ClearRecords();
    ThresholdGuard thresholdGuard(64);
    auto selfcheck = reinterpret_cast<uint64_t (*)(void)>(
        dlsym(RTLD_DEFAULT, "msmemscope_hostmem_selfcheck"));
    ASSERT_NE(selfcheck, nullptr);

    constexpr int kThreads = 4;
    constexpr int kRounds = 20;
    std::vector<std::vector<void*>> keeps(kThreads);  // 每线程独立keep(线程内独占)
    for (auto& k : keeps)
    {
        k.reserve(300);
    }
    svc->set_enabled(1);

    std::vector<std::thread> threads;
    for (int g = 0; g < kThreads; ++g)
    {
        threads.emplace_back([g, &keeps]() {
            for (int r = 0; r < kRounds; ++r)
            {
                std::vector<void*>& keep = keeps[g];
                switch (g)
                {
                    case 0:
                        HammerGen<0, 0>(300, keep);
                        break;
                    case 1:
                        HammerGen<0, 1>(300, keep);
                        break;
                    case 2:
                        HammerGen<0, 2>(300, keep);
                        break;
                    default:
                        HammerGen<0, 3>(300, keep);
                        break;
                }
                for (void* p : keep)
                {
                    free(p);  // 轮末释放:死栈供给再生
                }
                keep.clear();
            }
        });
    }
    for (auto& t : threads)
    {
        t.join();
    }
    EXPECT_EQ(selfcheck(), 0u) << "refs invariant broken under concurrent hammer + eviction";

    svc->set_enabled(0);
    ASSERT_TRUE(WaitForStageEnd());
    EXPECT_EQ(selfcheck(), 0u) << "refs invariant broken after close under concurrent hammer";

    for (auto& k : keeps)
    {
        for (void* p : k)
        {
            free(p);
        }
    }
}

// ---------------------------------------------------------------------------
// 父进程用例:定位钩子so并fork+exec子进程套件
// ---------------------------------------------------------------------------

namespace
{
std::string DirNameOf(const std::string& path)
{
    const size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::string LocateHookSo()
{
    // ① 环境变量显式指定
    const char* envPath = std::getenv(HOOK_SO_ENV);
    if (envPath != nullptr && access(envPath, F_OK) == 0)
    {
        return envPath;
    }
    // ② 测试二进制位于<repo>/build/test/ → <repo>/output/lib64/
    char exePath[4096] = {0};
    const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0)
    {
        exePath[len] = '\0';
        const std::string repoRoot = DirNameOf(DirNameOf(DirNameOf(exePath)));
        const std::string candidate = repoRoot + "/output/lib64/" + HOOK_SO_NAME;
        if (access(candidate.c_str(), F_OK) == 0)
        {
            return candidate;
        }
    }
    // ③ 相对cwd兜底(repo根/build目录下直接执行)
    const char* candidates[] = {"./output/lib64", "../output/lib64", "../../output/lib64"};
    for (const char* dir : candidates)
    {
        const std::string candidate = std::string(dir) + "/" + HOOK_SO_NAME;
        if (access(candidate.c_str(), F_OK) == 0)
        {
            return candidate;
        }
    }
    return "";
}

// 定位子套件专用二进制:与主测试二进制同目录(同一test构建目录产出,见test/CMakeLists.txt);
// 相对cwd兜底(repo根/build目录下直接执行)
std::string LocateChildExe()
{
    char exePath[4096] = {0};
    const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0)
    {
        exePath[len] = '\0';
        const std::string candidate = DirNameOf(exePath) + "/" + CHILD_EXE_NAME;
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return candidate;
        }
    }
    const char* candidates[] = {"./test", ".", ".."};
    for (const char* dir : candidates)
    {
        const std::string candidate = std::string(dir) + "/" + CHILD_EXE_NAME;
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return candidate;
        }
    }
    return "";
}

// fork+exec子进程并装配钩子preload环境(子进程侧执行,父进程直接返回pid)。
// 返回-1=fork失败。装配内容:LD_PRELOAD钩子so;LD_LIBRARY_PATH指向钩子目录
// (DT_NEEDED=libascend_leaks.so经此解析——与生产wrapper契约一致,source模式
// export LD_LIBRARY_PATH=<lib64>[:既有];钩子so另带$ORIGIN RPATH兜底。注意空条目
// 会向搜索路径引入".",须跳过);子套件标记。maxStacksEnv非空时压低栈表上限
// (孙进程用例用,构造期解析)
pid_t SpawnHookedChild(const std::string& hookSo, const std::string& childExe,
                       const std::string& gtestFilter, const char* maxStacksEnv)
{
    const pid_t pid = fork();
    if (pid != 0)
    {
        return pid;
    }
    setenv("LD_PRELOAD", hookSo.c_str(), 1);
    const std::string hookDir = DirNameOf(hookSo);
    const char* prevLdLibPath = std::getenv("LD_LIBRARY_PATH");
    const std::string ldLibPath = (prevLdLibPath == nullptr || prevLdLibPath[0] == '\0')
                                      ? hookDir
                                      : hookDir + ":" + prevLdLibPath;
    setenv("LD_LIBRARY_PATH", ldLibPath.c_str(), 1);
    setenv(CHILD_ENV_MARKER, "1", 1);
    if (maxStacksEnv != nullptr)
    {
        setenv("MSMEMSCOPE_HOSTMEM_MAX_STACKS", maxStacksEnv, 1);
    }
    const std::string filterArg = "--gtest_filter=" + gtestFilter;
    execl(childExe.c_str(), CHILD_EXE_NAME, filterArg.c_str(), nullptr);
    _exit(127);  // exec失败
}

// 限时等待子进程退出;超时杀进程并回收,返回false(statusOut仍被填充)
bool WaitChildExit(pid_t pid, int timeoutMs, int* statusOut)
{
    int status = 0;
    for (int i = 0; i < timeoutMs / 50; ++i)
    {
        const pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid)
        {
            *statusOut = status;
            return true;
        }
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    *statusOut = status;
    return false;
}
}  // namespace

// 驱动子进程套件:fork+exec专用二进制memscope_hostmem_child_test(避开主测试二进制
// 内嵌的dlopen/dlsym桩等符号污染),子进程LD_PRELOAD钩子so并以指定filter运行;
// 退出码0=子用例全绿。子进程stdout/stderr继承,失败细节直接可见。
// env装配契约见SpawnHookedChild注释
TEST(HostMemHookTest, LaunchChildSuite)
{
    if (std::getenv(CHILD_ENV_MARKER) != nullptr)
    {
        GTEST_SKIP() << "child process: skip launcher";
    }
    const std::string hookSo = LocateHookSo();
    if (hookSo.empty())
    {
        GTEST_SKIP() << "hook so not found (build csrc targets first); expected output/lib64/"
                     << HOOK_SO_NAME;
    }
    // 前置校验:钩子so的DT_NEEDED=libascend_leaks.so若不可解析,子进程将以127退出且加载
    // 错误混在子gtest输出里难以定位。钩子so与libascend_leaks.so由同一批csrc目标产出
    // (LIBRARY_OUTPUT_DIRECTORY同为output/lib64),同目录缺失=部署不完整,提前以明确信息失败
    const std::string leaksSo = DirNameOf(hookSo) + "/" + LEAKS_SO_NAME;
    ASSERT_EQ(access(leaksSo.c_str(), F_OK), 0)
        << "libascend_leaks.so not found at " << leaksSo << "; rebuild csrc targets";

    const std::string childExe = LocateChildExe();
    if (childExe.empty())
    {
        GTEST_SKIP() << "child test binary not found; expected " << CHILD_EXE_NAME
                     << " next to this binary (build target memscope_hostmem_child_test)";
    }

    const pid_t child = SpawnHookedChild(hookSo, childExe, "HostMemHookChild.*", nullptr);
    ASSERT_GE(child, 0) << "fork failed";

    // 父进程:限时等待(子用例含多窗口轮询,整体留足余量),超时杀进程判失败
    int status = 0;
    if (!WaitChildExit(child, CHILD_TIMEOUT_MS, &status))
    {
        FAIL() << "child suite timed out after " << CHILD_TIMEOUT_MS << "ms";
    }
    ASSERT_TRUE(WIFEXITED(status)) << "child terminated abnormally (signal " << WTERMSIG(status) << ")";
    ASSERT_EQ(WEXITSTATUS(status), 0) << "child suite has failures (see child gtest output above)";
}

// 孙进程驱动:栈表满转未知桶用例(stack_table_full_unattributed)以压低的栈表上限
// (128,分片容量2)运行——默认容量下600活站点的洪峰打不满表,无法构造"表满且无
// 可淘汰供给"的路径;收紧后第129个起新站点必然登记失败,验证其转未知桶后
// 账本继续(块表完整,仅归因粒度退化为未知桶)。fork+exec自未挂钩的父进程,
// preload环境整体继承自SpawnHookedChild的装配
TEST(HostMemHookTest, LaunchGrandchildUnattrCycle)
{
    if (std::getenv(CHILD_ENV_MARKER) != nullptr)
    {
        GTEST_SKIP() << "child process: skip launcher";
    }
    const std::string hookSo = LocateHookSo();
    if (hookSo.empty())
    {
        GTEST_SKIP() << "hook so not found (build csrc targets first); expected output/lib64/"
                     << HOOK_SO_NAME;
    }
    const std::string leaksSo = DirNameOf(hookSo) + "/" + LEAKS_SO_NAME;
    ASSERT_EQ(access(leaksSo.c_str(), F_OK), 0)
        << "libascend_leaks.so not found at " << leaksSo << "; rebuild csrc targets";

    const std::string childExe = LocateChildExe();
    if (childExe.empty())
    {
        GTEST_SKIP() << "child test binary not found; expected " << CHILD_EXE_NAME
                     << " next to this binary (build target memscope_hostmem_child_test)";
    }

    const pid_t grandchild = SpawnHookedChild(hookSo, childExe, "HostMemHookChild.stack_table_full_unattributed",
                                              "128");
    ASSERT_GE(grandchild, 0) << "fork failed";

    int status = 0;
    if (!WaitChildExit(grandchild, GRANDCHILD_TIMEOUT_MS, &status))
    {
        FAIL() << "grandchild unattr-cycle run timed out after " << GRANDCHILD_TIMEOUT_MS << "ms";
    }
    ASSERT_TRUE(WIFEXITED(status)) << "grandchild terminated abnormally (signal " << WTERMSIG(status) << ")";
    ASSERT_EQ(WEXITSTATUS(status), 0) << "unattributed cycle has failures (see grandchild gtest output above)";
}

// 孙进程驱动:死栈淘汰回收(stack_eviction_recycles_dead_stacks)与并发锤击
// (hammer_refs_integrity)以压低的栈表上限(128,分片容量2)运行——淘汰路径需
// 表满构造(600站点灌入打满128容量后释放制造死栈供给);锤击则需小容量放大
// 淘汰与refs增减的并发竞争(真悬垂检测)。fork+exec自未挂钩的父进程,
// preload环境整体继承自SpawnHookedChild的装配
TEST(HostMemHookTest, LaunchGrandchildEvictCycle)
{
    if (std::getenv(CHILD_ENV_MARKER) != nullptr)
    {
        GTEST_SKIP() << "child process: skip launcher";
    }
    const std::string hookSo = LocateHookSo();
    if (hookSo.empty())
    {
        GTEST_SKIP() << "hook so not found (build csrc targets first); expected output/lib64/"
                     << HOOK_SO_NAME;
    }
    const std::string leaksSo = DirNameOf(hookSo) + "/" + LEAKS_SO_NAME;
    ASSERT_EQ(access(leaksSo.c_str(), F_OK), 0)
        << "libascend_leaks.so not found at " << leaksSo << "; rebuild csrc targets";

    const std::string childExe = LocateChildExe();
    if (childExe.empty())
    {
        GTEST_SKIP() << "child test binary not found; expected " << CHILD_EXE_NAME
                     << " next to this binary (build target memscope_hostmem_child_test)";
    }

    const pid_t grandchild = SpawnHookedChild(
        hookSo, childExe,
        "HostMemHookChild.stack_eviction_recycles_dead_stacks:HostMemHookChild.hammer_refs_integrity", "128");
    ASSERT_GE(grandchild, 0) << "fork failed";

    int status = 0;
    if (!WaitChildExit(grandchild, GRANDCHILD_TIMEOUT_MS, &status))
    {
        FAIL() << "grandchild evict-cycle run timed out after " << GRANDCHILD_TIMEOUT_MS << "ms";
    }
    ASSERT_TRUE(WIFEXITED(status)) << "grandchild terminated abnormally (signal " << WTERMSIG(status) << ")";
    ASSERT_EQ(WEXITSTATUS(status), 0) << "evict cycle has failures (see grandchild gtest output above)";
}
