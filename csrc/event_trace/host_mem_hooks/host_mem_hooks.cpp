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

inline size_t BlockShardIndex(uint64_t addr)
{
    return static_cast<size_t>(addr >> 4) & static_cast<size_t>(BLOCK_SHARDS - 1);
}

// 块表桶定位(斐波那契乘法散列取高位):malloc地址低4位恒0(16B对齐)且高位段
// 段内变化少,乘以2^64/φ奇常数后截取高32位混合,再按掩码取所需位
inline uint32_t BlockBucketOf(uint64_t addr, uint32_t mask)
{
    return static_cast<uint32_t>((addr * 11400714785074694791ull) >> 32) & mask;
}

// 容量翻倍重哈希(必持分片锁):新表逐条重插(纯探测写,无分配);失败(OOM)返回
// false,旧表原样保留(调用方按表满降级计丢包)
bool BlockShardGrowLocked(BlockShard& shard)
{
    if (shard.capMask > (UINT32_MAX >> 1))
    {
        return false;  // 容量上界护栏(2^31槽=16TB键数组,仅畸形env可达),防翻倍回绕
    }
    const uint32_t newCap = (shard.capMask + 1) << 1;
    uint64_t* nk = static_cast<uint64_t*>(RealMalloc(static_cast<size_t>(newCap) * sizeof(uint64_t)));
    BlockEntry* nv = static_cast<BlockEntry*>(RealMalloc(static_cast<size_t>(newCap) * sizeof(BlockEntry)));
    if (nk == nullptr || nv == nullptr)
    {
        RealFree(nk);
        RealFree(nv);
        return false;
    }
    memset(nk, 0, static_cast<size_t>(newCap) * sizeof(uint64_t));
    const uint32_t nm = newCap - 1;
    for (uint32_t i = 0; i <= shard.capMask; ++i)
    {
        if (shard.keys[i] == 0)
        {
            continue;
        }
        uint32_t p = BlockBucketOf(shard.keys[i], nm);
        while (nk[p] != 0)
        {
            p = (p + 1) & nm;
        }
        nk[p] = shard.keys[i];
        nv[p] = shard.vals[i];
    }
    RealFree(shard.keys);
    RealFree(shard.vals);
    shard.keys = nk;
    shard.vals = nv;
    shard.capMask = nm;
    return true;
}

// 容量保障(必持分片锁):懒分配(首块落片时建1024槽)/负载越限(7/10)翻倍。
// 硬护栏count+1>=cap:线性探测至少留1空槽保证查找/插入可终止(仅持续OOM下
// 扩容反复失败时可达);返回false=不可得容量,调用方按表满降级
bool BlockShardEnsureRoomLocked(BlockShard& shard)
{
    if (shard.keys != nullptr)
    {
        const uint32_t cap = shard.capMask + 1;
        if (static_cast<uint64_t>(shard.count + 1) * 10 <= static_cast<uint64_t>(cap) * 7)
        {
            return true;  // 负载内
        }
        if (shard.count + 1 >= cap)
        {
            return false;  // 硬护栏(扩容失败后满载:拒绝防探测不终止)
        }
        return BlockShardGrowLocked(shard);
    }
    constexpr uint32_t kInitCap = 1024;  // 首配容量(负载上限716条,覆盖小窗口零增长)
    shard.keys = static_cast<uint64_t*>(RealMalloc(kInitCap * sizeof(uint64_t)));
    shard.vals = static_cast<BlockEntry*>(RealMalloc(kInitCap * sizeof(BlockEntry)));
    if (shard.keys == nullptr || shard.vals == nullptr)
    {
        RealFree(shard.keys);
        RealFree(shard.vals);
        shard.keys = nullptr;
        shard.vals = nullptr;
        return false;
    }
    memset(shard.keys, 0, kInitCap * sizeof(uint64_t));
    shard.capMask = kInitCap - 1;
    return true;
}

// 后向位移删除(必持分片锁):摘除keys[i]后从空洞向前扫描,把"home在本空洞之前"
// 的后续条目逐个上移(条件(新位移<=旧位移):home在j..k区间的条目上移会越过
// 其home致查找失效,跳过继续扫),每条目仍位于其home的探测路径上,无墓碑
void BlockShardRemoveAtLocked(BlockShard& shard, uint32_t i)
{
    const uint32_t mask = shard.capMask;
    uint32_t j = i;
    for (;;)
    {
        shard.keys[j] = 0;  // 挖空洞(先清后扫:簇即结束则该槽就此留空)
        uint32_t k = j;
        for (;;)
        {
            k = (k + 1) & mask;
            if (shard.keys[k] == 0)
            {
                return;  // 空槽=簇结束,空洞j保持空
            }
            const uint32_t home = BlockBucketOf(shard.keys[k], mask);
            if (((j - home) & mask) <= ((k - home) & mask))
            {
                break;  // k处条目可合法上移到j(新位移不小于0且不越过home)
            }
        }
        shard.keys[j] = shard.keys[k];
        shard.vals[j] = shard.vals[k];
        j = k;  // 空洞移到k,继续填补
    }
}

// 纯插入(必持分片锁,调用方保证addr不在表):false=表满/容量不可得(计丢包)
bool BlockShardInsertLocked(BlockShard& shard, uint64_t addr, const BlockEntry& rec)
{
    if (shard.count >= g_maxBlocksPerShard)
    {
        return false;  // 表满: 放弃记账该块(假泄漏规避)
    }
    if (!BlockShardEnsureRoomLocked(shard))
    {
        return false;
    }
    const uint32_t mask = shard.capMask;
    uint32_t i = BlockBucketOf(addr, mask);
    while (shard.keys[i] != 0)
    {
        i = (i + 1) & mask;
    }
    shard.keys[i] = addr;
    shard.vals[i] = rec;
    shard.count += 1;
    g_blockCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// 栈表分片定位:键缓存哈希低6位(与桶定位同一哈希值,xxHash64终结雪崩充分)
inline size_t StackShardIndex(const StackKey& key)
{
    return static_cast<size_t>(key.hash) & static_cast<size_t>(STACK_SHARDS - 1);
}
// stackId低6位=分片号(保留时编码),供free路径/上报线程按id定位分片
inline uint64_t MakeStackId(size_t shardIdx)
{
    return (g_stackIdCounter.fetch_add(1, std::memory_order_relaxed) << 6) | static_cast<uint64_t>(shardIdx);
}

inline size_t ShardOfStackId(uint64_t stackId)
{
    return static_cast<size_t>(stackId) & static_cast<size_t>(STACK_SHARDS - 1);
}

// =============================================================================
// 钩子自身帧过滤:以本so自身地址区间动态判定
// =============================================================================
// 本so装载区间(构造期一次写入;写入先于宿主main,而采栈仅发生在开窗后,无并发写)
uintptr_t g_hookSoLo = 0;
uintptr_t g_hookSoHi = 0;

inline bool InHookSoRange(uintptr_t pc) { return pc >= g_hookSoLo && pc < g_hookSoHi; }

struct HookSoSpan
{
    uintptr_t lo;
    uintptr_t hi;
};

// dl_iterate_phdr回调:以自身函数地址为锚点(匿名命名空间,必属本so,不受其他
// preload库劫持影响)定位本so对象,累计其全部PT_LOAD段span(覆盖text/rodata/
// data/bss;栈上的PC均为返回地址,只会落入可执行段,区间偏宽无副作用)
int HookSoRangeCb(dl_phdr_info* info, size_t size, void* data)
{
    (void)size;
    auto* span = static_cast<HookSoSpan*>(data);
    const uintptr_t anchor = reinterpret_cast<uintptr_t>(&HookSoRangeCb);
    uintptr_t lo = UINTPTR_MAX;
    uintptr_t hi = 0;
    bool hit = false;
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD)
        {
            continue;
        }
        const uintptr_t segLo = static_cast<uintptr_t>(info->dlpi_addr + ph.p_vaddr);
        const uintptr_t segHi = segLo + static_cast<uintptr_t>(ph.p_memsz);
        lo = segLo < lo ? segLo : lo;
        hi = segHi > hi ? segHi : hi;
        if (anchor >= segLo && anchor < segHi)
        {
            hit = true;
        }
    }
    if (hit)
    {
        span->lo = lo;
        span->hi = hi;
        return 1;  // 命中本so,终止迭代
    }
    return 0;
}

// 构造期解析本so区间(dl_load_lock为递归锁,构造期重入安全);失败(理论不可达)
// 区间保持[0,0)恒不匹配→钩子帧全保留,降级可见而非静默失真
void ResolveHookSoRange()
{
    HookSoSpan span{0, 0};
    if (dl_iterate_phdr(HookSoRangeCb, &span) > 0 && span.lo < span.hi)
    {
        g_hookSoLo = span.lo;
        g_hookSoHi = span.hi;
    }
}

// =============================================================================
// 帧指针快速走栈: 模块可执行段区间快照 + 线程栈界缓存 + fp链遍历
// =============================================================================

// ----- 模块可执行段区间快照(FP走栈pc校验依据) -----
// dl_iterate_phdr持加载器锁,不可在采栈热路径调用,故快照化:构造期+每次开窗+
// 预热线程1Hz(开窗态)刷新,4槽环形缓冲,写入"下一槽"后release发布,读取侧
// acquire取槽无锁。槽被复写最早发生在3次刷新后(≥3s),而单次走栈μs级,读侧
// 不可能读到复写中的槽;理论极端(读线程被抢占跨越3次刷新)下撕裂值仅影响校验
// 判定(误拒→截断回退backtrace/误纳→等价于无校验),无内存安全风险
constexpr uint32_t EXEC_SPAN_MAX = 2048;  // 可执行段容量上限:常规进程模块数百个,极端插件群千级
constexpr uint32_t EXEC_SNAP_SLOTS = 4;   // 环形槽数(复写宽限=槽数-1次刷新)
constexpr uint32_t EXEC_NAME_LEN = 64;    // 模块名截断长度(快照内拷贝,dlclose后加载器内存失效)

struct ExecSpan
{
    uintptr_t lo;
    uintptr_t hi;
    uintptr_t base;  // dlpi_addr:模块加载基址(pc-base=模块内偏移,与dladdr的dli_fbase同语义)
    char name[EXEC_NAME_LEN];
};

struct ExecRangeSnapshot
{
    uint32_t count;
    ExecSpan spans[EXEC_SPAN_MAX];
};

ExecRangeSnapshot g_execSnaps[EXEC_SNAP_SLOTS];  // bss零初始化,count==0即"未就绪"
std::atomic<uint32_t> g_execSnapPub{0};          // 当前发布槽号
// 刷新互斥:开窗(配置线程)与预热线程1Hz可能并发刷新,二者计算"下一槽"相同
// 会互相复写,互斥后串行化(冷路径,无争用)
pthread_mutex_t g_execSnapMtx = PTHREAD_MUTEX_INITIALIZER;

// pc∈任一可执行段?spans已按lo排序(刷新侧冷路径排序),二分定位最后一个
// lo≤pc的span后单次区间判定;模块段实际互不重叠,未命中即不命中。
// 返回命中span指针(供调用方做段内偏移安全判定),未命中返回nullptr
inline const ExecSpan* FindExecSpan(uintptr_t pc, const ExecRangeSnapshot& snap)
{
    uint32_t lo = 0;
    uint32_t hi = snap.count;
    while (lo < hi)
    {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (snap.spans[mid].lo <= pc)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo == 0 || pc >= snap.spans[lo - 1].hi)
    {
        return nullptr;
    }
    return &snap.spans[lo - 1];
}

int ExecSpanCb(dl_phdr_info* info, size_t size, void* data)
{
    (void)size;
    auto& snap = *static_cast<ExecRangeSnapshot*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0)
        {
            continue;  // 仅可执行PT_LOAD段:栈上的PC均为返回地址,只可能落在其中
        }
        if (snap.count >= EXEC_SPAN_MAX)
        {
            return 1;  // 容量满:截断(罕见;未收录模块的pc校验不通过→该等调用点回退backtrace)
        }
        const uintptr_t lo = static_cast<uintptr_t>(info->dlpi_addr + ph.p_vaddr);
        ExecSpan& span = snap.spans[snap.count];
        span.lo = lo;
        span.hi = lo + static_cast<uintptr_t>(ph.p_memsz);
        span.base = static_cast<uintptr_t>(info->dlpi_addr);
        // 模块名截断拷贝(加载器内部内存,dlclose后失效——快照必须自持拷贝);
        // 取路径尾基名(与dladdr输出同风格),空名兜底"?"
        const char* n = (info->dlpi_name != nullptr) ? info->dlpi_name : "";
        const char* slash = strrchr(n, '/');
        const char* baseName = (slash != nullptr) ? slash + 1 : n;
        if (baseName[0] == '\0')
        {
            baseName = "?";
        }
        strncpy(span.name, baseName, EXEC_NAME_LEN - 1);
        span.name[EXEC_NAME_LEN - 1] = '\0';
        snap.count += 1;
    }
    return 0;
}

void RefreshExecRanges()
{
    pthread_mutex_lock(&g_execSnapMtx);
    const uint32_t prev = g_execSnapPub.load(std::memory_order_relaxed);
    const uint32_t slot = (prev + 1) % EXEC_SNAP_SLOTS;
    ExecRangeSnapshot& snap = g_execSnaps[slot];
    snap.count = 0;  // 先清计数:该槽此刻对读侧不可见(发布的是prev槽),写入顺序无关紧要
    dl_iterate_phdr(ExecSpanCb, &snap);
    std::sort(snap.spans, snap.spans + snap.count,
              [](const ExecSpan& a, const ExecSpan& b) { return a.lo < b.lo; });  // 读侧二分前提
    g_execSnapPub.store(slot, std::memory_order_release);  // 发布(store前的span写入对acquire读侧可见)
    pthread_mutex_unlock(&g_execSnapMtx);
}

// ----- 线程栈边界(TLS缓存,线程首走一次) -----
struct ThreadStackBounds
{
    uintptr_t lo;
    uintptr_t hi;
    bool ok;
};

thread_local ThreadStackBounds t_stackBounds{0, 0, false};

// pthread_getattr_np取本线程栈界(主线程返回初始栈范围,对走栈校验是安全上界);
// 线程生命周期内不变,缓存后零成本。内部无堆分配,可在钩子热路径首次调用
const ThreadStackBounds& GetThreadStackBounds()
{
    if (!t_stackBounds.ok)
    {
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0)
        {
            void* base = nullptr;
            size_t size = 0;
            if (pthread_attr_getstack(&attr, &base, &size) == 0 && base != nullptr && size >= 2 * sizeof(uintptr_t))
            {
                t_stackBounds.lo = reinterpret_cast<uintptr_t>(base);
                t_stackBounds.hi = t_stackBounds.lo + static_cast<uintptr_t>(size);
                t_stackBounds.ok = true;
            }
            pthread_attr_destroy(&attr);
        }
    }
    return t_stackBounds;
}

// 取当前帧指针(x86_64=rbp,aarch64=x29);未适配架构返回0→走栈不可用直接回退。
// 注意AT&T与AArch64汇编的操作数方向相反:前者源在左、后者目的在左
inline uintptr_t CurrentFramePointer()
{
#if defined(__x86_64__)
    uintptr_t fp;
    asm volatile("movq %%rbp, %0" : "=r"(fp));
    return fp;
#elif defined(__aarch64__)
    uintptr_t fp;
    asm volatile("mov %0, x29" : "=r"(fp));
    return fp;
#else
    return 0;
#endif
}

// 帧指针快速走栈: 沿fp链逐帧取返回地址写入out(含钩子机械帧在内的
// 原始帧,头部过滤由调用方统一做),返回帧数;reachedBottom=true表示走至链底
// (nextFp==0——ELF入口_start按ABI清零帧指针标记最外层帧)而非被校验截断。
// 全程只读[初始fp,栈顶)——该区间被当前调用链占据,必已映射,无SIGSEGV风险
int WalkFramePointers(uintptr_t* out, int maxOut, bool& reachedBottom)
{
    reachedBottom = false;
    if (maxOut <= 0)
    {
        return 0;
    }
    const ThreadStackBounds& sb = GetThreadStackBounds();
    if (!sb.ok)
    {
        return 0;
    }
    const ExecRangeSnapshot& snap = g_execSnaps[g_execSnapPub.load(std::memory_order_acquire)];
    if (snap.count == 0)
    {
        return 0;  // 快照未就绪(构造期前):交由backtrace兜底
    }
    uintptr_t fp = CurrentFramePointer();
    int n = 0;
    while (n < maxOut && fp >= sb.lo && fp + 2 * sizeof(uintptr_t) <= sb.hi && (fp & 0xf) == 0)
    {
        // [fp]=上层帧指针,[fp+8]=本帧返回地址(先出帧再走链)
        const uintptr_t nextFp = *reinterpret_cast<const uintptr_t*>(fp);
        const uintptr_t pc = *reinterpret_cast<const uintptr_t*>(fp + sizeof(uintptr_t));
        if (FindExecSpan(pc, snap) == nullptr)
        {
            break;  // pc不在任何可执行段:毁链后落入数据区的垃圾帧,截断
        }
        out[n++] = pc;
        if (nextFp == 0)
        {
            reachedBottom = true;
            break;
        }
        if (nextFp <= fp)
        {
            break;  // 非递增(合法链必严格递增):截断
        }
        fp = nextFp;
    }
    return n;
}

// 采栈: 过滤头部钩子帧后返回有效帧数(≤maxOut)。跳过头部落在本so区间内的连续帧,
// 但保留其中最后一帧(malloc/calloc等劫持入口:报告的分配接口锚点,亦参与去重键——
// 不同分配接口同深调用链天然不合并);其余头部帧(backtrace蹦床/CaptureFrames/
// RecordMalloc)为纯钩子机械帧,整段跳过。深部不过滤:主线程回栈尾部的宿主main
// 边界trampoline帧是真实调用链组成部分,剔除会使main与__libc_start_main之间断链,
// 回溯不清晰——设计选择保留(主线程栈含两帧本so帧:头部锚点+栈底trampoline)
int CaptureFrames(uintptr_t* out, int maxOut)
{
    if (maxOut <= 0)
    {
        return 0;
    }
    // 热路径(maxOut≤STACK_KEY_FRAMES)用栈上缓冲;全深度登记(maxOut可达MAX_STACK_DEPTH)
    // 需临时堆缓冲——固定浅缓冲会把深度采集静默截断
    uintptr_t localBuf[STACK_KEY_FRAMES + HOOK_FRAME_HEADROOM];
    uintptr_t* buf = localBuf;
    int cap = STACK_KEY_FRAMES + HOOK_FRAME_HEADROOM;
    uintptr_t* heapBuf = nullptr;
    const int want = maxOut + HOOK_FRAME_HEADROOM;  // 多采头部余量,过滤后仍有maxOut帧
    if (want > cap)
    {
        heapBuf = static_cast<uintptr_t*>(RealMalloc(static_cast<size_t>(want) * sizeof(uintptr_t)));
        if (heapBuf != nullptr)
        {
            buf = heapBuf;
            cap = want;
        }
        // 堆分配失败退化为栈上容量(采栈截断,不丢整块记账)
    }
    // 采栈方法:FP快走栈为默认路径,采满请求帧数或走至链底才采用,
    // 否则回退backtrace。同一调用路径链长确定→逐次走同一分支,去重键稳定;
    // 新装载模块进入快照前后(≤1s)同一调用点可能先后走两法各登记一栈,短暂
    // 重复但语义正确。头部帧两种来源均在钩子so内(本so整体保留帧指针),动态
    // 头部过滤对两法统一生效
    const int target = want > cap ? cap : want;
    bool reachedBottom = false;
    int n = WalkFramePointers(buf, target, reachedBottom);
    if (n < target && !reachedBottom)
    {
        n = backtrace(reinterpret_cast<void**>(buf), target);
    }
    int lead = 0;
    while (lead < n && InHookSoRange(buf[lead]))
    {
        ++lead;
    }
    const int skip = lead > 0 ? lead - 1 : 0;  // 保留最后一帧钩子帧(劫持入口帧)
    int cnt = n - skip;
    if (cnt > maxOut)
    {
        cnt = maxOut;
    }
    if (cnt > 0)
    {
        memcpy(out, buf + skip, static_cast<size_t>(cnt) * sizeof(uintptr_t));
    }
    RealFree(heapBuf);
    return cnt;
}

// 显式采样门控: 每线程去相关采样器(xorshift32状态线程本地)。不用共享原子
// 计数器(缓存行乒乓)也不用"线程内计数%rate"(全部业务线程同相位,采样点与周期性
// 节拍——DataLoader批迭代/算子注册批——对齐时产生系统性偏差:恰好总采到批首
// 或总漏掉同一相位)。种子混合tid与单调钟防同批线程同序列。rate为2的幂
// (开窗快照时规范化),掩码判定单周期;rate<=1(不采样)直接返回true——
// 默认路径门控零额外开销
thread_local uint32_t t_sampleState = 0;

bool SampleGateHit()
{
    const uint32_t rate = g_sampleRate.load(std::memory_order_relaxed);
    if (rate <= 1)
    {
        return true;  // 不采样: 全部通过(默认)
    }
    uint32_t x = t_sampleState;
    if (x == 0)
    {
        // 首次调用:播种(tid混单调钟;xorshift拒绝0状态,全0则换金比例常数)
        x = static_cast<uint32_t>(CurrentTid()) ^ static_cast<uint32_t>(MonotonicNs() >> 4);
        if (x == 0)
        {
            x = 0x9e3779b9u;
        }
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t_sampleState = x;
    return (x & (rate - 1)) == 0;
}

// 死栈桶采样淘汰(必持栈分片锁,表满登记路径调用): 栈表触顶时回收"死栈"
// (refs==1=仅pin,零存活块零在途)条目,让表占用与持有存活内存的路径对齐。
// 采样: 8个随机桶(xorshift64,种子为全局原子递增计数器,每次调用取新种子),
// 收集桶链上候选;受害者=候选中refs==1且allocCount最小者(空条目allocCount=0
// 优先回收=自洁性;refs==1⟹liveBytes==0,预热top-K(liveBytes>0)候选按构造
// 不相交,淘汰不伤符号化预热)。拆除(全部锁内): 折叠计数→RealFree(fullPcs)→
// map.erase→g_stackCount.fetch_sub。每次最多淘汰1个,不循环不滞回;返回true=
// 已淘汰(调用方继续登记),false=采样无可淘汰者(全活表,调用方置bit1+转未知桶)。
// 内存序: 本函数持栈分片锁,与全部refs+1(除InsertBlock块引用+1在块表锁内,其
// 有调用方在途ref兜底,期间refs恒>=2)的释放侧同锁串行——release-acquire边保证
// 读到一切已完成的lookup+1;锁外-1(dispose)只发生在该线程对条目最后一次触碰,
// 与淘汰判读(refs==1)不并发
bool EvictDeadStackLocked(StackShard& shard)
{
    constexpr size_t kEvictSampleBuckets = 8;
    static std::atomic<uint64_t> sEvictSeed{0x9e3779b97f4a7c15ull};  // 原子递增种子
    uint64_t state = sEvictSeed.fetch_add(1, std::memory_order_relaxed) | 1ull;
    const size_t bucketCount = shard.map.bucket_count();
    if (bucketCount == 0)
    {
        return false;
    }
    StackKey victimKey{};  // 记录受害者键,采样后find定位(锁内无并发修改,必命中)
    bool found = false;
    uint64_t bestAlloc = UINT64_MAX;
    for (size_t i = 0; i < kEvictSampleBuckets; ++i)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const size_t b = static_cast<size_t>(state) % bucketCount;
        for (auto it = shard.map.begin(b); it != shard.map.end(b); ++it)
        {
            StackEntry& e = it->second;
            if (e.refs.load(std::memory_order_relaxed) != 1)
            {
                continue;  // 存活块/在途lookup: 不可淘汰(非零即owner悬垂)
            }
            const uint64_t ac = e.allocCount.load(std::memory_order_relaxed);
            if (ac < bestAlloc)
            {
                bestAlloc = ac;
                victimKey = it->first;  // 拷贝键(冷路径:淘汰频率=表满登记频率)
                found = true;
            }
        }
    }
    if (!found)
    {
        return false;  // 全活表: 采样无可淘汰者
    }
    auto vit = shard.map.find(victimKey);  // 锁内无并发修改,必命中(防御兜底)
    if (vit == shard.map.end())
    {
        return false;
    }
    // 折叠计数(refs==1⟹零存活块⟹unfreed折叠为零,释放=alloc-未释放派生算术
    // 自洽,"申请=释放+未释放"不变量保持;折叠并入未知桶行,evicted*另行交付)
    const uint64_t ac = vit->second.allocCount.load(std::memory_order_relaxed);
    const uint64_t ab = vit->second.allocBytes.load(std::memory_order_relaxed);
    g_unknownAllocCount.fetch_add(ac, std::memory_order_relaxed);
    g_unknownAllocBytes.fetch_add(ab, std::memory_order_relaxed);
    g_evictedStackCount.fetch_add(1, std::memory_order_relaxed);
    g_evictedAllocCount.fetch_add(ac, std::memory_order_relaxed);
    g_evictedAllocBytes.fetch_add(ab, std::memory_order_relaxed);
    RealFree(vit->second.fullPcs);  // 符号化PC释放(可null,RealFree防御)
    shard.map.erase(vit);
    g_stackCount.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

// 栈表查找/登记(热路径):
// 命中→返回条目指针(锁内refs+1=在途lookup引用,调用方记账后恰一次dispose);
// 未命中→(无锁全深度采栈)→重查→插入(新条目refs=1建表+锁内+1在途)。
// 纯查找不直接计数: 栈计数(allocCount/allocBytes/refs/liveBytes)由
// InsertBlock在块表分片锁临界区内与条目存在性原子更新——块表插入成功
// 才计数(块引用refs+1),失败不计数,闭窗快照下"申请=释放+未释放"恒等精确成立。
// 表满: 先尝试EvictDeadStackLocked回收死栈(refs==1)腾位继续登记;采样无可
// 淘汰者(全活表)→nullptr并计未归因(块转未知桶stackId=0继续记账,只损失
// 归因粒度)+置bit1。trylock失败→nullptr计未归因。malloc路径trylock不做
// 自旋重试,守住热路径延迟预算
StackEntry* LookupOrRegisterStack(const StackKey& key)
{
    StackShard& shard = g_stackShards[StackShardIndex(key)];
    if (pthread_mutex_trylock(&shard.mtx) != 0)
    {
        return nullptr;
    }
    auto it = shard.map.find(key);
    if (it != shard.map.end())
    {
        // 快路径: 同一分配点重复命中(AI场景核心优化),不采全深度不符号化
        it->second.refs.fetch_add(1, std::memory_order_relaxed);  // 在途lookup+1(锁内)
        pthread_mutex_unlock(&shard.mtx);
        return &it->second;
    }
    pthread_mutex_unlock(&shard.mtx);

    // 慢路径: 先无锁全深度采栈(避免持锁做μs级 unwind),再重锁复查(期间他线程可能已插入)
    uint32_t depth = g_stackDepth.load(std::memory_order_relaxed);
    if (depth == 0 || depth > MAX_STACK_DEPTH)
    {
        depth = DEFAULT_STACK_DEPTH;
    }
    uintptr_t* fullPcs = static_cast<uintptr_t*>(RealMalloc(depth * sizeof(uintptr_t)));
    uint32_t fullCount = 0;
    if (fullPcs != nullptr)
    {
        fullCount = static_cast<uint32_t>(CaptureFrames(fullPcs, static_cast<int>(depth)));
    }

    if (pthread_mutex_trylock(&shard.mtx) != 0)
    {
        RealFree(fullPcs);
        return nullptr;
    }
    it = shard.map.find(key);
    if (it != shard.map.end())
    {
        // 竞争插入: 他线程已登记,复用其条目
        it->second.refs.fetch_add(1, std::memory_order_relaxed);  // 在途lookup+1(锁内)
        pthread_mutex_unlock(&shard.mtx);
        RealFree(fullPcs);
        return &it->second;
    }
    if (shard.map.size() >= g_maxStacksPerShard)
    {
        // 表满: 先尝试死栈淘汰(回收refs==1的零存活块条目腾位,见
        // EvictDeadStackLocked)。实测400k栈约64%为死栈,回收让表占用与持有存活
        // 内存的路径对齐;采样无可淘汰者(全活表)→新栈登记失败,块转未知桶继续
        // 记账(深键假链风暴下栈表可被灌满,丢块会把账本完整度一并拖垮;只丢归因,
        // 未归因计数可观测)+置截断标注bit1(死栈回收已激活,仍无法腾位→兜底未知桶)
        if (!EvictDeadStackLocked(shard))
        {
            g_truncated.fetch_or(2u, std::memory_order_relaxed);
            pthread_mutex_unlock(&shard.mtx);
            RealFree(fullPcs);
            return nullptr;
        }
        // 淘汰成功: 表内有位,继续登记
    }

    StackEntry* entry = nullptr;
    try
    {
        // StackEntry含std::atomic成员,不可拷贝/移动:piecewise_construct原地
        // 默认构造节点,随后填字段(emplace(key, lvalue)需拷贝构造pair,编译不过)
        auto ins = shard.map.emplace(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple());
        if (ins.second)
        {
            entry = &ins.first->second;
            entry->stackId = MakeStackId(StackShardIndex(key));
            entry->fullPcs = fullPcs;
            entry->fullCount = fullCount;
            entry->refs.fetch_add(1, std::memory_order_relaxed);  // 在途lookup+1(锁内,建条目后)
            g_stackCount.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            // find刚miss而emplace命中: 容器内部不一致,块转未知桶(计未归因)
        }
    }
    catch (...)
    {
        // 节点/bucket分配失败(bad_alloc,libstdc++强异常保证下表不变):
        // 块转未知桶继续记账(计未归因)——异常绝不允许穿出malloc入口杀死宿主
    }
    pthread_mutex_unlock(&shard.mtx);
    if (entry == nullptr)
    {
        RealFree(fullPcs);
        return nullptr;
    }
    return entry;
}

// 块表插入结果:
// kTable=成功入块表(归因粒度完整); kOverflow=块表满转溢出账本(记账继续,归因退化
// 为整体); kFailed=未记账(trylock瞬时竞争静默跳过,或溢出账本亦满→bit2记账停止)
enum class BlockInsertResult
{
    kTable,
    kOverflow,
    kFailed
};
// 块表捕获结果: kBlock=命中块表(释放记账已做); kOverflow=命中溢出账本(逆向修正
// 已做); kMiss=均未命中(开窗前free,调用方独立通道统计); kLockFailed=有界自旋耗尽
// (块可能在表但未取到锁,条目残留→假泄漏风险有界且极罕见——静默跳过,不置截断:
// 瞬时竞争非数据降级)
enum class BlockRemoveResult
{
    kBlock,
    kOverflow,
    kMiss,
    kLockFailed
};

// 溢出账本插入(必持分片锁): 块表满降级时addr→size入set并累计溢出申请统计。
// 成功返回true;set满(安全阀,防无容量增长OOM)/OOM→bit2记账停止,
// 返回false。totalAlloc由调用方并入(InsertBlock转出时并入;ReinsertBlock为回插
// 不重复并入——块已在窗口内申请过一次)
bool InsertOverflowLocked(BlockShard& shard, uint64_t addr, uint64_t size)
{
    if (shard.overflow.size() >= g_maxOverflowPerShard)
    {
        g_truncated.fetch_or(4u, std::memory_order_relaxed);  // 溢出通道触顶: 记账停止
        return false;
    }
    try
    {
        shard.overflow[addr] = size;  // operator[]覆盖陈旧同名(残留自愈)
    }
    catch (...)
    {
        g_truncated.fetch_or(4u, std::memory_order_relaxed);  // 溢出账本OOM: 记账停止
        return false;
    }
    g_overflowAllocCount.fetch_add(1, std::memory_order_relaxed);
    g_overflowAllocBytes.fetch_add(size, std::memory_order_relaxed);
    return true;
}

// 块表插入(热路径): 成功返回kTable;trylock失败/表满/容量不可得→降级处理:
// 表满→转溢出账本(记账继续,kOverflow);溢出亦满→kFailed(bit2已置位);
// trylock瞬时竞争→kFailed(静默跳过,不置截断——瞬时态非数据降级)。
// 记账契约(全部在块表分片锁临界区内完成,与条目存在性原子):
//   ① 插入成功才计数: owner!=nullptr→其allocCount/allocBytes/refs/liveBytes
//      原子自增(块引用+1: 调用方在途ref兜底,期间refs恒>=2,淘汰判读refs==1
//      与之互斥,锁内直接fetch_add无需取栈表锁——跨分片并发更新由原子性保证);
//      owner==nullptr(未知桶)→g_unknownAllocCount/Bytes自增;恒有
//      g_totalAllocCount/Bytes自增。
//      溢出转出亦并入g_totalAllocCount/Bytes(累计值符合区间内实际内存变化),
//      溢出块无栈归因不计owner
//   ② 同地址覆盖(前次FREE未达/竞态残留后地址复用)=虚拟释放: 被覆盖旧记录
//      按其owner递减refs/liveBytes(块引用-1)并g_totalFreedCount/Bytes按旧size
//      自增——再按新分配记账。由此"申请=释放+未释放"在所有交织下精确成立
//   ③ 入表成功即清理溢出账本同名残留: 溢出账本中残留的该地址条目
//      (地址复用/锁耗尽未达free)现转入块表,必须移除防双记账——两账本互斥,
//      同一地址任一时刻只在一处记账
BlockInsertResult InsertBlock(uint64_t addr, uint64_t size, uint64_t ts, StackEntry* owner)
{
    BlockShard& shard = g_blockShards[BlockShardIndex(addr)];
    if (pthread_mutex_trylock(&shard.mtx) != 0)
    {
        return BlockInsertResult::kFailed;  // 瞬时竞争: 静默跳过(不置截断)
    }
    if (shard.count >= g_maxBlocksPerShard || !BlockShardEnsureRoomLocked(shard))
    {
        // 表满(条数上限)/容量不可得(OOM): 转溢出账本降级记账。
        // 记账继续,仅归因粒度退化为整体(无栈)
        if (!InsertOverflowLocked(shard, addr, size))
        {
            pthread_mutex_unlock(&shard.mtx);
            return BlockInsertResult::kFailed;  // 溢出账本亦满: bit2已置位
        }
        g_totalAllocCount.fetch_add(1, std::memory_order_relaxed);
        g_totalAllocBytes.fetch_add(size, std::memory_order_relaxed);
        pthread_mutex_unlock(&shard.mtx);
        return BlockInsertResult::kOverflow;
    }
    const uint32_t mask = shard.capMask;
    uint32_t i = BlockBucketOf(addr, mask);
    while (shard.keys[i] != 0 && shard.keys[i] != addr)
    {
        i = (i + 1) & mask;
    }
    if (shard.keys[i] == addr)
    {
        // 同地址遗留记录(前次FREE未达/竞态残留后地址复用): 覆盖=虚拟释放旧块
        const BlockEntry& old = shard.vals[i];
        if (old.owner != nullptr)
        {
            old.owner->refs.fetch_sub(1, std::memory_order_relaxed);  // 旧块引用-1(虚拟释放)
            old.owner->liveBytes.fetch_sub(static_cast<int64_t>(old.size), std::memory_order_relaxed);
        }
        g_totalFreedCount.fetch_add(1, std::memory_order_relaxed);
        g_totalFreedBytes.fetch_add(old.size, std::memory_order_relaxed);
        shard.vals[i].owner = owner;
        shard.vals[i].size = size;
        shard.vals[i].allocTs = ts;
    }
    else
    {
        shard.keys[i] = addr;
        shard.vals[i] = BlockEntry{owner, size, ts};
        shard.count += 1;
        g_blockCount.fetch_add(1, std::memory_order_relaxed);
    }
    // 残留自愈: 入表成功即移除溢出账本同名(见契约③)
    if (!shard.overflow.empty())
    {
        shard.overflow.erase(addr);
    }
    // 新分配记账(成功插入才计数,见函数头契约①)
    if (owner != nullptr)
    {
        owner->allocCount.fetch_add(1, std::memory_order_relaxed);
        owner->allocBytes.fetch_add(size, std::memory_order_relaxed);
        owner->refs.fetch_add(1, std::memory_order_relaxed);  // 块引用+1(调用方在途ref兜底)
        owner->liveBytes.fetch_add(static_cast<int64_t>(size), std::memory_order_relaxed);
    }
    else
    {
        g_unknownAllocCount.fetch_add(1, std::memory_order_relaxed);
        g_unknownAllocBytes.fetch_add(size, std::memory_order_relaxed);
    }
    g_totalAllocCount.fetch_add(1, std::memory_order_relaxed);
    g_totalAllocBytes.fetch_add(size, std::memory_order_relaxed);
    pthread_mutex_unlock(&shard.mtx);
    return BlockInsertResult::kTable;
}

// 块表查询并删除(free/realloc捕获路径): 返回BlockRemoveResult——
// kBlock=命中块表(释放记账已在锁内完成: 减liveBytes+增g_totalFreed*;块引用refs
// 不减——捕获即转移给out,调用方持有并终态恰一次dispose,见引用契约);
// kOverflow=命中溢出账本(逆向修正已在锁内完成: 增g_overflowFreed*+增g_totalFreed*,
// 条目已移除——溢出申请已并入g_totalAlloc*,其释放须回扣g_totalFreed*维持全局
// 不变量);
// kMiss=两账本均未命中(开窗前分配/记账被跳过→调用方独立通道统计);
// kLockFailed=有界自旋耗尽(块可能在表但未取到锁: 静默跳过,条目残留→假泄漏风险
// 有界且极罕见——瞬时竞争非数据降级,不置截断标注)。
// 命中块表时防御性清除溢出账本同名残留(地址复用/锁耗尽残留下同一地址
// 两账本互斥,防双记账)
BlockRemoveResult CaptureAndRemoveBlock(uint64_t addr, BlockEntry& out)
{
    BlockShard& shard = g_blockShards[BlockShardIndex(addr)];
    for (int spin = 0; spin < BLOCK_LOCK_SPINS; ++spin)
    {
        if (pthread_mutex_trylock(&shard.mtx) != 0)
        {
            CpuRelax();
            continue;
        }
        if (shard.keys != nullptr)
        {
            const uint32_t mask = shard.capMask;
            uint32_t i = BlockBucketOf(addr, mask);
            while (shard.keys[i] != 0)
            {
                if (shard.keys[i] == addr)
                {
                    out = shard.vals[i];
                    BlockShardRemoveAtLocked(shard, i);
                    shard.count -= 1;
                    g_blockCount.fetch_sub(1, std::memory_order_relaxed);
                    if (out.owner != nullptr)
                    {
                        // 块引用转移: 捕获即转移给out,调用方持有并终态恰一次dispose,
                        // 此处不减refs——realloc在途窗口靠此引用钉住条目(见引用契约)
                        out.owner->liveBytes.fetch_sub(static_cast<int64_t>(out.size), std::memory_order_relaxed);
                    }
                    g_totalFreedCount.fetch_add(1, std::memory_order_relaxed);
                    g_totalFreedBytes.fetch_add(out.size, std::memory_order_relaxed);
                    // 残留自愈: 命中块表即移除溢出账本同名
                    if (!shard.overflow.empty())
                    {
                        shard.overflow.erase(addr);
                    }
                    pthread_mutex_unlock(&shard.mtx);
                    return BlockRemoveResult::kBlock;
                }
                i = (i + 1) & mask;
            }
        }
        // 块表未命中→溢出账本查询: 命中即逆向修正
        const auto it = shard.overflow.find(addr);
        if (it != shard.overflow.end())
        {
            out.size = it->second;
            out.owner = nullptr;
            out.allocTs = 0;
            shard.overflow.erase(it);
            g_overflowFreedCount.fetch_add(1, std::memory_order_relaxed);
            g_overflowFreedBytes.fetch_add(out.size, std::memory_order_relaxed);
            g_totalFreedCount.fetch_add(1, std::memory_order_relaxed);
            g_totalFreedBytes.fetch_add(out.size, std::memory_order_relaxed);
            pthread_mutex_unlock(&shard.mtx);
            return BlockRemoveResult::kOverflow;
        }
        pthread_mutex_unlock(&shard.mtx);
        return BlockRemoveResult::kMiss;  // 两账本均未命中: 开窗前free,调用方独立通道
    }
    // 有界重试耗尽: 静默跳过(不置截断标注,见函数头注释)
    return BlockRemoveResult::kLockFailed;
}

// 块表回插(realloc失败恢复,源为块表kBlock): 捕获记录原样放回,并恢复
// CaptureAndRemoveBlock已做的释放记账(重增liveBytes+回扣g_totalFreed*)——
// 每个在途realloc在闭窗边界都完整表现为"in"或"out",不变量不被撕裂。
// 引用契约: 捕获时块引用已转移给rec(调用方持有),回插成功=引用归块(不增不减,
// 无fetch_add);转溢出/转溢出失败/自旋耗尽=块离开栈归因或不可见,转移引用
// 恰一次dispose(refs-1)。
// 表满/容量不可得→转溢出账本(与InsertBlock同降级语义,记账继续): 撤销捕获时的
// 释放记账(块仍存活,回扣g_totalFreed*)并计入溢出通道(g_overflowAlloc*,不重复
// g_totalAlloc*——块已在原分配时计数);owner计数不恢复(块离开原栈归因,转入
// 无栈溢出通道)。溢出账本亦满(InsertOverflowLocked失败,bit2已置位)→块自此
// 不可见,尽力而为(记账已停止,超出截断点的边界行为不保证)
// trylock失败→有界自旋重试(失败=块表缺口,后续free未命中,该块自窗口报告消失);
// 重试耗尽静默跳过(不置截断标注,同CaptureAndRemoveBlock——瞬时竞争非数据降级)
void ReinsertBlock(uint64_t addr, const BlockEntry& rec)
{
    BlockShard& shard = g_blockShards[BlockShardIndex(addr)];
    for (int spin = 0; spin < BLOCK_LOCK_SPINS; ++spin)
    {
        if (pthread_mutex_trylock(&shard.mtx) != 0)
        {
            CpuRelax();
            continue;
        }
        // addr刚被本线程捕获移除,必不在表(真分配器未归还该地址),纯插入;
        // 表满/容量不可得→转溢出账本降级(见InsertBlock同款语义)
        if (!BlockShardInsertLocked(shard, addr, rec))
        {
            if (InsertOverflowLocked(shard, addr, rec.size))
            {
                // 转溢出成功: 撤销捕获时的释放记账(块仍存活);overflowAlloc已随
                // InsertOverflowLocked自增,totalAlloc不重复(原分配已计数)
                g_totalFreedCount.fetch_sub(1, std::memory_order_relaxed);
                g_totalFreedBytes.fetch_sub(rec.size, std::memory_order_relaxed);
            }
            // 转溢出失败: bit2已置位(记账停止),块自此不可见(尽力而为)
            if (rec.owner != nullptr)
            {
                rec.owner->refs.fetch_sub(1, std::memory_order_relaxed);  // 离开栈归因,dispose转移引用
            }
        }
        else
        {
            if (rec.owner != nullptr)
            {
                // 回插成功: 引用归块(捕获时已转移,不增不减);重增liveBytes
                rec.owner->liveBytes.fetch_add(static_cast<int64_t>(rec.size), std::memory_order_relaxed);
            }
            g_totalFreedCount.fetch_sub(1, std::memory_order_relaxed);
            g_totalFreedBytes.fetch_sub(rec.size, std::memory_order_relaxed);
        }
        pthread_mutex_unlock(&shard.mtx);
        return;
    }
    // 有界重试耗尽: 静默跳过(块自此不可见,尽力而为,不置截断标注)
    if (rec.owner != nullptr)
    {
        rec.owner->refs.fetch_sub(1, std::memory_order_relaxed);  // 块不可见,dispose转移引用
    }
}

// 溢出账本回插(realloc失败恢复,源为溢出账本kOverflow): 撤销CaptureAndRemoveBlock
// 已做的逆向修正(回扣g_overflowFreed*+g_totalFreed*),addr→size按原值重新入set。
// set刚移除该addr,容量必有余量;rehash OOM理论可能→try/catch静默(块自此不可见,
// 尽力而为)。trylock耗尽静默跳过(同ReinsertBlock,不置截断)
void ReinsertOverflowBlock(uint64_t addr, uint64_t size)
{
    BlockShard& shard = g_blockShards[BlockShardIndex(addr)];
    for (int spin = 0; spin < BLOCK_LOCK_SPINS; ++spin)
    {
        if (pthread_mutex_trylock(&shard.mtx) != 0)
        {
            CpuRelax();
            continue;
        }
        try
        {
            shard.overflow[addr] = size;  // 刚移除,必能插入;operator[]覆盖同名
        }
        catch (...)
        {
            // rehash OOM: 块自此不可见(尽力而为)
        }
        g_overflowFreedCount.fetch_sub(1, std::memory_order_relaxed);
        g_overflowFreedBytes.fetch_sub(size, std::memory_order_relaxed);
        g_totalFreedCount.fetch_sub(1, std::memory_order_relaxed);
        g_totalFreedBytes.fetch_sub(size, std::memory_order_relaxed);
        pthread_mutex_unlock(&shard.mtx);
        return;
    }
    // 有界重试耗尽: 静默跳过(块自此不可见,尽力而为,不置截断标注)
}

// 块大小→排布桶下标: g_sizeBucketBounds为各桶下界(升序),桶数=bounds+1;
// 末桶[最后下界, UINT64_MAX)。线性扫描(bounds数≤16,块遍历的常数开销)。
// 记账主流程与闭窗聚合共用(RecordFree开窗前free归桶/CloseAggregate未释放归桶)
inline size_t SizeBucketIndex(uint64_t size)
{
    size_t i = 0;
    while (i < g_sizeBucketBounds.size() && g_sizeBucketBounds[i] <= size)
    {
        ++i;
    }
    return i;
}

// =============================================================================
// 记账主流程(各劫持入口共用)
// =============================================================================

// malloc族通用记账: 截断门→阈值过滤(计数)→采样门→采栈登记→块表插入
// 返回true=已记账(块表有条目或已转溢出账本);false=未记账(块对钩子不可见,
// 后续free静默或落入开窗前通道)——仅截断/阈值过滤/采样跳过/两账本均满四条路径;
// 栈层失败不丢块(转未知桶stackId=0照常记账)
bool RecordMalloc(uint64_t addr, size_t size)
{
    if (g_truncated.load(std::memory_order_relaxed) & 4u)
    {
        return false;  // 记账停止(块表与溢出账本均满,bit2)
    }
    const uint64_t threshold = g_blockThreshold.load(std::memory_order_relaxed);
    if (size < threshold)
    {
        // 小于阈值不采集(热路径开销优化,窗口与阈值由get_params快照)
        g_untrackedCount.fetch_add(1, std::memory_order_relaxed);
        g_untrackedBytes.fetch_add(static_cast<uint64_t>(size), std::memory_order_relaxed);
        return false;
    }
    if (g_sampleRate.load(std::memory_order_relaxed) > 1 && !SampleGateHit())
    {
        return false;  // 显式采样: 被跳过块对钩子完全不可见
    }
    const uint64_t ts = NowNs();

    // 栈阶段: 去重键一次展开到位(浅层哈希无法查找深层PC数组键的map,不做分层)
    uintptr_t keyPc[STACK_KEY_FRAMES];
    int n = CaptureFrames(keyPc, STACK_KEY_FRAMES);
    StackEntry* owner = nullptr;
    if (n > 0)
    {
        // 键=捕获帧的前K帧(CaptureFrames内部截断到maxOut=STACK_KEY_FRAMES)。
        // K=20账本键的职责与键深取舍见常量注释
        StackKey key{};
        key.count = static_cast<uint32_t>(n);
        memcpy(key.pc, keyPc, static_cast<size_t>(n) * sizeof(uintptr_t));
        key.hash = XxHash64Words(key.pc, static_cast<size_t>(n));  // 全生命周期仅此一次
        owner = LookupOrRegisterStack(key);
        // owner==nullptr(栈表trylock失败/表满/OOM): 块转
        // 未知桶(stackId=0)照常进块表,只损失归因粒度
    }

    // 块阶段(全部记账在InsertBlock的块表锁临界区内完成,见InsertBlock契约)
    const BlockInsertResult r = InsertBlock(addr, static_cast<uint64_t>(size), ts, owner);
    if (owner != nullptr)
    {
        // dispose在途lookup引用(恰一次,覆盖kTable/kOverflow/kFailed):
        // kTable时InsertBlock已+1块引用,dispose后净+1归块;其余路径无块引用,
        // dispose归零在途——条目不被无块路径污染(引用契约)
        owner->refs.fetch_sub(1, std::memory_order_relaxed);
    }
    if (r == BlockInsertResult::kFailed)
    {
        // 瞬时竞争(静默跳过)/溢出账本亦满(bit2已在InsertBlock内置位): 不在此处
        // 再置bit0——bit0现为块表触顶的信息标注,记录不停止
        return false;
    }
    if (owner == nullptr && r == BlockInsertResult::kTable)
    {
        // 未归因累计(跨窗口,诊断导出;闭窗打印取窗口内增量)——块落入未知桶
        // (stackId=0)照常记账,unknownAlloc计数已在InsertBlock临界区内累加;
        // 溢出通道块(kOverflow)无栈归因但不属未知桶,不计入未归因
        g_unattributedCount.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

// 开窗前free独立通道记账(free/realloc的kMiss共用): 窗口外分配或记账被跳过的
// 块被释放时,大小经malloc_usable_size近似(RealUsableSize,解析失败为0入0桶),
// 不并入totalFreed(窗口外分配不入账本,并入会破坏"申请=释放+未释放"不变量)
void RecordPreWindowFree(uint64_t addr)
{
    const uint64_t size = RealUsableSize(reinterpret_cast<void*>(addr));
    const size_t bi = SizeBucketIndex(size);
    g_preWindowFreeCount.fetch_add(1, std::memory_order_relaxed);
    g_preWindowFreeBytes.fetch_add(size, std::memory_order_relaxed);
    g_preWindowDistCount[bi].fetch_add(1, std::memory_order_relaxed);
    g_preWindowDistBytes[bi].fetch_add(size, std::memory_order_relaxed);
}

// free族通用记账: 块表命中→删除+释放记账(liveBytes减+totalFreed自增,均在
// CaptureAndRemoveBlock的块表锁临界区内完成)+dispose块引用(捕获已转移给rec,
// 块已释放,终态恰一次);溢出账本命中→逆向修正
// (overflowFreed+totalFreed自增,同一锁内,无owner无dispose);两账本均未命中→
// 开窗前free独立通道
// (次数/总量/单块大小分布,malloc_usable_size近似,不并入totalFreed);
// 自旋耗尽(kLockFailed)→静默跳过(块可能在表但未取到锁,不作开窗前统计)
void RecordFree(uint64_t addr)
{
    BlockEntry rec{};
    const BlockRemoveResult r = CaptureAndRemoveBlock(addr, rec);
    if (r == BlockRemoveResult::kBlock)
    {
        if (rec.owner != nullptr)
        {
            rec.owner->refs.fetch_sub(1, std::memory_order_relaxed);  // dispose块引用(块已释放)
        }
        return;  // 释放记账已在锁内完成
    }
    if (r == BlockRemoveResult::kOverflow)
    {
        return;  // 逆向修正已在锁内完成(无owner,无dispose)
    }
    if (r == BlockRemoveResult::kLockFailed)
    {
        return;  // 有界自旋耗尽: 静默跳过(条目残留,假泄漏风险有界且极罕见)
    }
    // kMiss: 开窗前分配/记账被跳过→独立通道(见RecordPreWindowFree)
    RecordPreWindowFree(addr);
}

// =============================================================================
// 符号化(冷路径:每栈一次,闭窗聚合/预热线程分时执行)
// =============================================================================

char* g_symBuf = nullptr;  // 符号化缓冲(冷路径独占,懒分配,进程内复用)
size_t g_symBufCap = 0;

bool EnsureSymBuf(size_t need)
{
    if (g_symBuf != nullptr && g_symBufCap >= need)
    {
        return true;
    }
    size_t cap = g_symBufCap == 0 ? 64 * 1024 : g_symBufCap;
    while (cap < need)
    {
        cap *= 2;
    }
    char* np = static_cast<char*>(RealMalloc(cap));
    if (np == nullptr)
    {
        return false;
    }
    RealFree(g_symBuf);
    g_symBuf = np;
    g_symBufCap = cap;
    return true;
}

// 帧文本缓存(冷路径独占,与g_symBuf同协议无锁):不同栈的公共链帧高度重叠
// (AI场景底软公共链可达20~30帧),洪峰符号化命中率极高。仅缓存完整未截断帧文本,
// 满容量后停止插入(首批公共链帧已热,后续新帧回落直查)。节点分配走
// RealMallocAllocator,串体内部堆分配经冷路径抑制守卫转真函数
using SymCacheMap = std::unordered_map<uintptr_t, std::string, std::hash<uintptr_t>, std::equal_to<uintptr_t>,
                                       RealMallocAllocator<std::pair<const uintptr_t, std::string>>>;
SymCacheMap g_symCache;

// 单帧符号化(backtrace_symbols风格),返回写入长度(与snprintf同语义:返回期望
// 长度,实际写入受cap截断)
int FormatFrame(char* dst, size_t cap, uintptr_t pc)
{
    auto hit = g_symCache.find(pc);
    if (hit != g_symCache.end())
    {
        const size_t n = hit->second.size();
        if (cap > 0 && dst != nullptr)
        {
            const size_t cpy = n < cap - 1 ? n : cap - 1;
            memcpy(dst, hit->second.data(), cpy);
            dst[cpy] = '\0';
        }
        return static_cast<int>(n);
    }
    // 退出期符号化禁用dladdr:dladdr内部持glibc全局dl_load_write_lock,退出期可
    // 被应用线程dlopen/dlclose永久持有(曾致预热线程卡死、进程退出挂起),且dladdr
    // 为阻塞调用无法超时。一律改走无锁模块快照(RefreshExecRanges维护:模块名+
    // 加载基址),快照外pc输出纯地址。代价:符号名缺失(仅模块名+模块内偏移),
    // 报告保留pc可离线addr2line补全
    const ExecRangeSnapshot& snap = g_execSnaps[g_execSnapPub.load(std::memory_order_acquire)];
    int w = 0;
    if (snap.count != 0)
    {
        const ExecSpan* span = FindExecSpan(pc, snap);
        if (span != nullptr)
        {
            const unsigned long modOff = static_cast<unsigned long>(pc - span->base);
            w = snprintf(dst, cap, "%s(+0x%lx) [0x%lx]\n", span->name, modOff, static_cast<unsigned long>(pc));
        }
    }
    if (w <= 0)
    {
        w = snprintf(dst, cap, "?? [0x%lx]\n", static_cast<unsigned long>(pc));
    }
    // 快照帧不写缓存:缓存只存预热产物的符号帧(mod(func+0xoff))。若快照帧
    // (mod(+0xoff))入缓存,预热以find miss判定"未处理"会被污染——该帧永久停留
    // 偏移格式,且跨窗口污染下个窗口的预热。不写则未预热帧每轮可重试,预热覆盖到
    // 后升级为符号;closing期共享帧的重复快照查询成本~几十ns,远低于dladdr
    return w;
}

// PC数组→引号包裹'\n'分隔帧串,写入g_symBuf并返回长度(不含NUL)。调用方须先
// EnsureSymBuf(pcCount×kFrameReserve+16)。仅CloseAggregate闭窗符号化使用。
// 不变式:帧文本永不侵占缓冲末尾2字节——
// 每帧可用空间为cap-off-2(snprintf的cap参数传room+1,其NUL落在预留区外),截断
// 时off按实际写入量min(w,room)收敛,循环出口off≤cap-2,闭引号与NUL必然写入界内。
// 勿按snprintf期望长度累加off:超长模块/符号名截断时off越过cap,串无NUL终止且
// len虚大,下游读越界
constexpr size_t kFrameReserve = 128;  // 每帧预留缓冲(模块名+符号+地址)

size_t BuildFrameDesc(const uintptr_t* pcs, uint32_t pcCount)
{
    size_t off = 0;
    g_symBuf[off++] = '"';
    for (uint32_t i = 0; i < pcCount; ++i)
    {
        if (off + kFrameReserve + 2 >= g_symBufCap)
        {
            break;  // 剩余不足一帧预算:提前停(宁可少帧,不逐字节逼近尾部)
        }
        const size_t room = g_symBufCap - off - 2;  // 帧文本可用字节(给'"'+NUL留位)
        int w = FormatFrame(g_symBuf + off, room + 1, pcs[i]);
        if (w <= 0)
        {
            break;
        }
        const size_t written = static_cast<size_t>(w) < room ? static_cast<size_t>(w) : room;
        off += written;
        if (static_cast<size_t>(w) > room)
        {
            break;  // 帧被截断:后续帧不再写
        }
    }
    g_symBuf[off++] = '"';
    g_symBuf[off] = '\0';
    return off;
}

// =============================================================================
// 符号化预热线程: 1s节拍top-K dladdr采样入共享帧缓存(无环无
// 事件,本线程唯一职责是稳态dladdr预热;闭窗时先置g_warmupStop再join)
// =============================================================================

// 单帧dladdr符号预热:命中缓存跳过;未命中解析入g_symCache(格式与closing期快照
// 输出同构"mod(func+0xoff) [0xpc]",报告/离线addr2line口径一致)。dladdr仅此函数
// 调用——稳态执行,closing期FormatFrame纯缓存命中,dl_load_write_lock绝不进入
// closing路径(退出期卡死根因防护)。解析失败(罕见)静默跳过,留给closing期快照
void WarmUpFrame(uintptr_t pc)
{
    // 退出/闭窗检查: 闭窗先置g_warmupStop再join,此检查兜底覆盖join等待
    // 窗口——置位后不再进入dladdr(退出期dl_load_write_lock卡死根因防护)
    if (g_closing.load(std::memory_order_acquire) || g_exiting.load(std::memory_order_relaxed))
    {
        return;
    }
    if (g_symCache.find(pc) != g_symCache.end())
    {
        return;
    }
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(pc), &info) == 0 || info.dli_sname == nullptr)
    {
        return;  // 无符号信息:由closing期快照补模块名
    }
    char buf[256];
    int w = 0;
    const char* mod = nullptr;
    if (info.dli_fname != nullptr)
    {
        const char* slash = strrchr(info.dli_fname, '/');
        mod = slash != nullptr ? slash + 1 : info.dli_fname;
    }
    const unsigned long symOff = static_cast<unsigned long>(pc - reinterpret_cast<uintptr_t>(info.dli_saddr));
    if (mod != nullptr)
    {
        w = snprintf(buf, sizeof(buf), "%s(%s+0x%lx) [0x%lx]\n", mod, info.dli_sname, symOff,
                     static_cast<unsigned long>(pc));
    }
    else
    {
        w = snprintf(buf, sizeof(buf), "%s(+0x%lx) [0x%lx]\n", info.dli_sname, symOff, static_cast<unsigned long>(pc));
    }
    if (w > 0 && static_cast<size_t>(w) < sizeof(buf) && g_symCache.size() < SYM_CACHE_MAX_ENTRIES)
    {
        try
        {
            g_symCache.emplace(pc, std::string(buf, static_cast<size_t>(w)));
        }
        catch (...)
        {
            // 缓存分配失败(bad_alloc):本次不入缓存,下轮自动重试
        }
    }
}

// 稳态top泄漏点符号采样(仅预热线程调用;closing/exiting/缓存满即停):
// 每节拍(TOP_LEAK_SAMPLE_INTERVAL_NS=1s)扫描全部分片(trylock,失败分片让位下轮),
// 锁内按liveBytes(存活字节=泄漏量真相源,与闭窗报告unfreedBytes同构)取每分片
// top-TOP_LEAK_CANDS_PER_SHARD候选,全局合并取top-TOP_LEAK_SYMBOLIZE_K,锁外逐个
// dladdr解析全深度帧PC入g_symCache(共享帧缓存去重:公共链帧只解析一次,每栈独立
// 帧≈2.5个实测),解析完成后按分片回锁置位入选者warmedUp。
// 优先级语义:liveBytes与闭窗报告排序键(unfreedBytes)同构——泄漏量大的栈稳态
// 即可知,无需等闭窗。字节口径修正旧"块数代理"排序错位(见常量注释):大块单发
// 泄漏不再被冷落。候选限定liveBytes>0(天然只在活跃栈中选)+未预热(warmedUp跳过,
// 其帧已入缓存,闭窗FormatFrame直接命中);缓存满/全部预热后自停,排序只影响
// 先后不影响最终覆盖(缓存上限见SYM_CACHE_MAX_ENTRIES)。
// 与闭窗关系:闭窗FormatFrame纯缓存命中(退出期卡死根因防护不变)——本采样器是
// 函数名唯一来源;采样已覆盖的栈闭窗直出符号,未覆盖栈走模块快照兜底+离线
// addr2line。
// 安全性:
// ①与1Hz RefreshExecRanges同dl锁暴露类——稳态期dl_load_write_lock仅瞬时持有,
//   且本路径与closing补扫时间上不重叠(closing置位立即停止),closing期FormatFrame
//   纯缓存命中,dladdr永不进入closing路径(退出期卡死根因防护);
// ②候选为liveBytes>0的存活栈条目:map遍历持分片trylock,失败即让位;liveBytes锁内
//   读(锁内变更,锁内读一致)。入选者(每分片top-32)锁内各取预热引用(refs+1,与
//   淘汰判读refs==1同锁互斥):跨锁符号化期间refs>=2条目不可被表满淘汰、fullPcs
//   不可被释放——消除"收集后块全释放→refs回落1→EvictDeadStackLocked释放fullPcs
//   →解析UAF"的TOCTOU窗口(死栈回收引入后该窗口不再是微秒级偶然,而是表满洪峰
//   下可复现的悬垂);解析+回锁置位完成后统一dispose(恰一次),条目恢复可淘汰。
// ③g_symCache仍为冷路径独占(预热线程稳态写入与closing聚合分时运行,无锁)。
// 采样是尽力而为:末次采样后新登记栈/极深栈深帧显示偏移,报告pc可离线addr2line
// 补全
void SampleTopLeaks()
{
    if (g_symCache.size() >= SYM_CACHE_MAX_ENTRIES || g_closing.load(std::memory_order_acquire) ||
        g_exiting.load(std::memory_order_relaxed))
    {
        return;
    }
    // 首次调用预留全部桶(1M条≈8MB桶数组):高频插入下免反复rehash。
    // 预留失败(bad_alloc: 桶数请求被破坏如曾现负桶数,或宿主真OOM)绝不穿出
    // 预热线程杀宿主——吞掉降级:不预留,后续emplace按需rehash(其自带try/catch)
    static bool sCacheReserved = false;
    if (!sCacheReserved)
    {
        try
        {
            g_symCache.reserve(SYM_CACHE_MAX_ENTRIES);
        }
        catch (...)
        {
        }
        sCacheReserved = true;  // 成败均只试一次: 失败后由emplace按需rehash兜底
    }
    // 收集: 每分片top-TOP_LEAK_CANDS_PER_SHARD(liveBytes降序,插入排序维护,锁内
    // 无分配);trylock失败分片让位下轮(生产者在锁内,零等待)。候选并入全局数组
    // (64×32=2048,预热线程栈上~32KB,默认线程栈内可承受)。map遍历=全条目扫描,
    // 覆盖所有liveBytes>0条目(含未挂链的登记态),不依赖pending链形态
    constexpr size_t kMaxCands = STACK_SHARDS * TOP_LEAK_CANDS_PER_SHARD;
    StackEntry* cands[kMaxCands];
    uint64_t candBytes[kMaxCands];
    size_t nCand = 0;
    for (size_t s = 0; s < STACK_SHARDS; ++s)
    {
        StackShard& shard = g_stackShards[s];
        if (pthread_mutex_trylock(&shard.mtx) != 0)
        {
            continue;  // 生产者在锁内:让位,下轮再试
        }
        StackEntry* local[TOP_LEAK_CANDS_PER_SHARD];
        uint64_t localBytes[TOP_LEAK_CANDS_PER_SHARD];
        size_t ln = 0;
        for (auto& kv : shard.map)
        {
            StackEntry& e = kv.second;
            if (e.liveBytes.load(std::memory_order_relaxed) == 0 || e.warmedUp || e.fullPcs == nullptr ||
                e.fullCount == 0)
            {
                continue;
            }
            const uint64_t b = e.liveBytes.load(std::memory_order_relaxed);
            size_t pos = ln < TOP_LEAK_CANDS_PER_SHARD ? ln : TOP_LEAK_CANDS_PER_SHARD;
            while (pos > 0 && localBytes[pos - 1] < b)
            {
                if (pos < TOP_LEAK_CANDS_PER_SHARD)
                {
                    local[pos] = local[pos - 1];
                    localBytes[pos] = localBytes[pos - 1];
                }
                --pos;
            }
            if (pos < TOP_LEAK_CANDS_PER_SHARD)
            {
                local[pos] = &e;
                localBytes[pos] = b;
                if (ln < TOP_LEAK_CANDS_PER_SHARD)
                {
                    ++ln;
                }
            }
        }
        // 入选者(本分片top-32)锁内各取一预热引用:候选收集时liveBytes>0仅保证此刻
        // refs>=2,跨锁符号化期间其块可能全部释放(refs回落1)被表满淘汰(EvictDead
        // StackLocked判读refs==1)释放fullPcs——锁内取ref钉住条目(淘汰与取ref同锁
        // 互斥),解析+置位完成后统一dispose,消除悬垂窗口
        for (size_t i = 0; i < ln; ++i)
        {
            local[i]->refs.fetch_add(1, std::memory_order_relaxed);
        }
        pthread_mutex_unlock(&shard.mtx);
        for (size_t i = 0; i < ln && nCand < kMaxCands; ++i)
        {
            cands[nCand] = local[i];
            candBytes[nCand] = localBytes[i];
            ++nCand;
        }
    }
    // 全局top-TOP_LEAK_SYMBOLIZE_K(简单选择排序,K×N≈50万次比较,微秒级)
    const size_t k = nCand < TOP_LEAK_SYMBOLIZE_K ? nCand : TOP_LEAK_SYMBOLIZE_K;
    for (size_t i = 0; i < k; ++i)
    {
        size_t best = i;
        for (size_t j = i + 1; j < nCand; ++j)
        {
            if (candBytes[j] > candBytes[best])
            {
                best = j;
            }
        }
        StackEntry* te = cands[i];
        cands[i] = cands[best];
        cands[best] = te;
        const uint64_t tb = candBytes[i];
        candBytes[i] = candBytes[best];
        candBytes[best] = tb;
    }
    // 锁外符号化top-K(指针有效性见函数头②):每栈全深度帧,帧级缓存去重
    for (size_t i = 0; i < k; ++i)
    {
        StackEntry* entry = cands[i];
        const uintptr_t* pcs = entry->fullPcs;
        if (pcs == nullptr)
        {
            continue;
        }
        const uint32_t cnt = entry->fullCount;
        for (uint32_t f = 0; f < cnt; ++f)
        {
            WarmUpFrame(pcs[f]);
        }
    }
    // 解析完成后回锁置位入选者——切勿把置位提前到收集时:本轮每分片top-32候选
    // 中只有全局top-256真正被解析,若收集时即置位,未入选全局top-K者被误标记
    // "已预热"而永久失去解析机会。解析后置位:未入选者不置位,下轮可重新竞选;
    // 解析失败仍置位——尽力而为,失败帧pc保留,报告可离线addr2line补全。按分片
    // 分组回锁(每分片至多一把锁内批量置位);trylock失败=生产者在锁内,放弃置位:
    // 下轮该栈重新入选,WarmUpFrame缓存命中即跳过,仅多占一次候选名额,无害
    for (size_t s = 0; s < STACK_SHARDS; ++s)
    {
        if (pthread_mutex_trylock(&g_stackShards[s].mtx) != 0)
        {
            continue;
        }
        for (size_t i = 0; i < k; ++i)
        {
            StackEntry* entry = cands[i];
            if (entry != nullptr && ShardOfStackId(entry->stackId) == s)
            {
                entry->warmedUp = true;
            }
        }
        pthread_mutex_unlock(&g_stackShards[s].mtx);
    }
    // 释放全部预热引用(恰一次dispose): 解析与置位均已结束,条目恢复可淘汰状态;
    // 此后不再触碰其内存——表满淘汰可正常回收该栈
    for (size_t i = 0; i < nCand; ++i)
    {
        cands[i]->refs.fetch_sub(1, std::memory_order_relaxed);
    }
}

// 采样节拍门(仅预热线程调用):距上次采样≥TOP_LEAK_SAMPLE_INTERVAL_NS才执行一次
// 全量top-K采样,间隔内单次比较即返。预热线程主循环每1s调用一次(见
// WarmupThreadMain),closing/exiting/缓存满由SampleTopLeaks内部立即返回兜底
void MaybeSampleTopLeaks()
{
    static uint64_t sLastSampleNs = 0;
    const uint64_t now = MonotonicNs();
    if (now - sLastSampleNs < TOP_LEAK_SAMPLE_INTERVAL_NS)
    {
        return;
    }
    sLastSampleNs = now;
    SampleTopLeaks();
}

// 符号化预热线程主体: 1s节拍——①1Hz刷新模块可执行段快照(dl锁暴露类
// 稳态瞬时,closing后join不再执行);②MaybeSampleTopLeaks采样节拍(top-K dladdr
// 预热入g_symCache)。无环无事件,不存在排空;闭窗由SvcSetEnabled调用线程同步
// 完成(停本线程→CloseAggregate→STAGE_END),本线程不参与闭窗。
void* WarmupThreadMain(void*)
{
    // 线程生命周期全程抑制(场景C): 预热线程内部分配(g_symCache容器增长/dladdr
    // 内部)经PLT回落本钩子→守卫拦截,不落记账
    HookSuppressGuard guard;
    pthread_setname_np(pthread_self(), "msmemscope-warm");
    uint64_t lastSnapNs = 0;  // 模块区间快照1Hz刷新节拍(仅本线程读写,无共享)
    while (!g_warmupStop.load(std::memory_order_acquire))
    {
        try
        {
            const uint64_t nowNs = MonotonicNs();
            // 1Hz模块快照刷新: 新dlopen模块的代码帧进入快照后其调用点才可走
            // FP快路(快照外帧pc校验不通过,自动回退backtrace)
            if (g_enabled.load(std::memory_order_relaxed) && nowNs - lastSnapNs >= 1000000000ull)
            {
                RefreshExecRanges();
                lastSnapNs = nowNs;
            }
            MaybeSampleTopLeaks();
        }
        catch (...)
        {
            // 兜底(绝不trap杀宿主): 预热线程内任何异常(dladdr内部/容器分配等)
            // 一律吞掉降级,下轮节拍继续——线程函数逃逸异常即std::terminate杀宿主
        }
        usleep(TOP_LEAK_SAMPLE_INTERVAL_NS / 1000);  // 1s节拍(函数内节流门兜底)
    }
    return nullptr;
}

// 预热线程创建(开窗时调用): 首次创建后常驻到StopWarmupThread;幂等(已创建直接
// 返回)。失败(线程资源耗尽)仅记日志——预热是符号质量的尽力而为,缺线程时闭窗
// 格式化作模块快照兜底,功能不受损
bool EnsureWarmupThread()
{
    if (g_warmupThreadCreated.load(std::memory_order_relaxed))
    {
        return true;
    }
    pthread_t tid;
    if (pthread_create(&tid, nullptr, WarmupThreadMain, nullptr) != 0)
    {
        return false;
    }
    g_warmupThread = tid;
    g_warmupThreadCreated.store(true, std::memory_order_release);
    return true;
}

// 预热线程停止(闭窗第一步,CloseAggregate之前): 置g_warmupStop→join。join双重
// 作用: ①g_symCache/g_symBuf由预热线程独占写入,join后闭窗聚合(同一调用线程)
// 读取无并发,无锁共享安全;②join兼作沉降期——g_enabled已在先置false,in-flight
// free在关闸后自然drain,闭窗聚合不撕裂计数
void StopWarmupThread()
{
    if (!g_warmupThreadCreated.load(std::memory_order_relaxed))
    {
        return;
    }
    g_warmupStop.store(true, std::memory_order_release);
    pthread_join(g_warmupThread, nullptr);
}

// 清表(开窗时调用,窗口关闭态无生产者): 栈表连待符号化缓冲一并释放,块表双数组
// 整体释放,全部窗口计数器归零,闭窗快照产物清理(跨窗口不保留,防陈旧数据被新
// 窗口的dump_*误读)。g_unattributedCount累计不清零(跨窗口差分),仅快照基线
void ClearTables()
{
    for (auto& shard : g_stackShards)
    {
        pthread_mutex_lock(&shard.mtx);
        for (auto& kv : shard.map)
        {
            RealFree(kv.second.fullPcs);
            kv.second.fullPcs = nullptr;
        }
        shard.map.clear();
        pthread_mutex_unlock(&shard.mtx);
    }
    for (auto& shard : g_blockShards)
    {
        pthread_mutex_lock(&shard.mtx);
        // 开放寻址双数组整体释放归零(窗口关闭态无生产者):跨窗口不保留高水位容量,
        // 空闲期零驻留;下窗口按需懒分配再增长
        RealFree(shard.keys);
        RealFree(shard.vals);
        shard.keys = nullptr;
        shard.vals = nullptr;
        shard.capMask = 0;
        shard.count = 0;
        shard.overflow.clear();  // 溢出账本随块表清空(窗口生命周期=块表,节点经RealMallocAllocator)
        pthread_mutex_unlock(&shard.mtx);
    }
    g_stackCount.store(0, std::memory_order_relaxed);
    g_blockCount.store(0, std::memory_order_relaxed);
    // 窗口计数器归零(本窗口统计量,闭窗后由CloseAggregate冻结进g_closeSnapshot)
    g_truncated.store(0, std::memory_order_relaxed);
    g_untrackedCount.store(0, std::memory_order_relaxed);
    g_untrackedBytes.store(0, std::memory_order_relaxed);
    g_unknownAllocCount.store(0, std::memory_order_relaxed);
    g_unknownAllocBytes.store(0, std::memory_order_relaxed);
    g_evictedStackCount.store(0, std::memory_order_relaxed);
    g_evictedAllocCount.store(0, std::memory_order_relaxed);
    g_evictedAllocBytes.store(0, std::memory_order_relaxed);
    g_totalAllocCount.store(0, std::memory_order_relaxed);
    g_totalAllocBytes.store(0, std::memory_order_relaxed);
    g_totalFreedCount.store(0, std::memory_order_relaxed);
    g_totalFreedBytes.store(0, std::memory_order_relaxed);
    // 溢出通道计数器归零(窗口统计量,已并入total*口径,见g_overflow*注释)
    g_overflowAllocCount.store(0, std::memory_order_relaxed);
    g_overflowAllocBytes.store(0, std::memory_order_relaxed);
    g_overflowFreedCount.store(0, std::memory_order_relaxed);
    g_overflowFreedBytes.store(0, std::memory_order_relaxed);
    // 开窗前free通道归零(次数/总量/分布桶原子向量逐桶清零;向量定容见HostMemHookInit)
    g_preWindowFreeCount.store(0, std::memory_order_relaxed);
    g_preWindowFreeBytes.store(0, std::memory_order_relaxed);
    for (auto& c : g_preWindowDistCount)
    {
        c.store(0, std::memory_order_relaxed);
    }
    for (auto& b : g_preWindowDistBytes)
    {
        b.store(0, std::memory_order_relaxed);
    }
    // 未归因窗口基线快照(计数本身累计不清零,分析器差分用;闭窗打印取窗口内增量)
    g_unattrWindowBase.store(g_unattributedCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    // 闭窗快照产物清理(跨窗口不保留):g_closeSnapshot.valid=false使get_stats回落
    // 实时计数器,防新窗口未闭窗时误读旧窗口冻结值
    g_closeStats.clear();
    g_closeSizeDist.clear();
    g_closePreWindowDist.clear();
    g_closeSnapshot = CloseSnapshot{};
}

// =============================================================================
// 闭窗聚合(set_enabled(false)调用线程同步执行;生产者已冻结):
// 步骤0 大小排布桶预置(g_sizeBucketBounds→g_closeSizeDist,末桶rangeHigh=UINT64_MAX);
// 步骤1 单次块表遍历: 聚合per-stack未释放(块表是未释放的唯一真源)+大小排布桶
//       +存活块/字节合计;owner==nullptr→未知桶(栈层失败块,与g_unknownAllocCount互证);
// 步骤2 栈表遍历: 读取原子申请计数(allocCount/allocBytes),未释放取步骤1聚合值,
//       释放=申请-未释放派生(不变量"申请=释放+未释放"精确成立,无独立释放计数);
//       捕获entry指针与行位置(不线性扫描map再查)——闭窗符号化仅对unfreed>0执行;
// 步骤3 未知桶行(stackId=0): 申请计数=unknown桶计数,未释放=步骤1未知桶聚合;
// 步骤4 仅对unfreed>0的栈符号化frameDesc(EnsureSymBuf失败→文本缺失,统计值优先);
//       排序: unfreedBytes降序, stackId升序(报告可读性与确定性);
// 步骤5 冻结合计进g_closeSnapshot(valid=true),get_stats此后返回冻结值。
// 截断标注: 块表触顶(bit0)时遍历数据=截断点前完整前缀,快照如实反映;
// 分片锁获取失败(skip)同样置bit0/bit1(数据降级,诚实标注)
// =============================================================================

// 闭窗分片锁获取: 生产者已冻结(g_enabled=false),锁应即时空闲;trylock+有界重试
// (纯CAS,与时钟无关——不用timedlock,防时钟回拨把截止推远),耗尽仍失败则调用方
// 跳过该分片并置截断标注(诚实降级)。重试间隔短退避(usleep基于CLOCK_MONOTONIC,
// 与时钟回拨无关)
constexpr int CLOSE_LOCK_RETRIES = 256;
bool CloseTryLockShard(pthread_mutex_t& mtx)
{
    for (int i = 0; i < CLOSE_LOCK_RETRIES; ++i)
    {
        if (pthread_mutex_trylock(&mtx) == 0)
        {
            return true;
        }
        CpuRelax();
        usleep(200);  // 200μs×256≈50ms封顶
    }
    return false;
}

// 步骤1的per-stack未释放聚合(块表遍历真源;栈表遍历消费)
struct UnfreedAgg
{
    uint64_t count = 0;
    uint64_t bytes = 0;
    uint64_t maxBlockSize = 0;
};

void CloseAggregate()
{
    // 步骤0: 大小排布桶预置(bounds为各桶下界,末桶上界UINT64_MAX)
    g_closeSizeDist.clear();
    const size_t bucketCount = g_sizeBucketBounds.size() + 1;
    g_closeSizeDist.reserve(bucketCount);
    for (size_t i = 0; i < bucketCount; ++i)
    {
        SizeBucket b{};
        b.rangeLow = i == 0 ? 0 : g_sizeBucketBounds[i - 1];
        b.rangeHigh = i + 1 < bucketCount ? g_sizeBucketBounds[i] : UINT64_MAX;
        g_closeSizeDist.push_back(b);
    }

    // 步骤1: 单次块表遍历(per-stack未释放聚合+大小桶+存活块/字节合计)+同锁遍历
    // 溢出账本(块表满降级转出): 溢出块计入大小排布桶(未释放内存的
    // 真实排布,含被转出的泄漏块)但不入per-stack聚合(无栈归因)不入liveBlocks
    // (liveBlocks为块表口径,totalFreed派生另减overflowLive,见步骤5)。
    // 分片trylock失败→跳过并置truncated bit2(数据为降级前缀,诚实标注;bit0已转
    // 信息标注不表达数据降级,故改置bit2);
    // 闭窗态无生产者,锁内unordered_map插入/增长经真函数分配(不落记账),无风险
    std::unordered_map<uint64_t, UnfreedAgg, std::hash<uint64_t>, std::equal_to<uint64_t>,
                       RealMallocAllocator<std::pair<const uint64_t, UnfreedAgg>>>
        unfreed;
    unfreed.reserve(g_stackCount.load(std::memory_order_relaxed) + 1);  // 免遍历中rehash
    uint64_t liveBlocks = 0;
    uint64_t liveBytesTotal = 0;
    uint64_t overflowLiveBlocks = 0;  // 溢出账本存活块(大小排布已含,派生用)
    uint64_t overflowLiveBytes = 0;
    for (auto& shard : g_blockShards)
    {
        if (!CloseTryLockShard(shard.mtx))
        {
            g_truncated.fetch_or(4u, std::memory_order_relaxed);  // 块表读取降级:数据为前缀
            continue;
        }
        if (shard.keys != nullptr)
        {
            for (uint32_t i = 0; i <= shard.capMask; ++i)
            {
                if (shard.keys[i] == 0)
                {
                    continue;
                }
                const BlockEntry& v = shard.vals[i];
                const uint64_t stackId = v.owner != nullptr ? v.owner->stackId : 0;
                UnfreedAgg& a = unfreed[stackId];
                a.count += 1;
                a.bytes += v.size;
                if (v.size > a.maxBlockSize)
                {
                    a.maxBlockSize = v.size;
                }
                const size_t bi = SizeBucketIndex(v.size);
                g_closeSizeDist[bi].blockCount += 1;
                g_closeSizeDist[bi].blockBytes += v.size;
                liveBlocks += 1;
                liveBytesTotal += v.size;
            }
        }
        // 溢出账本同锁遍历: 计入大小排布桶(未释放真实排布)+溢出存活合计
        for (const auto& kv : shard.overflow)
        {
            const uint64_t sz = kv.second;
            const size_t bi = SizeBucketIndex(sz);
            g_closeSizeDist[bi].blockCount += 1;
            g_closeSizeDist[bi].blockBytes += sz;
            overflowLiveBlocks += 1;
            overflowLiveBytes += sz;
        }
        pthread_mutex_unlock(&shard.mtx);
    }

    // 步骤2: 栈表遍历。释放=申请-未释放派生;unfreed>0的栈记入liveEntries供符号化
    g_closeStats.clear();
    g_closeStats.reserve(g_stackCount.load(std::memory_order_relaxed) + 1);
    std::vector<std::pair<StackEntry*, size_t>, RealMallocAllocator<std::pair<StackEntry*, size_t>>> liveEntries;
    for (auto& shard : g_stackShards)
    {
        if (!CloseTryLockShard(shard.mtx))
        {
            g_truncated.fetch_or(2u, std::memory_order_relaxed);  // 栈表读取降级:归因不完整
            continue;
        }
        for (auto& kv : shard.map)
        {
            StackEntry& e = kv.second;
            StackStatRow row{};
            row.stackId = e.stackId;
            row.allocCount = e.allocCount.load(std::memory_order_relaxed);
            row.allocBytes = e.allocBytes.load(std::memory_order_relaxed);
            const auto it = unfreed.find(e.stackId);
            if (it != unfreed.end())
            {
                row.unfreedCount = it->second.count;
                row.unfreedBytes = it->second.bytes;
                row.maxBlockSize = it->second.maxBlockSize;
            }
            // 释放=申请-未释放派生(不变量恒等;未释放为0则freed=alloc)
            row.freedCount = row.allocCount - row.unfreedCount;
            row.freedBytes = row.allocBytes - row.unfreedBytes;
            const size_t idx = g_closeStats.size();
            g_closeStats.push_back(std::move(row));
            if (it != unfreed.end())
            {
                liveEntries.emplace_back(&e, idx);
            }
        }
        pthread_mutex_unlock(&shard.mtx);
    }

    // 步骤3: 未知桶行(stackId=0,frameDesc为空): 申请=unknown桶计数,未释放=聚合
    {
        StackStatRow row{};
        row.stackId = 0;
        row.allocCount = g_unknownAllocCount.load(std::memory_order_relaxed);
        row.allocBytes = g_unknownAllocBytes.load(std::memory_order_relaxed);
        const auto it = unfreed.find(0);
        if (it != unfreed.end())
        {
            row.unfreedCount = it->second.count;
            row.unfreedBytes = it->second.bytes;
            row.maxBlockSize = it->second.maxBlockSize;
        }
        row.freedCount = row.allocCount - row.unfreedCount;
        row.freedBytes = row.allocBytes - row.unfreedBytes;
        g_closeStats.push_back(std::move(row));
    }

    // 步骤4: 符号化仅对unfreed>0的栈执行(无未释放块的栈无文本需求,省dladdr)。
    // 帧缓存由预热线程写入且已join(StopWarmupThread),此处读取无并发;未预热帧
    // 走模块快照兜底+离线addr2line。文本缺失(无PC/缓冲不可得/分配失败)不阻断
    // 统计行——统计值优先,报告标注文本缺省
    for (const auto& le : liveEntries)
    {
        StackEntry* e = le.first;
        StackStatRow& row = g_closeStats[le.second];
        if (e->fullPcs == nullptr || e->fullCount == 0)
        {
            continue;
        }
        uint32_t depth = g_stackDepth.load(std::memory_order_relaxed);
        if (depth == 0 || depth > MAX_STACK_DEPTH)
        {
            depth = DEFAULT_STACK_DEPTH;
        }
        if (!EnsureSymBuf(static_cast<size_t>(depth) * kFrameReserve + 16))
        {
            continue;
        }
        const size_t off = BuildFrameDesc(e->fullPcs, e->fullCount);
        try
        {
            row.frameDesc.assign(g_symBuf, off);
        }
        catch (...)
        {
            // std::string分配失败(bad_alloc):文本缺失,统计值优先
        }
    }
    // 排序: unfreedBytes降序, stackId升序(报告可读性与确定性)
    std::sort(g_closeStats.begin(), g_closeStats.end(),
              [](const StackStatRow& a, const StackStatRow& b)
              {
                  if (a.unfreedBytes != b.unfreedBytes)
                  {
                      return a.unfreedBytes > b.unfreedBytes;
                  }
                  return a.stackId < b.stackId;
              });

    // 步骤5: 冻结合计进g_closeSnapshot(valid=true;get_stats此后返回冻结值)。
    // 释放=申请-未释放派生(全局合计同款),与per-stack行口径一致。派生口径:
    // 溢出申请已并入totalAlloc、溢出释放已并入totalFreed,未释放
    // = totalAlloc − 块表存活 − 溢出存活(overflowLive=overflowAlloc−overflowFreed,
    // 即步骤1遍历的溢出账本真值overflowLiveBlocks)——global不变量"申请=释放+未释放"
    // 在含溢出通道的完整账本下精确成立
    g_closeSnapshot.valid = true;
    g_closeSnapshot.liveBlockCount = liveBlocks;
    g_closeSnapshot.totalAllocCount = g_totalAllocCount.load(std::memory_order_relaxed);
    g_closeSnapshot.totalAllocBytes = g_totalAllocBytes.load(std::memory_order_relaxed);
    g_closeSnapshot.totalFreedCount =
        g_totalAllocCount.load(std::memory_order_relaxed) - liveBlocks - overflowLiveBlocks;
    g_closeSnapshot.totalFreedBytes =
        g_totalAllocBytes.load(std::memory_order_relaxed) - liveBytesTotal - overflowLiveBytes;
    g_closeSnapshot.untrackedCount = g_untrackedCount.load(std::memory_order_relaxed);
    g_closeSnapshot.untrackedBytes = g_untrackedBytes.load(std::memory_order_relaxed);
    // 溢出通道快照: alloc/freed为窗口累计转出/逆向修正,存活=alloc-freed
    g_closeSnapshot.overflowAllocCount = g_overflowAllocCount.load(std::memory_order_relaxed);
    g_closeSnapshot.overflowAllocBytes = g_overflowAllocBytes.load(std::memory_order_relaxed);
    g_closeSnapshot.overflowFreedCount = g_overflowFreedCount.load(std::memory_order_relaxed);
    g_closeSnapshot.overflowFreedBytes = g_overflowFreedBytes.load(std::memory_order_relaxed);
    // 开窗前free独立通道快照(窗口外分配)
    g_closeSnapshot.preWindowFreeCount = g_preWindowFreeCount.load(std::memory_order_relaxed);
    g_closeSnapshot.preWindowFreeBytes = g_preWindowFreeBytes.load(std::memory_order_relaxed);
    // 死栈淘汰快照(本窗口被淘汰条目/折叠申请,折叠已并入unknown桶行——未知桶行
    // 的alloc=从未入表+死栈折叠,与g_evicted*互证)
    g_closeSnapshot.evictedStackCount = g_evictedStackCount.load(std::memory_order_relaxed);
    g_closeSnapshot.evictedAllocCount = g_evictedAllocCount.load(std::memory_order_relaxed);
    g_closeSnapshot.evictedAllocBytes = g_evictedAllocBytes.load(std::memory_order_relaxed);
    // 开窗前free大小排布冻结(原子桶向量→桶,与g_closeSizeDist同界;仅窗口关闭态读,
    // 快照隔离新窗口ClearTables逐桶清零)
    g_closePreWindowDist.clear();
    g_closePreWindowDist.reserve(bucketCount);
    // 防御: pre-window原子桶向量仅在HostMemHookInit(构造期)定容,此后无任何运行期
    // 缩容/清空路径(ClearTables只清元素)。此处向量未定容=元数据在运行期被越界写
    // 破坏(BSS损坏,实测:闭窗段错误AtomicU64::load this=0x0)——数据已不可信,跳过
    // 开窗前free分布并告警取证,不阻断闭窗主流程(统计行/大小排布/合计已就绪)。
    // 不在此处resize补齐: 补齐产出假数据(被破坏的全局范围不可知),且掩盖损坏症状
    if (g_preWindowDistCount.size() < bucketCount || g_preWindowDistBytes.size() < bucketCount)
    {
        fprintf(stderr,
                "[msmemscope] hostmem: [pid=%llu] CORRUPTION: pre-window dist vector undersized at "
                "close (count=%zu bytes=%zu expect=%zu) - BSS overwrite suspected, pre-window "
                "distribution dropped\n",
                static_cast<unsigned long long>(getpid()), g_preWindowDistCount.size(), g_preWindowDistBytes.size(),
                bucketCount);
    }
    else
    {
        for (size_t i = 0; i < bucketCount; ++i)
        {
            SizeBucket b{};
            b.rangeLow = i == 0 ? 0 : g_sizeBucketBounds[i - 1];
            b.rangeHigh = i + 1 < bucketCount ? g_sizeBucketBounds[i] : UINT64_MAX;
            b.blockCount = g_preWindowDistCount[i].load(std::memory_order_relaxed);
            b.blockBytes = g_preWindowDistBytes[i].load(std::memory_order_relaxed);
            g_closePreWindowDist.push_back(b);
        }
    }
    g_closeSnapshot.sampleRate = g_sampleRate.load(std::memory_order_relaxed);
    g_closeSnapshot.truncated = g_truncated.load(std::memory_order_relaxed);
}

// =============================================================================
// SVC实现(bind返回的服务表)
// =============================================================================

void SvcSetEnabled(int enabled)
{
    pthread_mutex_lock(&g_svcMtx);
    if (enabled != 0)
    {
        if (!g_ctorDone)
        {
            // 构造未完成(采集库静态初始化先于本so构造,见g_ctorDone注释):暂存
            // 开窗请求,HostMemHookInit末尾按已解析配置补开
            g_openPending = true;
            pthread_mutex_unlock(&g_svcMtx);
            return;
        }
        if (g_enabled.load(std::memory_order_relaxed))
        {
            pthread_mutex_unlock(&g_svcMtx);
            return;  // 幂等: 窗口已开
        }
        if (g_exiting.load(std::memory_order_relaxed))
        {
            pthread_mutex_unlock(&g_svcMtx);
            return;  // 退出期不再开窗(生产者已被g_exiting压住,开了也无事件)
        }
        if (g_forked.load(std::memory_order_acquire))
        {
            pthread_mutex_unlock(&g_svcMtx);
            return;  // fork后代不监控(见g_forked注释)
        }
        // 上一窗口仍在关闭中(闭窗聚合未完成): 等待STAGE_END发出,防END(N)晚于
        // START(N+1)。上界60s(200μs×30万次):大块表闭窗聚合(遍历+符号化)可达
        // 数十秒,1s级上界会在正常慢消化下静默跳过下一次开窗、丢一整窗数据;
        // 每5s(2.5万次)打印等待进度,超时放弃开窗并明示
        for (uint32_t i = 0; i < 300000u && g_closing.load(std::memory_order_acquire); ++i)
        {
            pthread_mutex_unlock(&g_svcMtx);
            usleep(200);
            pthread_mutex_lock(&g_svcMtx);
            if (i % 25000u == 24999u)
            {
                fprintf(stderr,
                        "[msmemscope] hostmem: [pid=%llu] previous window still closing (waiting close aggregate)\n",
                        static_cast<unsigned long long>(getpid()));
            }
        }
        if (g_closing.load(std::memory_order_relaxed))
        {
            fprintf(stderr, "[msmemscope] hostmem: [pid=%llu] previous window still closing after 60s, open skipped\n",
                    static_cast<unsigned long long>(getpid()));
            pthread_mutex_unlock(&g_svcMtx);
            return;
        }

        // 1. 运行参数快照(栈深度/块阈值/采样率,钩子不解析配置文件)
        if (g_api.get_params != nullptr)
        {
            MsmemscopeHostmemParams params{};
            g_api.get_params(&params);
            if (params.stackDepth > 0 && params.stackDepth <= MAX_STACK_DEPTH)
            {
                g_stackDepth.store(params.stackDepth, std::memory_order_relaxed);
            }
            g_blockThreshold.store(params.blockThreshold, std::memory_order_relaxed);
            // 采样率倒数: 0→1归一化(1=不采样);非1值向上规范化到2的幂(门控为
            // 掩码判定,热路径免取模),超过1<<30按1<<30截断
            uint32_t rate = params.sampleRate;
            if (rate == 0)
            {
                rate = 1;
            }
            else if (rate > 1)
            {
                uint32_t pow2 = 1;
                while (pow2 < rate && pow2 < (1u << 30))
                {
                    pow2 <<= 1;
                }
                rate = pow2;
            }
            g_sampleRate.store(rate, std::memory_order_relaxed);
        }

        // 2. 清零块表/栈表/窗口计数器(上一窗口已闭+聚合完成,表冻结无争用)
        ClearTables();

        // 3. 预热线程就绪(首次创建;须先于门控开启,窗口期dladdr预热尽早覆盖)。
        //    失败(线程资源耗尽)仅记日志——预热是符号质量的尽力而为,窗口照常开
        if (!EnsureWarmupThread())
        {
            fprintf(stderr, "[msmemscope] hostmem: [pid=%llu] warmup thread create failed, symbol warmup degraded\n",
                    static_cast<unsigned long long>(getpid()));
        }

        // 4. 模块可执行段快照刷新: FP走栈pc校验依据。窗口间宿主可能已
        //    dlopen新模块(分析库/插件),开窗时刷新一次,此后预热线程1Hz跟进
        RefreshExecRanges();

        // 5. 发STAGE_START(调用线程同步直调,先于任何窗口记账)。护栏:派发内部分配
        //    失败(采集库侧异常)不可穿出——本函数可能运行在宿主.init_array上下文,
        //    未捕获异常即宿主终止;失败时窗口照常开(账本与事件流无关,数据不丢)
        g_windowId.fetch_add(1, std::memory_order_relaxed);
        const uint64_t startTs = NowNs();
        // 完整宽度stageId(与闭窗同口径,uint64不截断)
        const uint64_t stageId = g_windowId.load(std::memory_order_relaxed);
        if (g_api.report_stage != nullptr)
        {
            try
            {
                g_api.report_stage(1, startTs, stageId);
            }
            catch (...)
            {
                fprintf(stderr,
                        "[msmemscope] hostmem: [pid=%llu] stage start dispatch failed, window data may be empty\n",
                        static_cast<unsigned long long>(getpid()));
            }
        }

        // 6. 开闸(release: 与生产者acquire配对,上述初始化全部可见)
        g_enabled.store(true, std::memory_order_release);
        // 窗口时间线:每窗一行(可维护性日志)——ClearTables刚执行完(表应归零),
        // 中途重开周期与进程归属在stderr直接可见,配合闭窗行还原完整窗口时间线
        fprintf(stderr, "[msmemscope] hostmem: [pid=%llu] window open id=%llu (stacks=%llu blocks=%llu)\n",
                static_cast<unsigned long long>(getpid()),
                static_cast<unsigned long long>(g_windowId.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_stackCount.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_blockCount.load(std::memory_order_relaxed)));
        pthread_mutex_unlock(&g_svcMtx);
        return;
    }

    // 关窗(调用线程同步完成): 记账冻结→停预热线程(join,兼作沉降期)→闭窗聚合
    // →发STAGE_END。闭窗是本线程的同步路径,STAGE_END即"聚合完成、快照可拉取"的信号
    if (!g_ctorDone)
    {
        g_openPending = false;  // 构造前"开→关"齐发:撤销暂存(窗口从未真开,无聚合需求)
    }
    if (!g_enabled.load(std::memory_order_relaxed))
    {
        pthread_mutex_unlock(&g_svcMtx);
        // 幂等: 窗口已关。g_windowId>0=本进程曾开过窗(退出期诊断打点):此行与
        // 本函数的"window close id=X done"互证——done出现=闭窗聚合完成、STAGE_END
        // 已发;本行出现而done缺失=闭窗从未真正启动(g_closing未置位)
        if (g_windowId.load(std::memory_order_relaxed) != 0)
        {
            fprintf(stderr,
                    "[msmemscope] hostmem: [pid=%llu] close requested but window already disabled "
                    "(windowId=%llu closing=%d)\n",
                    static_cast<unsigned long long>(getpid()),
                    static_cast<unsigned long long>(g_windowId.load(std::memory_order_relaxed)),
                    static_cast<int>(g_closing.load(std::memory_order_relaxed)));
        }
        return;
    }
    g_enabled.store(false, std::memory_order_release);  // 记账门控先关(生产者冻结)
    g_closing.store(true, std::memory_order_release);
    // 停预热线程: join兼作沉降期——关闸后in-flight free已drain(见StopWarmupThread)
    StopWarmupThread();
    // 闭窗聚合(遍历块表/栈表/符号化,同步完成;产物入g_closeStats/g_closeSizeDist/
    // g_closeSnapshot,窗口关闭态可经dump_*/get_stats拉取)
    CloseAggregate();
    const uint64_t ts = NowNs();
    // 完整宽度stageId(与开窗同口径,uint64不截断)
    const uint64_t stageId = g_windowId.load(std::memory_order_relaxed);
    if (g_api.report_stage != nullptr)
    {
        try
        {
            g_api.report_stage(0, ts, stageId);
        }
        catch (...)
        {
        }
    }
    // 窗口时间线:闭窗完成一行(可维护性日志)。此态条目留存表内,下次开窗才清空。
    // unattr=窗口内栈层失败转未知桶的块数(账本完整度100%而unattr上探=栈表拥塞,
    // 与分析器报告的未知栈桶行互证);truncated=截断标注(bit0块表满转溢出/bit1栈表满
    // 转未知桶/bit2溢出账本满记账停止)
    fprintf(stderr,
            "[msmemscope] hostmem: [pid=%llu] window close id=%llu done (stacks=%llu blocks=%llu "
            "unattr=%llu truncated=%u)\n",
            static_cast<unsigned long long>(getpid()),
            static_cast<unsigned long long>(g_windowId.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_stackCount.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_blockCount.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unattributedCount.load(std::memory_order_relaxed) -
                                            g_unattrWindowBase.load(std::memory_order_relaxed)),
            static_cast<unsigned int>(g_truncated.load(std::memory_order_relaxed)));
    // 符号缓存覆盖(诊断):top-K采样产物的符号帧数。接近SYM_CACHE_MAX_ENTRIES=
    // 缓存满提前停;明显小于=采样吞吐不足(调TOP_LEAK_SYMBOLIZE_K/INTERVAL)
    fprintf(stderr, "[msmemscope] hostmem: [pid=%llu] symCache: %zu entries (warmup coverage, cap=%zu)\n",
            static_cast<unsigned long long>(getpid()), g_symCache.size(), static_cast<size_t>(SYM_CACHE_MAX_ENTRIES));
    g_closing.store(false, std::memory_order_release);
    pthread_mutex_unlock(&g_svcMtx);
}

void SvcGetStats(MsmemscopeHostmemStats* stats)
{
    if (stats == nullptr)
    {
        return;
    }
    // 窗口关闭态: 返回闭窗冻结值(CloseAggregate产物,与dump_*同源一致);
    // 开启态: 实时计数器尽力而为值(未释放=申请-释放派生,与快照口径一致)
    if (g_closeSnapshot.valid)
    {
        stats->liveBlockCount = g_closeSnapshot.liveBlockCount;
        stats->totalAllocCount = g_closeSnapshot.totalAllocCount;
        stats->totalAllocBytes = g_closeSnapshot.totalAllocBytes;
        stats->totalFreedCount = g_closeSnapshot.totalFreedCount;
        stats->totalFreedBytes = g_closeSnapshot.totalFreedBytes;
        stats->untrackedCount = g_closeSnapshot.untrackedCount;
        stats->untrackedBytes = g_closeSnapshot.untrackedBytes;
        stats->overflowAllocCount = g_closeSnapshot.overflowAllocCount;
        stats->overflowAllocBytes = g_closeSnapshot.overflowAllocBytes;
        stats->overflowFreedCount = g_closeSnapshot.overflowFreedCount;
        stats->overflowFreedBytes = g_closeSnapshot.overflowFreedBytes;
        stats->preWindowFreeCount = g_closeSnapshot.preWindowFreeCount;
        stats->preWindowFreeBytes = g_closeSnapshot.preWindowFreeBytes;
        stats->evictedStackCount = g_closeSnapshot.evictedStackCount;
        stats->evictedAllocCount = g_closeSnapshot.evictedAllocCount;
        stats->evictedAllocBytes = g_closeSnapshot.evictedAllocBytes;
        stats->sampleRate = g_closeSnapshot.sampleRate;
        stats->truncated = g_closeSnapshot.truncated;
        return;
    }
    const uint64_t allocCount = g_totalAllocCount.load(std::memory_order_relaxed);
    const uint64_t allocBytes = g_totalAllocBytes.load(std::memory_order_relaxed);
    stats->liveBlockCount = g_blockCount.load(std::memory_order_relaxed);
    stats->totalAllocCount = allocCount;
    stats->totalAllocBytes = allocBytes;
    stats->totalFreedCount = g_totalFreedCount.load(std::memory_order_relaxed);
    stats->totalFreedBytes = g_totalFreedBytes.load(std::memory_order_relaxed);
    stats->untrackedCount = g_untrackedCount.load(std::memory_order_relaxed);
    stats->untrackedBytes = g_untrackedBytes.load(std::memory_order_relaxed);
    stats->overflowAllocCount = g_overflowAllocCount.load(std::memory_order_relaxed);
    stats->overflowAllocBytes = g_overflowAllocBytes.load(std::memory_order_relaxed);
    stats->overflowFreedCount = g_overflowFreedCount.load(std::memory_order_relaxed);
    stats->overflowFreedBytes = g_overflowFreedBytes.load(std::memory_order_relaxed);
    stats->preWindowFreeCount = g_preWindowFreeCount.load(std::memory_order_relaxed);
    stats->preWindowFreeBytes = g_preWindowFreeBytes.load(std::memory_order_relaxed);
    stats->evictedStackCount = g_evictedStackCount.load(std::memory_order_relaxed);
    stats->evictedAllocCount = g_evictedAllocCount.load(std::memory_order_relaxed);
    stats->evictedAllocBytes = g_evictedAllocBytes.load(std::memory_order_relaxed);
    stats->sampleRate = g_sampleRate.load(std::memory_order_relaxed);
    stats->truncated = g_truncated.load(std::memory_order_relaxed);
}

void SvcDumpLiveBlocks(void (*emit)(void* ctx, uint64_t addr, uint64_t size, uint64_t allocTs, uint64_t stackId),
                       void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    // 窗口关闭态调用(热路径冻结);分片锁与潜在的开窗清表互斥串行化。
    // owner指针安全性:块在表即持其ref(refs>=2,块引用+在途),归栈条目不可被
    // 淘汰(淘汰判读refs==1;闭窗态无生产者,指针稳定)。空分片整体跳过内层桶数组扫描
    for (auto& shard : g_blockShards)
    {
        pthread_mutex_lock(&shard.mtx);
        // count>0隐含keys已分配(count仅在EnsureRoomLocked分配keys后递增);
        // count在闭窗冻结态下稳定,分片锁内读取无争用
        if (shard.keys != nullptr && shard.count > 0)
        {
            for (uint32_t i = 0; i <= shard.capMask; ++i)
            {
                if (shard.keys[i] == 0)
                {
                    continue;
                }
                const BlockEntry& v = shard.vals[i];
                const uint64_t stackId = v.owner != nullptr ? v.owner->stackId : 0;
                emit(ctx, shard.keys[i], v.size, v.allocTs, stackId);
            }
        }
        pthread_mutex_unlock(&shard.mtx);
    }
}

void SvcDumpStackStats(void (*emit)(void* ctx, uint64_t stackId, uint64_t allocCount, uint64_t allocBytes,
                                    uint64_t freedCount, uint64_t freedBytes, uint64_t unfreedCount,
                                    uint64_t unfreedBytes, uint64_t maxBlockSize, const char* frameDesc, size_t len),
                       void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    // 仅窗口关闭态调用(闭窗聚合产物;开启态为空——CloseAggregate填充,ClearTables
    // 清空,跨窗口不残留)。重放g_closeStats(已按unfreedBytes降序排序)
    for (const StackStatRow& r : g_closeStats)
    {
        emit(ctx, r.stackId, r.allocCount, r.allocBytes, r.freedCount, r.freedBytes, r.unfreedCount, r.unfreedBytes,
             r.maxBlockSize, r.frameDesc.data(), r.frameDesc.size());
    }
}

void SvcDumpSizeDist(void (*emit)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount,
                                  uint64_t blockBytes),
                     void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    // 仅窗口关闭态调用(闭窗聚合产物,同g_closeStats生命周期);重放g_closeSizeDist
    for (const SizeBucket& b : g_closeSizeDist)
    {
        emit(ctx, b.rangeLow, b.rangeHigh, b.blockCount, b.blockBytes);
    }
}

void SvcDumpPreWindowDist(void (*emit)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount,
                                       uint64_t blockBytes),
                          void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    // 仅窗口关闭态调用(闭窗聚合产物,同g_closeStats生命周期);重放g_closePreWindowDist
    for (const SizeBucket& b : g_closePreWindowDist)
    {
        emit(ctx, b.rangeLow, b.rangeHigh, b.blockCount, b.blockBytes);
    }
}

const MsmemscopeHostmemSvc g_svcTable = {SvcSetEnabled,     SvcGetStats,     SvcDumpLiveBlocks,
                                         SvcDumpStackStats, SvcDumpSizeDist, SvcDumpPreWindowDist};

// =============================================================================
// fork安全: prepare冻结(锁全部分片)→parent释放→child退出监控(g_forked置位)
// =============================================================================

void ForkPrepare()
{
    // 依次获取全部128把分片锁: fork瞬间无他线程持锁写表;临界区纯内存操作,获取有界。
    // 父进程不清空、不排空任何数据(零丢失)
    for (auto& shard : g_blockShards)
    {
        pthread_mutex_lock(&shard.mtx);
    }
    for (auto& shard : g_stackShards)
    {
        pthread_mutex_lock(&shard.mtx);
    }
}

void ForkParent()
{
    for (auto& shard : g_blockShards)
    {
        pthread_mutex_unlock(&shard.mtx);
    }
    for (auto& shard : g_stackShards)
    {
        pthread_mutex_unlock(&shard.mtx);
    }
}

void ForkChild()
{
    // fork后代不监控(见g_forked注释):仅恢复子进程自身可运行性,不做任何表/计数
    // 清理——①子进程永不再开窗,g_enabled即将关闸,表数据无人消费;②清表需
    // 逐条目遍历,fork后首次写触发COW整页拷贝(块表可达百MB页),对fork密集
    // 型宿主是纯代价;③COW快照在子进程内无人触碰即无人踩坏。父进程侧数据
    // 零丢失(prepare只冻结不排空)。分片锁由fork调用线程自身在prepare中持有,
    // 子进程仅本线程存活,直接释放即完成重建
    for (auto& shard : g_blockShards)
    {
        pthread_mutex_unlock(&shard.mtx);
    }
    for (auto& shard : g_stackShards)
    {
        pthread_mutex_unlock(&shard.mtx);
    }

    // g_svcMtx可能被fork瞬间正在开窗/闭窗的其他线程持有(fork调用线程不可能同时
    // 在SvcSetEnabled内):子进程内重新初始化——仅本线程存活,无人在等这把锁;
    // COW下不影响父进程。子进程退出时atexit闭窗的set_enabled依赖此锁,不重建则
    // 竞态下子进程退出挂死
    pthread_mutex_init(&g_svcMtx, nullptr);

    // 关闸(fork时窗口若开启,子进程生产者即刻静止)+置fork标记(release配对
    // SvcSetEnabled的acquire)。g_closing必须复位:fork发生在父进程关窗聚合期间时
    // 子进程继承closing=1,采集侧atexit闭窗等待将永不满足,子进程退出挂死。
    // 预热线程未随fork存活,重建标志一并清零(子进程永不创建预热线程,清零仅防
    // get_status误报线程在位;g_warmupStop同样复位,防子进程内残留置位
    // 导致父进程已创建线程指针被复用前状态错乱)
    g_enabled.store(false, std::memory_order_release);
    g_closing.store(false, std::memory_order_release);
    g_warmupThreadCreated.store(false, std::memory_order_release);
    g_warmupStop.store(false, std::memory_order_release);
    g_forked.store(true, std::memory_order_release);
}

// =============================================================================
// 构造与配置覆盖
// =============================================================================

size_t ParseSizeEnv(const char* name, size_t defVal)
{
    const char* v = getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return defVal;
    }
    char* end = nullptr;
    unsigned long long n = strtoull(v, &end, 10);
    if (end == v)
    {
        return defVal;
    }
    return static_cast<size_t>(n);
}

// 大小排布桶边界解析(闭窗聚合步骤0用): MSMEMSCOPE_HOSTMEM_SIZE_BUCKETS为逗号分隔
// 的下界序列(升序,如"256,1024,4096"),桶数=下界数+1;末桶上界UINT64_MAX。
// 非法输入(空/非数字/非升序)按默认边界回退;首个下界必须>0(否则首桶为空,无意义)。
// 默认7桶按数量级划分(0~256B/256B~1K/1K~4K/4K~32K/32K~256K/256K~1M/>1M):
// 各桶覆盖一个数量级,报告可直接看出未释放块尺寸分布
void ParseSizeBuckets()
{
    const char* v = getenv("MSMEMSCOPE_HOSTMEM_SIZE_BUCKETS");
    std::vector<uint64_t, RealMallocAllocator<uint64_t>> bounds;
    const uint64_t kDefaults[] = {256, 1024, 4096, 32768, 262144, 1048576};
    bool ok = v != nullptr && *v != '\0';
    if (ok)
    {
        char* p = const_cast<char*>(v);
        uint64_t prev = 0;
        while (*p != '\0')
        {
            char* end = nullptr;
            unsigned long long n = strtoull(p, &end, 10);
            if (end == p)
            {
                ok = false;
                break;
            }
            if (n == 0 || n <= prev)
            {
                ok = false;  // 下界须>0且严格升序
                break;
            }
            bounds.push_back(static_cast<uint64_t>(n));
            prev = static_cast<uint64_t>(n);
            p = end;
            if (*p == ',')
            {
                ++p;
            }
            else if (*p != '\0')
            {
                ok = false;
                break;
            }
        }
    }
    if (!ok)
    {
        bounds.clear();
        for (uint64_t b : kDefaults)
        {
            bounds.push_back(b);
        }
    }
    g_sizeBucketBounds = std::move(bounds);
}

// 注意(初始化顺序,见g_preWindowDistCount处init_priority注释):本函数先于本TU的
// 动态静态初始化(_GLOBAL__sub_I)执行——本函数(含pendingOpen开窗路径)写入的动态
// 初始化全局必须带init_priority前置构造,否则写入结果被重放的默认构造覆盖;
// 常量初始化全局不受重放影响
__attribute__((constructor)) void HostMemHookInit()
{
    // 本so地址区间解析(钩子自身帧过滤用,见CaptureFrames注释)
    ResolveHookSoRange();

    // 模块可执行段快照首拍(FP走栈pc校验依据;此后开窗+预热线程1Hz刷新)
    RefreshExecRanges();

    // 容量覆盖(诊断/压测用): 栈上限/块上限/大小排布桶边界
    const size_t maxStacks = ParseSizeEnv("MSMEMSCOPE_HOSTMEM_MAX_STACKS", DEFAULT_MAX_STACKS);
    const size_t maxBlocks = ParseSizeEnv("MSMEMSCOPE_HOSTMEM_MAX_BLOCKS", DEFAULT_MAX_BLOCKS);
    g_maxStacksPerShard = maxStacks < STACK_SHARDS ? 1 : maxStacks / STACK_SHARDS;
    g_maxBlocksPerShard = maxBlocks < BLOCK_SHARDS ? 1 : maxBlocks / BLOCK_SHARDS;
    ParseSizeBuckets();

    // 溢出通道容量(安全阀): 全表上限经MSMEMSCOPE_HOSTMEM_MAX_OVERFLOW覆盖,
    // 按分片均分(每分片独立判满,bit2置位即记账停止)
    const size_t maxOverflow = ParseSizeEnv("MSMEMSCOPE_HOSTMEM_MAX_OVERFLOW", DEFAULT_MAX_OVERFLOW);
    g_maxOverflowPerShard = maxOverflow < BLOCK_SHARDS ? 1 : maxOverflow / BLOCK_SHARDS;

    // 开窗前free大小分布原子槽(按桶数分配;RecordFree热路径写入,关闭态快照读出)。
    // 注意:构造期即分配(g_mainStarted之前),与记账无关,零风险
    const size_t bucketCount = g_sizeBucketBounds.size() + 1;
    g_preWindowDistCount.resize(bucketCount);
    g_preWindowDistBytes.resize(bucketCount);

    // 真函数解析(构造期固化;入口惰性解析兜底)
    ResolveAllRealFns();

    // fork handler注册(fork早于本构造的极端场景handler未生效:已加载so的构造函数
    // 不随fork在子进程重跑,子进程无钩子状态、无监控——与fork后代不监控语义一致)
    pthread_atfork(ForkPrepare, ForkParent, ForkChild);

    // 进程锚点行(可维护性日志):每个加载本so的进程在stderr打一行pid+生效容量。
    // 多子进程场景下父/子进程共用同一stderr,各进程summary文件(首行带pid)靠此行
    // 与各日志行内嵌的[pid]归属匹配;子进程是否加载了钩子、容量env覆盖是否生效
    // 由此一行可见
    fprintf(stderr, "[msmemscope] hostmem: [pid=%llu] hook loaded (maxStacks=%llu maxBlocks=%llu maxOverflow=%llu)\n",
            static_cast<unsigned long long>(getpid()),
            static_cast<unsigned long long>(g_maxStacksPerShard * STACK_SHARDS),
            static_cast<unsigned long long>(g_maxBlocksPerShard * BLOCK_SHARDS),
            static_cast<unsigned long long>(g_maxOverflowPerShard * BLOCK_SHARDS));

    // 构造完成:此后set_enabled走正常路径;构造前暂存的开窗请求在此补开(配置
    // 已解析,环容量覆盖生效,STAGE_START不再提前到采集库自身构造期)
    pthread_mutex_lock(&g_svcMtx);
    g_ctorDone = true;
    const bool pendingOpen = g_openPending;
    g_openPending = false;
    pthread_mutex_unlock(&g_svcMtx);
    if (pendingOpen)
    {
        SvcSetEnabled(1);
    }
}

}  // namespace

// =============================================================================
// 退出闭窗触发(采集库侧msmemscope_hostmem_exit_close): 退出期必须在真exit()之前、
// 全进程存活时完成窗口关闭(停预热线程+闭窗聚合+STAGE_END→完整报告)。teardown期
// 任何atexit触发点都不可用(库级handler在钩子so被rtld_fini先于采集库拆卸后执行,
// 聚合线程无法运行→退出挂死;全局handler在_dl_fini末尾晚于全部静态析构→闭窗被
// destroyed_跳过),故由exit拦截器/main-return trampoline双路在真exit()之前触发
// (与g_exiting双路置位同构,契约详见event_report.cpp文件头注释)。懒解析+缓存;
// 采集库缺失(理论不可达,本so DT_NEEDED依赖)时安静跳过,由~HostLeakAnalyzer兜底。
// 递归exit防护:首个调用者负责触发并等待聚合完成,重入(闭窗路径内再exit)直接跳过
// =============================================================================
void (*g_exitCloseFn)(void) = nullptr;
std::atomic<bool> g_exitCloseTriggered{false};

static void TriggerHostMemExitClose()
{
    if (g_exitCloseFn == nullptr)
    {
        HookSuppressGuard guard;  // dlsym内部分配经PLT回落本钩子→守卫拦截→竞技场
        g_exitCloseFn = reinterpret_cast<void (*)(void)>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_exit_close"));
        if (g_exitCloseFn == nullptr)
        {
            return;
        }
    }
    // 触发行(可维护性日志):exit拦截器/main trampoline路径可见性;闭窗结果由
    // 采集库侧"window close id=X done"/"closing bit already clear"等打点互证
    fprintf(stderr, "[msmemscope] hostmem: [pid=%llu] exit path: triggering host mem window close\n",
            static_cast<unsigned long long>(getpid()));
    g_exitCloseFn();
}

// =============================================================================
// 劫持入口(extern "C",必须外部链接): 快路径四查(抑制/窗口/宿主main/采集库)不过则直转真函数
// 真函数调用+记账全程持抑制守卫(场景A);分配失败(NULL)不记账
// =============================================================================

// 宿主main边界trampoline(静态初始化防护,见g_mainStarted注释):
// __libc_start_main由_start在ld.so完成全部so初始化后、可执行文件.init_array之前调用;
// 把main替换为本trampoline,标志恰在"全部静态初始化完成后、真main首行"处置位。
// glibc该7参ABI自1998年至今稳定(2.34起init/fini恒为NULL但参数位保留)
using HostMemMainFn = int (*)(int, char**, char**);
static HostMemMainFn g_realMain = nullptr;

extern "C" __attribute__((visibility("hidden"))) int HostMemHookMainEntry(int argc, char** argv, char** envp)
{
    g_mainStarted.store(true, std::memory_order_release);
    const int rc = g_realMain(argc, argv, envp);
    // main返回即进入退出期:glibc随后经libc内部别名调exit(不经PLT,exit拦截器
    // 拦不到),此处兜底置位,与exit拦截器双路覆盖两类退出路径;同款双路覆盖退出
    // 闭窗触发——此路径真exit()在返回rc之后才发生,此刻触发与拦截器路径等价
    // (全进程存活,闭窗与正常stop()同等可靠)
    g_exiting.store(true, std::memory_order_release);
    if (!g_exitCloseTriggered.exchange(true, std::memory_order_acq_rel))
    {
        HookSuppressGuard guard;
        TriggerHostMemExitClose();
    }
    return rc;
}

// 宿主exit拦截(退出期防护,与__libc_start_main拦截同构):置位退出标志停记账后转真exit。
// 覆盖显式exit()/Py_Exit类调用(经PLT解析到本so);拦截器自身零分配(dlsym构造期已固化,
// 极端未解析时置守卫懒解析一次)。解析失败兜底syscall直退(跳过atexit语义降级退出),
// 绝不return——exit为noreturn,返回即调用方栈损坏
extern "C" void exit(int status)
{
    g_exiting.store(true, std::memory_order_release);
    // 真exit之前触发采集库退出闭窗:此刻装载器/聚合线程/分析器全部存活,停预热
    // 线程+聚合+STAGE_END与正常stop()同等可靠(teardown期触发会挂死,见
    // TriggerHostMemExitClose注释)。递归exit防护:首个调用者触发并等待,重入跳过
    if (!g_exitCloseTriggered.exchange(true, std::memory_order_acq_rel))
    {
        HookSuppressGuard guard;
        TriggerHostMemExitClose();
    }
    if (real_exit_fn == nullptr)
    {
        HookSuppressGuard guard;
        real_exit_fn = reinterpret_cast<void (*)(int)>(ResolveOne("exit"));
    }
    if (real_exit_fn != nullptr)
    {
        real_exit_fn(status);
    }
    syscall(SYS_exit_group, status);
    for (;;)
    {
        pause();
    }
}

extern "C" int __libc_start_main(HostMemMainFn main, int argc, char** argv, void (*init)(void), void (*fini)(void),
                                 void (*rtldFini)(void), void* stackEnd)
{
    using LibcStartMainFn = int (*)(HostMemMainFn, int, char**, void (*)(void), void (*)(void), void (*)(void), void*);
    g_realMain = main;
    LibcStartMainFn realFn = nullptr;
    {
        // 守卫只护dlsym解析窗口:其内部首次分配经PLT回落本钩子→守卫拦截→竞技场。
        // 严禁把守卫扩到realFn调用——真实__libc_start_main为noreturn(内部经exit退出,
        // 本帧永不返回),函数级守卫的析构永不执行→主线程抑制深度永久滞留1,而宿主main
        // 的整个生命周期都活在这帧之下→主线程所有分配/释放被静默跳过(块表与事件双
        // 缺失);realFn内部.init_array阶段的分配已由g_mainStarted门控排除,无需守卫兜底
        HookSuppressGuard guard;
        realFn = reinterpret_cast<LibcStartMainFn>(dlsym(RTLD_NEXT, "__libc_start_main"));
    }
    if (realFn != nullptr)
    {
        return realFn(&HostMemHookMainEntry, argc, argv, init, fini, rtldFini, stackEnd);
    }
    // 解析失败(理论不可达,libc恒在RTLD_NEXT链上):退化为无门控放行,宁采旧险不静默全丢;
    // 直接调main会丢失fini/rtld_fini注册,仅作最后兜底
    fprintf(stderr, "[msmemscope] hostmem: [pid=%llu] __libc_start_main resolve failed, main gate disabled\n",
            static_cast<unsigned long long>(getpid()));
    g_mainStarted.store(true, std::memory_order_release);
    return main(argc, argv, nullptr);
}

extern "C" void* malloc(size_t size)
{
    if (!ShouldTrace())
    {
        return RealMalloc(size);
    }
    HookSuppressGuard guard;
    void* ptr = RealMalloc(size);
    if (ptr == nullptr)
    {
        return nullptr;
    }
    RecordMalloc(reinterpret_cast<uint64_t>(ptr), size);
    return ptr;
}

extern "C" void free(void* ptr)
{
    if (ptr == nullptr)
    {
        return;
    }
    if (IsArenaPtr(ptr))
    {
        return;  // 自举竞技场内存不归还
    }
    if (!ShouldTrace())
    {
        RealFree(ptr);
        return;
    }
    HookSuppressGuard guard;
    // 先记账后真释放(消除ABA: 释放后他线程立即复用同地址会误删新记录)
    RecordFree(reinterpret_cast<uint64_t>(ptr));
    RealFree(ptr);
}

extern "C" void* calloc(size_t n, size_t size)
{
    // 溢出时glibc返回NULL,先判后调保持语义一致
    if (n != 0 && size > SIZE_MAX / n)
    {
        errno = ENOMEM;
        return nullptr;
    }
    if (!ShouldTrace())
    {
        return RealCalloc(n, size);
    }
    HookSuppressGuard guard;
    void* ptr = RealCalloc(n, size);
    if (ptr == nullptr)
    {
        return nullptr;
    }
    RecordMalloc(reinterpret_cast<uint64_t>(ptr), n * size);
    return ptr;
}

extern "C" void* realloc(void* ptr, size_t size)
{
    if (!ShouldTrace())
    {
        return RealRealloc(ptr, size);
    }
    HookSuppressGuard guard;

    // ptr为空: 同malloc
    if (ptr == nullptr)
    {
        void* np = RealRealloc(nullptr, size);
        if (np == nullptr)
        {
            return nullptr;
        }
        RecordMalloc(reinterpret_cast<uint64_t>(np), size);
        return np;
    }

    // 先捕获并删除旧块记录再调真函数(消除ABA: 真realloc返回后旧地址可能已被
    // 他线程重新分配,届时查到的可能是他人的新记录)。释放记账(liveBytes减+
    // totalFreed自增)已在CaptureAndRemoveBlock的块表锁临界区内完成;块引用
    // 捕获即转移给oldRec(不在此处dispose)——在途窗口(refs>=2)钉住条目,防
    // 淘汰;终态: size==0/成功=dispose,失败=ReinsertBlock接管(回插归块/转溢出
    // dispose),每块恰被记账一次
    BlockEntry oldRec{};
    const BlockRemoveResult oldRes = CaptureAndRemoveBlock(reinterpret_cast<uint64_t>(ptr), oldRec);

    if (size == 0)
    {
        // glibc语义: realloc(p,0)=释放旧块,不记MALLOC(释放记账已随捕获完成)
        if (oldRec.owner != nullptr)
        {
            oldRec.owner->refs.fetch_sub(1, std::memory_order_relaxed);  // dispose转移引用(旧块已释放)
        }
        else if (oldRes == BlockRemoveResult::kMiss)
        {
            // 开窗前/未记账块: 与free()的kMiss同语义,其释放落入开窗前free独立通道
            RecordPreWindowFree(reinterpret_cast<uint64_t>(ptr));
        }
        return RealRealloc(ptr, 0);
    }

    void* np = RealRealloc(ptr, size);
    if (np == nullptr)
    {
        // 失败(size>0): 旧块仍存活,按捕获源回插并恢复对应记账。
        // kBlock→ReinsertBlock内部接管转移引用(回插成功归块/转溢出或自旋耗尽
        // dispose);kOverflow→ReinsertOverflowBlock(溢出块无owner,无dispose)
        if (oldRes == BlockRemoveResult::kBlock)
        {
            ReinsertBlock(reinterpret_cast<uint64_t>(ptr), oldRec);
        }
        else if (oldRes == BlockRemoveResult::kOverflow)
        {
            ReinsertOverflowBlock(reinterpret_cast<uint64_t>(ptr), oldRec.size);
        }
        // kMiss/kLockFailed: 无回插对象(块不在表/溢出账本,或锁耗尽残留),无owner无dispose
        return nullptr;
    }

    // 成功: 旧块已释放(记账已随捕获完成),新块记账走RecordMalloc
    // (新size过阈值检查+新归栈=当前调用栈)
    if (oldRec.owner != nullptr)
    {
        oldRec.owner->refs.fetch_sub(1, std::memory_order_relaxed);  // dispose转移引用(旧块已释放)
    }
    else if (oldRes == BlockRemoveResult::kMiss)
    {
        // 开窗前/未记账块随realloc成功而释放(与free()的kMiss同语义):
        // 落入开窗前free独立通道;np==ptr原地扩容不改变此语义(与kBlock同款
        // "释放+新申请"记账口径)
        RecordPreWindowFree(reinterpret_cast<uint64_t>(ptr));
    }
    RecordMalloc(reinterpret_cast<uint64_t>(np), size);
    return np;
}

extern "C" int posix_memalign(void** memptr, size_t align, size_t size)
{
    if (!ShouldTrace())
    {
        return RealPosixMemalign(memptr, align, size);
    }
    HookSuppressGuard guard;
    int ret = RealPosixMemalign(memptr, align, size);
    if (ret == 0 && memptr != nullptr && *memptr != nullptr)
    {
        RecordMalloc(reinterpret_cast<uint64_t>(*memptr), size);
    }
    return ret;
}

extern "C" void* aligned_alloc(size_t align, size_t size)
{
    if (!ShouldTrace())
    {
        return RealAlignedAlloc(align, size);
    }
    HookSuppressGuard guard;
    void* ptr = RealAlignedAlloc(align, size);
    if (ptr == nullptr)
    {
        return nullptr;
    }
    RecordMalloc(reinterpret_cast<uint64_t>(ptr), size);
    return ptr;
}

extern "C" void* memalign(size_t align, size_t size)
{
    if (!ShouldTrace())
    {
        return RealMemalign(align, size);
    }
    HookSuppressGuard guard;
    void* ptr = RealMemalign(align, size);
    if (ptr == nullptr)
    {
        return nullptr;
    }
    RecordMalloc(reinterpret_cast<uint64_t>(ptr), size);
    return ptr;
}

extern "C" void* valloc(size_t size)
{
    if (!ShouldTrace())
    {
        return RealValloc(size);
    }
    HookSuppressGuard guard;
    void* ptr = RealValloc(size);
    if (ptr == nullptr)
    {
        return nullptr;
    }
    RecordMalloc(reinterpret_cast<uint64_t>(ptr), size);
    return ptr;
}

extern "C" void* pvalloc(size_t size)
{
    if (!ShouldTrace())
    {
        return RealPvalloc(size);
    }
    HookSuppressGuard guard;
    void* ptr = RealPvalloc(size);
    if (ptr == nullptr)
    {
        return nullptr;
    }
    RecordMalloc(reinterpret_cast<uint64_t>(ptr), size);
    return ptr;
}

// =============================================================================
// bind握手与诊断导出
// =============================================================================

extern "C" const MsmemscopeHostmemSvc* msmemscope_hostmem_bind(const MsmemscopeHostmemApi* api)
{
    if (api == nullptr)
    {
        return nullptr;
    }
    // 必备回调校验: 窗口边界系统事件(report_stage)是分析器唯一的窗口驱动,缺失则
    // host功能不可用,调用方回退。分析器不消费事件流,闭窗快照经dump_*拉取
    if (api->report_stage == nullptr)
    {
        return nullptr;
    }
    g_api = *api;  // 结构体拷贝(幂等:重复bind覆盖)
    g_bound.store(true, std::memory_order_release);
    return &g_svcTable;
}

// 未归因计数:栈层失败(trylock/表满/OOM)转未知桶的块数,累计跨窗口,调用方差分。
// 走独立符号而非扩MsmemscopeHostmemStats(svc表C ABI不变,
// 新旧so混部时头文件版本不齐也安全;旧版采集库不调用此符号,无副作用)
extern "C" uint64_t msmemscope_hostmem_get_unattributed_count(void)
{
    return g_unattributedCount.load(std::memory_order_relaxed);
}

extern "C" int msmemscope_hostmem_get_status(void)
{
    uint32_t flags = 0;
    if (g_bound.load(std::memory_order_relaxed))
    {
        flags |= 0x1;
    }
    if (g_enabled.load(std::memory_order_relaxed))
    {
        flags |= 0x2;
    }
    if (g_closing.load(std::memory_order_relaxed))
    {
        flags |= 0x4;
    }
    if (g_warmupThreadCreated.load(std::memory_order_relaxed))
    {
        flags |= 0x8;  // 符号化预热线程已创建
    }
    if (g_mainStarted.load(std::memory_order_relaxed))
    {
        flags |= 0x10;  // 宿主main边界已过(UT据此验证__libc_start_main拦截生效)
    }
    if (g_exiting.load(std::memory_order_relaxed))
    {
        flags |= 0x20;  // 退出期(exit已进/main已返,记账门控已关,UT据此验证exit拦截生效)
    }
    if (g_forked.load(std::memory_order_relaxed))
    {
        flags |= 0x40;  // fork后代(不再监控,开窗一律拒绝,UT据此验证fork语义)
    }
    return static_cast<int>(flags);
}

// 账本自检(UT诊断专用,独立符号不走svc表C ABI,
// 旧版采集库不调用无副作用)。返回违例计数,0=一致。逐分片持锁校验:
// ①栈表: g_stackCount与全部分片map大小之和一致
// ②块表: g_blockCount与全部分片count之和一致
// ③栈条目闭窗一致性绊线: refs==0→CORRUPTION(双重dispose的定向检测——唯一致命
//   失效方向: 条目refs被多次递减提前归0,淘汰后指针悬垂);
//   refs==1&&liveBytes!=0→CORRUPTION(引用/字节计数不一致: refs==1⟹零存活块
//   ⟹liveBytes必为0);liveBytes<0→CORRUPTION(释放侧仅递减,窗口内不可为负;
//   负数=覆盖/恢复记账不对称的bug信号)
extern "C" uint64_t msmemscope_hostmem_selfcheck(void)
{
    uint64_t violations = 0;
    size_t stackCount = 0;
    for (auto& shard : g_stackShards)
    {
        pthread_mutex_lock(&shard.mtx);
        stackCount += shard.map.size();
        for (auto& kv : shard.map)
        {
            const StackEntry& e = kv.second;
            if (e.refs.load(std::memory_order_relaxed) == 0)
            {
                violations += 1;  // ③ 双重dispose(淘汰前哨)
            }
            const int64_t lb = e.liveBytes.load(std::memory_order_relaxed);
            if (e.refs.load(std::memory_order_relaxed) == 1 && lb != 0)
            {
                violations += 1;  // ③ refs/liveBytes计数不一致
            }
            if (lb < 0)
            {
                violations += 1;  // ③ liveBytes非负
            }
        }
        pthread_mutex_unlock(&shard.mtx);
    }
    if (stackCount != g_stackCount.load(std::memory_order_relaxed))
    {
        violations += 1;  // ①
    }
    size_t blockCount = 0;
    for (auto& shard : g_blockShards)
    {
        pthread_mutex_lock(&shard.mtx);
        blockCount += shard.count;
        pthread_mutex_unlock(&shard.mtx);
    }
    if (blockCount != g_blockCount.load(std::memory_order_relaxed))
    {
        violations += 1;  // ②
    }
    return violations;
}
