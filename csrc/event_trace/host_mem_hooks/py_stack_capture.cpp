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

#include "py_stack_capture.h"

#include <Python.h>
#include <frameobject.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "cpython.h"         // PyStackCore::WalkFrames/PyInterpGuard/弱符号
#include "host_mem_hooks.h"  // MSMEMSCOPE_HOSTMEM_MIXED_STACK_MARKER(混合栈marker单一来源)
#include "ustring.h"

/*
 * 实现要点: 采集全程持GIL(PyInterpGuard Ensure/Release,已持GIL幂等,仅
 * tstate绑定线程参与——纯native线程无Python上下文直接跳过,不阻塞等GIL);
 * pyBuf经SharedStackPublishPyStack桥发布——栈分片锁内终判pyState并原子写入
 * pyBuf/pyLen/pyState(release store),与清表在途条目复位/并发第二采集互斥;
 * 读者acquire读CAPTURED后使用;采集期在途ref由RecordMalloc持有(dispose后移),
 * 条目恒存活;缓存/容器RealMalloc底座;无解释器/版本<3.9不采集,闭窗按unfreed
 * 判据派生NA
 */

namespace hostmem
{

namespace
{

// 约束阈值环境变量(开窗时读取,非热路径)。调试接口,不对外文档化
constexpr char ENV_LIVE_BYTES[] = "MSMEMSCOPE_HOSTMEM_PYSTACK_LIVE_BYTES";
constexpr char ENV_LIVE_BLOCKS[] = "MSMEMSCOPE_HOSTMEM_PYSTACK_LIVE_BLOCKS";
constexpr char ENV_LIVE_BYTES_BIG[] = "MSMEMSCOPE_HOSTMEM_PYSTACK_LIVE_BYTES_BIG";
constexpr char ENV_LIVE_BLOCKS_BIG[] = "MSMEMSCOPE_HOSTMEM_PYSTACK_LIVE_BLOCKS_BIG";

// 每线程scratch(零堆分配): 采集串格式化缓冲区,按需触页
constexpr size_t PY_SCRATCH_CAP = 32 * 1024;
// 帧串缓存上限(满后淘汰最久未用;8192×~64B串≈0.5MB级驻留)
constexpr size_t PY_FRAME_CACHE_LIMIT = 8192;
// 混合栈marker(host_mem_hooks.h单一来源,分析器按文本拆C栈/py帧两列)
constexpr char kMixedMarker[] = MSMEMSCOPE_HOSTMEM_MIXED_STACK_MARKER;

// 配置(写仅Configure,读在热路径/闭窗;经g_pyStackDepth release/acquire发布)
std::atomic<uint32_t> g_pyStackDepth{0};  // 0=未启用(默认,热路径零开销)
PyStackGateParams g_gate;                 // 阈值(Configure写后只读)

// 环境变量int64解析(非法/越界回落默认)
int64_t GetEnvI64(const char* name, int64_t fallback)
{
    const char* v = getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return fallback;
    }
    char* end = nullptr;
    errno = 0;
    long long n = strtoll(v, &end, 10);
    if (end == v || *end != '\0' || errno == ERANGE)
    {
        return fallback;
    }
    return static_cast<int64_t>(n);
}

// 解释器+版本门(无解释器进程自动退化,闭窗派生NA)
bool InterpReady() { return Utility::IsPyInterpRepeInited() && Utility::IsPyVersionAtLeast39(); }

// 帧串"file(lineno): func\n"格式化到buf(零堆分配;返回写入字节,0=空间不足)
size_t FormatFrame(const char* filename, const char* funcname, int lineno, char* buf, size_t cap)
{
    size_t off = 0;
    if (filename != nullptr)
    {
        for (const char* p = filename; *p != '\0'; ++p)
        {
            if (off + 1 >= cap)
            {
                return 0;
            }
            buf[off++] = *p;
        }
    }
    if (off + 1 >= cap)
    {
        return 0;
    }
    buf[off++] = '(';
    int64_t lv = lineno;
    if (lv < 0)
    {
        if (off + 1 >= cap)
        {
            return 0;
        }
        buf[off++] = '-';
        lv = -lv;  // 64位取负无溢出
    }
    char num[16];
    size_t nlen = 0;
    if (lv == 0)
    {
        num[nlen++] = '0';
    }
    while (lv > 0 && nlen + 1 < sizeof(num))
    {
        num[nlen++] = static_cast<char>('0' + lv % 10);
        lv /= 10;
    }
    while (nlen > 0)
    {
        if (off + 1 >= cap)
        {
            return 0;
        }
        buf[off++] = num[--nlen];
    }
    const char tail[] = "): ";
    for (size_t i = 0; i < sizeof(tail) - 1; ++i)
    {
        if (off + 1 >= cap)
        {
            return 0;
        }
        buf[off++] = tail[i];
    }
    if (funcname != nullptr)
    {
        for (const char* p = funcname; *p != '\0'; ++p)
        {
            if (off + 1 >= cap)
            {
                return 0;
            }
            buf[off++] = *p;
        }
    }
    if (off + 1 >= cap)
    {
        return 0;
    }
    buf[off++] = '\n';
    return off;
}

// py帧串LRU缓存: 键=PyCodeObject*(持引用),值=RealMalloc串;
// 写者与关窗Shutdown互斥锁串行,满则淘汰最久未用
class PyCodeFrameCache
{
   public:
    // 命中→memcpy缓存串;未命中→就地FormatFrame并RealMalloc入缓存(真OOM仅本次有效)
    void AppendTo(char* out, size_t cap, size_t& off, PyCodeObject* code, const char* filename, const char* funcname,
                  int lineno)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = map_.find(code);
        if (it != map_.end())
        {
            lru_.splice(lru_.end(), lru_, it->second.lruIt);  // 移至尾(最近使用)
            const size_t len = it->second.len;
            if (off + len <= cap)
            {
                memcpy(out + off, it->second.str, len);
                off += len;
            }
            return;
        }
        const size_t len = FormatFrame(filename, funcname, lineno, out + off, cap - off);
        if (len == 0)
        {
            return;
        }
        off += len;  // 格式化已就地写入;缓存拷贝自out+off-len
        if (map_.size() >= PY_FRAME_CACHE_LIMIT)
        {
            // 淘汰最久未用(缓存满): 释放串+DecRef键
            PyCodeObject* victim = lru_.front();
            lru_.pop_front();
            auto vit = map_.find(victim);
            if (vit != map_.end())
            {
                SharedRealFree(vit->second.str);
                map_.erase(vit);
                Py_DecRef(reinterpret_cast<PyObject*>(victim));
            }
        }
        char* copy = static_cast<char*>(SharedRealAlloc(len + 1));
        if (copy == nullptr)
        {
            return;  // 真OOM: 不缓存,就地文本保留(仅本次回调有效)
        }
        memcpy(copy, out + off - len, len);
        copy[len] = '\0';
        Py_IncRef(reinterpret_cast<PyObject*>(code));  // 键持引用
        auto lit = lru_.insert(lru_.end(), code);
        map_.emplace(code, Entry{copy, len, lit});
    }

    // 关窗清空(与采集写者互斥): 解释器已finalize时不DecRef键(对象可能已销毁)。
    // 键的DecRef必须持GIL(PyGILState_Ensure): 无GIL解引与解释器线程的IncRef
    // 并发是数据竞争(UB),计数触0时析构链也须在持GIL线程上跑。锁序约束: 采集
    // 路径持GIL后才取本锁(GIL→mtx),此处必须释放锁后再取GIL——锁内取GIL构成
    // 反向锁序(采集持GIL等mtx、本路径持mtx等GIL,死锁)。故分两阶段: 锁内清容器
    // 释放串+收集键引用,解锁后经PyInterpGuard逐一DecRef(键引用已从缓存转移,
    // 解锁后新采集重新入缓存者自行IncRef,引用各自配平)
    void Shutdown()
    {
        std::vector<PyCodeObject*, RealMallocAllocator<PyCodeObject*>> keys;
        try
        {
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto& kv : map_)
            {
                SharedRealFree(kv.second.str);
                keys.push_back(kv.first);
            }
            map_.clear();
            lru_.clear();
        }
        catch (...)
        {
            // 真OOM(收集vector分配失败): 容器仍须清空(防后续访问已释放str)——
            // 重新持锁清空(此时原lock_guard已随异常逸出析构,未持锁clear与并发
            // 写者竞态即UB);键引用整体不DecRef(上限8192个code对象泄漏引用,
            // 优于对未脱离缓存的键二次DecRef)
            std::lock_guard<std::mutex> lock(mtx_);
            map_.clear();
            lru_.clear();
            keys.clear();
        }
        const bool interpAlive = Utility::IsPyInterpRepeInited();
        if (interpAlive)
        {
            Utility::PyInterpGuard guard;  // 未持GIL线程临时取GIL(幂等)
            for (PyCodeObject* key : keys)
            {
                Py_DecRef(reinterpret_cast<PyObject*>(key));
            }
        }
    }

   private:
    struct Entry
    {
        char* str;  // RealMalloc底座串
        size_t len;
        std::list<PyCodeObject*, RealMallocAllocator<PyCodeObject*>>::iterator lruIt;
    };
    using List = std::list<PyCodeObject*, RealMallocAllocator<PyCodeObject*>>;
    using Map = std::unordered_map<PyCodeObject*, Entry, std::hash<PyCodeObject*>, std::equal_to<PyCodeObject*>,
                                   RealMallocAllocator<std::pair<PyCodeObject* const, Entry>>>;
    Map map_;
    List lru_;
    std::mutex mtx_;  // 串行采集写者与关窗Shutdown(慢路径,热路径不触锁)
};

PyCodeFrameCache s_frameCache;

struct CaptureCtx  // 采集上下文(scratch游标+帧串缓存)
{
    char* out;  // thread_local scratch
    size_t cap;
    size_t off;
    PyCodeFrameCache* cache;
};

// WalkFrames回调: 帧串经缓存追加进scratch;失败跳过该帧(无失败分支,不产生残缺串)
void AppendFrameCb(void* ctx, PyCodeObject* code, const char* filename, const char* funcname, int lineno,
                   uint32_t /*depth*/)
{
    CaptureCtx* c = static_cast<CaptureCtx*>(ctx);
    c->cache->AppendTo(c->out, c->cap, c->off, code, filename, funcname, lineno);
}

}  // namespace

// 泄漏候选判据(热路径门控与闭窗NA派生共用)
bool PyStackCapture::GateSatisfied(int64_t liveBytes, uint64_t liveBlocks)
{
    const PyStackGateParams& g = g_gate;
    return (liveBytes > g.liveBytesMin && liveBlocks > static_cast<uint64_t>(g.liveBlocksMin)) ||
           (liveBytes > g.liveBytesBig && liveBlocks > static_cast<uint64_t>(g.liveBlocksBig));
}

void PyStackCapture::Configure(uint32_t pyStackDepth)
{
    if (pyStackDepth == 0)
    {
        g_pyStackDepth.store(0, std::memory_order_release);
        return;  // 未启用(含阈值读取跳过),热路径零开销
    }
    PyStackGateParams g;
    g.liveBytesMin = GetEnvI64(ENV_LIVE_BYTES, 10485760);
    g.liveBlocksMin = GetEnvI64(ENV_LIVE_BLOCKS, 50);
    g.liveBytesBig = GetEnvI64(ENV_LIVE_BYTES_BIG, 52428800);
    g.liveBlocksBig = GetEnvI64(ENV_LIVE_BLOCKS_BIG, 10);
    g_gate = g;
    g_pyStackDepth.store(pyStackDepth, std::memory_order_release);  // 发布阈值
}

void PyStackCapture::CaptureIfEnabled(StackRecord& rec)
{
    // ① 配置门(未启用即返,默认热路径零开销)
    const uint32_t depth = g_pyStackDepth.load(std::memory_order_acquire);
    if (depth == 0)
    {
        return;
    }
    // ② 约束判定: 分配时刻当前存活量近似闭窗unfreed,不达标即返
    const int64_t liveBytes = rec.liveBytes.load(std::memory_order_relaxed);
    if (liveBytes <= 0)
    {
        return;
    }
    // 存活块数=refs-1(pin剥离;在途lookup高估一块)
    const uint64_t liveBlocks = rec.refs.load(std::memory_order_relaxed) - 1;
    if (!GateSatisfied(liveBytes, liveBlocks))
    {
        return;
    }
    // ③ 已采集快速判定(每栈至多一次;acquire与发布桥⑧的release配对,读到CAPTURED
    // 即同步pyBuf;GIL后⑥有权威复查,发布桥锁内有最终判读)
    if (rec.pyState.load(std::memory_order_acquire) != PY_STACK_NONE)
    {
        return;
    }
    // ④ 解释器/版本门(无解释器进程自动退化,闭窗派生NA)
    if (!InterpReady())
    {
        return;
    }
    // ⑤ 重入防护(先于GIL守卫: Ensure/走链的Python API可能触发分配→钩子递归,
    // thread_local标记使递归层直接返回,防栈溢出)
    thread_local static bool t_inCapture = false;
    if (t_inCapture)
    {
        return;
    }
    t_inCapture = true;
    struct ReentryReset
    {
        ~ReentryReset() { t_inCapture = false; }
    } reset;
    // ⑤.5 GIL身份门: 仅绑定过tstate的线程
    if (PyGILState_GetThisThreadState == nullptr || PyGILState_GetThisThreadState() == nullptr)
    {
        return;
    }
    // ⑥ GIL守卫: 未持GIL区间(线程从eval释放GIL进入C++扩展,如torch/aclnn路径)
    // 经PyInterpGuard临时取GIL(Ensure/Release RAII,已持GIL幂等);
    // Ensure经解释器tstate池恢复本线程tstate,current_frame保持,采集语义与持GIL一致
    Utility::PyInterpGuard guard;
    // ⑥后权威复查(③为GIL前预检优化): 全部发布者持GIL,获取GIL后读到CAPTURED
    // 即他人已采(他人发布必然已持GIL)——双采集竞争在GIL处收敛,免走链;最终
    // 判读在发布桥分片锁内(与清表复位/并发发布原子,见SharedStackPublishPyStack)
    if (rec.pyState.load(std::memory_order_acquire) != PY_STACK_NONE)
    {
        return;  // 他人已采: 本次作废(在途ref由调用方释放,见RecordMalloc注释)
    }
    // ⑦ 走链+格式化(GIL下互斥,缓存写者唯一;scratch零堆分配)
    thread_local static std::array<char, PY_SCRATCH_CAP> t_scratch;
    CaptureCtx ctx{t_scratch.data(), t_scratch.size(), 0, &s_frameCache};
    const uint32_t walked = PyStackCore::WalkFrames(depth, AppendFrameCb, &ctx);
    if (walked == 0 || ctx.off == 0)
    {
        return;  // 无帧: 保持NONE,下次再试(NA非终态)
    }
    // ⑧ 发布: RealMalloc pyBuf→发布桥。桥在栈分片锁内做最终pyState判读并原子
    // 写入pyBuf/pyLen/pyState(release store): 锁内判读与写入同一临界区,双采集
    // 至多一个发布成功;与ClearTables在途条目复位(同锁)互斥,写绝不与释放/复位
    // 并发。采集期持在途ref(RecordMalloc dispose后移),条目恒存活,写无UAF
    char* pyBuf = static_cast<char*>(SharedRealAlloc(ctx.off + 1));
    if (pyBuf == nullptr)
    {
        return;  // 真OOM: 保持NONE,下次再试
    }
    memcpy(pyBuf, t_scratch.data(), ctx.off);
    pyBuf[ctx.off] = '\0';
    if (!SharedStackPublishPyStack(rec, pyBuf, static_cast<uint32_t>(ctx.off)))
    {
        SharedRealFree(pyBuf);  // 锁内终判已被采集: 释放本次缓冲(防pyBuf泄漏)
    }
}

void PyStackCapture::Shutdown()
{
    // 先复位配置门(阻断新采集),再清缓存(互斥锁等in-flight采集的格式化完成)
    g_pyStackDepth.store(0, std::memory_order_release);
    s_frameCache.Shutdown();
}

bool PyStackCapture::AppendMixedStack(std::string& frameDesc, const StackRecord& rec, uint64_t unfreedBytes,
                                      uint64_t unfreedCount)
{
    // 采集态: 追加marker+pyBuf(acquire读CAPTURED⇒pyBuf写完整可见)
    if (rec.pyState.load(std::memory_order_acquire) == PY_STACK_CAPTURED && rec.pyBuf != nullptr && rec.pyLen > 0)
    {
        frameDesc.append(kMixedMarker);
        frameDesc.append(rec.pyBuf, rec.pyLen);
        return true;
    }
    // 未采集: 配置门(本窗口启用过py采集)且闭窗精确unfreed判据达标→派生NA
    // (字节钳制: uint64超INT64_MAX视为INT64_MAX,判据必然成立)
    if (g_pyStackDepth.load(std::memory_order_acquire) != 0 &&
        rec.pyState.load(std::memory_order_relaxed) == PY_STACK_NONE)
    {
        const int64_t lb =
            unfreedBytes > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(unfreedBytes);
        if (GateSatisfied(lb, unfreedCount))
        {
            frameDesc.append(kMixedMarker);
            frameDesc.append("NA");
            frameDesc.push_back('\n');
            return true;
        }
    }
    return false;
}

bool PyStackCapture::AppendMixedStack(std::string& frameDesc, uint32_t pyState, const char* pyText, size_t pyLen,
                                      uint64_t unfreedBytes, uint64_t unfreedCount)
{
    // 采集态: 追加marker+pyBuf(行内拷贝,快照冻结内锁内完成,无发布桥竞争)
    if (pyState == PY_STACK_CAPTURED && pyText != nullptr && pyLen > 0)
    {
        frameDesc.append(kMixedMarker);
        frameDesc.append(pyText, pyLen);
        return true;
    }
    // 未采集: 配置门(本窗口启用过py采集)且快照精确unfreed判据达标→派生NA
    if (g_pyStackDepth.load(std::memory_order_acquire) != 0 && pyState == PY_STACK_NONE)
    {
        const int64_t lb =
            unfreedBytes > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(unfreedBytes);
        if (GateSatisfied(lb, unfreedCount))
        {
            frameDesc.append(kMixedMarker);
            frameDesc.append("NA");
            frameDesc.push_back('\n');
            return true;
        }
    }
    return false;
}

}  // namespace hostmem
