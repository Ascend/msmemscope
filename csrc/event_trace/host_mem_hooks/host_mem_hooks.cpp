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

/*
 * host堆内存钩子
 *
 * LD_PRELOAD劫持malloc族函数,采集"检测区间内申请且区间结束时未释放"的host堆块。
 *
 * 单账本+闭窗快照:
 *   热路径(业务线程内全同步): malloc→栈查找/登记→块表插入(块表是唯一账本);
 *   free→块表删除。栈计数器在块表分片锁临界区内与条目存在性原子更新,
 *   闭窗快照下"申请=释放+未释放"不变量精确成立。
 *   闭窗(set_enabled(false),调用线程同步完成): 记账冻结→停预热线程(join)→
 *   遍历块表→per-stack未释放精确聚合+大小排布桶→遍历栈表→per-stack申请/释放
 *   计数(释放=申请-未释放派生)→符号化(帧缓存+稳态top-K预热)→发STAGE_END;
 *   分析器经dump_*拉闭窗快照出报告(概览txt+事件模式块明细csv)。
 *   诚实性契约: 默认全量(块阈值0、采样1),显式采样/溢出通道降级/整窗截断显式标注,
 *   不做逐块丢包(块表满转溢出账本照常记账)。
 *
 * 实现要点:
 *   栈表分片按StackKey哈希,stackId低6位编码所属分片号(free路径按stackId定位);
 *   栈去重键=捕获栈前K帧(K=20,编译期可覆盖),全深度报告栈由登记慢路径采集;
 *   栈条目引用计数refs(=在表pin+存活块数+在途lookup数,见引用契约): 凡持有
 *   StackEntry*跨锁/跨调用者必须持有对应ref,捕获即转移、终态恰一次dispose;
 *   栈表满→优先死栈淘汰(桶采样回收refs==1的零存活块条目腾位,见
 *   EvictDeadStackLocked),采样无可淘汰者(全活表)时新键才转未知桶(stackId=0)
 *   照常记账(truncated bit1),只损失归因粒度;淘汰折叠计数并入未知桶行;
 *   块表满→转每分片bounded溢出账本(addr→size)照常记账(truncated bit0),free命中
 *   溢出账本逆向修正统计;溢出账本亦满→记账停止(bit2);free命中两账本之外的地址=
 *   开窗前分配,独立通道统计(malloc_usable_size近似),不并入totalFreed;
 *   main边界门控(__libc_start_main拦截,静态初始化上下文不记账)、退出期门控
 *   (exit拦截+main返回兜底,退出期堆环境不可信)、fork后代不监控(g_forked);
 *   钩子自身帧动态过滤(构造期dl_iterate_phdr求本so地址区间);
 *   帧指针快速走栈(热路径沿fp链,校验fp栈界/对齐/递增+pc落在模块可执行段快照内,
 *   失败回退backtrace);符号化(闭窗延迟+稳态top-K预热+帧缓存,预热线程与闭窗
 *   分时运行,退出期dladdr不入退出路径);显式采样(2的幂,门控在记账之前判定);
 *   计数器与块表存在性原子(栈计数/未知桶/全局合计均在块表分片锁临界区内更新)。
 *
 * 引用契约(refs,详见EvictDeadStackLocked注释): 1=在表pin+存活块数+在途lookup数。
 *   所有+1在栈分片锁临界区内(除InsertBlock块引用+1在块表锁内,有调用方在途ref
 *   兜底,期间refs恒>=2);-1恰一次于dispose(释放侧对条目最后一次触碰)。
 *   块表持owner指针期间refs>=2,淘汰判读(refs==1)与之互斥——指针永不悬垂。
 *   失效模式: 漏dispose→退化今天行为(安全);双重dispose→refs提前归0→淘汰后
 *   指针悬垂(唯一致命方向,selfcheck绊线+UT红线覆盖)。
 *
 * 三类重入防护:
 *   A 劫持函数互相调用(真calloc内部调malloc@plt): thread_local抑制守卫
 *   B 钩子内部容器分配: 一律RealMallocAllocator(不经PLT)
 *   C 预热线程/闭窗聚合内部分配: 闭窗聚合执行期g_enabled=false,分配经真函数不落
 *     记账;预热线程全程抑制守卫兜底
 */

#include "host_mem_hooks.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

// =============================================================================
// 常量与配置(环境变量可覆盖,构造期一次性读取)
// =============================================================================

constexpr int STACK_SHARDS = 64;  // 栈表分片数(2的幂,stackId低6位编码分片号)
constexpr int BLOCK_SHARDS = 64;  // 块表分片数(2的幂)
// StackKey键数组容量(编译期可-D覆盖,默认K=20):账本去重键=热路径一次捕获的前K帧
// (CaptureFrames截断)。键为定长数组,深度变化需全表
// rehash,故保留-D宏。默认20帧:浅键(3帧)前几帧多为公共库入口帧,不同业务申请点
// 归并到同一"类"导致归因误导;20帧对真实调用链有实质区分度。
// 键深增大仅增加键体积与hash比较成本(键比较最长链),栈表条目数相应增多——块表
// 是唯一账本,键深只影响归因粒度与栈表容量,不影响记账完整性
#ifndef MSMEMSCOPE_STACK_KEY_FRAMES
#define MSMEMSCOPE_STACK_KEY_FRAMES 20
#endif
constexpr int STACK_KEY_FRAMES = MSMEMSCOPE_STACK_KEY_FRAMES;
// 头部钩子帧过滤余量:采栈多采的头部帧数上限(backtrace蹦床/CaptureFrames/
// RecordMalloc/劫持入口),过滤后仍能返回maxOut帧;超出的钩子帧漏入栈串
// (数据可见可修,不静默失真)
constexpr int HOOK_FRAME_HEADROOM = 6;
constexpr uint32_t DEFAULT_STACK_DEPTH = 50;   // 默认栈深(config.cStackDepth默认值)
constexpr uint32_t MAX_STACK_DEPTH = 1000;     // 栈深上限(config_info.h约定)
constexpr size_t DEFAULT_MAX_STACKS = 400000;  // 栈表上限默认(可配);洪峰下活栈数万+瞬时
                                               // 死栈;表满后新键转未知桶(truncated bit1)
// 块表上限默认(可配):块表是唯一账本,块表满→申请转溢出通道降级记账(truncated
// bit0信息标注,记账继续)。内存按需增长:键+BlockEntry双数组32B/槽、
// 负载0.7,满载约537MB(惰性增长,仅存活块真到20M才占~1.3GB,AI服务器可接受)
constexpr size_t DEFAULT_MAX_BLOCKS = 20000000;
// 溢出通道上限默认(可配,MSMEMSCOPE_HOSTMEM_MAX_OVERFLOW,总量按分片均摊):
// 块表满时无法入表的申请转入每分片bounded的addr→size溢出账本。安全阀防set
// 无界增长OOM——"无容量限制"须有界;触顶=记账停止
// (truncated bit2,窗口为截断点前的完整前缀)。每条目addr+size+节点头~48B,
// 默认1M条≈48MB(块表之外的有界增量)
constexpr size_t DEFAULT_MAX_OVERFLOW = 1000000;
constexpr size_t ARENA_SIZE = 64 * 1024;  // 自举竞技场: dlsym解析期间的内部分配
constexpr int BLOCK_LOCK_SPINS = 64;      // 块表分片锁trylock有界重试次数(free路径防假泄漏)
// 帧符号化缓存条目上限: 满后停止插入,进程内复用不淘汰。只存符号帧(快照帧
// 不入缓存,见FormatFrame)。1M条≈~64MB(每条~64B),配合预热足以覆盖全部报告栈
// (不同栈共享公共链帧,实际覆盖栈数更多)
constexpr size_t SYM_CACHE_MAX_ENTRIES = 1048576;

// =============================================================================
// 稳态top泄漏点符号采样(见预热线程): 每节拍全分片扫描,每分片取
// top-TOP_LEAK_CANDS_PER_SHARD(按liveBytes存活字节降序),全局合并取
// top-TOP_LEAK_SYMBOLIZE_K解析全深度帧符号入g_symCache(每栈~30帧×~1μs/帧,
// 256栈≈10-30ms/轮;缓存满即停)。字节口径修正块数代理的排序错位:块数是
// 块数,报告真相是字节(unfreedBytes)——大块单发泄漏(单个大权重块)块数少却被
// 冷落垫底;liveBytes与报告排序键同构,稳态top即报告top。K=256覆盖报告Top10之外
// 留足余量;节拍1s——泄漏渐进累积,秒级采样即可覆盖报告top;末次采样后新冒头的
// 泄漏栈有模块快照兜底+离线addr2line(尽力而为,非硬性缺口)
// =============================================================================
constexpr size_t TOP_LEAK_SYMBOLIZE_K = 256;                     // 每次采样全局符号化栈数上限
constexpr size_t TOP_LEAK_CANDS_PER_SHARD = 32;                  // 每分片候选上限(收集排序,锁内无分配)
constexpr uint64_t TOP_LEAK_SAMPLE_INTERVAL_NS = 1000000000ull;  // 采样节拍1s(预热线程)

// 自旋等待的CPU让步提示(竞争窗口内短停,配合有界重试,绝不长时间阻塞业务线程)
inline void CpuRelax()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    sched_yield();
#endif
}

// =============================================================================
// 自举竞技场(bootstrap): real_*未解析期间(dlsym内部calloc等)的分配兜底
// =============================================================================

alignas(16) char g_arena[ARENA_SIZE];
std::atomic<size_t> g_arenaOffset{0};

void* ArenaAlloc(size_t n)
{
    n = (n + 15) & ~static_cast<size_t>(15);
    size_t cur = g_arenaOffset.fetch_add(n, std::memory_order_relaxed);
    if (cur + n > ARENA_SIZE)
    {
        return nullptr;  // 耗尽:真实OOM场景,调用方判空处理
    }
    return g_arena + cur;
}

void* ArenaCalloc(size_t n, size_t size)
{
    // 乘法溢出检查
    if (n != 0 && size > SIZE_MAX / n)
    {
        return nullptr;
    }
    void* p = ArenaAlloc(n * size);
    if (p != nullptr)
    {
        memset(p, 0, n * size);
    }
    return p;
}

bool IsArenaPtr(const void* p) { return p >= g_arena && p < g_arena + ARENA_SIZE; }

// =============================================================================
// 真函数解析: 构造期固化 + 入口惰性解析双保险(构造函数不保证先于其他so构造执行)
// 未解析期间的调用走自举竞技场;解析路径由抑制守卫防dlsym内部calloc递归
// =============================================================================

void* (*real_malloc_fn)(size_t) = nullptr;
void (*real_free_fn)(void*) = nullptr;
void* (*real_calloc_fn)(size_t, size_t) = nullptr;
void* (*real_realloc_fn)(void*, size_t) = nullptr;
void (*real_exit_fn)(int) = nullptr;  // 宿主exit转发(退出期防护置位后调用,见exit拦截器注释)
int (*real_posix_memalign_fn)(void**, size_t, size_t) = nullptr;
void* (*real_aligned_alloc_fn)(size_t, size_t) = nullptr;
void* (*real_memalign_fn)(size_t, size_t) = nullptr;
void* (*real_valloc_fn)(size_t) = nullptr;
void* (*real_pvalloc_fn)(size_t) = nullptr;
// 开窗前free单块大小近似: malloc_usable_size(glibc特有)返回
// 块实际可用字节(>=申请值,含malloc头对齐垫),free未命中账本时作近似;解析失败
// (非glibc/极早期)为nullptr,回退0(仅大小统计,次数不受影响)
size_t (*real_malloc_usable_size_fn)(void*) = nullptr;

// thread_local抑制深度(场景A/C守卫): >0时钩子入口直接转真函数,零记账
thread_local uint32_t t_hookSuppressDepth = 0;

class HookSuppressGuard
{
   public:
    HookSuppressGuard() { ++t_hookSuppressDepth; }
    ~HookSuppressGuard() { --t_hookSuppressDepth; }
    HookSuppressGuard(const HookSuppressGuard&) = delete;
    HookSuppressGuard& operator=(const HookSuppressGuard&) = delete;
};

// dlsym(RTLD_NEXT)解析单符号(调用方必须已置抑制守卫)。
// 重入断链(必须): dlsym内部首次调用会经PLT分配dlerror的TLS错误结构(calloc族),
// 该分配落回本钩子→Real*发现真函数未解析→再入解析→dlsym再分配→无限递归栈溢出。
// 解析期间(t_inResolve=true)的再次解析请求直接返回nullptr,由Real*走竞技场兜底;
// 外层dlsym得以完成,真函数指针随后固化,后续调用恢复正常路径
thread_local bool t_inResolve = false;

void* ResolveOne(const char* name)
{
    if (t_inResolve)
    {
        return nullptr;
    }
    t_inResolve = true;
    void* sym = dlsym(RTLD_NEXT, name);
    t_inResolve = false;
    return sym;
}

void ResolveAllRealFns()
{
    HookSuppressGuard guard;  // dlsym内部calloc经PLT落入本钩子→守卫拦截→竞技场
    if (real_malloc_fn == nullptr)
    {
        real_malloc_fn = reinterpret_cast<void* (*)(size_t)>(ResolveOne("malloc"));
    }
    if (real_free_fn == nullptr)
    {
        real_free_fn = reinterpret_cast<void (*)(void*)>(ResolveOne("free"));
    }
    if (real_calloc_fn == nullptr)
    {
        real_calloc_fn = reinterpret_cast<void* (*)(size_t, size_t)>(ResolveOne("calloc"));
    }
    if (real_realloc_fn == nullptr)
    {
        real_realloc_fn = reinterpret_cast<void* (*)(void*, size_t)>(ResolveOne("realloc"));
    }
    if (real_posix_memalign_fn == nullptr)
    {
        real_posix_memalign_fn = reinterpret_cast<int (*)(void**, size_t, size_t)>(ResolveOne("posix_memalign"));
    }
    if (real_aligned_alloc_fn == nullptr)
    {
        real_aligned_alloc_fn = reinterpret_cast<void* (*)(size_t, size_t)>(ResolveOne("aligned_alloc"));
    }
    if (real_memalign_fn == nullptr)
    {
        real_memalign_fn = reinterpret_cast<void* (*)(size_t, size_t)>(ResolveOne("memalign"));
    }
    if (real_valloc_fn == nullptr)
    {
        real_valloc_fn = reinterpret_cast<void* (*)(size_t)>(ResolveOne("valloc"));
    }
    if (real_pvalloc_fn == nullptr)
    {
        real_pvalloc_fn = reinterpret_cast<void* (*)(size_t)>(ResolveOne("pvalloc"));
    }
    if (real_malloc_usable_size_fn == nullptr)
    {
        // 非glibc分配器无此符号,解析失败保持nullptr,开窗前free大小回退0
        real_malloc_usable_size_fn = reinterpret_cast<size_t (*)(void*)>(ResolveOne("malloc_usable_size"));
    }
    if (real_exit_fn == nullptr)
    {
        real_exit_fn = reinterpret_cast<void (*)(int)>(ResolveOne("exit"));
    }
}

// 真函数调用入口(判空,未解析时先惰性解析再兜底竞技场)
void* RealMalloc(size_t size)
{
    if (real_malloc_fn == nullptr)
    {
        ResolveAllRealFns();
        if (real_malloc_fn == nullptr)
        {
            return ArenaAlloc(size);
        }
    }
    return real_malloc_fn(size);
}

void RealFree(void* ptr)
{
    if (ptr == nullptr)
    {
        return;
    }
    if (IsArenaPtr(ptr))
    {
        return;  // 竞技场内存永不归还(bump分配无元数据)
    }
    if (real_free_fn == nullptr)
    {
        ResolveAllRealFns();
        if (real_free_fn == nullptr)
        {
            return;  // 释放函数不可得:静默泄漏(仅极早期场景)
        }
    }
    real_free_fn(ptr);
}

void* RealCalloc(size_t n, size_t size)
{
    if (real_calloc_fn == nullptr)
    {
        ResolveAllRealFns();
        if (real_calloc_fn == nullptr)
        {
            return ArenaCalloc(n, size);
        }
    }
    return real_calloc_fn(n, size);
}

void* RealRealloc(void* ptr, size_t size)
{
    if (ptr != nullptr && IsArenaPtr(ptr))
    {
        // 竞技场指针绝不可转交glibc realloc(会被当作chunk解析,立即堆元数据损坏):
        // 新分配+拷贝+旧块不归还(bump分配无元数据,旧尺寸未知)。拷贝量钳制在
        // 竞技场尾界内——读越界不出静态数组,不致崩;该场景仅限real_*解析完成前的
        // 极早期自举块被宿主realloc,触发面极窄,尽力而为
        void* np = RealMalloc(size);
        if (np != nullptr && size > 0)
        {
            const size_t arenaTail = static_cast<size_t>(g_arena + ARENA_SIZE - static_cast<const char*>(ptr));
            const size_t copyLen = size < arenaTail ? size : arenaTail;
            memcpy(np, ptr, copyLen);
        }
        return np;
    }
    if (real_realloc_fn == nullptr)
    {
        ResolveAllRealFns();
        if (real_realloc_fn == nullptr)
        {
            // 竞技场无realloc语义:新分配+拷贝+旧块不归还
            void* np = ArenaAlloc(size);
            if (np != nullptr && ptr != nullptr)
            {
                // 旧块大小未知,只能拷贝新尺寸(极早期场景,尽力而为)
                memcpy(np, ptr, size);
            }
            return np;
        }
    }
    return real_realloc_fn(ptr, size);
}

// 开窗前free单块大小近似(malloc_usable_size): 返回块实际可用
// 字节(>=申请值)。仅RecordFree未命中账本时调用,指针仍存活(真free未执行);
// 解析失败(非glibc)返回0(仅影响大小统计与分布归桶,次数不受影响)
uint64_t RealUsableSize(void* ptr)
{
    if (real_malloc_usable_size_fn == nullptr)
    {
        return 0;
    }
    return static_cast<uint64_t>(real_malloc_usable_size_fn(ptr));
}

int RealPosixMemalign(void** memptr, size_t align, size_t size)
{
    if (real_posix_memalign_fn == nullptr)
    {
        ResolveAllRealFns();
        if (real_posix_memalign_fn == nullptr)
        {
            return ENOMEM;
        }
    }
    return real_posix_memalign_fn(memptr, align, size);
}

void* RealAlignedAlloc(size_t align, size_t size)
{
    if (real_aligned_alloc_fn == nullptr)
    {
        ResolveAllRealFns();
        if (real_aligned_alloc_fn == nullptr)
        {
            return nullptr;
        }
    }
    return real_aligned_alloc_fn(align, size);
}

void* RealMemalign(size_t align, size_t size)
{
    if (real_memalign_fn == nullptr)
    {
        ResolveAllRealFns();
        if (real_memalign_fn == nullptr)
        {
            return nullptr;
        }
    }
    return real_memalign_fn(align, size);
}

void* RealValloc(size_t size)
{
    if (real_valloc_fn == nullptr)
    {
        ResolveAllRealFns();
        if (real_valloc_fn == nullptr)
        {
            return nullptr;
        }
    }
    return real_valloc_fn(size);
}

void* RealPvalloc(size_t size)
{
    if (real_pvalloc_fn == nullptr)
    {
        ResolveAllRealFns();
        if (real_pvalloc_fn == nullptr)
        {
            return nullptr;
        }
    }
    return real_pvalloc_fn(size);
}

// =============================================================================
// 钩子内自定义allocator(场景B硬性要求): 分配经real_malloc不经PLT,
// 构造早期由竞技场兜底;失败抛bad_alloc交调用方降级(丢包计数)——
// 绝不trap杀宿主:钩子寄生在任意宿主进程,崩溃请求可能源自内部异常状态
// (如曾出现的哈希表负桶数请求),宿主自身的malloc可能完全正常
// =============================================================================

template <typename T>
struct RealMallocAllocator
{
    using value_type = T;

    RealMallocAllocator() = default;
    template <typename U>
    RealMallocAllocator(const RealMallocAllocator<U>&)
    {
    }

    T* allocate(std::size_t n)
    {
        void* p = RealMalloc(n * sizeof(T));
        if (p == nullptr)
        {
            p = ArenaAlloc(n * sizeof(T));
        }
        if (p == nullptr)
        {
            throw std::bad_alloc();  // 真OOM或请求溢出:由LookupOrRegisterStack/InsertBlock捕获降级
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t) { RealFree(p); }
};

template <typename T, typename U>
bool operator==(const RealMallocAllocator<T>&, const RealMallocAllocator<U>&)
{
    return true;
}

template <typename T, typename U>
bool operator!=(const RealMallocAllocator<T>&, const RealMallocAllocator<U>&)
{
    return false;
}

// =============================================================================
// 闭窗快照类型(栈统计行/大小排布桶/合计快照): 仅CloseAggregate构造,
// 经dump_stack_stats/dump_size_distribution/get_stats交付分析器
// =============================================================================

// per-stack统计行: 申请/释放计数来自栈计数器,未释放来自块表闭窗聚合(派生),
// frameDesc为闭窗符号化文本('\n'分隔帧描述;stackId=0为未知桶行,frameDesc为空)
struct StackStatRow
{
    uint64_t stackId;
    uint64_t allocCount;
    uint64_t allocBytes;
    uint64_t freedCount;
    uint64_t freedBytes;
    uint64_t unfreedCount;
    uint64_t unfreedBytes;
    uint64_t maxBlockSize;
    std::string frameDesc;
};

// 大小排布桶: [rangeLow, rangeHigh),末桶rangeHigh=UINT64_MAX
struct SizeBucket
{
    uint64_t rangeLow;
    uint64_t rangeHigh;
    uint64_t blockCount;
    uint64_t blockBytes;
};

// 闭窗合计快照(get_stats在窗口关闭态返回冻结值,与dump_*同源一致)
struct CloseSnapshot
{
    bool valid = false;
    uint64_t liveBlockCount = 0;
    uint64_t totalAllocCount = 0;
    uint64_t totalAllocBytes = 0;
    uint64_t totalFreedCount = 0;
    uint64_t totalFreedBytes = 0;
    uint64_t untrackedCount = 0;
    uint64_t untrackedBytes = 0;
    // 溢出通道(块表满降级): 累计转出/逆向修正;存活=alloc-freed派生
    uint64_t overflowAllocCount = 0;
    uint64_t overflowAllocBytes = 0;
    uint64_t overflowFreedCount = 0;
    uint64_t overflowFreedBytes = 0;
    // 开窗前free独立通道(窗口外分配): 次数/总量(单块大小分布经g_closePreWindowDist)
    uint64_t preWindowFreeCount = 0;
    uint64_t preWindowFreeBytes = 0;
    // 死栈淘汰(栈表满时回收,见EvictDeadStackLocked): 本窗口被淘汰条目数与折叠的
    // 申请计数/字节。折叠已并入unknown桶行(行求和==全局合计闭合),此三字段仅
    // 交付统计供分析器健康区Evicted行展示,不参与合计算术
    uint64_t evictedStackCount = 0;
    uint64_t evictedAllocCount = 0;
    uint64_t evictedAllocBytes = 0;
    uint32_t sampleRate = 1;
    uint32_t truncated = 0;
};

// =============================================================================
// 窗口状态与门控
// =============================================================================

std::atomic<bool> g_bound{false};    // bind握手完成
std::atomic<bool> g_enabled{false};  // 窗口门控(生产者闸门)
std::atomic<bool> g_closing{false};  // 关闭中:闭窗聚合(停预热线程+聚合+STAGE_END)
// 宿主main边界(静态初始化防护):HostMemReportBoot(EventReport的constructor兜底)可能在
// .init_array期间开窗,此后宿主/其他so的静态初始化malloc会全部落入记账路径——该上下文里
// ld.so未完成初始化(backtrace/dladdr/部分构造的C++运行时均为未定义行为,会导致栈表内存
// 被踩、哈希表请求负桶数、allocator trap杀死宿主bash)。故宿主main开始前一律不记账;
// 经__libc_start_main包装在真main入口处置位(所有so与可执行文件的.init_array均完成于
// main进入之前,dlsym解析失败时退化为置位放行——宁退旧行为不静默丢数据)
std::atomic<bool> g_mainStarted{false};
// 进程退出边界(退出期防护):宿主exit()进入或main返回即置位,此后钩子不再记账。
// 退出期rtld_fini逐so逆序析构,而日志刷写等工作线程仍存活并在分配,析构器与工作
// 线程竞态下宿主堆操作环境不再可信(曾导致块表桶数组chunk被非法释放后与空闲块
// 合并复用,空表首插读到代码地址当桶指针,写_M_nxt即SIGSEGV)。泄漏快照语义随之
// 收敛为"退出时刻仍存活的块":退出期(atexit/析构序列内)的释放不抵扣,与分析器
// atExit析构兜底报告的口径一致
std::atomic<bool> g_exiting{false};
// 符号化预热线程: 1s节拍top-K dladdr采样(见WarmupThreadMain)。
// 开窗时创建(EnsureWarmupThread),闭窗时先置g_warmupStop再join(StopWarmupThread)
// ——join使预热写入的g_symCache对闭窗聚合线程可见且不再并发;窗口关闭态无预热
std::atomic<bool> g_warmupThreadCreated{false};
std::atomic<bool> g_warmupStop{false};
pthread_t g_warmupThread{};
// fork后代标记:fork子进程内置位且永不清除(fork+exec的子进程经exec重置数据段,
// 不受影响)。置位后SvcSetEnabled拒绝一切开窗——fork后代不监控:①采集侧锁
// (EventReport/分析器等)可能被fork瞬间存活的父线程持有,子进程重建预热线程会在
// 派发链上死锁;②子进程事件的pid与父进程窗口pid不匹配,数据无法归入任何窗口;
// ③子进程报告会写入父进程锁定的工程目录。fork时已开的窗口在父进程内正常走完,
// 子进程静默退出监控
std::atomic<bool> g_forked{false};
std::atomic<uint64_t> g_windowId{0};                      // 当前窗口号(STAGE_START自增,事件携带)
std::atomic<uint32_t> g_stackDepth{DEFAULT_STACK_DEPTH};  // 全深度栈帧数(config.cStackDepth快照)
std::atomic<uint64_t> g_blockThreshold{0};                // 块阈值(config.blockSizeThreshold快照)
// 未归因累计(栈层失败——栈表trylock失败/表满/OOM——转未知桶stackId=0的块数,
// 只丢归因不丢块)。累计跨窗口,分析器差分,
// 闭窗打印用g_unattrWindowBase取窗口内增量
std::atomic<uint64_t> g_unattributedCount{0};
std::atomic<uint64_t> g_unattrWindowBase{0};  // 未归因窗口基线(开窗ClearTables快照,闭窗打印取差)
std::atomic<uint64_t> g_stackIdCounter{1};    // 栈id高位计数(低6位为分片号,0保留未知栈)
std::atomic<size_t> g_blockCount{0};
std::atomic<size_t> g_stackCount{0};
// =============================================================================
// 采样/截断/合计/闭窗快照全局
// =============================================================================
// 显式采样率倒数(2的幂,默认1=不采样;config.sampleRate开窗快照,0→1归一化)。
// 采样门控在记账之前判定,被跳过块对钩子完全不可见(报告标注sampled=1/N)
std::atomic<uint32_t> g_sampleRate{1};
// 整窗标注: bit0=块表触顶(申请转溢出通道照常记账,仅归因粒度退化为整体,
// 不再停止记账)。bit1=栈表触顶且死栈回收无法腾位(死栈回收已激活: 表满时优先
// 淘汰refs==1的零存活块条目,见EvictDeadStackLocked;采样无可淘汰者=全活表时
// 新键才转未知桶照常记账,归因粒度退化)。bit2=溢出通道触顶(块表与溢出账本均满,
// 记账停止——窗口数据为截断点前的完整前缀)
std::atomic<uint32_t> g_truncated{0};
// 未追踪计数(仅blockThreshold>0时非零): size<阈值的分配,不进账本不计栈
std::atomic<uint64_t> g_untrackedCount{0};
std::atomic<uint64_t> g_untrackedBytes{0};
// 未知桶计数(本窗口内): 采栈/栈表失败(owner==nullptr)转stackId=0的块,闭窗交付
// 为stackId=0行(申请/未释放/派生释放),与g_unattributedCount(累计,诊断导出)互证。
// 死栈淘汰的折叠计数亦并入此处(淘汰时refs==1⟹零存活块⟹仅折叠alloc,派生算术
// 自洽)——未知桶行语义="从未入表 + 死栈淘汰折叠",两群由g_evicted*区分
std::atomic<uint64_t> g_unknownAllocCount{0};
std::atomic<uint64_t> g_unknownAllocBytes{0};
// 死栈淘汰(栈表满时回收,见EvictDeadStackLocked): 本窗口被淘汰条目数与折叠的
// 申请计数/字节(折叠已并入g_unknownAlloc*——行求和==全局合计的闭合关系保持,
// g_totalAlloc*不经过淘汰路径,"申请=释放+未释放"不变量零影响)。随ClearTables
// 开窗清零,闭窗经stats交付(分析器健康区Evicted行)
std::atomic<uint64_t> g_evictedStackCount{0};
std::atomic<uint64_t> g_evictedAllocCount{0};
std::atomic<uint64_t> g_evictedAllocBytes{0};
// 全局合计(本窗口内): 记账门控通过的申请/释放累计(块表临界区内原子更新,见
// InsertBlock/CaptureAndRemoveBlock)。不变量"申请=释放+未释放"在闭窗快照下精确成立
std::atomic<uint64_t> g_totalAllocCount{0};
std::atomic<uint64_t> g_totalAllocBytes{0};
std::atomic<uint64_t> g_totalFreedCount{0};
std::atomic<uint64_t> g_totalFreedBytes{0};
// 溢出通道(块表满降级): 块表无法入表的申请转出至溢出账本并
// 累计。记账口径: 转出计入g_overflowAlloc*并同时并入
// g_totalAlloc*(全局合计口径一致,派生未释放=alloc-freed含溢出存活);溢出账本命中
// free逆向修正计入g_overflowFreed*并同时并入g_totalFreed*。溢出块无栈归因(不入
// 未知桶),存活=overflowAlloc-overflowFreed,闭窗快照随g_closeSnapshot交付
std::atomic<uint64_t> g_overflowAllocCount{0};
std::atomic<uint64_t> g_overflowAllocBytes{0};
std::atomic<uint64_t> g_overflowFreedCount{0};
std::atomic<uint64_t> g_overflowFreedBytes{0};
// 开窗前free独立通道(窗口外分配): free未命中块表与溢出账本
// (开窗前申请/记账被跳过),按开窗前分配统计。次数/总量原子累计;单块大小分布
// 按g_sizeBucketBounds同界归桶(原子桶向量,构造期ParseSizeBuckets后定容,
// ClearTables归零,闭窗快照进g_closePreWindowDist经dump_pre_window_distribution交付)。
// 不并入totalFreed——窗口外分配不入账本,并入会破坏"申请=释放+未释放"不变量
std::atomic<uint64_t> g_preWindowFreeCount{0};
std::atomic<uint64_t> g_preWindowFreeBytes{0};
// 原子桶向量元素: std::atomic 拷贝/移动构造deleted,vector::resize扩容需重定位
// (libstdc++ _M_default_append 走 __uninitialized_move_if_noexcept_a)——包一层可
// 重定位包装。构造恒零初始化(跨标准版本行为一致,新桶计数从0起;拷贝/移动仅
// resize扩容时relaxed读源值,扩容只发生在构造期定容,不在记账热路径)
struct AtomicU64
{
    std::atomic<uint64_t> v{0};

    AtomicU64() = default;
    AtomicU64(const AtomicU64& o) : v(o.v.load(std::memory_order_relaxed)) {}
    AtomicU64(AtomicU64&& o) : v(o.v.load(std::memory_order_relaxed)) {}
    AtomicU64& operator=(const AtomicU64& o)
    {
        v.store(o.v.load(std::memory_order_relaxed));
        return *this;
    }
    AtomicU64& operator=(AtomicU64&& o)
    {
        v.store(o.v.load(std::memory_order_relaxed));
        return *this;
    }

    uint64_t load(std::memory_order mo = std::memory_order_seq_cst) const { return v.load(mo); }
    void store(uint64_t x, std::memory_order mo = std::memory_order_seq_cst) { v.store(x, mo); }
    uint64_t fetch_add(uint64_t x, std::memory_order mo = std::memory_order_seq_cst) { return v.fetch_add(x, mo); }
};

// 初始化顺序:本so的.init_array中,__attribute__((constructor))的HostMemHookInit先于
// _GLOBAL__sub_I(编译器固定输出在TU末尾)执行——ctor里ParseSizeBuckets赋值与resize
// 的结果,随后被动态静态初始化按声明序重放的默认构造覆盖回空向量。init_priority把
// 变量挪入.init_array.NNN段,链接器(SORT_BY_INIT_PRIORITY)将全部带优先级段排在裸
// .init_array之前——先于HostMemHookInit完成构造,ctor的定容得以保留。保护面=ctor
// (含其pendingOpen开窗路径)写入的全部动态初始化全局;其余动态初始化对象重放结果
// 与ctor时态等价(空表/默认快照,无害),常量初始化对象(g_api{}/各atomic)不参与
// 重放。给HostMemHookInit加constructor(N)无法修复:带优先级段永远整体排在裸
// .init_array之前,只会令其更早执行
__attribute__((init_priority(101))) std::vector<AtomicU64, RealMallocAllocator<AtomicU64>>
    g_preWindowDistCount;  // 开窗前free分布: 各桶块数(构造期定容,下标=SizeBucketIndex)
__attribute__((init_priority(102))) std::vector<AtomicU64, RealMallocAllocator<AtomicU64>>
    g_preWindowDistBytes;  // 开窗前free分布: 各桶字节
// 闭窗聚合产物(CloseAggregate填充;ClearTables跨窗口清理;窗口关闭态读):
// per-stack行/大小排布桶/开窗前free分布/合计快照
std::vector<StackStatRow, RealMallocAllocator<StackStatRow>> g_closeStats;
std::vector<SizeBucket, RealMallocAllocator<SizeBucket>> g_closeSizeDist;
std::vector<SizeBucket, RealMallocAllocator<SizeBucket>> g_closePreWindowDist;
__attribute__((init_priority(103)))  // 顺序保护,见g_preWindowDistCount处注释(ctor赋值须在重放之前完成)
std::vector<uint64_t, RealMallocAllocator<uint64_t>>
    g_sizeBucketBounds;  // 大小桶边界(构造期解析)
CloseSnapshot g_closeSnapshot;

MsmemscopeHostmemApi g_api{};  // bind注册的回调表(release发布,enabled acquire可见)

size_t g_maxStacksPerShard = DEFAULT_MAX_STACKS / STACK_SHARDS;
size_t g_maxBlocksPerShard = DEFAULT_MAX_BLOCKS / BLOCK_SHARDS;
size_t g_maxOverflowPerShard = DEFAULT_MAX_OVERFLOW / BLOCK_SHARDS;  // 溢出账本安全阀(每分片)

// 开窗/闭窗串行化(config线程调用,防重入)
pthread_mutex_t g_svcMtx = PTHREAD_MUTEX_INITIALIZER;

// 构造完成标志+构造期暂存开窗请求(均由g_svcMtx保护):本so DT_NEEDED依赖
// libascend_leaks,其静态初始化先于本so构造执行——期间分析器驱动的set_enabled(true)
// 若直接开窗,运行参数/容量环境变量尚未解析(覆盖永不生效),STAGE_START也会提前到
// 采集库自身构造期分发。故构造完成前的开窗请求只暂存,HostMemHookInit完成解析后
// 补开(闭窗请求则撤销暂存:全在构造前开的窗本就无数据)
bool g_ctorDone = false;
bool g_openPending = false;

// 门控五查(热路径,顺序按开销递增): 抑制→窗口→宿主main边界→退出边界→采集库内部线程。
// main边界见g_mainStarted注释:静态初始化上下文(可执行文件/其他so的.init_array)绝不做
// backtrace/哈希表/dladdr——该阶段进程初始化不完整,操作均为未定义行为
inline bool ShouldTrace()
{
    if (t_hookSuppressDepth > 0)
    {
        return false;
    }
    if (!g_enabled.load(std::memory_order_acquire))
    {
        return false;
    }
    if (!g_mainStarted.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (g_exiting.load(std::memory_order_relaxed))
    {
        return false;  // 退出期:堆环境不可信,不再记账(见g_exiting注释)
    }
    if (g_api.is_suppressed != nullptr && g_api.is_suppressed() != 0)
    {
        return false;
    }
    return true;
}

// 与Utility::GetTimeNanoseconds同源(libstdc++ high_resolution_clock=system_clock=CLOCK_REALTIME)
inline uint64_t NowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// 单调时钟(节拍/间隔测量专用,不受墙钟跳变影响)
inline uint64_t MonotonicNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// tid线程内缓存(syscall成本~百ns,缓存后~1ns)
inline uint32_t CurrentTid()
{
    thread_local uint32_t t_tid = 0;
    if (t_tid == 0)
    {
        t_tid = static_cast<uint32_t>(syscall(SYS_gettid));
    }
    return t_tid;
}

// =============================================================================
// 栈表/块表: 栈表为各64张彼此独立的unordered_map(锁与数据同桶封装,分桶间零共享);
// 块表为开放寻址双数组(线性探测+后向位移删除,零节点分配)
// 锁顺序: 块表分片锁与栈表分片锁均为顺序加锁(绝不嵌套),无死锁可能
// =============================================================================

inline uint64_t Rotl64(uint64_t v, int s) { return (v << s) | (v >> (64 - s)); }

// xxHash64(8字节字流特化,seed=0):栈键哈希。标准xxHash64的32字节四路累加+8字节
// 字尾部(输入恒为uintptr_t数组,无4字节/单字节尾);帧数经长度参与(h+=len),
// 同pc不同帧数的键必然落入不同哈希域。FNV-1a为32帧串行乘法(延迟链乘加依赖
// ~百周期),本实现四路独立乘加+短终结,约1/3周期数且结果随键缓存一次
inline uint64_t XxHash64Words(const uintptr_t* words, size_t wordCount)
{
    constexpr uint64_t P1 = 11400714785074694791ull;
    constexpr uint64_t P2 = 14029467366897019727ull;
    constexpr uint64_t P3 = 1609587929392839161ull;
    constexpr uint64_t P4 = 9650029242287828579ull;
    constexpr uint64_t P5 = 2870177450012600261ull;
    const size_t len = wordCount * sizeof(uintptr_t);
    uint64_t h;
    if (wordCount >= 4)
    {
        uint64_t v1 = P1 + P2;
        uint64_t v2 = P2;
        uint64_t v3 = 0;
        uint64_t v4 = ~P1;
        const size_t chunks = wordCount / 4;
        for (size_t c = 0; c < chunks; ++c)
        {
            v1 = Rotl64(v1 + words[c * 4 + 0] * P2, 31) * P1;
            v2 = Rotl64(v2 + words[c * 4 + 1] * P2, 31) * P1;
            v3 = Rotl64(v3 + words[c * 4 + 2] * P2, 31) * P1;
            v4 = Rotl64(v4 + words[c * 4 + 3] * P2, 31) * P1;
        }
        h = Rotl64(v1, 1) + Rotl64(v2, 7) + Rotl64(v3, 12) + Rotl64(v4, 18);
        auto merge = [&h](uint64_t v)
        {
            v = Rotl64(v * P2, 31) * P1;
            h = (h ^ v) * P1 + P4;
        };
        merge(v1);
        merge(v2);
        merge(v3);
        merge(v4);
    }
    else
    {
        h = P5;
    }
    h += static_cast<uint64_t>(len);
    for (size_t i = wordCount & ~static_cast<size_t>(3); i < wordCount; ++i)
    {
        h ^= Rotl64(words[i] * P2, 31) * P1;
        h = Rotl64(h, 27) * P1 + P4;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

struct StackKey
{
    uintptr_t pc[STACK_KEY_FRAMES];
    uint32_t count;  // 有效帧数(≤STACK_KEY_FRAMES)
    uint32_t pad;
    // pc+count的单次xxHash64,构造键时计算一次并随键存储:分片定位/unordered_map
    // 桶定位(哈希函数零成本回传)/相等判定快速预筛复用同一值,免每次map操作重算
    // (256B键,快路径每块find×2+emplace共3次)
    uint64_t hash;
};

bool operator==(const StackKey& a, const StackKey& b)
{
    if (a.hash != b.hash || a.count != b.count)
    {
        return false;  // hash先行预筛:等价键(pc+count相同)必等hash,不等必不等键
    }
    return memcmp(a.pc, b.pc, a.count * sizeof(uintptr_t)) == 0;
}

struct StackKeyHash
{
    size_t operator()(const StackKey& k) const
    {
        return static_cast<size_t>(k.hash);  // 键构造时已算好,此处零成本
    }
};

struct StackEntry
{
    uint64_t stackId = 0;  // 单调递增,低6位=所属分片号,0保留未知栈
    // 全深度PC(登记慢路径采集,闭窗聚合符号化用;符号化后释放)与帧数
    uintptr_t* fullPcs = nullptr;
    uint32_t fullCount = 0;
    bool warmedUp = false;  // 稳态预热已选中过(分片锁内写/读)。不能用缓存命中
                            // 判"已预热":fullPcs[0]是hook锚点帧,全栈共享同一
                            // pc,首个栈预热后其余栈全被误判跳过
    // 引用计数refs(在表pin+存活块数+在途lookup数,见文件头引用契约): 1=仅pin,
    // 零存活块零在途;refs==1条目可被淘汰(栈表满→死栈回收,见EvictDeadStackLocked)。
    // 增减relaxed: 所有+1在栈分片锁临界区内(除InsertBlock块引用+1在块表锁内,
    // 有调用方在途ref兜底,期间refs恒>=2);-1恰一次于dispose(块释放/捕获后
    // 终态)。块表持owner指针期间refs>=2(块引用兜底),与淘汰判读(refs==1)互斥
    std::atomic<uint32_t> refs{1};        // 1=在表pin + 存活块数 + 在途lookup数
    std::atomic<int64_t> liveBytes{0};    // 当前存活字节(泄漏量真相源,与闭窗
                                          // 报告unfreedBytes同构——top泄漏点
                                          // 符号采样的排序键)
    std::atomic<uint64_t> allocCount{0};  // 本窗口内申请次数(InsertBlock临界区内自增)
    std::atomic<uint64_t> allocBytes{0};  // 本窗口内申请字节(同上)
};

struct StackShard
{
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    std::unordered_map<StackKey, StackEntry, StackKeyHash, std::equal_to<StackKey>,
                       RealMallocAllocator<std::pair<const StackKey, StackEntry>>>
        map;
};

// init_priority(顺序保护,见g_preWindowDistCount处注释):分片表含unordered_map为
// 动态初始化对象,裸init下HostMemHookInit的pendingOpen开窗路径启动的预热线程
// (WarmupThreadMain首拍无睡眠直接迭代分片表)会与本TU动态初始化重放(重建map)
// 数据竞态;前置构造后无重放,竞态消除
__attribute__((init_priority(104))) StackShard g_stackShards[STACK_SHARDS];

struct BlockEntry
{
    StackEntry* owner;  // 归栈条目指针(分片锁内维护;块在表即持其ref,refs>=2期间
                        // 指针有效——淘汰判读refs==1与之互斥,见引用契约)
    uint64_t size;      // 申请字节
    uint64_t allocTs;   // 申请时间戳
};

// 块表分片: 开放寻址(键值分离双数组,addr==0为空槽哨兵——malloc成功地址非0,
// 钩子入口对nullptr已过滤;容量恒2的幂)。相较unordered_map:零节点分配
// (插入/删除无malloc/free——洪峰下百万级节点分配是分配器热点与缓存不友好的
// 主源,块表生命周期=窗口生命周期,逐节点new/delete纯浪费)、探测路径连续
// (keys数组8B/槽,未命中扫描每cacheline过8槽)。负载因子7/10,越限翻倍重哈希;
// count上限g_maxBlocksPerShard(表满→申请转溢出账本降级记账)
struct BlockShard
{
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    uint64_t* keys = nullptr;    // 键数组(0=空槽)
    BlockEntry* vals = nullptr;  // 值数组(与keys同下标)
    uint32_t capMask = 0;        // 容量-1(容量=capMask+1,2的幂;0=未分配)
    uint32_t count = 0;          // 在表条目数
    // 溢出账本(块表满降级): addr→size,同一分片锁保护。
    // 表满无法入表的申请转出至此(容量有界g_maxOverflowPerShard,安全阀防OOM);
    // free命中→逆向修正统计并移除。带size(字节精确逆向修正),
    // 热路径常空(仅表满后非空)——节点分配走RealMallocAllocator(见BlockShard
    // 上方注释:块表本身零分配是常态路径,溢出账本仅在表满时启用)
    std::unordered_map<uint64_t, uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>,
                       RealMallocAllocator<std::pair<const uint64_t, uint64_t>>>
        overflow;
};

__attribute__((init_priority(105))) BlockShard g_blockShards[BLOCK_SHARDS];  // 顺序保护,同g_stackShards
