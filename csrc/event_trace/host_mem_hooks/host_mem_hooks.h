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

#ifndef MSMEMSCOPE_HOST_MEM_HOOKS_H
#define MSMEMSCOPE_HOST_MEM_HOOKS_H

#include <stddef.h>
#include <stdint.h>

/*
 * host堆内存钩子 ABI
 *
 * 钩子so（libmsmemscope_host_mem_hook.so，LD_PRELOAD劫持malloc族）与libascend_leaks
 * 之间只通过本头文件定义的纯C ABI函数指针表交互，禁止跨so解析任何C++符号：
 *   - API表：libascend_leaks实现并经bind注册，钩子调用（窗口边界上报/抑制查询/参数获取）
 *   - SVC表：钩子实现，bind返回给libascend_leaks（窗口开关/统计/闭窗快照拉取）
 *
 * 架构：单账本+闭窗快照。
 *   - 热路径（业务线程内同步）：malloc→块表插入+栈计数自增；free→块表删除+栈计数自减。
 *     无环形缓冲、无上报线程、无事件流——块表是唯一账本，账本不丢数据；
 *   - 闭窗（set_enabled(false)，调用线程同步执行）：遍历块表→per-stack未释放精确聚合
 *     +大小排布桶；遍历栈表→per-stack申请/释放计数；符号化（帧缓存+稳态预热）；
 *     发STAGE_END；分析器经dump_*拉快照出报告；
 *   - 诚实性契约：默认全量（块阈值0、采样1），显式采样/溢出通道降级/整窗截断标注，
 *     不做逐块丢包（块表满转溢出账本照常记账）。
 *
 * ABI清单：
 *   - report_malloc/report_free/report_stack等逐事件上报不存在——分析器不消费事件流，
 *     闭窗时经dump_*拉冻结快照；
 *   - params.sampleRate为显式采样率倒数（默认1=不采样）；
 *   - stats含全局申请/释放累计与truncated整窗截断标注（无丢包概念）；
 *   - SVC含dump_stack_stats/dump_size_distribution/dump_pre_window_distribution
 *     （闭窗快照拉取）；dump_live_blocks的emit带allocTs（block_detail CSV列）；
 *   - 诊断导出符号见文件尾声明（不依赖bind，供外部控制通道/调试器/测试用）。
 *
 * 溢出通道：块表满不再停止记账——申请转出至按地址散列的溢出账本（addr→size，
 * 闭窗快照精确聚合），truncated bit0语义为"块表触顶→溢出通道继续记账，仅归因
 * 粒度退化"；溢出账本自身有容量上限（MSMEMSCOPE_HOSTMEM_MAX_OVERFLOW，默认1M），
 * 触顶置truncated bit2（记账停止，窗口为截断点前的完整前缀）；新键转未知栈桶为
 * bit1。free未命中块表与溢出账本视为开窗前释放，独立通道stats.preWindowFreeCount/
 * Bytes + dump_pre_window_distribution大小分布交付（malloc_usable_size近似单块
 * 大小），不并入totalFreed（保持"申请=释放+未释放"不变量）。自旋/trylock瞬时失败
 * 不置任何截断位（静默跳过，见实现内注释）。
 *
 * 栈表容量与死栈淘汰：栈表有分片容量上限（默认400k，环境变量可调）。实测满表时
 * 约64%条目为死栈（申请已全部释放、无存活块），故表满时优先执行死栈回收——
 * 桶采样（8个随机桶）找出refs==1（仅pin、零存活块零在途）且申请次数最小的条目
 * 淘汰腾位，折叠其计数并入未知桶行（行求和==全局合计的闭合关系保持，诚实性零
 * 损失）；采样无可淘汰者（全活表）时新键才转未知桶（truncated bit1）。回收依赖
 * 栈条目的引用计数refs（=在表pin+存活块数+在途lookup数，实现内注释详述契约）：
 * 块在表即持其ref，淘汰判读refs==1与块表持指针路径（refs>=2）互斥，指针永不
 * 悬垂。被淘汰条目经stats.evictedStackCount/AllocCount/AllocBytes交付统计。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /* 钩子侧运行参数快照（libascend_leaks按当前生效配置填充，开窗时经get_params获取） */
    typedef struct MsmemscopeHostmemParams
    {
        uint32_t stackDepth; /* 调用栈采集深度（config.cStackDepth，默认50，上限1000） */
        /* 显式采样率倒数（2的幂：1/2/4/...，默认1=不采样；config.sampleRate）。
         * 采样门控在记账（栈捕获/块表插入/计数）之前判定，被跳过块对钩子完全不可见
         * （无块表条目、无计数），报告标注sampled=1/N的采样视图 */
        uint32_t sampleRate;
        uint64_t blockThreshold; /* 块大小阈值（字节），size<该值的分配不采集（默认0=全部采集，显式>0按字节过滤） */
    } MsmemscopeHostmemParams;

    /* API表：libascend_leaks实现并注册，钩子调用 */
    typedef struct MsmemscopeHostmemApi
    {
        /* 窗口边界系统事件：isStart=true开窗（set_enabled调用线程同步发出，开窗清理+STAGE_START
         * 先于记账门控置1）/false闭窗（set_enabled调用线程完成闭窗聚合后同步发出），
         * stageId即windowId。窗口生命周期事件是分析器唯一的窗口驱动 */
        void (*report_stage)(int isStart, uint64_t timestamp, uint64_t stageId);
        /* 采集库内部线程是否处于抑制状态（EventReportSuppressor，防钩子递归上报）；
         * 可为NULL（采集库不提供时钩子只依赖自身抑制守卫） */
        int (*is_suppressed)(void);
        /* 运行参数快照（栈深度/采样率/块阈值），钩子开窗时调用一次；可为NULL（用默认值） */
        void (*get_params)(MsmemscopeHostmemParams* params);
    } MsmemscopeHostmemApi;

    /* 钩子侧统计（get_stats输出） */
    typedef struct MsmemscopeHostmemStats
    {
        /* 当前块表存活块数（窗口关闭态为冻结值；块表触顶后申请转溢出通道照常记账，
         * 该值不随之增加——仅溢出通道也触顶（truncated bit2）才停止记账，此后恒为截断点值） */
        uint64_t liveBlockCount;
        /* 本窗口内经记账门控的申请/释放累计（窗口打开时清零；未释放=alloc-freed，
         * per-stack未释放的精确真源是块表闭窗聚合，见dump_stack_stats） */
        uint64_t totalAllocCount;
        uint64_t totalAllocBytes;
        uint64_t totalFreedCount;
        uint64_t totalFreedBytes;
        /* size<块阈值的分配数/字节（仅blockThreshold>0时非零；阈值=0全量无未追踪） */
        uint64_t untrackedCount;
        uint64_t untrackedBytes;
        /* 溢出通道（块表满降级）：块表满无法入表的申请转出至溢出账本。
         * alloc=本窗口累计转出（已并入totalAllocCount/Bytes，全局合计口径一致）；
         * freed=free命中溢出账本逆向修正的累计释放（已并入totalFreedCount/Bytes）；
         * 存活=alloc-freed（溢出账本自身不变量，闭窗快照精确成立） */
        uint64_t overflowAllocCount;
        uint64_t overflowAllocBytes;
        uint64_t overflowFreedCount;
        uint64_t overflowFreedBytes;
        /* 开窗前free（窗口外分配，独立通道）：free命中块表与溢出账本之外的
         * 事件——申请时间在开窗前/记账被跳过，按开窗前分配统计（次数/总量/大小分布经
         * dump_pre_window_distribution交付）。不并入totalFreed（窗口外分配不入账本，
         * 并入会破坏"申请=释放+未释放"不变量） */
        uint64_t preWindowFreeCount;
        uint64_t preWindowFreeBytes;
        /* 生效采样率倒数（1=不采样；配置值开窗时归一化为2的幂） */
        uint32_t sampleRate;
        /* 整窗标注：bit0=块表触顶（申请转溢出通道照常记账，仅归因粒度退化为整体；
         * 不再停止记账）。bit1=栈表触顶且死栈回收无法腾位（死栈回收已激活：表满时
         * 优先淘汰refs==1的零存活块条目；采样无可淘汰者=全活表时新键才转未知桶
         * stackId=0照常记账，归因粒度退化）。
         * bit2=溢出通道触顶（块表与溢出账本均满，记账停止——窗口数据为截断点前的
         * 完整前缀，此后申请对钩子不可见，free落入开窗前通道） */
        uint32_t truncated;
        /* 死栈淘汰（栈表满时回收，见钩子实现EvictDeadStackLocked）：本窗口被淘汰
         * 条目数与折叠的申请计数/字节（折叠已并入未知桶行，行求和==全局合计的闭合
         * 关系保持）。仅统计展示用，不参与合计算术 */
        uint64_t evictedStackCount;
        uint64_t evictedAllocCount;
        uint64_t evictedAllocBytes;
    } MsmemscopeHostmemStats;

    /* SVC表：钩子实现，bind返回给libascend_leaks */
    typedef struct MsmemscopeHostmemSvc
    {
        /* 窗口开关（幂等，先比较后生效）：
         * true=清零块表/栈表→发STAGE_START→记账门控置1（调用线程执行）；
         * false=记账门控置0→闭窗聚合（遍历块表/栈表/符号化，调用线程同步完成）
         * →发STAGE_END。窗口关闭态下dump_*可用 */
        void (*set_enabled)(int enabled);
        /* 统计查询（窗口关闭态调用无争用；开启态为尽力而为值） */
        void (*get_stats)(MsmemscopeHostmemStats* stats);
        /* 存活块全量投影（block_detail数据源）：对块表每个条目调用emit(ctx, addr, size, allocTs, stackId)。
         * 仅窗口关闭态调用（热路径冻结、块表为冻结真相）；emit内部不得回调钩子 */
        void (*dump_live_blocks)(void (*emit)(void* ctx, uint64_t addr, uint64_t size, uint64_t allocTs,
                                              uint64_t stackId),
                                 void* ctx);
        /* 闭窗栈统计快照（leak_overview数据源）：对每个栈调用emit一行，unfreed系列为闭窗
         * 块表遍历聚合的精确值（真源是块表），alloc/freed系列为栈计数器值；
         * frameDesc为闭窗符号化文本（'\n'分隔帧描述；stackId=0为未知桶行，frameDesc为空）。
         * 仅窗口关闭态调用（闭窗聚合已就绪）；emit内部不得回调钩子 */
        void (*dump_stack_stats)(void (*emit)(void* ctx, uint64_t stackId, uint64_t allocCount, uint64_t allocBytes,
                                              uint64_t freedCount, uint64_t freedBytes, uint64_t unfreedCount,
                                              uint64_t unfreedBytes, uint64_t maxBlockSize, const char* frameDesc,
                                              size_t len),
                                 void* ctx);
        /* 闭窗大小排布（leak_overview数据源）：对每个桶调用emit(ctx, rangeLow, rangeHigh,
         * blockCount, blockBytes)，rangeLow含、rangeHigh不含（末桶rangeHigh=UINT64_MAX）。
         * 与per-stack聚合同一次闭窗遍历得出；仅窗口关闭态调用 */
        void (*dump_size_distribution)(void (*emit)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh,
                                                    uint64_t blockCount, uint64_t blockBytes),
                                       void* ctx);
        /* 开窗前free大小排布快照：本窗口free未命中块表与溢出账本（开窗前分配/记账被跳过）的
         * 事件按大小归桶（大小经malloc_usable_size近似，解析失败为0），emit签名与
         * dump_size_distribution一致。仅窗口关闭态调用 */
        void (*dump_pre_window_distribution)(void (*emit)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh,
                                                          uint64_t blockCount, uint64_t blockBytes),
                                             void* ctx);
    } MsmemscopeHostmemSvc;

    /*
     * bind注册：libascend_leaks初始化时经dlsym(RTLD_DEFAULT, "msmemscope_hostmem_bind")调用。
     * api表指针由钩子so静态保存（bind幂等，重复调用覆盖）；返回SVC表指针，
     * api为NULL或必备回调（report_stage）缺失时返回NULL，调用方据此回退为host-leak功能不可用。
     */
    const MsmemscopeHostmemSvc* msmemscope_hostmem_bind(const MsmemscopeHostmemApi* api);

    /* 诊断导出（外部控制通道/调试器/测试用，extern "C"，不依赖bind） */
    /* 未归因计数（栈层健康度）：栈表满且死栈回收无法腾位（全活表）/trylock失败/OOM
     * 转未知桶（stackId=0）继续记账的块数，跨窗口累计，调用方自行差分。与块表截断分离：
     * 块表截断=整窗停止记账（truncated bit0），此处=单块归因粒度退化为未知桶，账本未失真 */
    uint64_t msmemscope_hostmem_get_unattributed_count(void);
    /* 窗口状态标志位:bit0=已bind bit1=窗口开启 bit2=关闭中 bit3=符号化预热线程已创建
     * bit4=宿主main已进 bit5=退出期(exit已进/main已返,记账门控已关)
     * bit6=fork后代(不监控,此后一切开窗被拒；fork+exec的子进程经exec重置数据段不受影响) */
    int msmemscope_hostmem_get_status(void);

#ifdef __cplusplus
}
#endif

#endif /* MSMEMSCOPE_HOST_MEM_HOOKS_H */
