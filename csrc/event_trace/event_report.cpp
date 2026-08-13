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

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

#include "bit_field.h"
#include "cpython.h"
#include "decompose_analyzer.h"
#include "describe_trace.h"
#include "health_analyzer.h"
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
    std::call_once(dcmiInitFlag_, [this]() {
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
    std::call_once(dcmiMapInitFlag_, [this]() {
        static auto getCardList = VallinaSymbol<DcmiLibLoader>::Instance().Get<DcmiGetCardListFunc>("dcmi_get_card_list");
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
                        LOG_ERROR("dcmi_get_device_logic_id failed, card_id %d device_id %d", cardList[i], deviceId);
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
    struct dcmi_hbm_info info{};
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
    return instance;
}

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
    Dump::GetInstance();

    // 注册通过EventDispatcher订阅的分析器（替代Process::SendEvent中的switch-case分发）
    // 构造顺序即派发顺序（同优先级下后插入的订阅者先收到事件）：
    // 先HealthAnalyzer后LeakAnalyzer，保证MALLOC/FREE先经LeakAnalyzer（对应原StepInnerAnalyzer槽位），
    // MSTX先经LeakAnalyzer::CheckNpuLeak再经HealthAnalyzer::CheckGap（保持原告警顺序）
    // 统计字段回填（used/processUsed）由MemoryStateManager::UpdateUsage在事件处理阶段一完成，
    // 先于所有分析器，无需订阅
    HealthAnalyzer::GetInstance();
    LeakAnalyzer::GetInstance();

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

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = space == MemOpSpace::HOST ? EventSubType::HOST_PINNED : EventSubType::HAL;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);
    event->poolType = PoolType::HAL;
    event->addr = addr;
    event->name = "N/A";
    event->space = space;
    event->device = devId;
    event->size = size;
    event->moduleId = moduleId;
    event->flag = flag;
    event->kernelIndex = kernelLaunchRecordIndex_;
    if (space == MemOpSpace::HOST)
    {
        event->used = static_cast<int64_t>(Utility::GetProcessVmRss());
    }

    {
        if (!destroyed_.load())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            halPtrs_.emplace(addr, devId);
        }
    }

    // MALLOC（flag 入口）事件值为申请后的整卡/本进程用量，按 flag 解析设备查询并缓存；
    // HOST 空间（HOST_PINNED）无整卡概念，不查询（deviceUsed/processUsed 保持 -1）
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

    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = space == MemOpSpace::HOST ? EventSubType::HOST_PINNED : EventSubType::HAL;
    event->poolType = PoolType::HAL;
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
            halPtrs_.emplace(addr, devId);
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
    }

    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = EventSubType::HAL;
    event->poolType = PoolType::HAL;
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

bool EventReport::ReportHostRegister(uint64_t addr, uint64_t size, CallStackString&& stack)
{
    if (IsNeedSkip(DEVICE_ID_CPU))
    {
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HOST_PINNED;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);
    event->poolType = PoolType::HAL;
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
            // HOST_PINNED映射到CPU设备（DEVICE_ID_CPU），与事件device一致
            halPtrs_.emplace(addr, DEVICE_ID_CPU);
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
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->eventSubType = EventSubType::HAL;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);
    event->poolType = PoolType::HAL;
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
    if (IsNeedSkip(GD_INVALID_NUM))
    {
        return true;
    }

    std::shared_ptr<OOMMemRecordEvent> event = std::make_shared<OOMMemRecordEvent>();
    event->eventSubType = subType;
    event->device = GD_INVALID_NUM;
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

}  // namespace MemScope
