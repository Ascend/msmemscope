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

#ifndef HOST_LEAK_ANALYZER_H
#define HOST_LEAK_ANALYZER_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "event.h"
#include "event_dispatcher.h"
#include "host_mem_hooks/host_mem_hooks.h"

namespace MemScope
{

/*
 * HostLeakAnalyzer(host堆内存泄漏检测-单账本+闭窗快照的分析器)
 * 主要功能:
 * 1. 订阅SYSTEM事件——窗口状态只由STAGE事件驱动(分析器不得在config回调里自行开窗,
 *    窗口边界以钩子上报的HOST_LEAK_STAGE_START/END系统事件为准,与事件流水线天然有序)
 * 2. 闭窗(STAGE_END)时经EventReport的SVC桥拉取冻结快照(钩子块表是唯一账本,无事件流):
 *    get_stats(全局申请/释放累计、溢出通道/开窗前free统计、采样率、截断标注) +
 *    dump_stack_stats(per-stack聚合+符号化文本) + dump_size_distribution(大小排布桶) +
 *    dump_pre_window_distribution(开窗前free大小排布桶);event模式额外
 *    dump_live_blocks(逐块明细)。窗口期间分析器不维护任何逐块/逐栈状态
 * 3. 报告经ofstream直接写出(不复用Dump/CsvHandler基础设施):
 *    <output>/host_leak/leak_overview_<stage>.txt(泄漏概览报告,两种模式均输出:
 *    数据健康度分析/总泄漏量/泄漏块大小排布/开窗前free大小排布/TOP N泄漏点调用栈) +
 *    <output>/host_leak/block_detail_<stage>.csv(逐块明细,仅event模式,
 *    栈串经stack_id关联同窗概览报告)
 * 4. 窗口仍开时进程退出→析构兜底:dump_*仅窗口关闭态可调(ABI约束,热路径未冻结),
 *    只能取get_stats尽力而为值出退化概览(标注窗口未正常关闭/快照不完整)
 *
 * 诚实性契约:块表是唯一账本,无丢包/完整度/校准概念;追踪策略(块阈值/采样率/
 * 键深/溢出通道降级/截断)在概览"数据健康度分析"章节如实标注
 */
class HostLeakAnalyzer
{
   public:
    static HostLeakAnalyzer& GetInstance();
    // dispatcher入口(经bind回调的C包装层→SendEvent→EventRouter→EventDispatcher同步进入,
    // processMutex_已串行化;内部另有mutex_防御UT直调并发)
    void EventHandle(std::shared_ptr<EventBase>& event, MemoryState* state);
    void Subscribe();
    void UnSubscribe() const;

   private:
    struct WindowState;

   private:
    HostLeakAnalyzer();
    ~HostLeakAnalyzer();
    HostLeakAnalyzer(const HostLeakAnalyzer&) = delete;
    HostLeakAnalyzer& operator=(const HostLeakAnalyzer&) = delete;
    HostLeakAnalyzer(HostLeakAnalyzer&&) = delete;
    HostLeakAnalyzer& operator=(HostLeakAnalyzer&&) = delete;

    // 窗口边界事件处理:STAGE_START开窗(清零窗口状态),STAGE_END闭窗拉快照出报告;
    // stageId经SystemEvent::name承载(std::to_string(stageId))
    void HandleStageStart(std::shared_ptr<EventBase>& event);
    void HandleStageEnd(std::shared_ptr<EventBase>& event);
    // 报告输出:atExit=true为析构兜底路径(窗口未闭,dump_*不可调,仅stats尽力而为值,
    // 报告标注不完整;正常路径atExit=false且快照齐备)
    void WriteWindowReport(uint64_t pid, WindowState& ws, bool atExit);
    // dump_*投影收集回调(纯C签名,ctx为对应收集向量*)
    static void CollectStackStatsCb(void* ctx, uint64_t stackId, uint64_t allocCount, uint64_t allocBytes,
                                    uint64_t freedCount, uint64_t freedBytes, uint64_t unfreedCount,
                                    uint64_t unfreedBytes, uint64_t maxBlockSize, const char* frameDesc, size_t len);
    static void CollectSizeDistCb(void* ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount,
                                  uint64_t blockBytes);
    static void CollectLiveBlockCb(void* ctx, uint64_t addr, uint64_t size, uint64_t allocTs, uint64_t stackId);

   private:
    // 闭窗快照per-stack行(dump_stack_stats投影;frameDesc=闭窗符号化文本,
    // '\n'分隔帧描述,空=未符号化/未知桶)
    struct StackRow
    {
        uint64_t stackId = 0;
        uint64_t allocCount = 0;
        uint64_t allocBytes = 0;
        uint64_t freedCount = 0;
        uint64_t freedBytes = 0;
        uint64_t unfreedCount = 0;
        uint64_t unfreedBytes = 0;
        uint64_t maxBlockSize = 0;
        std::string frameDesc;
    };
    // 闭窗大小排布桶(dump_size_distribution投影;rangeLow含、rangeHigh不含,末桶
    // rangeHigh=UINT64_MAX;与per-stack聚合同一次闭窗遍历得出)
    struct SizeBucket
    {
        uint64_t rangeLow = 0;
        uint64_t rangeHigh = 0;
        uint64_t blockCount = 0;
        uint64_t blockBytes = 0;
    };
    // 逐块明细行(dump_live_blocks投影,block_detail CSV数据源,仅event模式)
    struct LiveBlock
    {
        uint64_t addr = 0;
        uint64_t size = 0;
        uint64_t allocTs = 0;
        uint64_t stackId = 0;
    };
    // 单pid窗口状态(实际进程内host事件恒为单pid,per-pid隔离为防御性设计)
    struct WindowState
    {
        bool open = false;
        uint64_t stageId = 0;
        uint64_t startTs = 0;
        uint64_t endTs = 0;
        // statsAvailable=false(bind未就绪/合成事件测试/析构兜底)时统计列以unknown处理
        bool statsAvailable = false;
        MsmemscopeHostmemStats stats{};
        std::vector<StackRow> stacks;     // dump_stack_stats投影(含stackId=0未知桶行)
        std::vector<SizeBucket> buckets;  // dump_size_distribution投影
        // dump_pre_window_distribution投影:开窗前free(窗口外分配)按大小归桶,
        // 与buckets同构(范围口径一致),闭窗时随buckets一并拉取
        std::vector<SizeBucket> preWindowBuckets;
        std::vector<LiveBlock> blocks;  // dump_live_blocks投影(event模式)
    };

    std::unordered_map<uint64_t /*pid*/, WindowState> windows_;
    // timed_mutex:退出期逃生——dispatch链持锁调用本分析器handler,任何上游(含
    // 本分析器内)持锁死锁都会让事件处理永久阻塞→上报线程挂起→进程退出挂起。
    // EventHandle用try_lock_for(15s)超时跳过本次处理(STAGE事件丢失,由
    // ~HostLeakAnalyzer析构兜底),进程得以退出;正常路径锁竞争毫秒级,15s不可达
    mutable std::timed_mutex mutex_;
    // 构造进程pid(fork子进程守卫:析构时与当前pid不一致=COW继承自父进程的实例,
    // 跳过全部清理——见析构函数注释)
    const uint64_t createPid_{0};
};

}  // namespace MemScope

#endif
