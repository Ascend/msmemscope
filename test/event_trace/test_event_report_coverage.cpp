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
// event_report.cpp 行覆盖率补充用例。
//
// 分工原则：
//   1) 主进程用例（EventReportCoverageTest）只驱动与运行环境无关的分支：上报入口的
//      早退判定、事件字段填充、host 地址簿的去重/属主校验、窗口开关与 bind 桥、
//      退出闭窗的轮询分支。主进程的 EventReport/GetDeviceInfo 是进程级单例，dcmi/env
//      探测的 once_flag 已被更早的用例消费，因此这里不对 dcmi 查询结果做断言。
//   2) dcmi/env 探测的各种符号在位/缺位组合、EventReport 构造期分支、bind 表接受/拒绝、
//      未初始化进程的退出处理，都是"每个进程只能发生一次"的路径，统一经 fork+exec
//      子进程驱动（复用 test_kernel_hooks.cpp 的 dlopen/dlsym 桩与 RunMemScopeTestChild）。
//
// 说明：DcmiInitHasHangRisk 命中的环境（非容器且 /usr/bin/systemd-detect-virt 不可执行）
// 下 dcmi_init 会被设计性短路，相关用例在该环境跳过——这是被测代码的预期降级路径。
#include <gtest/gtest.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// Python.h 的正式入口：本 TU 的 OOM 快照用例需直接初始化解释器（Py_Initialize/Py_IsInitialized）
#include "cpython.h"

#define private public
#include "event_trace/trace_manager/event_trace_manager.h"
#include "event_trace/event_report.h"
#include "memory_state_manager.h"
#undef private

#include "bit_field.h"
#include "config_info.h"
#include "event_dispatcher.h"
#include "event_trace/vallina_symbol.h"
#include "process.h"
#include "record_info.h"
#include "ustring.h"
#include "utils.h"

using namespace MemScope;

// 符号劫持桩（定义于 test_kernel_hooks.cpp，与 test_kernel_excute_trace.cpp 同款复用）
extern std::unordered_map<std::string, void *> g_funcMocks;  // NOLINT
extern bool g_isDlsymNullptr;
bool RunMemScopeTestChild(const char *gtestFilter);

// event_report.cpp 内部符号（无头文件声明，仅供本用例直达内部实现）
namespace MemScope
{
MemPageType GetMemPageType(unsigned long long flag);
int QueryHostMemStatus();
extern int (*g_hostMemStatusFn)(void);
extern "C" void msmemscope_hostmem_report_stage(int isStart, uint64_t timestamp, uint64_t stageId);
extern "C" int msmemscope_hostmem_is_suppressed(void);
extern "C" void msmemscope_hostmem_log(int severity, const char *fmt, va_list args);
extern "C" void msmemscope_hostmem_get_params(MsmemscopeHostmemParams *params);
extern "C" void msmemscope_hostmem_exit_close(void);
}  // namespace MemScope

namespace
{
constexpr const char *kCovDir = "./memscope_ut_er_cov";
constexpr const char *kCovChildDir = "./memscope_ut_er_cov_child";

// flag 位域常量（与 event_report.cpp 内部常量同源）
constexpr uint64_t MEM_VIRT_BIT_POS = 10;
constexpr uint64_t MEM_SPACE_SVM = 0x0ULL << MEM_VIRT_BIT_POS;
constexpr uint64_t MEM_SPACE_DEV = 0x1ULL << MEM_VIRT_BIT_POS;
constexpr uint64_t MEM_SPACE_HOST = 0x2ULL << MEM_VIRT_BIT_POS;
constexpr uint64_t MEM_SPACE_DVPP = 0x3ULL << MEM_VIRT_BIT_POS;
constexpr uint64_t MEM_SPACE_BAD = 0x4ULL << MEM_VIRT_BIT_POS;  // 非法类型：走 default 告警分支
constexpr uint64_t MEM_PAGE_GIANT_MASK = 1ULL << 31;
constexpr uint64_t MEM_PAGE_HUGE_MASK = 1ULL << 17;
constexpr uint64_t MEM_MODULE_SHIFT = 56;

uint64_t MakeHalFlag(uint64_t space, int32_t devId, uint32_t moduleId, bool giantPage = false, bool hugePage = false)
{
    uint64_t flag = space | (static_cast<uint64_t>(devId) & 0x3FFULL);
    flag |= (static_cast<uint64_t>(moduleId) & 0xFFULL) << MEM_MODULE_SHIFT;
    if (giantPage)
    {
        flag |= MEM_PAGE_GIANT_MASK;
    }
    if (hugePage)
    {
        flag |= MEM_PAGE_HUGE_MASK;
    }
    return flag;
}

drv_mem_prop MakeProp(int32_t devId, uint32_t moduleId, uint32_t pgType)
{
    drv_mem_prop prop{};
    prop.side = 0;
    prop.devid = static_cast<uint32_t>(devId);
    prop.module_id = moduleId;
    prop.pg_type = pgType;
    prop.mem_type = 0;
    prop.reserve = 0;
    return prop;
}

CallStackString MakeStack()
{
    CallStackString stack;
    stack.cStack = "cov_c_stack";
    stack.pyStack = "cov_py_stack";
    return stack;
}

// 全量采集配置：所有卡 + 所有 CPU 空间 + 四类事件 + 两级 level
Config MakeCovConfig()
{
    Config config{};
    BitField<decltype(config.eventType)> eventBits;
    eventBits.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBits.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBits.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    eventBits.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    config.eventType = eventBits.getValue();
    BitField<decltype(config.levelType)> levelBits;
    levelBits.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    levelBits.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    config.levelType = levelBits.getValue();
    config.collectAllNpu = true;
    config.collectCpu = true;
    config.cStackDepth = 50;
    config.pyStackDepth = 8;
    config.enablePyStack = true;
    config.sampleRate = 1;
    config.logLevel = static_cast<uint8_t>(LogLv::ERROR);
    std::snprintf(config.outputDir, sizeof(config.outputDir), "%s", kCovDir);
    return config;
}

// config_ 直写：绕开 SetConfig 的落盘/ATen/Triton/分析器联动副作用（那些副作用由其它用例覆盖）。
// 加 ConfigManager::mutex_ 避免与事件上报线程读配置构成数据竞争
void SetCovConfig(const Config &config)
{
    ConfigManager &manager = ConfigManager::Instance();
    std::lock_guard<std::mutex> lock(manager.mutex_);
    manager.config_ = config;
}

Config GetCovConfig()
{
    ConfigManager &manager = ConfigManager::Instance();
    std::lock_guard<std::mutex> lock(manager.mutex_);
    return manager.config_;
}

std::vector<EventBaseType> CapturedTypes()
{
    return {EventBaseType::MALLOC,       EventBaseType::FREE,      EventBaseType::ACCESS,
            EventBaseType::MEMORY_OWNER, EventBaseType::MSTX,      EventBaseType::OP_LAUNCH,
            EventBaseType::KERNEL_LAUNCH, EventBaseType::SYSTEM,   EventBaseType::SNAPSHOT,
            EventBaseType::OOM_DETAIL};
}

// 事件捕获订阅者：挂在废弃占位订阅号上，同步收集上报层发出的事件（含具体子类型，供字段断言）
class CovCaptor
{
   public:
    std::vector<std::shared_ptr<EventBase>> events;

    void Handle(std::shared_ptr<EventBase> &event, MemoryState *state)
    {
        (void)state;
        if (event != nullptr)
        {
            events.push_back(event);
        }
    }

    void Clear() { events.clear(); }

    size_t CountOf(EventBaseType type) const
    {
        size_t count = 0;
        for (const auto &event : events)
        {
            if (event != nullptr && event->eventType == type)
            {
                ++count;
            }
        }
        return count;
    }

    template <typename T>
    std::shared_ptr<T> LastAs(EventBaseType type) const
    {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
        {
            if ((*it)->eventType != type)
            {
                continue;
            }
            auto typed = std::dynamic_pointer_cast<T>(*it);
            if (typed != nullptr)
            {
                return typed;
            }
        }
        return nullptr;
    }
};

// ==================== host 内存钩子 svc mock 表 ====================
std::vector<int> g_covSetEnabledArgs;
uint64_t g_covEmitCount = 0;

void CovMockSetEnabled(int enabled) { g_covSetEnabledArgs.push_back(enabled); }

void CovMockGetStats(MsmemscopeHostmemStats *stats)
{
    if (stats != nullptr)
    {
        std::memset(stats, 0, sizeof(*stats));
    }
}

void CovEmitLiveBlock(void *ctx, uint64_t addr, uint64_t size, uint64_t allocTs, uint64_t stackId)
{
    (void)ctx;
    (void)addr;
    (void)size;
    (void)allocTs;
    (void)stackId;
    ++g_covEmitCount;
}

void CovEmitStack(void *ctx, uint64_t stackId, uint64_t allocCount, uint64_t allocBytes, uint64_t freedCount,
                  uint64_t freedBytes, uint64_t unfreedCount, uint64_t unfreedBytes, uint64_t maxBlockSize,
                  uint64_t maxAllocTsNs, uint64_t freedLifetimeSumNs, uint64_t liveAgeSumNs, const char *frameDesc,
                  size_t len)
{
    (void)ctx;
    (void)stackId;
    (void)allocCount;
    (void)allocBytes;
    (void)freedCount;
    (void)freedBytes;
    (void)unfreedCount;
    (void)unfreedBytes;
    (void)maxBlockSize;
    (void)maxAllocTsNs;
    (void)freedLifetimeSumNs;
    (void)liveAgeSumNs;
    (void)frameDesc;
    (void)len;
    ++g_covEmitCount;
}

void CovEmitSeries(void *ctx, uint64_t stackId, uint32_t beat, uint64_t liveBytes, uint32_t liveCount, uint32_t flags)
{
    (void)ctx;
    (void)stackId;
    (void)beat;
    (void)liveBytes;
    (void)liveCount;
    (void)flags;
    ++g_covEmitCount;
}

void CovEmitSizeDist(void *ctx, uint64_t rangeLow, uint64_t rangeHigh, uint64_t blockCount, uint64_t blockBytes)
{
    (void)ctx;
    (void)rangeLow;
    (void)rangeHigh;
    (void)blockCount;
    (void)blockBytes;
    ++g_covEmitCount;
}

void CovMockDumpLiveBlocks(void (*emit)(void *, uint64_t, uint64_t, uint64_t, uint64_t), void *ctx)
{
    if (emit != nullptr)
    {
        emit(ctx, 0x1000, 16, 0, 0);
    }
}

void CovMockDumpStackStats(void (*emit)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t, uint64_t, uint64_t, const char *, size_t),
                           void *ctx)
{
    if (emit != nullptr)
    {
        emit(ctx, 1, 2, 32, 1, 16, 1, 16, 16, 0, 0, 0, nullptr, 0);
    }
}

void CovMockDumpUnfreedSeries(void (*emit)(void *, uint64_t, uint32_t, uint64_t, uint32_t, uint32_t), void *ctx)
{
    if (emit != nullptr)
    {
        emit(ctx, 1, 0, 16, 1, 0);
    }
}

void CovMockDumpSizeDist(void (*emit)(void *, uint64_t, uint64_t, uint64_t, uint64_t), void *ctx)
{
    if (emit != nullptr)
    {
        emit(ctx, 0, 64, 1, 16);
    }
}

void CovMockDumpPreWindowDist(void (*emit)(void *, uint64_t, uint64_t, uint64_t, uint64_t), void *ctx)
{
    if (emit != nullptr)
    {
        emit(ctx, 0, 64, 1, 16);
    }
}

void CovMockDumpInterimSnapshot(void (*emitStack)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                                  uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, const char *, size_t),
                                void (*emitSizeDist)(void *, uint64_t, uint64_t, uint64_t, uint64_t),
                                void (*emitPreWindow)(void *, uint64_t, uint64_t, uint64_t, uint64_t),
                                void (*emitSeries)(void *, uint64_t, uint32_t, uint64_t, uint32_t, uint32_t),
                                MsmemscopeInterimStats *stats, void *ctx)
{
    if (emitStack != nullptr)
    {
        emitStack(ctx, 1, 2, 32, 1, 16, 1, 16, 16, 0, 0, 0, nullptr, 0);
    }
    if (emitSizeDist != nullptr)
    {
        emitSizeDist(ctx, 0, 64, 1, 16);
    }
    if (emitPreWindow != nullptr)
    {
        emitPreWindow(ctx, 0, 64, 1, 16);
    }
    if (emitSeries != nullptr)
    {
        emitSeries(ctx, 1, 0, 16, 1, 0);
    }
    if (stats != nullptr)
    {
        std::memset(stats, 0, sizeof(*stats));
    }
}

// 8 个成员按 host_mem_hooks.h 中 Svc 表顺序全量初始化（缺省成员为 null 时包装层会提前返回）
const MsmemscopeHostmemSvc g_covMockSvc = {
    CovMockSetEnabled,       CovMockGetStats,          CovMockDumpLiveBlocks, CovMockDumpStackStats,
    CovMockDumpUnfreedSeries, CovMockDumpSizeDist,     CovMockDumpPreWindowDist, CovMockDumpInterimSnapshot,
};

// 退出闭窗轮询信号桩：g_hostMemStatusFn 由用例直接注入，返回状态位（bit2=closing）
int g_covStatusValue = 0;
int g_covStatusCalls = 0;

int MockCovHostMemStatus()
{
    ++g_covStatusCalls;
    if (g_covStatusValue < 0)
    {
        return -1;
    }
    if (g_covStatusValue == 1)
    {
        // 首次轮询报告"仍在关闭"，第二次起报告已排空：驱动等待循环的进度日志与正常退出分支
        return g_covStatusCalls == 1 ? 0x4 : 0x0;
    }
    return g_covStatusValue;
}

void CallHostMemLog(int severity, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    msmemscope_hostmem_log(severity, fmt, args);
    va_end(args);
}

// bind 侧桩：接受时返回 mock svc 表，拒绝时返回 nullptr
bool g_covBindAccept = true;

const MsmemscopeHostmemSvc *MockCovHostMemBind(const MsmemscopeHostmemApi *api)
{
    (void)api;
    return g_covBindAccept ? &g_covMockSvc : nullptr;
}

// ==================== dcmi/aclrt 桩（仅子进程安装） ====================
int g_dcmiInitRet = 0;
int g_cardListRet = 0;
int g_deviceNumRet = 0;
int g_logicIdRet = 0;
int g_cardNum = 1;
int g_deviceNumInCard = 2;
bool g_logicIdHole = false;
int g_hbmRet = 0;
int g_procMemRet = 0;
bool g_procHasSelf = true;
int g_deviceCount = 2;
int g_deviceCountRet = 0;
int g_rtDeviceCountRet = 0;
int g_rtGetDeviceRet = 0;
int32_t g_rtGetDeviceId = 0;

int MockCovDcmiInit()
{
    return g_dcmiInitRet;
}

int MockCovDcmiGetCardList(int *cardNum, int *cardList, int len)
{
    if (cardNum == nullptr || cardList == nullptr)
    {
        return 1;
    }
    if (g_cardListRet != 0)
    {
        return g_cardListRet;
    }
    const int count = (g_cardNum < len) ? g_cardNum : len;
    *cardNum = count;
    for (int i = 0; i < count; ++i)
    {
        cardList[i] = i;
    }
    return 0;
}

int MockCovDcmiGetDeviceNumInCard(int cardId, int *deviceNum)
{
    (void)cardId;
    if (deviceNum == nullptr)
    {
        return 1;
    }
    if (g_deviceNumRet != 0)
    {
        return g_deviceNumRet;
    }
    *deviceNum = g_deviceNumInCard;
    return 0;
}

int MockCovDcmiGetDeviceLogicId(int *logicId, int cardId, int deviceId)
{
    (void)cardId;
    if (logicId == nullptr)
    {
        return 1;
    }
    if (g_logicIdRet != 0)
    {
        return g_logicIdRet;
    }
    // 空洞语义：第 1 号设备映射到逻辑号 2，使表内 {0,2} 缺 1
    *logicId = (g_logicIdHole && deviceId == 1) ? 2 : deviceId;
    return 0;
}

int MockCovDcmiGetHbmInfo(int cardId, int deviceId, struct dcmi_hbm_info *info)
{
    (void)cardId;
    (void)deviceId;
    if (info == nullptr)
    {
        return 1;
    }
    if (g_hbmRet != 0)
    {
        return g_hbmRet;
    }
    info->memory_usage = 100;
    info->memory_size = 1024;
    return 0;
}

int MockCovDcmiGetProcMemInfo(int cardId, int deviceId, struct dcmi_proc_mem_info *info, int *procNum)
{
    (void)cardId;
    (void)deviceId;
    if (info == nullptr || procNum == nullptr)
    {
        return 1;
    }
    if (g_procMemRet != 0)
    {
        return g_procMemRet;
    }
    *procNum = 1;
    const uint64_t selfPid = Utility::GetPid();
    info[0].proc_id = g_procHasSelf ? static_cast<int>(selfPid) : static_cast<int>(selfPid + 1);
    info[0].proc_mem_usage = 4096;
    return 0;
}

aclError MockCovAclrtGetDeviceCount(uint32_t *count)
{
    if (count != nullptr)
    {
        *count = static_cast<uint32_t>(g_deviceCount);
    }
    return g_deviceCountRet;
}

aclError MockCovAclrtGetDevice(int32_t *devId)
{
    if (devId != nullptr)
    {
        *devId = g_rtGetDeviceId;
    }
    return g_rtGetDeviceRet;
}

rtError_t MockCovRtGetDevice(int32_t *devId)
{
    if (devId != nullptr)
    {
        *devId = g_rtGetDeviceId;
    }
    return static_cast<rtError_t>(g_rtGetDeviceRet);
}

rtError_t MockCovRtGetDeviceCount(int32_t *count)
{
    if (count != nullptr)
    {
        *count = g_deviceCount;
    }
    return static_cast<rtError_t>(g_rtDeviceCountRet);
}

void ResetDcmiMockState()
{
    g_dcmiInitRet = 0;
    g_cardListRet = 0;
    g_deviceNumRet = 0;
    g_logicIdRet = 0;
    g_cardNum = 1;
    g_deviceNumInCard = 2;
    g_logicIdHole = false;
    g_hbmRet = 0;
    g_procMemRet = 0;
    g_procHasSelf = true;
    g_deviceCount = 2;
    g_deviceCountRet = 0;
    g_rtDeviceCountRet = 0;
    g_rtGetDeviceRet = 0;
    g_rtGetDeviceId = 0;
    g_covBindAccept = true;
    g_covStatusValue = 0;
    g_covStatusCalls = 0;
}

void InstallDcmiSymbols()
{
    g_funcMocks["dcmi_init"] = reinterpret_cast<void *>(&MockCovDcmiInit);
    g_funcMocks["dcmi_get_card_list"] = reinterpret_cast<void *>(&MockCovDcmiGetCardList);
    g_funcMocks["dcmi_get_device_num_in_card"] = reinterpret_cast<void *>(&MockCovDcmiGetDeviceNumInCard);
    g_funcMocks["dcmi_get_device_logic_id"] = reinterpret_cast<void *>(&MockCovDcmiGetDeviceLogicId);
    g_funcMocks["dcmi_get_device_hbm_info"] = reinterpret_cast<void *>(&MockCovDcmiGetHbmInfo);
    g_funcMocks["dcmi_get_npu_proc_mem_info"] = reinterpret_cast<void *>(&MockCovDcmiGetProcMemInfo);
}

bool IsCovChild()
{
    return std::getenv("MSMEMSCOPE_UT_CHILD") != nullptr;
}

// 与 DcmiInitHasHangRisk 同源的预判：命中时 dcmi_init 被设计性短路，符号桩无法生效
bool DcmiInitWouldRun()
{
    if (access("/.dockerenv", F_OK) == 0)
    {
        return true;
    }
    FILE *fp = std::fopen("/proc/self/cgroup", "r");
    if (fp != nullptr)
    {
        char line[256];
        while (std::fgets(line, sizeof(line), fp) != nullptr)
        {
            if (std::strstr(line, "docker-") != nullptr || std::strstr(line, "docker/") != nullptr)
            {
                std::fclose(fp);
                return true;
            }
        }
        std::fclose(fp);
    }
    return access("/usr/bin/systemd-detect-virt", X_OK) == 0;
}
}  // namespace

// ============================================================================
// 主进程用例
// ============================================================================
class EventReportCoverageTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        Utility::FileCreateManager::GetInstance(kCovDir).SetProjectDir(kCovDir);
        savedConfig_ = GetCovConfig();
        SetCovConfig(MakeCovConfig());

        EventTraceManager &traceManager = EventTraceManager::Instance();
        savedStatus_ = traceManager.status_;
        savedDeviceReady_ = traceManager.deviceReady_.load();
        savedAclInit_ = traceManager.aclInit_.load();
        traceManager.status_ = EventTraceStatus::IN_TRACING;
        traceManager.deviceReady_ = true;

        report_ = &EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
        {
            std::lock_guard<std::mutex> lock(report_->mutex_);
            savedHalPtrs_ = report_->halPtrs_;
            savedHostPtrs_ = report_->hostPtrs_;
            report_->halPtrs_.clear();
            report_->hostPtrs_.clear();
        }
        savedStepInfo_ = report_->stepInfo_;
        report_->stepInfo_ = MstxStepInfo();
        savedPyStepId_ = report_->pyStepId_.load();
        savedKernelIndex_ = report_->kernelLaunchRecordIndex_.load();
        savedRecordIndex_ = report_->recordIndex_.load();
        savedDestroyed_ = report_->destroyed_.load();
        savedSvc_ = report_->svcHostMem_;
        savedLatch_ = report_->hostMemStopLatch_.load();
        savedDeviceWarned_ = report_->deviceUsedQueryWarned_.exchange(false);
        savedProcWarned_ = report_->processUsedQueryWarned_.exchange(false);
        report_->svcHostMem_ = nullptr;
        report_->hostMemStopLatch_.store(false);

        ResetStateManagerCaches();

        // 借用废弃占位订阅号(HAL_ANALYZER)做事件捕获：Subscribe 不会替换同号已有订阅者，
        // 因此绝不能占用 DUMP 等真实订阅号，否则会把 Dump 分析器从分发链上摘掉且无法恢复
        EventDispatcher::GetInstance().UnSubscribe(SubscriberId::HAL_ANALYZER);
        auto func = std::bind(&CovCaptor::Handle, &captor_, std::placeholders::_1, std::placeholders::_2);
        EventDispatcher::GetInstance().Subscribe(SubscriberId::HAL_ANALYZER, CapturedTypes(),
                                                 EventDispatcher::Priority::Lowest, func, "er_cov");
    }

    void TearDown() override
    {
        EventDispatcher::GetInstance().UnSubscribe(SubscriberId::HAL_ANALYZER);
        captor_.Clear();
        {
            std::lock_guard<std::mutex> lock(report_->mutex_);
            report_->halPtrs_ = savedHalPtrs_;
            report_->hostPtrs_ = savedHostPtrs_;
        }
        report_->stepInfo_ = savedStepInfo_;
        report_->pyStepId_.store(savedPyStepId_);
        report_->kernelLaunchRecordIndex_.store(savedKernelIndex_);
        report_->recordIndex_.store(savedRecordIndex_);
        report_->destroyed_.store(savedDestroyed_);
        report_->svcHostMem_ = savedSvc_;
        report_->hostMemStopLatch_.store(savedLatch_);
        report_->deviceUsedQueryWarned_.store(savedDeviceWarned_);
        report_->processUsedQueryWarned_.store(savedProcWarned_);

        ResetStateManagerCaches();

        EventTraceManager &traceManager = EventTraceManager::Instance();
        traceManager.status_ = savedStatus_;
        traceManager.deviceReady_ = savedDeviceReady_;
        traceManager.aclInit_ = savedAclInit_;
        SetCovConfig(savedConfig_);
        Utility::FileCreateManager::GetInstance(kCovDir).SetProjectDir("");
    }

    void ResetStateManagerCaches()
    {
        MemoryStateManager &manager = MemoryStateManager::GetInstance();
        std::lock_guard<std::mutex> lock(manager.mtx_);
        manager.halUsed_.clear();
        manager.hostUsed_ = 0;
        manager.hostTensorTotal_ = 0;
        manager.hostPeak_ = 0;
        manager.hostTensorPeak_ = 0;
        manager.halPeak_.clear();
        manager.poolsMap_.clear();
        manager.poolUsage_.clear();
        manager.deviceUsedCache_.fill(-1);
        manager.processUsedCache_.fill(-1);
    }

    // 跳过判定全真配置：stepList 只有 step=5，当前 step=0 且不在 range 内
    void InstallSkipConfig()
    {
        Config config = MakeCovConfig();
        config.stepList.stepCount = 1;
        config.stepList.stepIdList[0] = 5;
        SetCovConfig(config);
    }

    CovCaptor captor_;
    EventReport *report_ = nullptr;

    Config savedConfig_{};
    EventTraceStatus savedStatus_ = EventTraceStatus::NOT_IN_TRACING;
    bool savedDeviceReady_ = false;
    bool savedAclInit_ = false;
    std::unordered_map<uint64_t, int32_t> savedHalPtrs_;
    std::unordered_map<uint64_t, uint8_t> savedHostPtrs_;
    MstxStepInfo savedStepInfo_;
    uint64_t savedPyStepId_ = 0;
    uint64_t savedKernelIndex_ = 0;
    uint64_t savedRecordIndex_ = 0;
    bool savedDestroyed_ = false;
    const MsmemscopeHostmemSvc *savedSvc_ = nullptr;
    bool savedLatch_ = false;
    bool savedDeviceWarned_ = false;
    bool savedProcWarned_ = false;
};

// GetMemPageType/GetMemOpSpace 的页类型与内存空间解析（含非法 memType 告警分支）
TEST_F(EventReportCoverageTest, flag_bit_parse_branches)
{
    EXPECT_EQ(GetMemPageType(0), MemPageType::MEM_NORMAL_PAGE_TYPE);
    EXPECT_EQ(GetMemPageType(MEM_PAGE_HUGE_MASK), MemPageType::MEM_HUGE_PAGE_TYPE);
    EXPECT_EQ(GetMemPageType(MEM_PAGE_GIANT_MASK), MemPageType::MEM_GIANT_PAGE_TYPE);
    // giant 优先于 huge
    EXPECT_EQ(GetMemPageType(MEM_PAGE_GIANT_MASK | MEM_PAGE_HUGE_MASK), MemPageType::MEM_GIANT_PAGE_TYPE);

    EXPECT_EQ(GetMemOpSpace(MEM_SPACE_SVM), MemOpSpace::SVM);
    EXPECT_EQ(GetMemOpSpace(MEM_SPACE_DEV), MemOpSpace::DEVICE);
    EXPECT_EQ(GetMemOpSpace(MEM_SPACE_HOST), MemOpSpace::HOST);
    EXPECT_EQ(GetMemOpSpace(MEM_SPACE_DVPP), MemOpSpace::DVPP);
    EXPECT_EQ(GetMemOpSpace(MEM_SPACE_BAD), MemOpSpace::INVALID);
}

// HAL Create/Release 正常路径：字段填充 + halPtrs_ 注册与回填 + 未注册地址过滤
TEST_F(EventReportCoverageTest, hal_create_release_normal)
{
    const uint64_t addr = 0xC0DE0001ULL;
    drv_mem_prop prop = MakeProp(0, 7, static_cast<uint32_t>(MemPageType::MEM_HUGE_PAGE_TYPE));
    EXPECT_TRUE(report_->ReportHalCreate(addr, 1024, prop, MakeStack()));
    ASSERT_TRUE(report_->halPtrs_.count(addr) == 1);
    EXPECT_EQ(report_->halPtrs_[addr], 0);

    auto mallocEvent = captor_.LastAs<MemoryEvent>(EventBaseType::MALLOC);
    ASSERT_NE(mallocEvent, nullptr);
    EXPECT_EQ(mallocEvent->eventSubType, EventSubType::HAL);
    EXPECT_EQ(mallocEvent->poolType, PoolType::HAL);
    EXPECT_EQ(mallocEvent->addr, addr);
    EXPECT_EQ(mallocEvent->size, 1024);
    EXPECT_EQ(mallocEvent->device, 0);
    EXPECT_EQ(mallocEvent->moduleId, 7);
    EXPECT_EQ(mallocEvent->pageType, MemPageType::MEM_HUGE_PAGE_TYPE);
    EXPECT_EQ(mallocEvent->space, MemOpSpace::DEVICE);
    EXPECT_EQ(mallocEvent->flag, FLAG_INVALID);
    EXPECT_FALSE(mallocEvent->isShadowEvent);
    EXPECT_EQ(mallocEvent->cCallStack, "cov_c_stack");
    EXPECT_EQ(mallocEvent->pyCallStack, "cov_py_stack");

    EXPECT_TRUE(report_->ReportHalRelease(addr, MakeStack()));
    auto freeEvent = captor_.LastAs<MemoryEvent>(EventBaseType::FREE);
    ASSERT_NE(freeEvent, nullptr);
    EXPECT_EQ(freeEvent->eventSubType, EventSubType::HAL);
    EXPECT_EQ(freeEvent->device, 0);  // 分配时语义回填
    // 上报层置0后，EventHandler→MemoryStateManager 会用配对的MALLOC块回填size(HAL/HOST语义)
    EXPECT_EQ(freeEvent->size, 1024);
    EXPECT_EQ(freeEvent->moduleId, -1);
    EXPECT_FALSE(report_->halPtrs_.count(addr));

    // 未注册地址：静默过滤，不再派发
    const size_t before = captor_.events.size();
    EXPECT_TRUE(report_->ReportHalRelease(0xDEAD0000ULL, MakeStack()));
    EXPECT_EQ(captor_.events.size(), before);
}

// 影子 Create/Free：只维护地址簿，不派发事件
TEST_F(EventReportCoverageTest, hal_create_free_shadow)
{
    const uint64_t addr = 0xC0DE0002ULL;
    drv_mem_prop prop = MakeProp(1, 0, 0);
    captor_.Clear();
    EXPECT_TRUE(report_->ReportHalCreate(addr, 2048, prop));
    EXPECT_EQ(captor_.CountOf(EventBaseType::MALLOC), 0u) << "shadow event must not be dispatched";
    ASSERT_EQ(report_->halPtrs_.count(addr), 1u);
    EXPECT_EQ(report_->halPtrs_[addr], 1);

    EXPECT_TRUE(report_->ReportHalFree(addr));
    EXPECT_EQ(captor_.CountOf(EventBaseType::FREE), 0u);
    EXPECT_FALSE(report_->halPtrs_.count(addr));

    // 未注册地址的影子 FREE：直接过滤
    EXPECT_TRUE(report_->ReportHalFree(addr));
}

// flag 入口 Malloc：DEVICE/HOST/DVPP 三种空间、模块号与页大小解析、HOST 去重
TEST_F(EventReportCoverageTest, hal_malloc_flag_variants)
{
    const uint64_t devAddr = 0xC0DE0010ULL;
    EXPECT_TRUE(report_->ReportHalMalloc(
        devAddr, 4096, MakeHalFlag(MEM_SPACE_DEV, 0, 5, false, true), MakeStack()));
    auto devEvent = captor_.LastAs<MemoryEvent>(EventBaseType::MALLOC);
    ASSERT_NE(devEvent, nullptr);
    EXPECT_EQ(devEvent->eventSubType, EventSubType::HAL);
    EXPECT_EQ(devEvent->space, MemOpSpace::DEVICE);
    EXPECT_FALSE(devEvent->isPinned);
    EXPECT_EQ(devEvent->moduleId, 5);
    EXPECT_EQ(devEvent->size, 4096);

    const uint64_t hostAddr = 0xC0DE0011ULL;
    EXPECT_TRUE(report_->ReportHalMalloc(hostAddr, 128, MakeHalFlag(MEM_SPACE_HOST, 0, 0), MakeStack()));
    auto hostEvent = captor_.LastAs<MemoryEvent>(EventBaseType::MALLOC);
    ASSERT_NE(hostEvent, nullptr);
    EXPECT_EQ(hostEvent->eventSubType, EventSubType::HOST);
    EXPECT_EQ(hostEvent->poolType, PoolType::HOST);
    EXPECT_EQ(hostEvent->device, DEVICE_ID_CPU);
    EXPECT_TRUE(hostEvent->isPinned);
    // used 由 Utility::GetProcessVmRss() 填充，容器环境下可能为 0，故不校验具体数值
    ASSERT_EQ(report_->hostPtrs_.count(hostAddr), 1u);
    EXPECT_EQ(report_->hostPtrs_[hostAddr], 0);  // HOST_PTR_FROM_HAL_PINNED

    // 同一 HOST 地址重复申请：先到先得，静默去重不派发
    const size_t before = captor_.events.size();
    EXPECT_TRUE(report_->ReportHalMalloc(hostAddr, 128, MakeHalFlag(MEM_SPACE_HOST, 0, 0), MakeStack()));
    EXPECT_EQ(captor_.events.size(), before);

    const uint64_t dvppAddr = 0xC0DE0012ULL;
    EXPECT_TRUE(report_->ReportHalMalloc(dvppAddr, 64, MakeHalFlag(MEM_SPACE_DVPP, 0, 0), MakeStack()));
    auto dvppEvent = captor_.LastAs<MemoryEvent>(EventBaseType::MALLOC);
    ASSERT_NE(dvppEvent, nullptr);
    EXPECT_EQ(dvppEvent->space, MemOpSpace::DVPP);

    // 非法 memType：GetMemOpSpace 走 default 告警，事件仍以 INVALID 空间上报
    const uint64_t badAddr = 0xC0DE0013ULL;
    EXPECT_TRUE(report_->ReportHalMalloc(badAddr, 32, MakeHalFlag(MEM_SPACE_BAD, 0, 0), MakeStack()));
    auto badEvent = captor_.LastAs<MemoryEvent>(EventBaseType::MALLOC);
    ASSERT_NE(badEvent, nullptr);
    EXPECT_EQ(badEvent->space, MemOpSpace::INVALID);

    // FREE 回填分配时 device
    EXPECT_TRUE(report_->ReportHalFree(devAddr, MakeStack()));
    auto freeEvent = captor_.LastAs<MemoryEvent>(EventBaseType::FREE);
    ASSERT_NE(freeEvent, nullptr);
    EXPECT_EQ(freeEvent->eventSubType, EventSubType::HAL);
    EXPECT_EQ(freeEvent->device, 0);
    // HOST 地址 free：属主比对本通道，hostPtrs_ 同步清除
    EXPECT_TRUE(report_->ReportHalFree(hostAddr, MakeStack()));
    EXPECT_FALSE(report_->hostPtrs_.count(hostAddr));
    EXPECT_FALSE(report_->halPtrs_.count(hostAddr));
}

// 影子 flag 入口：HOST 去重、CPU 设备判定与 FREE 的 HOST 子类型
TEST_F(EventReportCoverageTest, hal_malloc_free_shadow_flag)
{
    const uint64_t hostAddr = 0xC0DE0020ULL;
    captor_.Clear();
    EXPECT_TRUE(report_->ReportHalMalloc(hostAddr, 64, MakeHalFlag(MEM_SPACE_HOST, 0, 0)));
    EXPECT_EQ(captor_.CountOf(EventBaseType::MALLOC), 0u);
    EXPECT_EQ(report_->halPtrs_[hostAddr], DEVICE_ID_CPU);
    ASSERT_EQ(report_->hostPtrs_.count(hostAddr), 1u);

    // 重复影子 HOST 申请：去重早退
    EXPECT_TRUE(report_->ReportHalMalloc(hostAddr, 64, MakeHalFlag(MEM_SPACE_HOST, 0, 0)));

    EXPECT_TRUE(report_->ReportHalFree(hostAddr));
    EXPECT_EQ(captor_.CountOf(EventBaseType::FREE), 0u);
    EXPECT_FALSE(report_->hostPtrs_.count(hostAddr));
    EXPECT_FALSE(report_->halPtrs_.count(hostAddr));
}

// HostRegister/Unregister：注册派发 + 与 cpu-tensor 通道的属主互斥
TEST_F(EventReportCoverageTest, host_register_unregister_owner_checks)
{
    const uint64_t addr = 0xC0DE0030ULL;
    EXPECT_TRUE(report_->ReportHostRegister(addr, 256, MakeStack()));
    auto event = captor_.LastAs<MemoryEvent>(EventBaseType::MALLOC);
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->eventSubType, EventSubType::HOST);
    EXPECT_TRUE(event->isPinned);
    EXPECT_EQ(event->device, DEVICE_ID_CPU);
    EXPECT_EQ(event->space, MemOpSpace::HOST);
    ASSERT_EQ(report_->hostPtrs_.count(addr), 1u);

    // host-leak 钩子通道占用的地址，cpu-tensor 通道申请/归还应被拒绝
    EXPECT_FALSE(report_->ReportCpuTensor(addr, 256, true, "py"));
    EXPECT_FALSE(report_->ReportCpuTensor(addr, 256, false, "py"));

    EXPECT_TRUE(report_->ReportHostUnregister(addr, MakeStack()));
    auto freeEvent = captor_.LastAs<MemoryEvent>(EventBaseType::FREE);
    ASSERT_NE(freeEvent, nullptr);
    EXPECT_EQ(freeEvent->eventSubType, EventSubType::HOST);
    EXPECT_EQ(freeEvent->device, DEVICE_ID_CPU);
    EXPECT_FALSE(report_->halPtrs_.count(addr));
    EXPECT_FALSE(report_->hostPtrs_.count(addr));

    // cpu-tensor 通道自持地址：HAL 注册去重、HAL 注销查表未命中
    const uint64_t tensorAddr = 0xC0DE0031ULL;
    EXPECT_TRUE(report_->ReportCpuTensor(tensorAddr, 64, true, "py"));
    const size_t before = captor_.events.size();
    EXPECT_TRUE(report_->ReportHostRegister(tensorAddr, 64, MakeStack()));
    EXPECT_EQ(captor_.events.size(), before) << "重复占用必须静默去重";
    EXPECT_TRUE(report_->ReportHostUnregister(tensorAddr, MakeStack()));
    EXPECT_EQ(captor_.events.size(), before) << "halPtrs_ 未登记地址必须在查表处早退";
    EXPECT_TRUE(report_->ReportCpuTensor(tensorAddr, 64, false, "py"));
    EXPECT_FALSE(report_->ReportCpuTensor(tensorAddr, 64, false, "py"));

    // 属主不一致的白盒分支：注销/归还都必须保留他通道的地址簿条目
    const uint64_t mixedAddr = 0xC0DE0032ULL;
    {
        std::lock_guard<std::mutex> lock(report_->mutex_);
        report_->halPtrs_[mixedAddr] = DEVICE_ID_CPU;
        report_->hostPtrs_[mixedAddr] = 1;  // HOST_PTR_FROM_CPU_TENSOR
    }
    EXPECT_TRUE(report_->ReportHostUnregister(mixedAddr, MakeStack()));
    EXPECT_FALSE(report_->halPtrs_.count(mixedAddr));
    EXPECT_EQ(report_->hostPtrs_[mixedAddr], 1) << "属主校验必须拒绝跨通道误擦";

    {
        std::lock_guard<std::mutex> lock(report_->mutex_);
        report_->halPtrs_[mixedAddr] = DEVICE_ID_CPU;
    }
    EXPECT_TRUE(report_->ReportHalFree(mixedAddr, MakeStack()));
    EXPECT_EQ(report_->hostPtrs_[mixedAddr], 1) << "属主校验必须拒绝跨通道误擦";
    {
        std::lock_guard<std::mutex> lock(report_->mutex_);
        report_->hostPtrs_.erase(mixedAddr);
    }
}

// 析构后（destroyed_）各入口不再访问地址簿，但仍不崩溃
TEST_F(EventReportCoverageTest, destroyed_flag_guards)
{
    const uint64_t addr = 0xC0DE0040ULL;
    {
        std::lock_guard<std::mutex> lock(report_->mutex_);
        report_->halPtrs_[addr] = 0;
        report_->hostPtrs_[addr] = 0;
    }
    report_->destroyed_.store(true);
    EXPECT_TRUE(report_->ReportHalRelease(addr, MakeStack()));
    EXPECT_TRUE(report_->ReportHalFree(addr));
    EXPECT_TRUE(report_->ReportHalFree(addr, MakeStack()));
    EXPECT_TRUE(report_->ReportHostUnregister(addr, MakeStack()));
    EXPECT_FALSE(report_->ReportCpuTensor(addr, 32, true, "py"));
    EXPECT_FALSE(report_->ReportCpuTensor(addr, 32, false, "py"));
    EXPECT_FALSE(report_->ReportHostStage(true, 0, 0));

    // Create/Register 的地址簿写入被 destroyed_ 跳过，事件仍按既有语义派发
    const uint64_t newAddr = 0xC0DE0041ULL;
    EXPECT_TRUE(report_->ReportHalCreate(newAddr, 16, MakeProp(0, 0, 0), MakeStack()));
    EXPECT_TRUE(report_->ReportHostRegister(newAddr, 16, MakeStack()));
    EXPECT_FALSE(report_->halPtrs_.count(newAddr));
    EXPECT_FALSE(report_->hostPtrs_.count(newAddr));

    report_->destroyed_.store(false);
    {
        std::lock_guard<std::mutex> lock(report_->mutex_);
        report_->halPtrs_.clear();
        report_->hostPtrs_.clear();
    }
}

// IsNeedSkip 的卡/位图/step 三类判定
TEST_F(EventReportCoverageTest, is_need_skip_branches)
{
    Config config = MakeCovConfig();

    config.collectCpu = false;
    SetCovConfig(config);
    EXPECT_TRUE(report_->IsNeedSkip(DEVICE_ID_CPU));
    config.collectCpu = true;
    SetCovConfig(config);
    EXPECT_FALSE(report_->IsNeedSkip(DEVICE_ID_CPU));

    // 指定卡位图：未置位被跳过，GD_INVALID_NUM 不受位图约束
    config.collectAllNpu = false;
    config.npuSlots = 0;
    SetCovConfig(config);
    EXPECT_TRUE(report_->IsNeedSkip(3));
    EXPECT_FALSE(report_->IsNeedSkip(GD_INVALID_NUM));
    BitField<decltype(config.npuSlots)> slots(config.npuSlots);
    slots.setBit(3);
    config.npuSlots = slots.getValue();
    SetCovConfig(config);
    EXPECT_FALSE(report_->IsNeedSkip(3));
    config.collectAllNpu = true;
    SetCovConfig(config);

    // step 过滤：未命中/命中/不在 range 内
    config.stepList.stepCount = 2;
    config.stepList.stepIdList[0] = 2;
    config.stepList.stepIdList[1] = 4;
    SetCovConfig(config);
    report_->stepInfo_.currentStepId = 1;
    report_->stepInfo_.inStepRange = true;
    EXPECT_TRUE(report_->IsNeedSkip(0));
    report_->stepInfo_.currentStepId = 4;
    EXPECT_FALSE(report_->IsNeedSkip(0));
    report_->stepInfo_.inStepRange = false;
    EXPECT_TRUE(report_->IsNeedSkip(0));

    config.stepList.stepCount = 0;
    SetCovConfig(config);
    report_->stepInfo_.currentStepId = 0;
    EXPECT_FALSE(report_->IsNeedSkip(0));
}

// 全部上报入口的"跳过"早退分支：不派发任何事件，返回值恒为 true
TEST_F(EventReportCoverageTest, skip_paths_return_true)
{
    InstallSkipConfig();
    report_->stepInfo_.currentStepId = 0;
    report_->stepInfo_.inStepRange = false;
    captor_.Clear();

    std::string msg = "cov_mark";
    std::string name = "cov_kernel";
    std::string pyStack = "py";
    std::vector<std::pair<OwnerLevel, std::string>> labels{{OwnerLevel::FRAMEWORK, "PTA"}};
    MemoryUsage usage{};
    usage.deviceIndex = 0;
    usage.ptr = 0xC0DE0050ULL;
    usage.allocSize = 64;
    TaskKey key = std::make_tuple(static_cast<uint16_t>(0), static_cast<uint16_t>(1), static_cast<uint16_t>(2));
    AclnnKernelMapInfo kernelInfo{0, key, "cov_kernel"};
    MemorySnapshotInfo snapshot{};
    snapshot.name = "cov_snapshot";
    OOMTriggerInfo trigger{};
    trigger.deviceId = 0;
    OOMMemRecord record{};
    record.deviceId = 0;

    EXPECT_TRUE(report_->ReportAddrInfo(EventSubType::DESCRIBE_OWNER, 0xC0DE0051ULL, labels));
    EXPECT_TRUE(report_->ReportPyStepRecord());
    EXPECT_TRUE(report_->ReportHalCreate(0xC0DE0052ULL, 16, MakeProp(0, 0, 0), MakeStack()));
    EXPECT_TRUE(report_->ReportHalCreate(0xC0DE0053ULL, 16, MakeProp(0, 0, 0)));
    EXPECT_TRUE(report_->ReportHalRelease(0xC0DE0052ULL, MakeStack()));
    EXPECT_TRUE(report_->ReportHalMalloc(0xC0DE0054ULL, 16, MakeHalFlag(MEM_SPACE_DEV, 0, 0), MakeStack()));
    EXPECT_TRUE(report_->ReportHalMalloc(0xC0DE0055ULL, 16, MakeHalFlag(MEM_SPACE_DEV, 0, 0)));
    EXPECT_TRUE(report_->ReportHalFree(0xC0DE0054ULL, MakeStack()));
    EXPECT_TRUE(report_->ReportHostRegister(0xC0DE0056ULL, 16, MakeStack()));
    EXPECT_TRUE(report_->ReportHostUnregister(0xC0DE0056ULL, MakeStack()));
    EXPECT_TRUE(report_->ReportMemPoolRecord(EventSubType::PTA_CACHING, usage, MakeStack()));
    EXPECT_TRUE(report_->ReportMark(MarkType::MARK_A, msg, 1, 0));
    EXPECT_TRUE(report_->ReportAtenLaunch("aten", true, std::string(pyStack)));
    EXPECT_TRUE(report_->ReportAtenAccess("aten", "attr", AccessType::READ, 0xC0DE0057ULL, 16, std::string(pyStack)));
    EXPECT_TRUE(report_->ReportKernelLaunch(kernelInfo));
    EXPECT_TRUE(report_->ReportKernelExcute(key, name, 1, RecordSubType::KERNEL_START));
    EXPECT_TRUE(report_->ReportAclItf(RecordSubType::INIT));
    EXPECT_TRUE(report_->ReportTraceStatus(EventTraceStatus::IN_TRACING));
    EXPECT_TRUE(report_->ReportAtbOpExecute("atb", 3, "attr", 4, RecordSubType::ATB_START));
    EXPECT_TRUE(report_->ReportAtbKernel("atb", 3, "attr", 4, RecordSubType::KERNEL_START));
    EXPECT_TRUE(report_->ReportAtbAccessMemory("atb", 3, "attr", 4, 0xC0DE0058ULL, 16, AccessType::READ));
    EXPECT_TRUE(report_->ReportMemorySnapshot(snapshot, MakeStack()));
    EXPECT_TRUE(report_->ReportOOMTrigger(trigger));
    EXPECT_TRUE(report_->ReportOOMMemRecord(record, EventSubType::OOM_RECENT_ALLOC));
    // ReportHostStage 无 IsNeedSkip 判定（窗口边界事件不受采集范围过滤），不在此用例断言
    EXPECT_EQ(captor_.events.size(), 0u) << "跳过分支不得派发任何事件";
    // step 闩锁只由 Mark 修改，本用例未进入 range，step 不变
    EXPECT_EQ(report_->stepInfo_.currentStepId, 0u);
}

// Mark/SetStepInfo：step 计数、range 栈、非法消息字符清洗与越界 MarkType
TEST_F(EventReportCoverageTest, mark_and_step_info)
{
    report_->pyStepId_.store(0);
    EXPECT_TRUE(report_->ReportPyStepRecord());
    EXPECT_EQ(report_->GetPyStepId(), 1u);
    auto stepEvent = captor_.LastAs<SystemEvent>(EventBaseType::SYSTEM);
    ASSERT_NE(stepEvent, nullptr);
    EXPECT_EQ(stepEvent->eventSubType, EventSubType::STEP);
    EXPECT_EQ(stepEvent->name, "1");

    // MARK_A：不改 step 状态
    std::string markMsg = "cov_mark";
    EXPECT_TRUE(report_->ReportMark(MarkType::MARK_A, markMsg, 3, 11));
    auto mstxEvent = captor_.LastAs<MstxEvent>(EventBaseType::MSTX);
    ASSERT_NE(mstxEvent, nullptr);
    EXPECT_EQ(mstxEvent->eventSubType, EventSubType::MSTX_MARK);
    EXPECT_EQ(mstxEvent->name, "cov_mark");
    EXPECT_EQ(mstxEvent->streamId, 3);
    EXPECT_EQ(report_->stepInfo_.currentStepId, 0u);

    // RANGE_START_A 但消息不是 "step start"：只上报事件，不进入 step range
    std::string notStep = "not step start";
    EXPECT_TRUE(report_->ReportMark(MarkType::RANGE_START_A, notStep, 0, 12));
    EXPECT_EQ(report_->stepInfo_.currentStepId, 0u);
    EXPECT_FALSE(report_->stepInfo_.inStepRange);

    // 合法 step start：step 自增、入 range、rangeId 入栈
    std::string stepStart = "step start";
    EXPECT_TRUE(report_->ReportMark(MarkType::RANGE_START_A, stepStart, 0, 12));
    EXPECT_EQ(report_->stepInfo_.currentStepId, 1u);
    EXPECT_TRUE(report_->stepInfo_.inStepRange);
    ASSERT_EQ(report_->stepInfo_.stepMarkRangeIdList.size(), 1u);
    EXPECT_EQ(report_->stepInfo_.stepMarkRangeIdList[0], 12u);
    mstxEvent = captor_.LastAs<MstxEvent>(EventBaseType::MSTX);
    ASSERT_NE(mstxEvent, nullptr);
    EXPECT_EQ(mstxEvent->eventSubType, EventSubType::MSTX_RANGE_START);
    EXPECT_EQ(mstxEvent->stepId, 1u);

    // 未登记的 rangeId 结束：不改变 range 状态
    std::string rangeEnd = "step end";
    EXPECT_TRUE(report_->ReportMark(MarkType::RANGE_END, rangeEnd, 0, 99));
    EXPECT_TRUE(report_->stepInfo_.inStepRange);
    EXPECT_EQ(report_->stepInfo_.stepMarkRangeIdList.size(), 1u);

    // 已登记 rangeId 结束：出 range、出栈
    EXPECT_TRUE(report_->ReportMark(MarkType::RANGE_END, rangeEnd, 0, 12));
    EXPECT_FALSE(report_->stepInfo_.inStepRange);
    EXPECT_EQ(report_->stepInfo_.stepMarkRangeIdList.size(), 0u);
    mstxEvent = captor_.LastAs<MstxEvent>(EventBaseType::MSTX);
    ASSERT_NE(mstxEvent, nullptr);
    EXPECT_EQ(mstxEvent->eventSubType, EventSubType::MSTX_RANGE_END);

    // 非法首字符：清洗后置空，事件名称为空
    std::string badMsg = "=bad_mark";
    EXPECT_TRUE(report_->ReportMark(MarkType::MARK_A, badMsg, 0, 13));
    EXPECT_TRUE(badMsg.empty());
    mstxEvent = captor_.LastAs<MstxEvent>(EventBaseType::MSTX);
    ASSERT_NE(mstxEvent, nullptr);
    EXPECT_TRUE(mstxEvent->name.empty());
    std::string plusMsg = "+bad";
    std::string minusMsg = "-bad";
    std::string atMsg = "@bad";
    EXPECT_TRUE(report_->ReportMark(MarkType::MARK_A, plusMsg, 0, 14));
    EXPECT_TRUE(report_->ReportMark(MarkType::MARK_A, minusMsg, 0, 15));
    EXPECT_TRUE(report_->ReportMark(MarkType::MARK_A, atMsg, 0, 16));

    // 越界 MarkType：SetStepInfo 走到兜底 return，事件子类型按 RANGE_END 兜底
    MarkType invalidType = static_cast<MarkType>(99);
    std::string invalidMsg = "invalid";
    EXPECT_TRUE(report_->ReportMark(invalidType, invalidMsg, 0, 17));
    EXPECT_EQ(report_->stepInfo_.currentStepId, 1u);
}

// 池事件：SKIP/SHADOW/NORMAL 三种追踪模式与四类池类型的字段映射
TEST_F(EventReportCoverageTest, mem_pool_record_trace_modes)
{
    MemoryUsage usage{};
    usage.deviceIndex = 0;
    usage.dataType = 0;

    // SKIP：LAUNCH/ACCESS 掩码下 DetermineTraceMode 返回 SKIP，直接早退
    Config config = MakeCovConfig();
    BitField<decltype(config.eventType)> skipBits;
    skipBits.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    skipBits.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    config.eventType = skipBits.getValue();
    SetCovConfig(config);
    captor_.Clear();
    usage.ptr = 0xC0DE0060ULL;
    EXPECT_TRUE(report_->ReportMemPoolRecord(EventSubType::PTA_CACHING, usage, MakeStack()));
    EXPECT_EQ(captor_.events.size(), 0u);

    // SHADOW：非采集态 + alloc/free 位打开，事件标记 isShadowEvent 且不派发
    EventTraceManager::Instance().status_ = EventTraceStatus::NOT_IN_TRACING;
    SetCovConfig(MakeCovConfig());
    usage.ptr = 0xC0DE0061ULL;
    EXPECT_TRUE(report_->ReportMemPoolRecord(EventSubType::PTA_CACHING, usage, MakeStack()));
    EXPECT_EQ(captor_.events.size(), 0u);
    usage.dataType = 1;  // 影子 FREE 走 HandleShadowEvent
    usage.ptr = 0xC0DE0061ULL;
    EXPECT_TRUE(report_->ReportMemPoolRecord(EventSubType::PTA_CACHING, usage, MakeStack()));
    EXPECT_EQ(captor_.events.size(), 0u);

    // NORMAL：四类池类型各自映射，MALLOC/FREE 由 dataType 决定
    EventTraceManager::Instance().status_ = EventTraceStatus::IN_TRACING;
    // 池事件不查询驱动，直接读 MemoryStateManager 的查询缓存；事件 device 会经
    // TransDeviceId 归一（受 ASCEND_RT_VISIBLE_DEVICES 影响），取值在本机不可预知，
    // 故对全部缓存槽位写同一哨兵值，消除环境差异
    for (int32_t slot = 0; slot < 16; ++slot)
    {
        MemoryStateManager::GetInstance().UpdateDeviceUsedCache(slot, 1111);
        MemoryStateManager::GetInstance().UpdateProcessUsedCache(slot, 2222);
    }
    const EventSubType poolTypes[] = {EventSubType::PTA_CACHING, EventSubType::PTA_WORKSPACE, EventSubType::ATB,
                                      EventSubType::MINDSPORE};
    const PoolType expectPools[] = {PoolType::PTA_CACHING, PoolType::PTA_WORKSPACE, PoolType::ATB, PoolType::MINDSPORE};
    for (size_t i = 0; i < sizeof(poolTypes) / sizeof(poolTypes[0]); ++i)
    {
        usage.deviceIndex = 0;
        usage.dataType = 0;
        usage.ptr = 0xC0DE0070ULL + i;
        usage.allocSize = 100 + static_cast<int64_t>(i);
        usage.totalReserved = 900 + static_cast<int64_t>(i);
        usage.totalAllocated = 500 + static_cast<int64_t>(i);
        EXPECT_TRUE(report_->ReportMemPoolRecord(poolTypes[i], usage, MakeStack()));
        auto event = captor_.LastAs<MemoryEvent>(EventBaseType::MALLOC);
        ASSERT_NE(event, nullptr);
        EXPECT_EQ(event->poolType, expectPools[i]);
        EXPECT_EQ(event->eventSubType, poolTypes[i]);
        EXPECT_EQ(event->size, usage.allocSize);
        EXPECT_EQ(event->total, usage.totalReserved);
        EXPECT_EQ(event->used, usage.totalAllocated);
        EXPECT_EQ(event->deviceUsed, 1111);
        EXPECT_EQ(event->processUsed, 2222);
        EXPECT_EQ(event->name, "N/A");
        EXPECT_FALSE(event->isShadowEvent);

        usage.dataType = 1;
        EXPECT_TRUE(report_->ReportMemPoolRecord(poolTypes[i], usage, MakeStack()));
        event = captor_.LastAs<MemoryEvent>(EventBaseType::FREE);
        ASSERT_NE(event, nullptr);
        EXPECT_EQ(event->poolType, expectPools[i]);
    }
}

// ATen/ATB/Kernel/ACL/快照/OOM 的上报字段与 addr==0 丢弃分支
TEST_F(EventReportCoverageTest, launch_and_access_reporting)
{
    // ATen
    EXPECT_TRUE(report_->ReportAtenLaunch("aten_op", true, std::string("py")));
    auto opEvent = captor_.LastAs<OpLaunchEvent>(EventBaseType::OP_LAUNCH);
    ASSERT_NE(opEvent, nullptr);
    EXPECT_EQ(opEvent->eventSubType, EventSubType::ATEN_START);
    EXPECT_EQ(opEvent->name, "aten_op");
    EXPECT_TRUE(report_->ReportAtenLaunch("aten_op", false, std::string("py")));
    opEvent = captor_.LastAs<OpLaunchEvent>(EventBaseType::OP_LAUNCH);
    ASSERT_NE(opEvent, nullptr);
    EXPECT_EQ(opEvent->eventSubType, EventSubType::ATEN_END);

    EXPECT_TRUE(report_->ReportAtenAccess("aten_acc", "attr", AccessType::READ, 0xC0DE0080ULL, 16, std::string("py")));
    auto accessEvent = captor_.LastAs<MemoryEvent>(EventBaseType::ACCESS);
    ASSERT_NE(accessEvent, nullptr);
    EXPECT_EQ(accessEvent->eventSubType, EventSubType::ATEN_READ);
    EXPECT_EQ(accessEvent->poolType, PoolType::PTA_CACHING);
    EXPECT_EQ(accessEvent->addr, 0xC0DE0080ULL);
    EXPECT_TRUE(
        report_->ReportAtenAccess("aten_acc", "attr", AccessType::WRITE, 0xC0DE0080ULL, 16, std::string("py")));
    EXPECT_EQ(captor_.LastAs<MemoryEvent>(EventBaseType::ACCESS)->eventSubType, EventSubType::ATEN_WRITE);
    EXPECT_TRUE(
        report_->ReportAtenAccess("aten_acc", "attr", AccessType::UNKNOWN, 0xC0DE0080ULL, 16, std::string("py")));
    EXPECT_EQ(captor_.LastAs<MemoryEvent>(EventBaseType::ACCESS)->eventSubType, EventSubType::ATEN_READ_OR_WRITE);

    // addr==0：丢弃且不派发
    const size_t beforeZero = captor_.events.size();
    EXPECT_TRUE(report_->ReportAtenAccess("aten_acc", "attr", AccessType::READ, 0, 16, std::string("py")));
    EXPECT_EQ(captor_.events.size(), beforeZero);

    // ATB
    EXPECT_TRUE(report_->ReportAtbOpExecute("atb", 3, "attr", 4, RecordSubType::ATB_START));
    opEvent = captor_.LastAs<OpLaunchEvent>(EventBaseType::OP_LAUNCH);
    ASSERT_NE(opEvent, nullptr);
    EXPECT_EQ(opEvent->eventSubType, EventSubType::ATB_START);
    EXPECT_TRUE(report_->ReportAtbOpExecute("atb", 3, "attr", 4, RecordSubType::ATB_END));
    EXPECT_EQ(captor_.LastAs<OpLaunchEvent>(EventBaseType::OP_LAUNCH)->eventSubType, EventSubType::ATB_END);

    EXPECT_TRUE(report_->ReportAtbKernel("atb", 3, "attr", 4, RecordSubType::KERNEL_START));
    auto kernelEvent = captor_.LastAs<KernelLaunchEvent>(EventBaseType::KERNEL_LAUNCH);
    ASSERT_NE(kernelEvent, nullptr);
    EXPECT_EQ(kernelEvent->eventSubType, EventSubType::ATB_KERNEL_START);
    EXPECT_TRUE(report_->ReportAtbKernel("atb", 3, "attr", 4, RecordSubType::KERNEL_END));
    EXPECT_EQ(captor_.LastAs<KernelLaunchEvent>(EventBaseType::KERNEL_LAUNCH)->eventSubType,
              EventSubType::ATB_KERNEL_END);

    EXPECT_TRUE(
        report_->ReportAtbAccessMemory("atb", 3, "attr", 4, 0xC0DE0081ULL, 16, AccessType::READ));
    accessEvent = captor_.LastAs<MemoryEvent>(EventBaseType::ACCESS);
    ASSERT_NE(accessEvent, nullptr);
    EXPECT_EQ(accessEvent->eventSubType, EventSubType::ATB_READ);
    EXPECT_EQ(accessEvent->poolType, PoolType::ATB);
    EXPECT_TRUE(
        report_->ReportAtbAccessMemory("atb", 3, "attr", 4, 0xC0DE0081ULL, 16, AccessType::WRITE));
    EXPECT_EQ(captor_.LastAs<MemoryEvent>(EventBaseType::ACCESS)->eventSubType, EventSubType::ATB_WRITE);
    EXPECT_TRUE(
        report_->ReportAtbAccessMemory("atb", 3, "attr", 4, 0xC0DE0081ULL, 16, AccessType::UNKNOWN));
    EXPECT_EQ(captor_.LastAs<MemoryEvent>(EventBaseType::ACCESS)->eventSubType, EventSubType::ATB_READ_OR_WRITE);
    const size_t beforeAtbZero = captor_.events.size();
    EXPECT_TRUE(report_->ReportAtbAccessMemory("atb", 3, "attr", 4, 0, 16, AccessType::READ));
    EXPECT_EQ(captor_.events.size(), beforeAtbZero);

    // KernelLaunch / KernelExcute
    const uint64_t kernelIndexBefore = report_->kernelLaunchRecordIndex_.load();
    TaskKey key = std::make_tuple(static_cast<uint16_t>(0), static_cast<uint16_t>(1), static_cast<uint16_t>(2));
    AclnnKernelMapInfo kernelInfo{1234, key, "kernel_add"};
    EXPECT_TRUE(report_->ReportKernelLaunch(kernelInfo));
    kernelEvent = captor_.LastAs<KernelLaunchEvent>(EventBaseType::KERNEL_LAUNCH);
    ASSERT_NE(kernelEvent, nullptr);
    EXPECT_EQ(kernelEvent->eventSubType, EventSubType::KERNEL_LAUNCH);
    EXPECT_EQ(kernelEvent->name, "kernel_add");
    EXPECT_EQ(kernelEvent->streamId, "1");
    EXPECT_EQ(kernelEvent->taskId, "2");
    EXPECT_EQ(kernelEvent->timestamp, 1234U);
    EXPECT_EQ(kernelEvent->kernelIndex, kernelIndexBefore + 1);
    EXPECT_EQ(report_->kernelLaunchRecordIndex_.load(), kernelIndexBefore + 1);

    std::string kernelName = "kernel_add";
    EXPECT_TRUE(report_->ReportKernelExcute(key, kernelName, 456, RecordSubType::KERNEL_START));
    kernelEvent = captor_.LastAs<KernelLaunchEvent>(EventBaseType::KERNEL_LAUNCH);
    ASSERT_NE(kernelEvent, nullptr);
    EXPECT_EQ(kernelEvent->eventSubType, EventSubType::KERNEL_EXECUTE_START);
    EXPECT_EQ(kernelEvent->timestamp, 456U);
    EXPECT_EQ(kernelEvent->pid, INVALID_PROCESSID);
    EXPECT_EQ(kernelEvent->tid, INVALID_THREADID);
    EXPECT_TRUE(report_->ReportKernelExcute(key, kernelName, 789, RecordSubType::KERNEL_END));
    EXPECT_EQ(captor_.LastAs<KernelLaunchEvent>(EventBaseType::KERNEL_LAUNCH)->eventSubType,
              EventSubType::KERNEL_EXECUTE_END);

    // 频率未就绪时驱动时间换算返回0：时间戳回退为上报时刻，不产生1970时间戳
    EXPECT_TRUE(report_->ReportKernelExcute(key, kernelName, 0, RecordSubType::KERNEL_START));
    EXPECT_NE(captor_.LastAs<KernelLaunchEvent>(EventBaseType::KERNEL_LAUNCH)->timestamp, 0U);

    // ACL 初始化事件（FINALIZE 会触达驱动符号解析，放到子进程用例）
    EXPECT_TRUE(report_->ReportAclItf(RecordSubType::INIT));
    auto systemEvent = captor_.LastAs<SystemEvent>(EventBaseType::SYSTEM);
    ASSERT_NE(systemEvent, nullptr);
    EXPECT_EQ(systemEvent->eventSubType, EventSubType::ACL_INIT);

    // 采集状态事件：TRACE_START/TRACE_STOP 与 host 窗口闩锁联动
    EXPECT_TRUE(report_->ReportTraceStatus(EventTraceStatus::IN_TRACING));
    systemEvent = captor_.LastAs<SystemEvent>(EventBaseType::SYSTEM);
    ASSERT_NE(systemEvent, nullptr);
    EXPECT_EQ(systemEvent->eventSubType, EventSubType::TRACE_START);
    EXPECT_FALSE(report_->hostMemStopLatch_.load());
    EXPECT_TRUE(report_->ReportTraceStatus(EventTraceStatus::NOT_IN_TRACING));
    systemEvent = captor_.LastAs<SystemEvent>(EventBaseType::SYSTEM);
    ASSERT_NE(systemEvent, nullptr);
    EXPECT_EQ(systemEvent->eventSubType, EventSubType::TRACE_STOP);
    EXPECT_TRUE(report_->hostMemStopLatch_.load());
    report_->hostMemStopLatch_.store(false);
    EventTraceManager::Instance().status_ = EventTraceStatus::IN_TRACING;

    // 快照事件
    MemorySnapshotInfo snapshot{};
    snapshot.device = 0;
    snapshot.memory_reserved = 10;
    snapshot.max_memory_reserved = 20;
    snapshot.memory_allocated = 30;
    snapshot.max_memory_allocated = 40;
    snapshot.total_memory = 50;
    snapshot.free_memory = 60;
    snapshot.name = "cov_snapshot";
    EXPECT_TRUE(report_->ReportMemorySnapshot(snapshot, MakeStack()));
    auto snapshotEvent = captor_.LastAs<SnapshotEvent>(EventBaseType::SNAPSHOT);
    ASSERT_NE(snapshotEvent, nullptr);
    EXPECT_EQ(snapshotEvent->eventSubType, EventSubType::SNAPSHOT);
    EXPECT_EQ(snapshotEvent->name, "cov_snapshot");
    EXPECT_EQ(snapshotEvent->memory_reserved, 10u);
    EXPECT_EQ(snapshotEvent->max_memory_reserved, 20u);
    EXPECT_EQ(snapshotEvent->memory_allocated, 30u);
    EXPECT_EQ(snapshotEvent->max_memory_allocated, 40u);
    EXPECT_EQ(snapshotEvent->total_memory, 50u);
    EXPECT_EQ(snapshotEvent->free_memory, 60u);

    // OOM 触发/记录：显式 deviceId 走事件；缺省 GD_INVALID_NUM 且取不到设备号时返回 false
    OOMTriggerInfo trigger{};
    trigger.deviceId = 0;
    trigger.requestSize = 4096;
    trigger.flag = 0x2A;
    trigger.funcName = "cov_alloc";
    trigger.timestamp = 777;
    EXPECT_TRUE(report_->ReportOOMTrigger(trigger));
    auto oomEvent = captor_.LastAs<OOMTriggerEvent>(EventBaseType::OOM_DETAIL);
    ASSERT_NE(oomEvent, nullptr);
    EXPECT_EQ(oomEvent->eventSubType, EventSubType::OOM_TRIGGER);
    EXPECT_EQ(oomEvent->requestSize, 4096u);
    EXPECT_EQ(oomEvent->flag, 0x2Au);
    EXPECT_EQ(oomEvent->funcName, "cov_alloc");
    EXPECT_EQ(oomEvent->timestamp, 777u);
    EXPECT_EQ(oomEvent->name, "OOM_Trigger");

    // 缺省 GD_INVALID_NUM：本机取不到设备号时返回 false 且不派发；若环境能取到真实卡号则照常上报
    OOMTriggerInfo triggerNoDevice{};
    const size_t beforeNoDevice = captor_.events.size();
    if (!report_->ReportOOMTrigger(triggerNoDevice))
    {
        EXPECT_EQ(captor_.events.size(), beforeNoDevice);
    }

    OOMMemRecord record{};
    record.deviceId = 0;
    record.poolType = PoolType::PTA_CACHING;
    record.ptr = 0xC0DE0090ULL;
    record.memSize = 8192;
    record.allocTimestamp = 888;
    EXPECT_TRUE(report_->ReportOOMMemRecord(record, EventSubType::OOM_RECENT_ALLOC));
    auto recordEvent = captor_.LastAs<OOMMemRecordEvent>(EventBaseType::OOM_DETAIL);
    ASSERT_NE(recordEvent, nullptr);
    EXPECT_EQ(recordEvent->eventSubType, EventSubType::OOM_RECENT_ALLOC);
    EXPECT_EQ(recordEvent->name, "OOM_RecentAlloc");
    EXPECT_EQ(recordEvent->memSize, 8192);
    EXPECT_EQ(recordEvent->allocTimestamp, 888u);
    EXPECT_EQ(recordEvent->poolType, PoolType::PTA_CACHING);
    EXPECT_TRUE(report_->ReportOOMMemRecord(record, EventSubType::OOM_TOP_ALLOC));
    recordEvent = captor_.LastAs<OOMMemRecordEvent>(EventBaseType::OOM_DETAIL);
    ASSERT_NE(recordEvent, nullptr);
    EXPECT_EQ(recordEvent->name, "OOM_TopAlloc");

    // 缺省 GD_INVALID_NUM：取不到设备号时返回 false 且不派发；能取到真实卡号则照常上报
    OOMMemRecord recordNoDevice{};
    const size_t beforeRecordNoDevice = captor_.events.size();
    if (!report_->ReportOOMMemRecord(recordNoDevice, EventSubType::OOM_RECENT_ALLOC))
    {
        EXPECT_EQ(captor_.events.size(), beforeRecordNoDevice);
    }
}

// host 窗口：短路判定（独占通道直连 vs 队列派发）、闩锁与 set_enabled 联动、svc 未装配告警
TEST_F(EventReportCoverageTest, host_window_and_stage_dispatch)
{
    Config config = MakeCovConfig();
    BitField<decltype(config.analysisType)> analysisBits;
    analysisBits.setBit(static_cast<size_t>(AnalysisType::HOST_LEAK_ANALYSIS));
    config.analysisType = analysisBits.getValue();

    // collectCpu=false + host-leaks：独占通道，绕过事件队列直连派发
    config.collectCpu = false;
    SetCovConfig(config);
    EXPECT_TRUE(report_->HostEventBypassEnabled());
    EXPECT_TRUE(report_->ReportHostStage(true, 111, 5));
    auto stageEvent = captor_.LastAs<SystemEvent>(EventBaseType::SYSTEM);
    ASSERT_NE(stageEvent, nullptr);
    EXPECT_EQ(stageEvent->eventSubType, EventSubType::HOST_LEAK_STAGE_START);
    EXPECT_EQ(stageEvent->name, "5");
    EXPECT_EQ(stageEvent->timestamp, 111u);
    EXPECT_EQ(stageEvent->device, GD_INVALID_NUM);
    EXPECT_TRUE(report_->ReportHostStage(false, 222, 6));
    stageEvent = captor_.LastAs<SystemEvent>(EventBaseType::SYSTEM);
    ASSERT_NE(stageEvent, nullptr);
    EXPECT_EQ(stageEvent->eventSubType, EventSubType::HOST_LEAK_STAGE_END);

    // collectCpu=true：退回 Process::SendEvent 队列路径
    config.collectCpu = true;
    SetCovConfig(config);
    EXPECT_FALSE(report_->HostEventBypassEnabled());

    // svc 未装配时 UpdateHostMemWindow 只告警；装配后按 (host-leaks && !闩锁) 下发 set_enabled
    g_covSetEnabledArgs.clear();
    report_->svcHostMem_ = nullptr;
    report_->hostMemStopLatch_.store(false);
    report_->UpdateHostMemWindow();
    EXPECT_TRUE(g_covSetEnabledArgs.empty());

    report_->SetHostMemSvcForTest(&g_covMockSvc);
    report_->UpdateHostMemWindow();
    ASSERT_EQ(g_covSetEnabledArgs.size(), 1u);
    EXPECT_EQ(g_covSetEnabledArgs.back(), 1);

    report_->hostMemStopLatch_.store(true);
    report_->UpdateHostMemWindow();
    EXPECT_EQ(g_covSetEnabledArgs.back(), 0);

    // 非 host-leaks 配置：交集为空，恒关窗
    SetCovConfig(MakeCovConfig());
    report_->hostMemStopLatch_.store(false);
    report_->UpdateHostMemWindow();
    EXPECT_EQ(g_covSetEnabledArgs.back(), 0);

    // ReportTraceStatus 的闩锁联动再加上 svc：start 复位、stop 置位
    EXPECT_TRUE(report_->ReportTraceStatus(EventTraceStatus::IN_TRACING));
    EXPECT_EQ(g_covSetEnabledArgs.back(), 0) << "非 host-leaks 配置下即使开始采集也不开窗";
    EXPECT_TRUE(report_->ReportTraceStatus(EventTraceStatus::NOT_IN_TRACING));
    EXPECT_TRUE(report_->hostMemStopLatch_.load());
    report_->hostMemStopLatch_.store(false);
    report_->SetHostMemSvcForTest(nullptr);
}

// host-leak 快照包装层：svc 未装配/已析构/emit 为空时返回 false，装配后转发并回调 emit
TEST_F(EventReportCoverageTest, host_mem_dump_wrappers)
{
    MsmemscopeHostmemStats stats{};
    EXPECT_FALSE(report_->GetHostMemStats(stats));
    EXPECT_FALSE(report_->DumpHostMemLiveBlocks(CovEmitLiveBlock, nullptr));
    EXPECT_FALSE(report_->DumpHostMemStackStats(CovEmitStack, nullptr));
    EXPECT_FALSE(report_->DumpHostMemUnfreedSeries(CovEmitSeries, nullptr));
    EXPECT_FALSE(report_->DumpHostMemSizeDist(CovEmitSizeDist, nullptr));
    EXPECT_FALSE(report_->DumpHostMemPreWindowDist(CovEmitSizeDist, nullptr));
    EXPECT_FALSE(report_->DumpHostMemInterimSnapshot(CovEmitStack, CovEmitSizeDist, CovEmitSizeDist, CovEmitSeries,
                                                     nullptr, nullptr));

    report_->SetHostMemSvcForTest(&g_covMockSvc);
    g_covEmitCount = 0;

    EXPECT_TRUE(report_->GetHostMemStats(stats));
    EXPECT_TRUE(report_->DumpHostMemLiveBlocks(CovEmitLiveBlock, nullptr));
    EXPECT_TRUE(report_->DumpHostMemStackStats(CovEmitStack, nullptr));
    EXPECT_TRUE(report_->DumpHostMemUnfreedSeries(CovEmitSeries, nullptr));
    EXPECT_TRUE(report_->DumpHostMemSizeDist(CovEmitSizeDist, nullptr));
    EXPECT_TRUE(report_->DumpHostMemPreWindowDist(CovEmitSizeDist, nullptr));
    MsmemscopeInterimStats interim{};
    EXPECT_TRUE(report_->DumpHostMemInterimSnapshot(CovEmitStack, CovEmitSizeDist, CovEmitSizeDist, CovEmitSeries,
                                                    &interim, nullptr));
    EXPECT_EQ(g_covEmitCount, 9u) << "五类单次 dump 各回调 emit 一次 + 中间快照 4 类各一次";

    // emit 为空：守卫提前返回，不得触达 svc 表
    EXPECT_FALSE(report_->DumpHostMemLiveBlocks(nullptr, nullptr));
    EXPECT_FALSE(report_->DumpHostMemStackStats(nullptr, nullptr));
    EXPECT_FALSE(report_->DumpHostMemUnfreedSeries(nullptr, nullptr));
    EXPECT_FALSE(report_->DumpHostMemSizeDist(nullptr, nullptr));
    EXPECT_FALSE(report_->DumpHostMemPreWindowDist(nullptr, nullptr));
    EXPECT_FALSE(report_->DumpHostMemInterimSnapshot(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));

    // 已析构：同样返回 false
    report_->destroyed_.store(true);
    EXPECT_FALSE(report_->GetHostMemStats(stats));
    EXPECT_FALSE(report_->DumpHostMemLiveBlocks(CovEmitLiveBlock, nullptr));
    report_->destroyed_.store(false);
    report_->SetHostMemSvcForTest(nullptr);
}

// 退出闭窗：状态查询不可得/已排空/等待中三种轮询分支
TEST_F(EventReportCoverageTest, close_window_at_exit_polling)
{
    int (*savedStatusFn)(void) = g_hostMemStatusFn;
    g_hostMemStatusFn = nullptr;
    report_->svcHostMem_ = &g_covMockSvc;
    report_->destroyed_.store(false);

    // 钩子符号不可得（QueryHostMemStatus 返回 -1）：告警后放行
    g_hostMemStatusFn = nullptr;
    report_->hostMemStopLatch_.store(false);
    report_->CloseHostMemWindowAtExit();
    EXPECT_TRUE(report_->hostMemStopLatch_.load()) << "置位后不得再开窗";

    // 显式桩返回 -1：同一分支
    report_->hostMemStopLatch_.store(false);
    g_hostMemStatusFn = &MockCovHostMemStatus;
    g_covStatusValue = -1;
    g_covStatusCalls = 0;
    report_->CloseHostMemWindowAtExit();
    EXPECT_EQ(g_covStatusCalls, 1);

    // closing 位为 0：闭窗已完成，立即返回
    report_->hostMemStopLatch_.store(false);
    g_covStatusValue = 0;
    g_covStatusCalls = 0;
    report_->CloseHostMemWindowAtExit();
    EXPECT_EQ(g_covStatusCalls, 1);

    // 首次 closing 置位→进度日志+休眠，第二次已排空→返回
    report_->hostMemStopLatch_.store(false);
    g_covStatusValue = 1;
    g_covStatusCalls = 0;
    const auto start = std::chrono::steady_clock::now();
    report_->CloseHostMemWindowAtExit();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(g_covStatusCalls, 2);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 20);

    g_hostMemStatusFn = savedStatusFn;
    report_->SetHostMemSvcForTest(nullptr);
}

// 分析类型变更：decompose/inefficient 随配置位订阅与退订；host-leaks 只订阅不退订
TEST_F(EventReportCoverageTest, update_analysis_type)
{
    auto hasSubscriber = [](const char *name) {
        const std::vector<std::string> names = EventDispatcher::GetInstance().GetSubscriberNames();
        return std::find(names.begin(), names.end(), name) != names.end();
    };

    // 清零后调用：decompose/inefficient 必须退订
    Config config = MakeCovConfig();
    config.analysisType = 0;
    SetCovConfig(config);
    report_->UpdateAnalysisType();
    EXPECT_FALSE(hasSubscriber("decompose"));
    EXPECT_FALSE(hasSubscriber("inefficient"));

    // 同时置位：两个分析器都必须出现在派发器订阅表中
    BitField<decltype(config.analysisType)> analysisBits;
    analysisBits.setBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS));
    analysisBits.setBit(static_cast<size_t>(AnalysisType::INEFFICIENCY_ANALYSIS));
    config.analysisType = analysisBits.getValue();
    SetCovConfig(config);
    report_->UpdateAnalysisType();
    EXPECT_TRUE(hasSubscriber("decompose"));
    EXPECT_TRUE(hasSubscriber("inefficient"));

    // 只清除 decompose：inefficient 的订阅必须保留
    BitField<decltype(config.analysisType)> inefficientBits;
    inefficientBits.setBit(static_cast<size_t>(AnalysisType::INEFFICIENCY_ANALYSIS));
    config.analysisType = inefficientBits.getValue();
    SetCovConfig(config);
    report_->UpdateAnalysisType();
    EXPECT_FALSE(hasSubscriber("decompose"));
    EXPECT_TRUE(hasSubscriber("inefficient"));

    // host-leaks 只订阅不退订：置位后在线，清零后仍在线（窗口异步关闭）
    BitField<decltype(config.analysisType)> hostBits;
    hostBits.setBit(static_cast<size_t>(AnalysisType::HOST_LEAK_ANALYSIS));
    config.analysisType = hostBits.getValue();
    SetCovConfig(config);
    report_->UpdateAnalysisType();
    EXPECT_TRUE(hasSubscriber("host_leak"));
    config.analysisType = 0;
    SetCovConfig(config);
    report_->UpdateAnalysisType();
    EXPECT_TRUE(hasSubscriber("host_leak"));
}

// C 桥：report_stage / is_suppressed / log / get_params
TEST_F(EventReportCoverageTest, hostmem_c_bridges)
{
    // report_stage：isStart=0 在采集态下走诊断日志分支，isStart=1 直接派发
    captor_.Clear();
    msmemscope_hostmem_report_stage(1, 100, 9);
    EXPECT_EQ(captor_.CountOf(EventBaseType::SYSTEM), 1u);
    msmemscope_hostmem_report_stage(0, 200, 10);
    EXPECT_EQ(captor_.CountOf(EventBaseType::SYSTEM), 2u);
    EXPECT_EQ(captor_.LastAs<SystemEvent>(EventBaseType::SYSTEM)->eventSubType,
              EventSubType::HOST_LEAK_STAGE_END);
    // 非采集态：诊断日志分支关闭，事件仍派发
    EventTraceManager::Instance().status_ = EventTraceStatus::NOT_IN_TRACING;
    msmemscope_hostmem_report_stage(0, 300, 11);
    EXPECT_EQ(captor_.CountOf(EventBaseType::SYSTEM), 3u);
    EventTraceManager::Instance().status_ = EventTraceStatus::IN_TRACING;

    // is_suppressed：抑制守卫嵌套计数
    EXPECT_EQ(msmemscope_hostmem_is_suppressed(), 0);
    {
        EventReportSuppressor suppressor;
        EXPECT_EQ(msmemscope_hostmem_is_suppressed(), 1);
        {
            EventReportSuppressor nested;
            EXPECT_EQ(msmemscope_hostmem_is_suppressed(), 1);
        }
        EXPECT_EQ(msmemscope_hostmem_is_suppressed(), 1);
    }
    EXPECT_EQ(msmemscope_hostmem_is_suppressed(), 0);

    // log：空格式串早退；DEBUG 受 tracing 门控；INFO/WARN/ERROR/未知级别各自落日志
    CallHostMemLog(MSMEMSCOPE_HOSTMEM_LOG_DEBUG, nullptr);
    CallHostMemLog(MSMEMSCOPE_HOSTMEM_LOG_DEBUG, "cov debug %d", 1);
    EventTraceManager::Instance().status_ = EventTraceStatus::NOT_IN_TRACING;
    CallHostMemLog(MSMEMSCOPE_HOSTMEM_LOG_DEBUG, "cov debug gated %d", 2);
    EventTraceManager::Instance().status_ = EventTraceStatus::IN_TRACING;
    CallHostMemLog(MSMEMSCOPE_HOSTMEM_LOG_INFO, "cov info %s", "x");
    CallHostMemLog(MSMEMSCOPE_HOSTMEM_LOG_WARN, "cov warn %s", "x");
    CallHostMemLog(MSMEMSCOPE_HOSTMEM_LOG_ERROR, "cov error %s", "x");
    CallHostMemLog(99, "cov unknown severity %s", "x");

    // get_params：空指针早退；cStackDepth=0 回退 50；Py 栈开关关闭时深度归零
    msmemscope_hostmem_get_params(nullptr);
    MsmemscopeHostmemParams params{};
    Config config = MakeCovConfig();
    config.cStackDepth = 0;
    config.enablePyStack = false;
    config.pyStackDepth = 33;
    SetCovConfig(config);
    msmemscope_hostmem_get_params(&params);
    EXPECT_EQ(params.stackDepth, DEFAULT_CALL_STACK_DEPTH);
    EXPECT_EQ(params.sampleRate, 1u);
    EXPECT_EQ(params.blockThreshold, 0u);
    EXPECT_EQ(params.pyStackDepth, 0u);

    config.cStackDepth = 12;
    config.enablePyStack = true;
    config.pyStackDepth = 7;
    config.sampleRate = 4;
    config.blockSizeThreshold = 99;
    SetCovConfig(config);
    msmemscope_hostmem_get_params(&params);
    EXPECT_EQ(params.stackDepth, 12u);
    EXPECT_EQ(params.sampleRate, 4u);
    EXPECT_EQ(params.blockThreshold, 99u);
    EXPECT_EQ(params.pyStackDepth, 7u);
}

// 退出处理入口：实例在位时经 exit_close 触发闭窗；未 bind 时安静跳过
TEST_F(EventReportCoverageTest, exit_handler_entry)
{
    int (*savedStatusFn)(void) = g_hostMemStatusFn;
    g_hostMemStatusFn = &MockCovHostMemStatus;
    g_covStatusValue = 0;
    g_covStatusCalls = 0;

    report_->svcHostMem_ = &g_covMockSvc;
    report_->hostMemStopLatch_.store(false);
    EventReport::HostMemExitHandler();
    EXPECT_TRUE(report_->hostMemStopLatch_.load());
    EXPECT_EQ(g_covStatusCalls, 1);

    // C 入口等价路径
    report_->hostMemStopLatch_.store(false);
    msmemscope_hostmem_exit_close();
    EXPECT_TRUE(report_->hostMemStopLatch_.load());

    // 未装配 svc：告警后直接返回，不触达状态查询
    g_covStatusCalls = 0;
    report_->svcHostMem_ = nullptr;
    report_->hostMemStopLatch_.store(false);
    EventReport::HostMemExitHandler();
    EXPECT_EQ(g_covStatusCalls, 0);

    // 已析构：同样跳过
    report_->svcHostMem_ = &g_covMockSvc;
    report_->destroyed_.store(true);
    EventReport::HostMemExitHandler();
    EXPECT_EQ(g_covStatusCalls, 0);
    report_->destroyed_.store(false);

    g_hostMemStatusFn = savedStatusFn;
    report_->SetHostMemSvcForTest(nullptr);
}

// ============================================================================
// 子进程用例（EventReport/GetDeviceInfo 的构造期与符号在位组合）
// ============================================================================
namespace
{
void PrepareChildEnv()
{
    Utility::FileCreateManager::GetInstance(kCovChildDir).SetProjectDir(kCovChildDir);
    ResetDcmiMockState();
    g_isDlsymNullptr = false;
}

// env 解析：token 截断/溢出/乱序/重复/受限探测各分支
void RunEnvParsingFlow()
{
    PrepareChildEnv();
    g_funcMocks["aclrtGetDeviceCountImpl"] = reinterpret_cast<void *>(&MockCovAclrtGetDeviceCount);
    g_deviceCountRet = static_cast<int>(ACL_SUCCESS);

    // 1) env 未设置：EnsureEnvParsed 首行早退，TransDeviceId 直传
    unsetenv("ASCEND_RT_VISIBLE_DEVICES");
    {
        GetDeviceInfo info;
        int32_t devId = 3;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_EQ(devId, 3);
        EXPECT_FALSE(info.setVisibleDevice);
        EXPECT_FALSE(info.runtimeRestricted_);
    }

    // 2) 首字符非数字/空串：token 列表为空 → 整体拒绝
    const char *invalidEnv[] = {"", ",1", "abc", " 1"};
    for (const char *env : invalidEnv)
    {
        setenv("ASCEND_RT_VISIBLE_DEVICES", env, 1);
        GetDeviceInfo info;
        int32_t devId = 2;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_EQ(devId, 2) << "env=" << env;
        EXPECT_FALSE(info.setVisibleDevice) << "env=" << env;
    }

    // 3) 非法字符截断：保留前缀 token
    setenv("ASCEND_RT_VISIBLE_DEVICES", "1,abc", 1);
    {
        g_deviceCount = 5;
        GetDeviceInfo info;
        int32_t devId = 0;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_EQ(devId, 0) << "deviceCnt != userCnt ⇒ 非受限，直传";
        EXPECT_TRUE(info.setVisibleDevice);
        EXPECT_FALSE(info.runtimeRestricted_);
        EXPECT_EQ(info.visibleDeviceMap.size(), 1u) << "非法字符处截断，仅保留首个 token";
    }

    // 4) 双逗号/连续分隔符：同样在第二个逗号处截断
    setenv("ASCEND_RT_VISIBLE_DEVICES", "0,,1", 1);
    {
        GetDeviceInfo info;
        int32_t devId = 0;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_EQ(info.visibleDeviceMap.size(), 1u);
    }
    setenv("ASCEND_RT_VISIBLE_DEVICES", "0,1,,2", 1);
    {
        GetDeviceInfo info;
        int32_t devId = 0;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_EQ(info.visibleDeviceMap.size(), 2u);
    }

    // 5) 数值溢出/超 int32：各自截断成空列表 → 整体拒绝
    const char *overflowEnv[] = {"99999999999999999999", "2147483648"};
    for (const char *env : overflowEnv)
    {
        setenv("ASCEND_RT_VISIBLE_DEVICES", env, 1);
        GetDeviceInfo info;
        int32_t devId = 1;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_FALSE(info.setVisibleDevice) << "env=" << env;
    }

    // 6) 乱序：整体拒绝（保留已解析前缀，但不启用转换）
    setenv("ASCEND_RT_VISIBLE_DEVICES", "2,1", 1);
    {
        GetDeviceInfo info;
        int32_t devId = 0;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_FALSE(info.setVisibleDevice);
        EXPECT_EQ(info.visibleDeviceMap[0], 2);
    }

    // 7) 重复：整体拒绝
    setenv("ASCEND_RT_VISIBLE_DEVICES", "1,1", 1);
    {
        GetDeviceInfo info;
        int32_t devId = 0;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_FALSE(info.setVisibleDevice);
    }

    // 8) 受限模式：deviceCnt == userCnt → 做 user→real 转换；越界地址直传并限频告警
    setenv("ASCEND_RT_VISIBLE_DEVICES", "3,4", 1);
    {
        g_deviceCount = 2;
        GetDeviceInfo info;
        int32_t devId = 0;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_EQ(devId, 3);
        EXPECT_TRUE(info.runtimeRestricted_);
        int32_t realDevId = 1;
        EXPECT_TRUE(info.TransDeviceId(realDevId));
        EXPECT_EQ(realDevId, 4);
        int32_t missDevId = 7;
        EXPECT_FALSE(info.TransDeviceId(missDevId));
        EXPECT_EQ(missDevId, 7);
        // 第二次越界：告警已限频，行为不变
        EXPECT_FALSE(info.TransDeviceId(missDevId));
    }

    // 9) 探测失败（符号缺失）：保守按受限处理
    setenv("ASCEND_RT_VISIBLE_DEVICES", "0", 1);
    unsetenv("ASCEND_RT_VISIBLE_DEVICES");
    {
        GetDeviceInfo info;
        int32_t devId = 0;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_FALSE(info.setVisibleDevice);
    }
    unsetenv("ASCEND_RT_VISIBLE_DEVICES");
}

// rtGetDeviceCount 兜底探测：成功/失败两条腿
void RunEnvParsingRtFallbackFlow()
{
    PrepareChildEnv();
    g_funcMocks["rtGetDeviceCount"] = reinterpret_cast<void *>(&MockCovRtGetDeviceCount);
    // 固定 dcmi 不可得(realCnt=0)：受限判据才会走 deviceCnt == userCnt 分支；
    // 真机上 dcmi 若可用会改走 deviceCnt < realCnt，结果随物理卡数变化而不可确定
    g_funcMocks["dcmi_init"] = reinterpret_cast<void *>(&MockCovDcmiInit);
    g_dcmiInitRet = 1;

    setenv("ASCEND_RT_VISIBLE_DEVICES", "1", 1);
    {
        g_rtDeviceCountRet = static_cast<int>(RT_ERROR_NONE);
        g_covStatusCalls = 0;
        g_deviceCount = 1;  // 运行时可见设备数与 env 解析出的设备数一致
        GetDeviceInfo info;
        int32_t devId = 0;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_TRUE(info.setVisibleDevice);
        EXPECT_TRUE(info.runtimeRestricted_) << "dcmi不可得且deviceCnt == userCnt ⇒ 受限";
    }
    {
        g_rtDeviceCountRet = static_cast<int>(RT_ERROR_INVALID_VALUE);
        GetDeviceInfo info;
        int32_t devId = 0;
        EXPECT_TRUE(info.TransDeviceId(devId));
        EXPECT_TRUE(info.runtimeRestricted_) << "探测失败 ⇒ 保守受限";
    }
    unsetenv("ASCEND_RT_VISIBLE_DEVICES");
}

// 两种设备号符号都缺失的探测路径
void RunEnvParsingNoSymbolFlow()
{
    PrepareChildEnv();
    setenv("ASCEND_RT_VISIBLE_DEVICES", "0", 1);
    GetDeviceInfo info;
    int32_t devId = 0;
    EXPECT_TRUE(info.TransDeviceId(devId));
    EXPECT_TRUE(info.setVisibleDevice);
    EXPECT_TRUE(info.runtimeRestricted_) << "符号缺失 ⇒ 保守受限";
    unsetenv("ASCEND_RT_VISIBLE_DEVICES");
}

// GetDeviceId：老包 rtGetDevice 路径（成功/失败）与 aclrt 路径
void RunDeviceIdFlow()
{
    PrepareChildEnv();
    unsetenv("ASCEND_RT_VISIBLE_DEVICES");

    // 两条符号都缺失：返回 false
    {
        GetDeviceInfo info;
        int32_t devId = GD_INVALID_NUM;
        EXPECT_FALSE(info.GetDeviceId(devId));
    }

    // 老包 rtGetDevice：无效值返回 false，成功返回 true
    g_funcMocks["rtGetDevice"] = reinterpret_cast<void *>(&MockCovRtGetDevice);
    {
        g_rtGetDeviceRet = RT_ERROR_INVALID_VALUE;
        GetDeviceInfo info;
        int32_t devId = GD_INVALID_NUM;
        EXPECT_FALSE(info.GetDeviceId(devId));
    }
    {
        g_rtGetDeviceRet = RT_ERROR_NONE;
        g_rtGetDeviceId = 5;
        GetDeviceInfo info;
        int32_t devId = GD_INVALID_NUM;
        EXPECT_TRUE(info.GetDeviceId(devId));
        EXPECT_EQ(devId, 5);
    }

    // aclrt 优先路径：非 0 返回 false，成功返回 true
    g_funcMocks["aclrtGetDeviceImpl"] = reinterpret_cast<void *>(&MockCovAclrtGetDevice);
    {
        g_rtGetDeviceRet = ACL_ERROR_INTERNAL_ERROR;
        GetDeviceInfo info;
        int32_t devId = GD_INVALID_NUM;
        EXPECT_FALSE(info.GetDeviceId(devId));
    }
    {
        g_rtGetDeviceRet = ACL_SUCCESS;
        g_rtGetDeviceId = 0;
        GetDeviceInfo info;
        int32_t devId = GD_INVALID_NUM;
        EXPECT_TRUE(info.GetDeviceId(devId));
        EXPECT_EQ(devId, 0);
    }
}

// dcmi_init 成功/失败两条路径
void RunDcmiInitFlow()
{
    PrepareChildEnv();
    g_funcMocks["dcmi_init"] = reinterpret_cast<void *>(&MockCovDcmiInit);

    g_dcmiInitRet = 1;
    {
        GetDeviceInfo info;
        EXPECT_FALSE(info.EnsureDcmiInit());
        EXPECT_FALSE(info.dcmiReady_);
        // 失败后本进程永久降级：重试仍为 false
        EXPECT_FALSE(info.EnsureDcmiInit());
    }
    g_dcmiInitRet = 0;
    {
        GetDeviceInfo info;
        EXPECT_TRUE(info.EnsureDcmiInit());
        EXPECT_TRUE(info.dcmiReady_);
    }
}

// dcmi 设备映射：建表各失败分支与成功（含设备号空洞检测）
void RunDcmiDeviceMapFlow()
{
    PrepareChildEnv();
    InstallDcmiSymbols();

    // 卡列表查询失败
    g_cardListRet = 1;
    {
        GetDeviceInfo info;
        EXPECT_FALSE(info.BuildDeviceMap());
        EXPECT_FALSE(info.dcmiMapReady_);
    }
    g_cardListRet = 0;

    // 单卡设备数查询失败
    g_deviceNumRet = 1;
    {
        GetDeviceInfo info;
        EXPECT_FALSE(info.BuildDeviceMap());
    }
    g_deviceNumRet = 0;

    // 逻辑号查询失败
    g_logicIdRet = 1;
    {
        GetDeviceInfo info;
        EXPECT_FALSE(info.BuildDeviceMap());
    }
    g_logicIdRet = 0;

    // 空设备集合
    g_cardNum = 0;
    {
        GetDeviceInfo info;
        EXPECT_FALSE(info.BuildDeviceMap());
        EXPECT_FALSE(info.dcmiMapReady_);
    }
    g_cardNum = 1;
    g_deviceNumInCard = 2;

    // 逻辑号空洞（0 与 2 缺 1）：整表丢弃
    g_logicIdHole = true;
    {
        GetDeviceInfo info;
        EXPECT_FALSE(info.BuildDeviceMap());
        EXPECT_FALSE(info.dcmiMapReady_);
    }
    g_logicIdHole = false;

    // 正常建表
    {
        GetDeviceInfo info;
        EXPECT_TRUE(info.BuildDeviceMap());
        EXPECT_TRUE(info.dcmiMapReady_);
        EXPECT_EQ(info.devIdToDcmiMap_.size(), 2u);
        EXPECT_EQ(info.devIdToDcmiMap_[1].first, 0);
        EXPECT_EQ(info.devIdToDcmiMap_[1].second, 1);
    }
}

// dcmi 建表的符号缺失分支（必须先于任何一次建表调用，静态缓存尚未固化）
void RunDcmiMapSymbolMissingFlow()
{
    PrepareChildEnv();
    g_funcMocks["dcmi_init"] = reinterpret_cast<void *>(&MockCovDcmiInit);
    GetDeviceInfo info;
    EXPECT_FALSE(info.BuildDeviceMap());
    EXPECT_FALSE(info.dcmiMapReady_);
}

// HBM / 进程显存查询：成功、映射缺失、驱动失败、pid 未命中等分支
void RunHbmAndProcFlow()
{
    PrepareChildEnv();
    InstallDcmiSymbols();
    EventTraceManager::Instance().deviceReady_ = true;

    GetDeviceInfo info;
    uint64_t usedMb = 0;
    uint64_t totalMb = 0;
    EXPECT_TRUE(info.GetDeviceHbmInfo(0, usedMb, totalMb));
    EXPECT_EQ(usedMb, 100u);
    EXPECT_EQ(totalMb, 1024u);
    // 查询成功值按 MB→字节写入整卡缓存
    EventReport &report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    EXPECT_EQ(report.QueryDeviceUsed(0), 100 * 1024 * 1024);
    EXPECT_EQ(MemoryStateManager::GetInstance().GetDeviceUsed(0), 100 * 1024 * 1024);

    g_hbmRet = 1;
    EXPECT_FALSE(info.GetDeviceHbmInfo(1, usedMb, totalMb));
    g_hbmRet = 0;

    // 映射缺失：HOST/无效号走 DEBUG 分支，其余走 ERROR 分支
    EXPECT_FALSE(info.GetDeviceHbmInfo(DEVICE_ID_CPU, usedMb, totalMb));
    EXPECT_FALSE(info.GetDeviceHbmInfo(GD_INVALID_NUM, usedMb, totalMb));
    EXPECT_FALSE(info.GetDeviceHbmInfo(7, usedMb, totalMb));

    // dcmi 进程显存：命中本进程 pid
    uint64_t usedBytes = 0;
    EXPECT_TRUE(info.QueryDcmiProcMemInfo(0, usedBytes));
    EXPECT_EQ(usedBytes, 4096u);
    // 包装层：devdrv 直连可能成功，故只验证不崩溃
    uint64_t wrappedBytes = 0;
    (void)info.GetDeviceProcMemInfo(0, wrappedBytes);

    // pid 不在列表
    g_procHasSelf = false;
    EXPECT_FALSE(info.QueryDcmiProcMemInfo(0, usedBytes));
    g_procHasSelf = true;

    // 驱动返回失败
    g_procMemRet = 1;
    EXPECT_FALSE(info.QueryDcmiProcMemInfo(0, usedBytes));
    g_procMemRet = 0;

    // 映射缺失（HOST/无效号 DEBUG，其余 ERROR）
    EXPECT_FALSE(info.QueryDcmiProcMemInfo(DEVICE_ID_CPU, usedBytes));
    EXPECT_FALSE(info.QueryDcmiProcMemInfo(GD_INVALID_NUM, usedBytes));
    EXPECT_FALSE(info.QueryDcmiProcMemInfo(7, usedBytes));

    // QueryProcessUsed 门控：设备未就绪直接返回 -1 且不告警
    EventTraceManager::Instance().deviceReady_ = false;
    report.processUsedQueryWarned_.store(false);
    EXPECT_EQ(report.QueryProcessUsed(0), -1);
    EXPECT_FALSE(report.processUsedQueryWarned_.load());
    EventTraceManager::Instance().deviceReady_ = true;

    // 设备就绪：devdrv 直连可能先行成功，返回值只做区间校验
    const int64_t processUsed = report.QueryProcessUsed(0);
    EXPECT_GE(processUsed, -1);
    if (processUsed >= 0)
    {
        EXPECT_EQ(MemoryStateManager::GetInstance().GetProcessUsed(0), processUsed);
    }
}

// HBM/进程显存符号缺失分支（需在首次查询前调用，静态缓存尚未固化）
void RunQuerySymbolMissingFlow()
{
    PrepareChildEnv();
    InstallDcmiSymbols();
    // 只移除查询符号，保留建表所需符号
    g_funcMocks.erase("dcmi_get_device_hbm_info");
    g_funcMocks.erase("dcmi_get_npu_proc_mem_info");

    GetDeviceInfo info;
    uint64_t usedMb = 0;
    uint64_t totalMb = 0;
    EXPECT_FALSE(info.GetDeviceHbmInfo(0, usedMb, totalMb));
    uint64_t usedBytes = 0;
    EXPECT_FALSE(info.QueryDcmiProcMemInfo(0, usedBytes));
}

// 构造期分支：deferred 模式 + 三个分析位置位（分析器随 EventReport 构造即创建）
void RunCtorDeferredWithAnalysisFlow()
{
    PrepareChildEnv();
    Config config = MakeCovConfig();
    config.collectMode = static_cast<uint8_t>(CollectMode::DEFERRED);
    BitField<decltype(config.analysisType)> analysisBits;
    analysisBits.setBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS));
    analysisBits.setBit(static_cast<size_t>(AnalysisType::INEFFICIENCY_ANALYSIS));
    analysisBits.setBit(static_cast<size_t>(AnalysisType::HOST_LEAK_ANALYSIS));
    config.analysisType = analysisBits.getValue();
    std::snprintf(config.outputDir, sizeof(config.outputDir), "%s", kCovChildDir);
    SetCovConfig(config);

    EventReport &report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    EXPECT_TRUE(report.hostMemStopLatch_.load()) << "deferred 模式下窗口初始关闭";
    EXPECT_EQ(report.GetPyStepId(), 0u);
    // bind 未装配（无钩子符号）：svc 为空，host 窗口重算只告警
    EXPECT_EQ(report.svcHostMem_, nullptr);
}

// bind 表接受/拒绝：EventReport 构造期一次性解析钩子符号
void RunBindHostMemHookFlow(bool accept)
{
    PrepareChildEnv();
    g_covBindAccept = accept;
    g_funcMocks["msmemscope_hostmem_bind"] = reinterpret_cast<void *>(&MockCovHostMemBind);
    g_covSetEnabledArgs.clear();

    EventReport &report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    if (accept)
    {
        EXPECT_EQ(report.svcHostMem_, &g_covMockSvc);
        // Instance() 的首开窗重算：分析位未开 → set_enabled(0)
        ASSERT_FALSE(g_covSetEnabledArgs.empty());
        EXPECT_EQ(g_covSetEnabledArgs.back(), 0);
    }
    else
    {
        EXPECT_EQ(report.svcHostMem_, nullptr);
        EXPECT_TRUE(g_covSetEnabledArgs.empty());
    }
}

// 未构造实例时的退出处理：直达指针为空，安静跳过；随后构造实例覆盖 ACL FINALIZE 路径
void RunExitBeforeInstanceFlow()
{
    PrepareChildEnv();
    unsetenv("ASCEND_RT_VISIBLE_DEVICES");
    // 无实例：HostMemExitHandler/report_stage 均为空转
    EventReport::HostMemExitHandler();
    msmemscope_hostmem_report_stage(1, 1, 1);
    msmemscope_hostmem_report_stage(0, 2, 2);
    msmemscope_hostmem_exit_close();

    EventReport &report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    // 采集态下 FINALIZE：驱动符号缺失（无 prof_stop 桩）→ 记录错误后继续上报
    Config config = GetCovConfig();
    SetCovConfig(config);
    EventTraceManager::Instance().status_ = EventTraceStatus::IN_TRACING;
    EventTraceManager::Instance().deviceReady_ = true;
    EXPECT_TRUE(report.ReportAclItf(RecordSubType::FINALIZE));
    // 未 bind：退出闭窗只告警
    report.svcHostMem_ = nullptr;
    report.hostMemStopLatch_.store(false);
    EventReport::HostMemExitHandler();
}

// 解释器已初始化时的 OOM 快照路径（import/取函数/取设备号任一失败即返回）
void RunPythonOomSnapshotFlow()
{
    PrepareChildEnv();
    unsetenv("ASCEND_RT_VISIBLE_DEVICES");
    Py_Initialize();
    ASSERT_NE(Py_IsInitialized(), 0);
    EventReport &report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    report.stepInfo_ = MstxStepInfo();
    report.ReportMemorySnapshotOnOOM();
    CallStackString stack = MakeStack();
    report.ReportMemorySnapshotOnOOM(stack);
}
}  // namespace

// --- EventReport 构造期/符号组合子用例 -------------------------------------

TEST(ErCovChild, EnvParsing)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    RunEnvParsingFlow();
}

TEST(ErCovChild, EnvParsingRtFallback)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    RunEnvParsingRtFallbackFlow();
}

TEST(ErCovChild, EnvParsingNoSymbol)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    RunEnvParsingNoSymbolFlow();
}

TEST(ErCovChild, DeviceIdPaths)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    RunDeviceIdFlow();
}

TEST(ErCovChild, DcmiInitPaths)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    if (!DcmiInitWouldRun())
    {
        GTEST_SKIP() << "当前环境 dcmi_init 被设计性短路，符号桩不生效";
    }
    RunDcmiInitFlow();
}

TEST(ErCovChild, DcmiDeviceMap)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    if (!DcmiInitWouldRun())
    {
        GTEST_SKIP() << "当前环境 dcmi_init 被设计性短路，符号桩不生效";
    }
    RunDcmiDeviceMapFlow();
}

TEST(ErCovChild, DcmiMapSymbolMissing)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    if (!DcmiInitWouldRun())
    {
        GTEST_SKIP() << "当前环境 dcmi_init 被设计性短路，符号桩不生效";
    }
    RunDcmiMapSymbolMissingFlow();
}

TEST(ErCovChild, HbmAndProcQueries)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    if (!DcmiInitWouldRun())
    {
        GTEST_SKIP() << "当前环境 dcmi_init 被设计性短路，符号桩不生效";
    }
    RunHbmAndProcFlow();
}

TEST(ErCovChild, QuerySymbolMissing)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    if (!DcmiInitWouldRun())
    {
        GTEST_SKIP() << "当前环境 dcmi_init 被设计性短路，符号桩不生效";
    }
    RunQuerySymbolMissingFlow();
}

TEST(ErCovChild, CtorDeferredWithAnalysis)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    RunCtorDeferredWithAnalysisFlow();
}

TEST(ErCovChild, BindHostMemHookAccept)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    RunBindHostMemHookFlow(true);
}

TEST(ErCovChild, BindHostMemHookReject)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    RunBindHostMemHookFlow(false);
}

TEST(ErCovChild, ExitBeforeInstance)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    RunExitBeforeInstanceFlow();
}

TEST(ErCovChild, PythonOomSnapshot)
{
    if (!IsCovChild())
    {
        GTEST_SKIP() << "仅在 RunMemScopeTestChild 拉起的子进程中执行";
    }
    RunPythonOomSnapshotFlow();
}

// --- 父用例：拉起隔离子进程并断言其正常退出 --------------------------------

class ErCovIsolation : public ::testing::Test
{
};

TEST_F(ErCovIsolation, env_parsing)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.EnvParsing"));
}

TEST_F(ErCovIsolation, env_parsing_rt_fallback)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.EnvParsingRtFallback"));
}

TEST_F(ErCovIsolation, env_parsing_no_symbol)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.EnvParsingNoSymbol"));
}

TEST_F(ErCovIsolation, device_id_paths)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.DeviceIdPaths"));
}

TEST_F(ErCovIsolation, dcmi_init_paths)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.DcmiInitPaths"));
}

TEST_F(ErCovIsolation, dcmi_device_map)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.DcmiDeviceMap"));
}

TEST_F(ErCovIsolation, dcmi_map_symbol_missing)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.DcmiMapSymbolMissing"));
}

TEST_F(ErCovIsolation, hbm_and_proc_queries)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.HbmAndProcQueries"));
}

TEST_F(ErCovIsolation, query_symbol_missing)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.QuerySymbolMissing"));
}

TEST_F(ErCovIsolation, ctor_deferred_with_analysis)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.CtorDeferredWithAnalysis"));
}

TEST_F(ErCovIsolation, bind_host_mem_hook_accept)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.BindHostMemHookAccept"));
}

TEST_F(ErCovIsolation, bind_host_mem_hook_reject)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.BindHostMemHookReject"));
}

TEST_F(ErCovIsolation, exit_before_instance)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.ExitBeforeInstance"));
}

TEST_F(ErCovIsolation, python_oom_snapshot)
{
    EXPECT_TRUE(RunMemScopeTestChild("ErCovChild.PythonOomSnapshot"));
}
