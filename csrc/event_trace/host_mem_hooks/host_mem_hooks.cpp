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
