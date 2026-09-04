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

#include "event_report.h"

#include <dlfcn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>

#include "bit_field.h"
#include "cpython.h"
#include "decompose_analyzer.h"
#include "describe_trace.h"
#include "event_dispatcher.h"
#include "health_analyzer.h"
#include "host_leak_analyzer.h"
#include "inefficient_analyzer.h"
#include "json_manager.h"
#include "kernel_hooks/runtime_prof_api.h"
#include "leak_analyzer.h"
#include "log.h"
#include "memory_state_manager.h"
#include "securec.h"
#include "trace_manager/event_trace_manager.h"
#include "umask_guard.h"
#include "ustring.h"
#include "utils.h"
#include "vallina_symbol.h"

// 退出闭窗触发契约:本so内任何atexit handler都在teardown期执行,均不可用——
// 库级(__dso_handle)handler在libascend_leaks自身fini阶段(钩子so已被rtld_fini
// 先于本so拆卸,上报线程无法完成排空,实测g_closing永不清零→退出挂死);全局
// (dso=NULL)handler在_dl_fini末尾的__cxa_finalize(NULL)执行,晚于全部静态析构
// (实测~EventReport先于handler,闭窗因destroyed_直接跳过)。故退出闭窗改由钩子
// so的exit拦截器/main-return trampoline在调用真exit()之前经msmemscope_hostmem_exit_close
// 触发(见host_mem_hooks.cpp):此刻上报线程/分析器/装载器全部存活,闭窗与正常
// stop()同等可靠。钩子未装配/未拦截的路径退化为~HostLeakAnalyzer析构兜底

namespace MemScope
{
bool g_isReportHostMem = false;

// DCMI 接口常量与函数指针（签名与驱动 dcmi_interface 对齐）
constexpr int DCMI_OK = 0;
constexpr int DCMI_MAX_CARD_NUM = 32;
using DcmiInitFunc = int (*)();
using DcmiGetCardListFunc = int (*)(int*, int*, int);
using DcmiGetDeviceNumInCardFunc = int (*)(int, int*);
using DcmiGetDeviceLogicIdFunc = int (*)(int*, int, int);
using DcmiGetHbmInfoFunc = int (*)(int, int, struct dcmi_hbm_info*);
using DcmiProcMemInfoFunc = int (*)(int, int, struct dcmi_proc_mem_info*, int*);

constexpr uint64_t MEM_MODULE_ID_BIT = 56;
constexpr uint64_t MEM_VIRT_BIT = 10;
constexpr uint64_t MEM_SVM_VAL = 0x0;
constexpr uint64_t MEM_DEV_VAL = 0x1;
constexpr uint64_t MEM_HOST_VAL = 0x2;
constexpr uint64_t MEM_DVPP_VAL = 0x3;
constexpr uint32_t MAX_THREAD_NUM = 200;

MemOpSpace GetMemOpSpace(unsigned long long flag)
{
    // bit10~13: virt mem type(svm\dev\host\dvpp)
    int32_t memType = (flag & 0b11110000000000) >> MEM_VIRT_BIT;
    MemOpSpace space = MemOpSpace::INVALID;
    switch (memType)
    {
        case MEM_SVM_VAL:
            space = MemOpSpace::SVM;
            break;
        case MEM_DEV_VAL:
            space = MemOpSpace::DEVICE;
            break;
        case MEM_HOST_VAL:
            space = MemOpSpace::HOST;
            break;
        case MEM_DVPP_VAL:
            space = MemOpSpace::DVPP;
            break;
        default:
            LOG_ERROR("No matching memType for %d", memType);
    }
    return space;
}

constexpr unsigned long long MEM_PAGE_GIANT_BIT = 31;
constexpr unsigned long long MEM_PAGE_GIANT = (0X1UL << MEM_PAGE_GIANT_BIT);

constexpr unsigned long long MEM_PAGE_BIT = 17;
constexpr unsigned long long MEM_PAGE_NORMAL = (0X0UL << MEM_PAGE_BIT);
constexpr unsigned long long MEM_PAGE_HUGE = (0X1UL << MEM_PAGE_BIT);

MemPageType GetMemPageType(unsigned long long flag)
{
    // bit17: page size(nomal\huge)
    // bit31: page size(giant)

    if ((flag & MEM_PAGE_GIANT) != 0)
    {
        return MemPageType::MEM_GIANT_PAGE_TYPE;
    }
    else if ((flag & MEM_PAGE_HUGE) != 0)
    {
        return MemPageType::MEM_HUGE_PAGE_TYPE;
    }
    else
    {
        return MemPageType::MEM_NORMAL_PAGE_TYPE;
    }
}

inline int32_t GetMallocModuleId(unsigned long long flag)
{
    // bit56~63: model id
    return (flag & 0xFF00000000000000) >> MEM_MODULE_ID_BIT;
}

constexpr int32_t INVALID_MODID = -1;

GetDeviceInfo& GetDeviceInfo::Instance()
{
    static GetDeviceInfo instance;
    return instance;
}

bool GetDeviceInfo::GetDeviceId(int32_t& devId)
{
    char const* sym = "aclrtGetDeviceImpl";
    using AclrtGetDevice = aclError (*)(int32_t*);
    static AclrtGetDevice vallina = nullptr;
    if (vallina == nullptr)
    {
        vallina = VallinaSymbol<ACLImplLibLoader>::Instance().Get<AclrtGetDevice>(sym);
    }
    if (vallina == nullptr)
    {
        LOG_ERROR("vallina func get FAILED: %s, try to get it in legacy way.", __func__);

        // 添加老版本的GetDevice逻辑，用于兼容情况如开放态场景
        char const* l_sym = "rtGetDevice";
        using RtGetDevice = rtError_t (*)(int32_t*);
        static RtGetDevice l_vallina = nullptr;
        if (l_vallina == nullptr)
        {
            l_vallina = VallinaSymbol<RuntimeLibLoader>::Instance().Get<RtGetDevice>(l_sym);
        }
        if (l_vallina == nullptr)
        {
            LOG_ERROR("vallina func get FAILED in legacy way: %s", __func__);
            return false;
        }

        // 进入真实运行时调用窗口：运行时内部的内存申请会被hook捕获并尝试上报，
        // 抑制期间直接跳过，防止递归上报与幻影事件
        EventReportSuppressor suppressor;
        rtError_t ret = l_vallina(&devId);
        if (ret == RT_ERROR_INVALID_VALUE)
        {
            return false;
        }
        return true;
    }

    // 进入真实运行时调用窗口：运行时内部的内存申请会被hook捕获并尝试上报，
    // 抑制期间直接跳过，防止递归上报与幻影事件
    EventReportSuppressor suppressor;
    aclError ret = vallina(&devId);
    if (ret != ACL_SUCCESS)
    {
        return false;
    }

    return TransDeviceId(devId);
}

bool GetDeviceInfo::TransDeviceId(int32_t& devId)
{
    // 新增可见卡选项
    if (!setVisibleDevice)
    {
        return true;
    }
    auto it = visibleDeviceMap.find(devId);
    if (it == visibleDeviceMap.end())
    {
        LOG_ERROR("Key %d not found in visibleDeviceMap!", devId);
        return false;
    }
    devId = it->second;
    return true;
}

bool GetDeviceInfo::EnsureDcmiInit()
{
    // dcmi_init 只尝试一次：失败（权限/驱动问题）后本进程内永久降级，
    // 重试无意义且每次失败都走一次驱动初始化会拖慢查询路径
    std::call_once(dcmiInitFlag_,
                   [this]()
                   {
                       static auto initFunc = VallinaSymbol<DcmiLibLoader>::Instance().Get<DcmiInitFunc>("dcmi_init");
                       if (initFunc == nullptr)
                       {
                           LOG_ERROR("Get dcmi_init func ptr failed");
                           return;
                       }
                       // 进入真实驱动调用窗口：驱动初始化内部的内存申请/上报会被hook捕获，
                       // 抑制期间直接跳过，防止递归上报与幻影事件
                       EventReportSuppressor suppressor;
                       if (initFunc() != DCMI_OK)
                       {
                           LOG_ERROR("dcmi_init failed");
                           return;
                       }
                       dcmiReady_ = true;
                   });
    return dcmiReady_;
}

bool GetDeviceInfo::BuildDeviceMap()
{
    // 建表只做一次：失败（符号缺失/设备枚举失败）与成功都置已尝试标记，
    // 避免每次查询都重复枚举卡与设备
    std::call_once(
        dcmiMapInitFlag_,
        [this]()
        {
            static auto getCardList =
                VallinaSymbol<DcmiLibLoader>::Instance().Get<DcmiGetCardListFunc>("dcmi_get_card_list");
            static auto getDeviceNum =
                VallinaSymbol<DcmiLibLoader>::Instance().Get<DcmiGetDeviceNumInCardFunc>("dcmi_get_device_num_in_card");
            static auto getLogicId =
                VallinaSymbol<DcmiLibLoader>::Instance().Get<DcmiGetDeviceLogicIdFunc>("dcmi_get_device_logic_id");
            if (getCardList == nullptr || getDeviceNum == nullptr || getLogicId == nullptr)
            {
                LOG_ERROR("Get dcmi device map func ptr failed");
                return;
            }

            int cardList[DCMI_MAX_CARD_NUM] = {0};
            int cardNum = 0;
            {
                EventReportSuppressor suppressor;
                if (getCardList(&cardNum, cardList, DCMI_MAX_CARD_NUM) != DCMI_OK)
                {
                    LOG_ERROR("dcmi_get_card_list failed");
                    return;
                }
            }
            for (int i = 0; i < cardNum && i < DCMI_MAX_CARD_NUM; i++)
            {
                int deviceNum = 0;
                {
                    EventReportSuppressor suppressor;
                    if (getDeviceNum(cardList[i], &deviceNum) != DCMI_OK)
                    {
                        LOG_ERROR("dcmi_get_device_num_in_card failed, card_id %d", cardList[i]);
                        return;
                    }
                }
                for (int deviceId = 0; deviceId < deviceNum; deviceId++)
                {
                    int logicId = 0;
                    {
                        EventReportSuppressor suppressor;
                        if (getLogicId(&logicId, cardList[i], deviceId) != DCMI_OK)
                        {
                            LOG_ERROR("dcmi_get_device_logic_id failed, card_id %d device_id %d", cardList[i],
                                      deviceId);
                            return;
                        }
                    }
                    // acl 物理逻辑号 = 硬件逻辑号，HAL 事件 devId（flag 解析）与其同源，
                    // 直接以逻辑号建表
                    devIdToDcmiMap_[logicId] = {cardList[i], deviceId};
                }
            }
            dcmiMapReady_ = true;
        });
    return dcmiMapReady_;
}

bool GetDeviceInfo::GetDeviceHbmInfo(int32_t devId, uint64_t& usedMb, uint64_t& totalMb)
{
    if (!EnsureDcmiInit() || !BuildDeviceMap())
    {
        return false;
    }
    auto it = devIdToDcmiMap_.find(devId);
    if (it == devIdToDcmiMap_.end())
    {
        // 未在设备映射中的逻辑号（如 HOST 设备 DEVICE_ID_CPU）无整卡用量
        LOG_DEBUG("devId %d not in dcmi device map", devId);
        return false;
    }
    static auto getHbmInfo =
        VallinaSymbol<DcmiLibLoader>::Instance().Get<DcmiGetHbmInfoFunc>("dcmi_get_device_hbm_info");
    if (getHbmInfo == nullptr)
    {
        LOG_ERROR("Get dcmi_get_device_hbm_info func ptr failed");
        return false;
    }
    struct dcmi_hbm_info info
    {
    };
    {
        // 驱动查询内部可能有临时内存申请，抑制期间跳过上报，防止递归上报与幻影事件
        EventReportSuppressor suppressor;
        if (getHbmInfo(it->second.first, it->second.second, &info) != DCMI_OK)
        {
            LOG_ERROR("dcmi_get_device_hbm_info failed, devId %d", devId);
            return false;
        }
    }
    usedMb = info.memory_usage;
    totalMb = info.memory_size;
    return true;
}

bool GetDeviceInfo::GetDeviceProcMemInfo(int32_t devId, uint64_t& usedBytes)
{
    if (!EnsureDcmiInit() || !BuildDeviceMap())
    {
        return false;
    }
    auto it = devIdToDcmiMap_.find(devId);
    if (it == devIdToDcmiMap_.end())
    {
        // 未在设备映射中的逻辑号（如 HOST 设备 DEVICE_ID_CPU）无进程占用可查
        LOG_DEBUG("devId %d not in dcmi device map", devId);
        return false;
    }
    static auto getProcMemInfo =
        VallinaSymbol<DcmiLibLoader>::Instance().Get<DcmiProcMemInfoFunc>("dcmi_get_npu_proc_mem_info");
    if (getProcMemInfo == nullptr)
    {
        LOG_ERROR("Get dcmi_get_npu_proc_mem_info func ptr failed");
        return false;
    }
    // 驱动按 DCMI_MAX_PROC_NUM_IN_DEVICE(64) 枚举该卡进程列表，输出数组需预留同等容量
    struct dcmi_proc_mem_info procInfo[64]{};
    int procNum = 0;
    {
        // 驱动查询内部可能有临时内存申请，抑制期间跳过上报，防止递归上报与幻影事件
        EventReportSuppressor suppressor;
        if (getProcMemInfo(it->second.first, it->second.second, procInfo, &procNum) != DCMI_OK)
        {
            LOG_ERROR("dcmi_get_npu_proc_mem_info failed, devId %d", devId);
            return false;
        }
    }
    // 进程列表按 pid 过滤本进程：proc_mem_usage 即 npu-smi Process memory 同源值（字节）
    const uint64_t selfPid = Utility::GetPid();
    for (int i = 0; i < procNum; i++)
    {
        if (static_cast<uint64_t>(procInfo[i].proc_id) == selfPid)
        {
            usedBytes = static_cast<uint64_t>(procInfo[i].proc_mem_usage);
            return true;
        }
    }
    // 本进程在该卡无显存占用记录（未申请过内存/已退出），按查询失败处理
    LOG_DEBUG("pid %llu not in device %d proc mem list", static_cast<unsigned long long>(selfPid), devId);
    return false;
}

int64_t EventReport::QueryProcessUsed(int32_t devId)
{
    uint64_t usedBytes = 0;
    if (!GetDeviceInfo::Instance().GetDeviceProcMemInfo(devId, usedBytes))
    {
        // 查询失败：不更新缓存（保留最近一次成功值），限频告警一次后静默
        if (!processUsedQueryWarned_.exchange(true))
        {
            LOG_WARN("Query device proc mem info failed, process_used will be -1");
        }
        return -1;
    }
    int64_t used = static_cast<int64_t>(usedBytes);
    // 缓存数据在 MemoryStateManager（槽位与事件 device 同源，与 QueryDeviceUsed 一致），
    // EventReport 仅负责查询动作
    MemoryStateManager::GetInstance().UpdateProcessUsedCache(devId, used);
    return used;
}

int64_t EventReport::QueryDeviceUsed(int32_t devId)
{
    uint64_t usedMb = 0;
    uint64_t totalMb = 0;
    if (!GetDeviceInfo::Instance().GetDeviceHbmInfo(devId, usedMb, totalMb))
    {
        // 查询失败：不更新缓存（保留最近一次成功值），限频告警一次后静默
        if (!deviceUsedQueryWarned_.exchange(true))
        {
            LOG_WARN("Query device hbm info failed, device_used will be -1");
        }
        return -1;
    }
    // dcmi 返回 MB，事件字段 deviceUsed 保持字节（与 dump 输出语义一致）
    int64_t used = static_cast<int64_t>(usedMb) * 1024 * 1024;
    // 缓存数据在 MemoryStateManager（槽位与事件 device 同源：HAL 事件 flag 解析/池事件
    // TransDeviceId 均归一为物理卡号），EventReport 仅负责查询动作
    MemoryStateManager::GetInstance().UpdateDeviceUsedCache(devId, used);
    return used;
}

EventReport& EventReport::Instance(MemScopeCommType type)
{
    static EventReport instance(type);
    // host窗口首开必须在static初始化完成、guard释放之后执行:开窗经set_enabled(true)同步
    // 派发STAGE_START,派发路径上的分析器会重入Instance()(如HostLeakAnalyzer::HandleStageStart
    // 经GetHostMemStats读丢包基线),若guard仍被本线程持有,重入__cxa_guard_acquire即死锁
    // (CLI host模式在so加载期100%复现)。故构造函数只bind不开窗,首开窗在此补做。
    // call_once保证全进程恰好一次;thread_local护栏让开窗派发内的同线程嵌套Instance()
    // 跳过call_once直接返回(同线程重入call_once同样会自锁);其余线程在开窗完成前
    // 于call_once上等待(开窗线程不依赖它们,无死锁)
    static std::once_flag hostMemWindowOnce;
    static thread_local bool t_inHostMemWindowOpen = false;
    if (!t_inHostMemWindowOpen)
    {
        std::call_once(hostMemWindowOnce,
                       []()
                       {
                           t_inHostMemWindowOpen = true;
                           instance.UpdateHostMemWindow();
                           // 退出闭窗不再做atexit注册(理由见文件头"退出闭窗触发契约"注释):
                           // 库级/全局handler均晚于可用时点,由钩子so的exit拦截器/main-return
                           // trampoline在真exit()之前经msmemscope_hostmem_exit_close触发。钩子
                           // 未装配/未拦截的路径退化为~HostLeakAnalyzer析构兜底(unknown报告)
                       });
    }
    return instance;
}

// host钩子回调直达实例的裸指针(防御层,与Instance()内开窗时机互补):
// 4个report_*包装层不经Instance()访问实例——magic-static的guard不可重入,回调路径上
// 任何形式的重入(构造期回调、或开窗派发期间的跨线程guard等待)都依赖调用时序才安全,
// 直达指针使该层与guard彻底解耦。时序合法性:回调只可能经bind注册后触发,构造函数在
// bind之前先store(this, release)(此刻成员/分析器均已就绪),包装层load(acquire)配对。
// 析构后安全性不回退:包装层仍命中本指针,由ReportHost*入口的destroyed_检查兜底
// (与Instance()返回已析构单例的既有行为一致)
static std::atomic<EventReport*> g_hostMemReportInstance{nullptr};

// 退出闭窗排空等待的轮询信号:钩子独立诊断导出msmemscope_hostmem_get_status的
// closing位(bit2)。走独立符号而非svc表成员——避免svc表C ABI变更,新旧so混部时
// 头文件版本不齐也安全。svcHostMem_有效⇒钩子so在位⇒符号必在;返回-1=符号不可得
// (调用方按无需等待处理)
int (*g_hostMemStatusFn)(void) = nullptr;

int QueryHostMemStatus()
{
    if (g_hostMemStatusFn == nullptr)
    {
        g_hostMemStatusFn = reinterpret_cast<int (*)(void)>(dlsym(RTLD_DEFAULT, "msmemscope_hostmem_get_status"));
        if (g_hostMemStatusFn == nullptr)
        {
            return -1;
        }
    }
    return g_hostMemStatusFn();
}

// 无环形缓冲/待补扫栈,退出等待的周期日志为纯状态打点(见CloseHostMemWindowAtExit)

// 无逐块丢包/栈层失败口径,归因粒度退化为未知桶,由stats.truncated bit1标注

// 进程退出闭窗入口(见event_report.h声明注释与msmemscope_hostmem_exit_close;
// 钩子exit拦截器/main trampoline在真exit()之前调用,异常绝不穿出——拦截器上下文
// 未捕获异常即std::terminate杀宿主)
void EventReport::HostMemExitHandler()
{
    EventReport* instance = g_hostMemReportInstance.load(std::memory_order_acquire);
    if (instance == nullptr)
    {
        fprintf(stderr, "[msmemscope] host leak [pid=%llu] exit handler: no report instance\n",
                static_cast<unsigned long long>(getpid()));
        return;
    }
    fprintf(stderr, "[msmemscope] host leak [pid=%llu] exit close running (pre-exit interceptor)\n",
            static_cast<unsigned long long>(getpid()));
    try
    {
        instance->CloseHostMemWindowAtExit();
    }
    catch (...)
    {
        fprintf(stderr, "[msmemscope] host leak [pid=%llu] exit close aborted\n",
                static_cast<unsigned long long>(getpid()));
    }
}

// 退出闭窗C入口(钩子so经dlsym调用,契约见文件头"退出闭窗触发契约"注释):
// 钩子exit拦截器/main-return trampoline在调用真exit()之前调用本符号——此刻
// 上报线程/分析器/装载器全部存活,闭窗与正常stop()同等可靠。钩子未装配或
// 已在teardown期(理论不可达)调用时由内部各层守卫自洽跳过
extern "C" void msmemscope_hostmem_exit_close(void) { MemScope::EventReport::HostMemExitHandler(); }

void EventReport::Init()
{
    recordIndex_.store(0);
    kernelLaunchRecordIndex_.store(0);
    pyStepId_.store(0);
}

EventReport::EventReport(MemScopeCommType type)
{
    Init();
    // 动态读取当前配置（不持有副本），保证后续行为与运行中修改的配置保持一致
    const Config& config = GetConfig();

    // host窗口初始门控按采集模式定:deferred下窗口初始关闭,等待start()经
    // ReportTraceStatus(IN_TRACING)复位闩锁后才开窗——否则CLI deferred模式
    // (HostMemReportBoot在so加载期构造本对象)在start()之前即全程采集,
    // "等待start"语义失效(用户实测bug)。immediate保持false=构造即开窗
    if (config.collectMode == static_cast<uint8_t>(CollectMode::DEFERRED))
    {
        hostMemStopLatch_.store(true, std::memory_order_relaxed);
    }

    // subscribe订阅
    BitField<decltype(config.analysisType)> analysisType(config.analysisType);
    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS)))
    {
        DecomposeAnalyzer::GetInstance();
    }
    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::INEFFICIENCY_ANALYSIS)))
    {
        InefficientAnalyzer::GetInstance();
    }
    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::HOST_LEAK_ANALYSIS)))
    {
        HostLeakAnalyzer::GetInstance();
    }
    Dump::GetInstance();

    // 注册通过EventDispatcher订阅的分析器（替代Process::SendEvent中的switch-case分发）
    // 构造顺序即派发顺序（同优先级下后插入的订阅者先收到事件）：
    // 先HealthAnalyzer后LeakAnalyzer，保证MALLOC/FREE先经LeakAnalyzer（对应原StepInnerAnalyzer槽位），
    // MSTX先经LeakAnalyzer::CheckNpuLeak再经HealthAnalyzer::CheckGap（保持原告警顺序）
    // 统计字段回填（used/processUsed）由MemoryStateManager::UpdateUsage在事件处理阶段一完成，
    // 先于所有分析器，无需订阅
    HealthAnalyzer::GetInstance();
    LeakAnalyzer::GetInstance();

    // host堆钩子bind注册(钩子so未装配时安静跳过)。构造期只bind不开窗:开窗会同步派发
    // STAGE_START,分析器重入Instance()在guard持有期即死锁,首开窗由Instance()在static
    // 初始化完成后补做(见其注释)。先发布裸指针再bind:回调只可能经bind注册后触发,
    // 发布点先于bind即安全(见g_hostMemReportInstance注释)
    g_hostMemReportInstance.store(this, std::memory_order_release);
    BindHostMemHook();

    RegisterRtProfileCallback();

    return;
}

void EventReport::UpdateAnalysisType()
{
    const Config& config = GetConfig();
    BitField<decltype(config.analysisType)> analysisType(config.analysisType);

    // 根据config确认是否订阅或者取消订阅
    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS)))
    {
        DecomposeAnalyzer::GetInstance().Subscribe();
    }
    else
    {
        DecomposeAnalyzer::GetInstance().UnSubscribe();
    }

    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::INEFFICIENCY_ANALYSIS)))
    {
        InefficientAnalyzer::GetInstance().Subscribe();
    }
    else
    {
        InefficientAnalyzer::GetInstance().UnSubscribe();
    }

    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::HOST_LEAK_ANALYSIS)))
    {
        HostLeakAnalyzer::GetInstance().Subscribe();
    }
    // 移除host-leaks时不再UnSubscribe:关窗(set_enabled(0))由钩子上报线程异步
    // 派发STAGE_END/栈文本,若在此先退订,END与栈串派发到空订阅被整体丢弃——
    // 分析器窗口残留"孤儿"(open恒true,endTs不落),退出时由~HostLeakAnalyzer兜底
    // 出"Data integrity: unknown (process exit path)"+全栈unresolved的报告。
    // 窗口关闭后分析器无开窗即忽略全部事件(HandleHost*入口的open检查),残留订阅
    // 无副作用;重新加入host-leaks时Subscribe幂等,退订由析构(UnSubscribe)完成

    // host-leak窗口跟随analysis位迁移(移除host-leaks即闭窗,输出报告)
    UpdateHostMemWindow();
}

EventReport::~EventReport() { destroyed_.store(true); }

bool EventReport::IsNeedSkip(int32_t devid)
{
    // 是否为指定卡
    const Config& config = GetConfig();
    if (devid == DEVICE_ID_CPU)
    {
        if (!config.collectCpu)
        {
            return true;
        }
    }
    else if (!config.collectAllNpu)
    {
        BitField<decltype(config.npuSlots)> npuSlots(config.npuSlots);
        if (devid != GD_INVALID_NUM && !npuSlots.checkBit(static_cast<size_t>(devid)))
        {
            return true;
        }
    }

    // 目前仅命令行支持选择--steps，因此当存在stepList时代表启用了命令行，我们不推荐同时使用命令行和python接口。这里不考虑
    // msmemscope.step()接口所带来的的step信息。
    auto& stepList = config.stepList;
    if (stepList.stepCount == 0)
    {
        return false;
    }

    for (uint8_t loop = 0; (loop < stepList.stepCount && loop < SELECTED_STEP_MAX_NUM); loop++)
    {
        if (stepInfo_.currentStepId == stepList.stepIdList[loop] && stepInfo_.inStepRange)
        {
            return false;
        }
    }

    return true;
}

bool EventReport::ReportAddrInfo(EventSubType type, uint64_t addr,
                                 const std::vector<std::pair<OwnerLevel, std::string>>& labels)
{
    int32_t devId = GD_INVALID_NUM;
    GetDeviceInfo::Instance().GetDeviceId(devId);
    if (IsNeedSkip(devId))
    {
        return true;
    }
    std::vector<std::pair<OwnerLevel, std::string>> safeLabels = labels;
    for (auto& item : safeLabels)
    {
        Utility::ToSafeString(item.second);
    }
    std::shared_ptr<MemoryOwnerEvent> event = std::make_shared<MemoryOwnerEvent>();
    event->eventType = EventBaseType::MEMORY_OWNER;
    event->eventSubType = type;
    event->ownerLabels = safeLabels;
    event->addr = addr;
    event->device = devId;

    Process::GetInstance().SendEvent(event);
    return true;
}

bool EventReport::ReportPyStepRecord()
{
    int32_t devId = GD_INVALID_NUM;
    GetDeviceInfo::Instance().GetDeviceId(devId);
    if (IsNeedSkip(devId))
    {
        return true;
    }

    std::shared_ptr<SystemEvent> event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = EventSubType::STEP;
    event->device = devId;
    event->name = std::to_string(++pyStepId_);

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportMemPoolRecord(EventSubType type, const MemoryUsage& info, CallStackString&& stack)
{
    TraceMode traceMode = DetermineTraceMode();
    if (traceMode == TraceMode::SKIP)
    {
        return true;
    }

    int32_t realDevice = static_cast<int32_t>(info.deviceIndex);
    GetDeviceInfo::Instance().TransDeviceId(realDevice);
    if (IsNeedSkip(realDevice))
    {
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = info.dataType == 0 ? EventBaseType::MALLOC : EventBaseType::FREE;
    if (type == EventSubType::PTA_CACHING)
    {
        event->poolType = PoolType::PTA_CACHING;
        event->eventSubType = EventSubType::PTA_CACHING;
    }
    else if (type == EventSubType::PTA_WORKSPACE)
    {
        event->poolType = PoolType::PTA_WORKSPACE;
        event->eventSubType = EventSubType::PTA_WORKSPACE;
    }
    else if (type == EventSubType::ATB)
    {
        event->poolType = PoolType::ATB;
        event->eventSubType = EventSubType::ATB;
    }
    else
    {
        event->poolType = PoolType::MINDSPORE;
        event->eventSubType = EventSubType::MINDSPORE;
    }

    event->addr = info.ptr;
    event->name = "N/A";
    event->device = realDevice;
    event->size = info.allocSize;
    event->kernelIndex = kernelLaunchRecordIndex_;

    if (traceMode == TraceMode::SHADOW)
    {
        // 影子模式：仅上报最小数据集，不上报调用栈和owner信息
        event->isShadowEvent = true;
        Process::GetInstance().SendEvent(event);
        return true;
    }

    // 正常采集模式：上报完整数据(分级标签不随事件携带, 由 DecomposeAnalyzer::InitOwner 分析时
    // 直接从 DescribeTrace 读取——采集与分析同线程同步路由, 线程局部标签栈即申请时刻状态)
    event->total = info.totalReserved;
    event->used = info.totalAllocated;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);

    // 池事件不查询（池分配高频），直接读最近一次 HAL 查询缓存（数据在 MemoryStateManager）
    event->deviceUsed = MemoryStateManager::GetInstance().GetDeviceUsed(event->device);
    event->processUsed = MemoryStateManager::GetInstance().GetProcessUsed(event->device);

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportHalCreate(uint64_t addr, uint64_t size, const drv_mem_prop& prop, CallStackString&& stack)
{
    if (IsNeedSkip(prop.devid))
    {
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HAL;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);
    event->poolType = PoolType::HAL;
    event->addr = addr;
    event->name = "N/A";
    event->space = MemOpSpace::DEVICE;
    event->device = prop.devid;
    event->size = size;
    event->moduleId = prop.module_id;
    event->pageType = static_cast<MemScope::MemPageType>(prop.pg_type);
    event->flag = FLAG_INVALID;
    event->kernelIndex = kernelLaunchRecordIndex_;

    {
        if (!destroyed_.load())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            halPtrs_.emplace(addr, prop.devid);
        }
    }

    // MALLOC（Create 入口）事件值为申请后的整卡/本进程用量，按分配设备查询并缓存
    event->deviceUsed = QueryDeviceUsed(prop.devid);
    event->processUsed = QueryProcessUsed(prop.devid);

    Process::GetInstance().SendEvent(event);
    return true;
}

bool EventReport::ReportHalRelease(uint64_t addr, CallStackString&& stack)
{
    if (IsNeedSkip(GD_INVALID_NUM))
    {
        return true;
    }

    // 分配时记录的device，free事件据此回填（分配时语义）
    int32_t devId = GD_INVALID_NUM;
    {
        // 单例类析构之后不再访问其成员变量
        if (destroyed_.load())
        {
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = halPtrs_.find(addr);
        if (it == halPtrs_.end())
        {
            return true;
        }
        devId = it->second;
        halPtrs_.erase(it);
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = EventSubType::HAL;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);
    event->poolType = PoolType::HAL;
    event->addr = addr;
    event->name = "N/A";
    event->space = MemOpSpace::INVALID;
    event->device = devId;
    event->size = 0;
    event->moduleId = INVALID_MODID;
    event->flag = FLAG_INVALID;
    event->kernelIndex = kernelLaunchRecordIndex_;

    // FREE（Release/Free 入口）事件值为释放后的整卡/本进程用量，按 halPtrs_ 查表回填的分配设备查询并缓存
    event->deviceUsed = QueryDeviceUsed(devId);
    event->processUsed = QueryProcessUsed(devId);

    Process::GetInstance().SendEvent(event);

    return true;
}

// hostPtrs_属主通道标记(跨通道地址簿防误擦): 锁页地址与malloc堆地址实际来自不同映射区,
// cpu-tensor通道的地址重叠才是真实场景(tensor不经框架池直接malloc)。
// host堆钩子通道不经EventReport上报,地址簿仅剩HAL锁页内存与cpu-tensor两个通道
constexpr uint8_t HOST_PTR_FROM_HAL_PINNED = 0;  // HAL锁页内存(aclrtMallocHost族/ReportHostRegister)
constexpr uint8_t HOST_PTR_FROM_CPU_TENSOR = 1;  // cpu-tensor采集通道(ReportCpuTensor)

bool EventReport::ReportHalMalloc(uint64_t addr, uint64_t size, unsigned long long flag, CallStackString&& stack)
{
    // bit0~9 devId
    int32_t devId = (flag & 0x3FF);
    MemOpSpace space = GetMemOpSpace(flag);
    if (space == MemOpSpace::HOST)
    {
        devId = DEVICE_ID_CPU;
    }
    if (IsNeedSkip(devId))
    {
        return true;
    }

    int32_t moduleId = GetMallocModuleId(flag);

    bool isHostSpace = (space == MemOpSpace::HOST);
    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = isHostSpace ? EventSubType::HOST : EventSubType::HAL;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);
    event->poolType = isHostSpace ? PoolType::HOST : PoolType::HAL;
    event->isPinned = isHostSpace;
    event->addr = addr;
    event->name = "N/A";
    event->space = space;
    event->device = devId;
    event->size = size;
    event->moduleId = moduleId;
    event->flag = flag;
    event->kernelIndex = kernelLaunchRecordIndex_;
    if (isHostSpace)
    {
        event->used = static_cast<int64_t>(Utility::GetProcessVmRss());
    }

    {
        if (!destroyed_.load())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (isHostSpace && hostPtrs_.count(addr) > 0)
            {
                return true;  // HOST 地址已被其他通道持有（先到先得），静默去重
            }
            halPtrs_.emplace(addr, devId);
            if (isHostSpace)
            {
                hostPtrs_.emplace(addr, HOST_PTR_FROM_HAL_PINNED);
            }
        }
    }

    // MALLOC（flag 入口）事件值为申请后的整卡/本进程用量，按 flag 解析设备查询并缓存；
    // HOST 空间（锁页内存）无整卡概念，不查询（deviceUsed/processUsed 保持 -1）
    if (space != MemOpSpace::HOST)
    {
        event->deviceUsed = QueryDeviceUsed(devId);
        event->processUsed = QueryProcessUsed(devId);
    }

    Process::GetInstance().SendEvent(event);

    return true;
}

// Shadow overload (no callstack): minimal event for NOT_IN_TRACING mode
bool EventReport::ReportHalMalloc(uint64_t addr, uint64_t size, unsigned long long flag)
{
    int32_t devId = (flag & 0x3FF);
    MemOpSpace space = GetMemOpSpace(flag);
    if (space == MemOpSpace::HOST)
    {
        devId = DEVICE_ID_CPU;
    }
    if (IsNeedSkip(devId))
    {
        return true;
    }

    bool isHostSpace = (space == MemOpSpace::HOST);
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = isHostSpace ? EventSubType::HOST : EventSubType::HAL;
    event->poolType = isHostSpace ? PoolType::HOST : PoolType::HAL;
    event->isPinned = isHostSpace;
    event->addr = addr;
    event->name = "N/A";
    event->size = static_cast<int64_t>(size);
    event->device = devId;
    event->space = space;
    event->isShadowEvent = true;
    event->kernelIndex = kernelLaunchRecordIndex_;
    // No callstack, no moduleId, no pageType, no owner for shadow events

    {
        if (!destroyed_.load())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (isHostSpace && hostPtrs_.count(addr) > 0)
            {
                return true;  // HOST 地址已被其他通道持有（先到先得），静默去重
            }
            halPtrs_.emplace(addr, devId);
            if (isHostSpace)
            {
                hostPtrs_.emplace(addr, HOST_PTR_FROM_HAL_PINNED);
            }
        }
    }

    Process::GetInstance().SendEvent(event);

    return true;
}

// Shadow overload (no callstack): minimal event for NOT_IN_TRACING mode
bool EventReport::ReportHalFree(uint64_t addr)
{
    // 分配时记录的device，free事件据此回填（分配时语义）
    int32_t devId = GD_INVALID_NUM;
    {
        if (destroyed_.load())
        {
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = halPtrs_.find(addr);
        if (it == halPtrs_.end())
        {
            return true;  // 未在halMemAlloc中注册过的地址，过滤掉
        }
        devId = it->second;
        halPtrs_.erase(it);
        if (devId == DEVICE_ID_CPU)
        {
            // 属主校验: 仅归还本通道登记的条目(锁页地址与堆地址来自不同映射区,重叠为防御场景)
            auto hostIt = hostPtrs_.find(addr);
            if (hostIt != hostPtrs_.end() && hostIt->second == HOST_PTR_FROM_HAL_PINNED)
            {
                hostPtrs_.erase(hostIt);
            }
        }
    }

    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = (devId == DEVICE_ID_CPU) ? EventSubType::HOST : EventSubType::HAL;
    event->poolType = (devId == DEVICE_ID_CPU) ? PoolType::HOST : PoolType::HAL;
    event->isPinned = (devId == DEVICE_ID_CPU);
    event->addr = addr;
    event->name = "N/A";
    event->device = devId;  // 分配时语义：与MALLOC事件flag解析同源
    event->isShadowEvent = true;
    event->kernelIndex = kernelLaunchRecordIndex_;
    // No callstack for shadow events

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportHalFree(uint64_t addr, CallStackString&& stack)
{
    if (IsNeedSkip(GD_INVALID_NUM))
    {
        return true;
    }

    // 分配时记录的device，free事件据此回填（分配时语义）
    int32_t devId = GD_INVALID_NUM;
    {
        // 单例类析构之后不再访问其成员变量
        if (destroyed_.load())
        {
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = halPtrs_.find(addr);
        if (it == halPtrs_.end())
        {
            return true;
        }
        devId = it->second;
        halPtrs_.erase(it);
        if (devId == DEVICE_ID_CPU)
        {
            // 属主校验: 仅归还本通道登记的条目(锁页地址与堆地址来自不同映射区,重叠为防御场景)
            auto hostIt = hostPtrs_.find(addr);
            if (hostIt != hostPtrs_.end() && hostIt->second == HOST_PTR_FROM_HAL_PINNED)
            {
                hostPtrs_.erase(hostIt);
            }
        }
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = (devId == DEVICE_ID_CPU) ? EventSubType::HOST : EventSubType::HAL;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);
    event->poolType = (devId == DEVICE_ID_CPU) ? PoolType::HOST : PoolType::HAL;
    event->isPinned = (devId == DEVICE_ID_CPU);
    event->addr = addr;
    event->name = "N/A";
    event->space = MemOpSpace::INVALID;
    event->device = devId;
    event->size = 0;
    event->moduleId = INVALID_MODID;
    event->flag = FLAG_INVALID;
    event->kernelIndex = kernelLaunchRecordIndex_;

    // FREE（Release/Free 入口）事件值为释放后的整卡/本进程用量，按 halPtrs_ 查表回填的分配设备查询并缓存
    event->deviceUsed = QueryDeviceUsed(devId);
    event->processUsed = QueryProcessUsed(devId);

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportHostRegister(uint64_t addr, uint64_t size, CallStackString&& stack)
{
    if (IsNeedSkip(DEVICE_ID_CPU))
    {
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HOST;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);
    event->poolType = PoolType::HOST;
    event->isPinned = true;
    event->addr = addr;
    event->name = "N/A";
    event->space = MemOpSpace::HOST;
    event->device = DEVICE_ID_CPU;
    event->size = size;
    event->kernelIndex = kernelLaunchRecordIndex_;
    event->used = static_cast<int64_t>(Utility::GetProcessVmRss());

    {
        if (!destroyed_.load())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (hostPtrs_.count(addr) > 0)
            {
                return true;  // HOST 地址已被其他通道持有（先到先得），静默去重
            }
            // 锁页内存（HOST）映射到CPU设备（DEVICE_ID_CPU），与事件device一致
            halPtrs_.emplace(addr, DEVICE_ID_CPU);
            hostPtrs_.emplace(addr, HOST_PTR_FROM_HAL_PINNED);
        }
    }

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportHostUnregister(uint64_t addr, CallStackString&& stack)
{
    if (IsNeedSkip(DEVICE_ID_CPU))
    {
        return true;
    }

    {
        // 单例类析构之后不再访问其成员变量
        if (destroyed_.load())
        {
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = halPtrs_.find(addr);
        if (it == halPtrs_.end())
        {
            return true;
        }
        halPtrs_.erase(it);
        // 属主校验: 仅归还本通道登记的条目,防误擦他通道地址簿项
        auto hostIt = hostPtrs_.find(addr);
        if (hostIt != hostPtrs_.end() && hostIt->second == HOST_PTR_FROM_HAL_PINNED)
        {
            hostPtrs_.erase(hostIt);
        }
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = EventSubType::HOST;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);
    event->poolType = PoolType::HOST;
    event->isPinned = true;
    event->addr = addr;
    event->name = "N/A";
    event->space = MemOpSpace::HOST;
    event->device = DEVICE_ID_CPU;
    event->size = 0;
    event->moduleId = INVALID_MODID;
    event->flag = FLAG_INVALID;
    event->kernelIndex = kernelLaunchRecordIndex_;

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportCpuTensor(uint64_t addr, uint64_t size, bool isAlloc, std::string&& pyStack)
{
    if (IsNeedSkip(DEVICE_ID_CPU))
    {
        return false;
    }

    {
        // 单例类析构之后不再访问其成员变量
        if (destroyed_.load())
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (isAlloc)
        {
            if (hostPtrs_.count(addr) > 0)
            {
                return false;  // another channel already reported this address, silently reject
            }
            hostPtrs_.emplace(addr, HOST_PTR_FROM_CPU_TENSOR);
        }
        else
        {
            // 属主校验: 地址被其他通道(锁页/host堆钩子)占用时拒绝归还,防跨通道误擦
            auto it = hostPtrs_.find(addr);
            if (it == hostPtrs_.end() || it->second != HOST_PTR_FROM_CPU_TENSOR)
            {
                return false;  // never reported by this channel, silently reject (prevent orphan FREE)
            }
            hostPtrs_.erase(it);
        }
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = isAlloc ? EventBaseType::MALLOC : EventBaseType::FREE;
    event->eventSubType = EventSubType::HOST;
    event->poolType = PoolType::HOST;
    event->addr = addr;
    event->name = "N/A";
    event->space = MemOpSpace::HOST;
    event->device = DEVICE_ID_CPU;
    event->size = static_cast<int64_t>(size);
    event->pyCallStack = std::move(pyStack);
    event->kernelIndex = kernelLaunchRecordIndex_;

    Process::GetInstance().SendEvent(event);
    return true;
}

// =============================================================================
// host堆钩子上报入口
// 仅窗口边界STAGE事件经bind回调进入:
// 分析器闭窗时经dump_*拉取冻结快照,不再消费事件流
// =============================================================================

// host-leak独占通道短路判定:analysis含host-leaks且collectCpu关闭时,
// STAGE事件直连EventDispatcher派发,跳过Process::SendEvent(共享内存队列
// +主进程事件循环)。并发语义:直连绕过Process::processMutex_,
// STAGE事件来自开窗/闭窗调用线程,与经SendEvent的其他线程
// 事件并发时由分析器内部锁兜底。
// 每次调用即时判定(config可运行时变更)
bool EventReport::HostEventBypassEnabled() const
{
    const Config& config = GetConfig();
    BitField<decltype(config.analysisType)> analysisType(config.analysisType);
    return analysisType.checkBit(static_cast<size_t>(AnalysisType::HOST_LEAK_ANALYSIS)) && !config.collectCpu;
}

bool EventReport::ReportHostStage(bool isStart, uint64_t timestamp, uint64_t stageId)
{
    // 单例类析构之后不再访问其成员变量
    if (destroyed_.load())
    {
        return false;
    }
    const bool direct = HostEventBypassEnabled();

    std::shared_ptr<SystemEvent> event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = isStart ? EventSubType::HOST_LEAK_STAGE_START : EventSubType::HOST_LEAK_STAGE_END;
    event->device = GD_INVALID_NUM;
    // stageId(即windowId)经name承载(STEP事件同款约定),分析器据此命名报告文件
    event->name = std::to_string(stageId);
    event->timestamp = timestamp;

    // SYSTEM型STAGE事件经EventRouter的阶段一(HandleTraceEvent仅识别TRACE_*)与
    // 阶段三(非FREE不清理)均为空转;独占通道下直连派发等价送达全部订阅者
    // (HostLeakAnalyzer+Dump等SYSTEM订阅者,DispatchEvent按事件类型精确路由)
    if (direct)
    {
        std::shared_ptr<EventBase> base = event;
        EventDispatcher::GetInstance().DispatchEvent(base, nullptr);
        return true;
    }
    Process::GetInstance().SendEvent(event);
    return true;
}

void EventReport::SetStepInfo(MarkType type, std::string msg, uint64_t rangeId)
{
    if (type == MarkType::MARK_A)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (type == MarkType::RANGE_START_A)
    {
        if (strcmp(msg.c_str(), "step start") != 0)
        {
            return;
        }
        stepInfo_.currentStepId++;
        stepInfo_.inStepRange = true;
        stepInfo_.stepMarkRangeIdList.emplace_back(rangeId);
        return;
    }

    if (type == MarkType::RANGE_END)
    {
        auto ret = find(stepInfo_.stepMarkRangeIdList.begin(), stepInfo_.stepMarkRangeIdList.end(), rangeId);
        if (ret == stepInfo_.stepMarkRangeIdList.end())
        {
            return;
        }
        stepInfo_.inStepRange = false;
        stepInfo_.stepMarkRangeIdList.erase(ret);
        return;
    }

    return;
}

bool EventReport::ReportMark(MarkType type, std::string& msg, uint32_t streamId, uint64_t rangeId)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
    {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    SetStepInfo(type, msg, rangeId);
    if (IsNeedSkip(devId))
    {
        return true;
    }

    std::shared_ptr<MstxEvent> event = std::make_shared<MstxEvent>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = (type == MarkType::MARK_A)          ? EventSubType::MSTX_MARK
                          : (type == MarkType::RANGE_START_A) ? EventSubType::MSTX_RANGE_START
                                                              : EventSubType::MSTX_RANGE_END;
    event->device = devId;
    if (Utility::CheckStrIsStartsWithInvalidChar(msg.c_str()))
    {
        Utility::ToSafeString(msg);
        LOG_ERROR("mstx msg %s is invalid!", msg.c_str());
        msg = "";
    }
    event->name = msg;
    event->streamId = streamId;
    event->stepId = stepInfo_.currentStepId;
    event->kernelIndex = kernelLaunchRecordIndex_;

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportAtenLaunch(const std::string& name, bool isStart, std::string&& pystack)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
    {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId))
    {
        return true;
    }

    std::shared_ptr<OpLaunchEvent> event = std::make_shared<OpLaunchEvent>();
    event->eventType = EventBaseType::OP_LAUNCH;
    event->eventSubType = isStart ? EventSubType::ATEN_START : EventSubType::ATEN_END;
    event->device = devId;
    event->name = name;
    event->pyCallStack = std::move(pystack);

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportAtenAccess(const std::string& name, const std::string& attr, AccessType type, uint64_t addr,
                                   uint64_t size, std::string&& pystack)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
    {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId))
    {
        return true;
    }

    if (addr == 0)
    {
        LOG_ERROR("[mark] Aten access addr is 0, skip");
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::ACCESS;
    event->eventSubType = type == AccessType::READ    ? EventSubType::ATEN_READ
                          : type == AccessType::WRITE ? EventSubType::ATEN_WRITE
                                                      : EventSubType::ATEN_READ_OR_WRITE;
    event->poolType = PoolType::PTA_CACHING;
    event->device = devId;
    event->name = name;
    event->addr = addr;
    event->size = size;
    event->attr = attr;
    event->pyCallStack = std::move(pystack);

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportKernelLaunch(const AclnnKernelMapInfo& kernelLaunchInfo)
{
    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::KERNEL_LAUNCH))
    {
        return true;
    }

    int32_t devId = std::get<0>(kernelLaunchInfo.taskKey);
    if (devId < 0)
    {
        if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
        {
            LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
        }
    }

    if (IsNeedSkip(devId))
    {
        return true;
    }

    std::shared_ptr<KernelLaunchEvent> event = std::make_shared<KernelLaunchEvent>();
    event->eventType = EventBaseType::KERNEL_LAUNCH;
    event->eventSubType = EventSubType::KERNEL_LAUNCH;
    event->device = devId;
    event->streamId = std::to_string(std::get<1>(kernelLaunchInfo.taskKey));
    event->taskId = std::to_string(std::get<2>(kernelLaunchInfo.taskKey));
    event->name = kernelLaunchInfo.kernelName;
    event->kernelIndex = ++kernelLaunchRecordIndex_;

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportKernelExcute(const TaskKey& key, std::string& name, uint64_t time, RecordSubType type)
{
    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::KERNEL_LAUNCH))
    {
        return true;
    }

    if (IsNeedSkip(std::get<0>(key)))
    {
        return true;
    }

    std::shared_ptr<KernelLaunchEvent> event = std::make_shared<KernelLaunchEvent>();
    event->eventType = EventBaseType::KERNEL_LAUNCH;
    event->eventSubType =
        type == RecordSubType::KERNEL_START ? EventSubType::KERNEL_EXECUTE_START : EventSubType::KERNEL_EXECUTE_END;
    event->device = std::get<0>(key);
    event->streamId = std::to_string(std::get<1>(key));
    event->taskId = std::to_string(std::get<2>(key));
    event->name = name;

    Process::GetInstance().SendEvent(event);

    return true;
}
bool EventReport::ReportAclItf(RecordSubType subtype)
{
    if (IsNeedSkip(GD_INVALID_NUM))
    {
        return true;
    }

    if (subtype == RecordSubType::FINALIZE)
    {
        KernelEventTrace::GetInstance().EndKernelEventTrace();
    }

    std::shared_ptr<SystemEvent> event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = subtype == RecordSubType::INIT ? EventSubType::ACL_INIT : EventSubType::ACL_FINI;
    event->device = GD_INVALID_NUM;
    event->name = "N/A";

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportTraceStatus(const EventTraceStatus status)
{
    // host-leak窗口驱动(交集语义):stop()置闩锁优先关窗(闭窗内部
    // 由钩子上报线程排空后发STAGE_END,分析器届时生成报告);start()复位闩锁可重开新窗口。
    // 先于TRACE事件发送执行,尽早关闸减少在途事件量
    if (status == EventTraceStatus::IN_TRACING)
    {
        hostMemStopLatch_.store(false, std::memory_order_relaxed);
    }
    else
    {
        hostMemStopLatch_.store(true, std::memory_order_relaxed);
    }
    UpdateHostMemWindow();

    if (IsNeedSkip(GD_INVALID_NUM))
    {
        return true;
    }

    std::shared_ptr<SystemEvent> event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType =
        (status == EventTraceStatus::IN_TRACING) ? EventSubType::TRACE_START : EventSubType::TRACE_STOP;
    event->device = GD_INVALID_NUM;
    event->name = "N/A";

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportAtbOpExecute(const char* name, size_t nameSize, const char* attr, size_t attrSize,
                                     RecordSubType type)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
    {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId))
    {
        return true;
    }

    std::shared_ptr<OpLaunchEvent> event = std::make_shared<OpLaunchEvent>();
    event->eventType = EventBaseType::OP_LAUNCH;
    event->eventSubType = type == RecordSubType::ATB_START ? EventSubType::ATB_START : EventSubType::ATB_END;
    event->device = devId;
    event->name = name;
    event->attr = attr;

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportAtbKernel(const char* name, size_t nameSize, const char* attr, size_t attrSize,
                                  RecordSubType type)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
    {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId))
    {
        return true;
    }

    std::shared_ptr<KernelLaunchEvent> event = std::make_shared<KernelLaunchEvent>();
    event->eventType = EventBaseType::KERNEL_LAUNCH;
    event->eventSubType =
        type == RecordSubType::KERNEL_START ? EventSubType::ATB_KERNEL_START : EventSubType::ATB_KERNEL_END;
    event->device = devId;
    event->name = name;
    event->attr = attr;

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportAtbAccessMemory(const char* name, size_t nameSize, const char* attr, size_t attrSize,
                                        uint64_t addr, uint64_t size, AccessType type)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
    {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId))
    {
        return true;
    }

    if (addr == 0)
    {
        // tensor地址为空(0)的访问事件无法关联任何内存块，
        // 直接丢弃，防止在MemoryStateManager中产生无MALLOC的幽灵内存块状态
        LOG_ERROR("[mark] ATB access addr is 0, skip");
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::ACCESS;
    event->eventSubType = type == AccessType::READ    ? EventSubType::ATB_READ
                          : type == AccessType::WRITE ? EventSubType::ATB_WRITE
                                                      : EventSubType::ATB_READ_OR_WRITE;
    event->poolType = PoolType::ATB;
    event->addr = addr;
    event->size = size;
    event->device = devId;
    event->name = name;
    event->attr = attr;

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportMemorySnapshot(const MemorySnapshotInfo& memory_info, CallStackString&& stack)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
    {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId))
    {
        return true;
    }

    std::shared_ptr<SnapshotEvent> event = std::make_shared<SnapshotEvent>();
    event->eventType = EventBaseType::SNAPSHOT;
    event->eventSubType = EventSubType::SNAPSHOT;
    event->device = devId;
    event->name = memory_info.name;
    event->memory_reserved = memory_info.memory_reserved;
    event->max_memory_reserved = memory_info.max_memory_reserved;
    event->memory_allocated = memory_info.memory_allocated;
    event->max_memory_allocated = memory_info.max_memory_allocated;
    event->total_memory = memory_info.total_memory;
    event->free_memory = memory_info.free_memory;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);

    Process::GetInstance().SendEvent(event);

    return true;
}

void EventReport::ReportMemorySnapshotOnOOM(const CallStackString& stack)
{
    // Try to call Python's take_snapshot function to get accurate memory info
    if (Utility::IsPyInterpRepeInited())
    {
        Utility::PyInterpGuard guard;

        try
        {
            // Import msmemscope module
            Utility::PythonObject msmemscope_module = Utility::PythonObject::Import("msmemscope", false, true);
            if (msmemscope_module.IsBad())
            {
                LOG_ERROR("Failed to import msmemscope module for OOM snapshot");
                return;
            }

            // Get take_snapshot function
            Utility::PythonObject take_snapshot_func = msmemscope_module.Get("take_snapshot");
            if (take_snapshot_func.IsBad() || !take_snapshot_func.IsCallable())
            {
                LOG_ERROR("Failed to get take_snapshot function for OOM snapshot");
                return;
            }

            // Get current device ID
            int32_t devId = GD_INVALID_NUM;
            if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
            {
                LOG_ERROR("Failed to get device ID for OOM snapshot");
                return;
            }

            // Prepare arguments for take_snapshot
            Utility::PythonObject dev_arg = Utility::PythonObject(devId);
            Utility::PythonObject name_arg = Utility::PythonObject("OOM_Snapshot");

            // Call take_snapshot function with arguments
            Utility::PythonListObject args_list;
            args_list.Append(dev_arg);
            args_list.Append(name_arg);
            Utility::PythonTupleObject tuple_args = args_list.ToTuple();
            Utility::PythonObject result = take_snapshot_func.Call(tuple_args, true);

            if (!result.IsBad())
            {
                LOG_INFO("OOM memory snapshot created via Python take_snapshot");
                return;
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Exception in Python take_snapshot: %s", e.what());
        }
    }
}

bool EventReport::ReportOOMTrigger(const OOMTriggerInfo& info)
{
    int32_t devId = info.deviceId;
    if (devId == GD_INVALID_NUM)
    {
        if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
        {
            LOG_ERROR("Failed to get device ID for OOM trigger event");
            return false;
        }
    }

    if (IsNeedSkip(devId))
    {
        return true;
    }

    std::shared_ptr<OOMTriggerEvent> event = std::make_shared<OOMTriggerEvent>();
    event->device = devId;
    event->requestSize = info.requestSize;
    event->flag = info.flag;
    event->funcName = info.funcName;
    event->cCallStack = info.stack.cStack;
    event->pyCallStack = info.stack.pyStack;
    event->timestamp = info.timestamp;
    event->name = "OOM_Trigger";

    Process::GetInstance().SendEvent(event);

    return true;
}

bool EventReport::ReportOOMMemRecord(const OOMMemRecord& record, EventSubType subType)
{
    int32_t devId = record.deviceId;
    if (devId == GD_INVALID_NUM)
    {
        if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
        {
            LOG_ERROR("Failed to get device ID for OOM mem record event");
            return false;
        }
    }

    if (IsNeedSkip(devId))
    {
        return true;
    }

    std::shared_ptr<OOMMemRecordEvent> event = std::make_shared<OOMMemRecordEvent>();
    event->eventSubType = subType;
    event->device = devId;
    event->poolType = record.poolType;
    event->addr = record.ptr;
    event->memSize = record.memSize;
    event->allocTimestamp = record.allocTimestamp;
    event->cCallStack = record.cCallStack;
    event->pyCallStack = record.pyCallStack;
    event->name = (subType == EventSubType::OOM_RECENT_ALLOC) ? "OOM_RecentAlloc" : "OOM_TopAlloc";

    Process::GetInstance().SendEvent(event);

    return true;
}

// =============================================================================
// host堆钩子bind桥
// 钩子so与libascend_leaks之间只经本节C包装层交互:全部extern "C"、无C++类型跨so、
// 异常不外泄(钩子上报线程为进程内单消费线程,中断即丢后续全部事件)
// =============================================================================

extern "C" void msmemscope_hostmem_report_stage(int isStart, uint64_t timestamp, uint64_t stageId)
{
    EventReportSuppressor suppressor;
    if (isStart == 0)
    {
        // 退出闭窗派发链诊断(STAGE_END,极低频):到达采集库=钩子closing分支的
        // report_stage已越过bind边界;若此打点出现后分析器侧打点缺失,卡点在
        // ReportHostStage→DispatchEvent的锁等待(与钩子侧"calling report_stage"互证)
        fprintf(stderr, "[msmemscope] host leak [pid=%llu] report_stage: STAGE_END entered (stage=%llu)\n",
                static_cast<unsigned long long>(getpid()), static_cast<unsigned long long>(stageId));
    }
    try
    {
        // 直达实例不经Instance():本回调在EventReport构造期被set_enabled(true)同步触发,
        // 重入Instance()即死锁(见g_hostMemReportInstance注释)
        EventReport* report = g_hostMemReportInstance.load(std::memory_order_acquire);
        if (report != nullptr)
        {
            report->ReportHostStage(isStart != 0, timestamp, stageId);
        }
    }
    catch (...)
    {
    }
}

extern "C" int msmemscope_hostmem_is_suppressed(void) { return IsEventReportSuppressed() ? 1 : 0; }

extern "C" void msmemscope_hostmem_get_params(MsmemscopeHostmemParams* params)
{
    if (params == nullptr)
    {
        return;
    }
    try
    {
        const Config& config = GetConfig();
        // cStackDepth为0表示未配置(ClientParser默认50未生效),回退钩子侧默认
        params->stackDepth = config.cStackDepth == 0 ? 50 : config.cStackDepth;
        // 显式采样率倒数(默认1=不采样;config.sampleRate),钩子按2的幂归一化
        params->sampleRate = config.sampleRate;
        params->blockThreshold = config.blockSizeThreshold;
    }
    catch (...)
    {
        params->stackDepth = 50;
        params->sampleRate = 1;
        params->blockThreshold = 0;
    }
}

void EventReport::BindHostMemHook()
{
    // dlsym(RTLD_DEFAULT):钩子so经LD_PRELOAD位于全局命名空间,无需DT_NEEDED符号依赖
    auto bindFn = reinterpret_cast<const MsmemscopeHostmemSvc* (*)(const MsmemscopeHostmemApi*)>(
        dlsym(RTLD_DEFAULT, "msmemscope_hostmem_bind"));
    if (bindFn == nullptr)
    {
        return;  // 钩子so未装配:host功能不激活(安静跳过)
    }

    MsmemscopeHostmemApi api = {};
    api.report_stage = msmemscope_hostmem_report_stage;
    api.is_suppressed = msmemscope_hostmem_is_suppressed;
    api.get_params = msmemscope_hostmem_get_params;
    svcHostMem_ = bindFn(&api);
    if (svcHostMem_ == nullptr)
    {
        LOG_WARN("Bind host mem hook failed: hook rejected api table, host leak analysis disabled");
    }
}

void EventReport::UpdateHostMemWindow()
{
    const Config& config = GetConfig();
    BitField<decltype(config.analysisType)> analysisType(config.analysisType);
    const bool hostLeakOn = analysisType.checkBit(static_cast<size_t>(AnalysisType::HOST_LEAK_ANALYSIS));
    if (svcHostMem_ == nullptr)
    {
        if (hostLeakOn)
        {
            LOG_WARN("Host leak analysis is enabled but host mem hook is not loaded, no host data will be collected");
        }
        return;
    }
    // 交集语义:(analysis含host-leaks) && !(stop闩锁);set_enabled不持EventReport::mutex_
    // (闭窗内部可能同步等待上报线程排空,持锁将阻塞钩子上报线程的hostPtrs_访问造成秒级停顿)
    const bool enabled = hostLeakOn && !hostMemStopLatch_.load(std::memory_order_relaxed);
    svcHostMem_->set_enabled(enabled ? 1 : 0);
}

void EventReport::CloseHostMemWindowAtExit()
{
    // 未bind(钩子未装配)/已析构:无窗口可言。区分打点:前者=钩子so缺失,
    // 后者=本对象先于handler析构(时序异常,需查注册顺序)
    if (destroyed_.load() || svcHostMem_ == nullptr)
    {
        fprintf(stderr, "[msmemscope] host leak [pid=%llu] exit close skipped: %s\n",
                static_cast<unsigned long long>(getpid()),
                destroyed_.load() ? "event report destroyed before handler" : "host hook not bound");
        return;
    }
    // 复用stop闩锁语义(交集语义):置位后UpdateHostMemWindow必闭窗且此后不再
    // 重开(钩子侧g_exiting短路开窗分支为双保险);进程已在退出,该状态无回退需求
    hostMemStopLatch_.store(true, std::memory_order_relaxed);
    UpdateHostMemWindow();

    // 等待闭窗排空完成:closing位(bit2)清零=STAGE_END已同步派发完成=完整路径
    // 报告(丢包差分+O4校准)已落盘——派发链全程同步(钩子上报线程的report_stage
    // 调用栈内走完HandleStageEnd→WriteWindowReport),钩子侧closing在report_stage
    // 返回后才清零。窗口从未开过时closing恒0立即通过。120s安全上界(此前3s上界在
    // 大栈量场景截断排空、滞留环中的栈注册事件永久丢失,故放宽至120s覆盖正常慢消化
    // ——4万级栈符号化+变体交付+报告落盘实测40s级):超时=上报线程卡死(退出期派发链
    // 锁竞争/进程内持锁线程卡死),放弃等待并明示,报告由~HostLeakAnalyzer析构兜底
    // (完整度unknown降级),进程得以退出——不因工具自身缺陷拖死宿主进程退出;
    // 等待期间每5s打印剩余待处理量(环积压事件数+待补扫栈数)使进度可观测
    const auto closeStart = std::chrono::steady_clock::now();
    auto nextLog = std::chrono::steady_clock::now();
    for (;;)
    {
        const int status = QueryHostMemStatus();
        if (status < 0)
        {
            // dlsym失败(钩子so已卸载/符号不可得):无法观测closing,无法确认闭窗
            // 完成。此态在退出期理论不可达(钩子so仍加载),出现即需查卸载时序;
            // 按"无需等待"放行,窗口报告由~HostLeakAnalyzer兜底
            fprintf(stderr, "[msmemscope] host leak [pid=%llu] exit close skipped: status query unavailable\n",
                    static_cast<unsigned long long>(getpid()));
            return;
        }
        if ((status & 0x4) == 0)
        {
            // closing位已清零:闭窗排空已完成(或从未开始)。若窗口在分析器侧仍open,
            // 说明闭窗STAGE_END未送达分析器(订阅/派发链断),由~HostLeakAnalyzer兜底——
            // 此打点与该判断互证
            fprintf(stderr,
                    "[msmemscope] host leak [pid=%llu] exit close: closing bit already clear "
                    "(window closed or never closing)\n",
                    static_cast<unsigned long long>(getpid()));
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - closeStart >= std::chrono::seconds(120))
        {
            fprintf(stderr,
                    "[msmemscope] host leak [pid=%llu] exit close timed out after 120s, "
                    "giving up (report degraded via destructor fallback)\n",
                    static_cast<unsigned long long>(getpid()));
            return;
        }
        if (now >= nextLog)
        {
            // 无环形缓冲/待补扫栈,进度日志为纯状态打点
            fprintf(stderr, "[msmemscope] host leak [pid=%llu] window still closing at exit\n",
                    static_cast<unsigned long long>(getpid()));
            nextLog = now + std::chrono::seconds(5);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool EventReport::GetHostMemStats(MsmemscopeHostmemStats& stats)
{
    if (svcHostMem_ == nullptr || destroyed_.load())
    {
        return false;
    }
    svcHostMem_->get_stats(&stats);
    return true;
}

bool EventReport::DumpHostMemLiveBlocks(void (*emit)(void* ctx, uint64_t addr, uint64_t size, uint64_t allocTs,
                                                     uint64_t stackId),
                                        void* ctx)
{
    if (svcHostMem_ == nullptr || destroyed_.load() || emit == nullptr)
    {
        return false;
    }
    svcHostMem_->dump_live_blocks(emit, ctx);
    return true;
}

bool EventReport::DumpHostMemStackStats(void (*emit)(void* ctx, uint64_t stackId, uint64_t allocCount,
                                                     uint64_t allocBytes, uint64_t freedCount, uint64_t freedBytes,
                                                     uint64_t unfreedCount, uint64_t unfreedBytes,
                                                     uint64_t maxBlockSize, const char* frameDesc, size_t len),
                                        void* ctx)
{
    if (svcHostMem_ == nullptr || destroyed_.load() || emit == nullptr)
    {
        return false;
    }
    svcHostMem_->dump_stack_stats(emit, ctx);
    return true;
}

bool EventReport::DumpHostMemSizeDist(void (*emit)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh,
                                                   uint64_t blockCount, uint64_t blockBytes),
                                      void* ctx)
{
    if (svcHostMem_ == nullptr || destroyed_.load() || emit == nullptr)
    {
        return false;
    }
    svcHostMem_->dump_size_distribution(emit, ctx);
    return true;
}

bool EventReport::DumpHostMemPreWindowDist(void (*emit)(void* ctx, uint64_t rangeLow, uint64_t rangeHigh,
                                                        uint64_t blockCount, uint64_t blockBytes),
                                           void* ctx)
{
    if (svcHostMem_ == nullptr || destroyed_.load() || emit == nullptr)
    {
        return false;
    }
    svcHostMem_->dump_pre_window_distribution(emit, ctx);
    return true;
}

void EventReport::SetHostMemSvcForTest(const MsmemscopeHostmemSvc* svc)
{
    // 测试缝:注入后bind语义被替代(仅测试用);注入nullptr即清除,恢复未装配态
    svcHostMem_ = svc;
}

// host模式CLI装配兜底:钩子so经DT_NEEDED带入本库后,目标进程内没有任何API调用会触发
// EventReport构造(npu路径由首个钩子回调触发;host钩子只经bind回调被动等待),不主动构造则bind
// 永不发生、钩子空转。仅在CLI模式(MSMEMSCOPE_CONFIG_PATH已由wrapper写入目标进程环境)且钩子bind
// 符号在位时立即构造;python API路径保持惰性(由首个config()/start()触发构造),避免
// FileCreateManager在用户config()设定输出目录之前锁定默认路径
__attribute__((constructor)) static void HostMemReportBoot()
{
    if (std::getenv(Utility::MSMEMSCOPE_CONFIG_ENV) == nullptr)
    {
        return;
    }
    if (dlsym(RTLD_DEFAULT, "msmemscope_hostmem_bind") == nullptr)
    {
        return;
    }
    EventReport::Instance(MemScopeCommType::SHARED_MEMORY);
}

}  // namespace MemScope
