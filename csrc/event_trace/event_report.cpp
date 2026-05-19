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
#include "log.h"
#include "utils.h"
#include "vallina_symbol.h"
#include "ustring.h"
#include "umask_guard.h"
#include "securec.h"
#include "bit_field.h"
#include "kernel_hooks/runtime_prof_api.h"
#include "describe_trace.h"
#include "decompose_analyzer.h"
#include "inefficient_analyzer.h"
#include "json_manager.h"
#include "cpython.h"

namespace MemScope {
bool g_isReportHostMem = false;

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
    switch (memType) {
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
 
    if ((flag & MEM_PAGE_GIANT) != 0) {
        return MemPageType::MEM_GIANT_PAGE_TYPE;
    } else if ((flag & MEM_PAGE_HUGE) != 0) {
        return MemPageType::MEM_HUGE_PAGE_TYPE;
    } else {
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

bool GetDeviceInfo::GetDeviceId(int32_t &devId)
{
    char const *sym = "aclrtGetDeviceImpl";
    using AclrtGetDevice = aclError (*)(int32_t*);
    static AclrtGetDevice vallina = nullptr;
    if (vallina == nullptr) {
        vallina = VallinaSymbol<ACLImplLibLoader>::Instance().Get<AclrtGetDevice>(sym);
    }
    if (vallina == nullptr) {
        LOG_ERROR("vallina func get FAILED: %s, try to get it in legacy way.", __func__);
        
        // 添加老版本的GetDevice逻辑，用于兼容情况如开放态场景
        char const *l_sym = "rtGetDevice";
        using RtGetDevice = rtError_t (*)(int32_t*);
        static RtGetDevice l_vallina = nullptr;
        if (l_vallina == nullptr) {
            l_vallina = VallinaSymbol<RuntimeLibLoader>::Instance().Get<RtGetDevice>(l_sym);
        }
        if (l_vallina == nullptr) {
            LOG_ERROR("vallina func get FAILED in legacy way: %s", __func__);
            return false;
        }

        rtError_t ret = l_vallina(&devId);
        if (ret == RT_ERROR_INVALID_VALUE) {
            return false;
        }
        return true;
    }

    aclError ret = vallina(&devId);
    if (ret != ACL_SUCCESS) {
        return false;
    }

    return TransDeviceId(devId);
}

bool GetDeviceInfo::TransDeviceId(int32_t &devId)
{
    // 新增可见卡选项
    if(!setVisibleDevice) {
        return true;
    }
    auto it = visibleDeviceMap.find(devId);
    if (it == visibleDeviceMap.end()) {
        LOG_ERROR("Key %d not found in visibleDeviceMap!", devId);
        return false;
    }
    devId = it->second;
    return true;
}

bool GetDeviceInfo::GetDeviceMemInfo(size_t &freeMem, size_t &totalMem)
{
    using func = decltype(&aclrtGetMemInfo);
    static auto vallina = VallinaSymbol<AclLibLoader>::Instance().Get<func>("aclrtGetMemInfo");
    if (vallina == nullptr) {
        LOG_ERROR("Get aclrtGetMemInfo func ptr failed");
        return false;
    }

    int ret = vallina(ACL_HBM_MEM, &freeMem, &totalMem);
    if (ret != ACL_SUCCESS) {
        LOG_ERROR("Get device mem info failed, ret is %d", ret);
        return false;
    }
    return true;
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
    initConfig_ = GetConfig();

    // subscribe订阅
    BitField<decltype(initConfig_.analysisType)> analysisType(initConfig_.analysisType);
    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS))) {
        DecomposeAnalyzer::GetInstance();
    }
    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::INEFFICIENCY_ANALYSIS))) {
        InefficientAnalyzer::GetInstance();
    }
    Dump::GetInstance(initConfig_);
    LOG_INFO("LOG INIT");
    RegisterRtProfileCallback();

    return;
}

void EventReport::UpdateAnalysisType()
{
    initConfig_ = GetConfig();
    BitField<decltype(initConfig_.analysisType)> analysisType(initConfig_.analysisType);

    // 根据config确认是否订阅或者取消订阅
    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::DECOMPOSE_ANALYSIS))) {
        DecomposeAnalyzer::GetInstance().Subscribe();
    } else {
        DecomposeAnalyzer::GetInstance().UnSubscribe();
    }

    if (analysisType.checkBit(static_cast<size_t>(AnalysisType::INEFFICIENCY_ANALYSIS))) {
        InefficientAnalyzer::GetInstance().Subscribe();
    } else {
        InefficientAnalyzer::GetInstance().UnSubscribe();
    }
}

EventReport::~EventReport()
{
    destroyed_.store(true);
}

bool EventReport::IsNeedSkip(int32_t devid)
{
    // 是否为指定卡
    if (!GetConfig().collectAllNpu) {
        BitField<decltype(GetConfig().npuSlots)> npuSlots(GetConfig().npuSlots);
        if (devid != GD_INVALID_NUM && !npuSlots.checkBit(static_cast<size_t>(devid))) {
            return true;
        }
    }

    // 目前仅命令行支持选择--steps，因此当存在stepList时代表启用了命令行，我们不推荐同时使用命令行和python接口。这里不考虑
    // msmemscope.step()接口所带来的的step信息。
    auto stepList = GetConfig().stepList;
    if (stepList.stepCount == 0) {
        return false;
    }

    for (uint8_t loop = 0; (loop < stepList.stepCount && loop < SELECTED_STEP_MAX_NUM); loop++) {
        if (stepInfo_.currentStepId == stepList.stepIdList[loop] && stepInfo_.inStepRange) {
            return false;
        }
    }

    return true;
}

bool EventReport::ReportAddrInfo(EventSubType type, uint64_t addr, std::string owner)
{
    int32_t devId = GD_INVALID_NUM;
    GetDeviceInfo::Instance().GetDeviceId(devId);
    if (IsNeedSkip(devId)) {
        return true;
    }
    Utility::ToSafeString(owner);
    std::shared_ptr<MemoryOwnerEvent> event = std::make_shared<MemoryOwnerEvent>();
    event->eventType = EventBaseType::MEMORY_OWNER;
    event->eventSubType = type;
    event->owner = owner;
    event->addr = addr;
    event->device = devId;

    Process::GetInstance(initConfig_).SendEvent(event);
    return true;
}

bool EventReport::ReportPyStepRecord()
{
    int32_t devId = GD_INVALID_NUM;
    GetDeviceInfo::Instance().GetDeviceId(devId);
    if (IsNeedSkip(devId)) {
        return true;
    }

    std::shared_ptr<SystemEvent> event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = EventSubType::STEP;
    event->device = devId;
    event->name = std::to_string(++pyStepId_);

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}
 
bool EventReport::ReportMemPoolRecord(EventSubType type, const MemoryUsage& info, const std::string& owner,
                                      CallStackString&& stack)
{
    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::MALLOC) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::FREE)) {
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = info.dataType == 0 ? EventBaseType::MALLOC : EventBaseType::FREE;
    if (type == EventSubType::PTA_CACHING) {
        event->poolType = PoolType::PTA_CACHING;
        event->eventSubType = EventSubType::PTA_CACHING;
    } else if (type == EventSubType::PTA_WORKSPACE) {
        event->poolType = PoolType::PTA_WORKSPACE;
        event->eventSubType = EventSubType::PTA_WORKSPACE;
    } else if (type == EventSubType::ATB) {
        event->poolType = PoolType::ATB;
        event->eventSubType = EventSubType::ATB;
    } else {
        event->poolType = PoolType::MINDSPORE;
        event->eventSubType = EventSubType::MINDSPORE;
    }

    int32_t realDevice = static_cast<int32_t>(info.deviceIndex);
    GetDeviceInfo::Instance().TransDeviceId(realDevice);
    event->addr = info.ptr;
    event->name = "N/A";
    event->device = realDevice;
    event->size = info.allocSize;
    event->total = info.totalReserved;
    event->used = info.totalAllocated;
    event->describeOwner = owner;
    event->kernelIndex = kernelLaunchRecordIndex_;
    event->cCallStack = std::move(stack.cStack);
    event->pyCallStack = std::move(stack.pyStack);

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportHalCreate(uint64_t addr, uint64_t size, const drv_mem_prop& prop, CallStackString&& stack)
{
    if (IsNeedSkip(prop.devid)) {
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HAL;
    event->describeOwner = DescribeTrace::GetInstance().GetDescribe();
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
        if (!destroyed_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            halPtrs_.insert(addr);
        }
    }
 
    Process::GetInstance(initConfig_).SendEvent(event);
    return true;
}
 
bool EventReport::ReportHalRelease(uint64_t addr, CallStackString&& stack)
{
    if (IsNeedSkip(GD_INVALID_NUM)) {
        return true;
    }
 
    {
        // 单例类析构之后不再访问其成员变量
        if (destroyed_.load()) {
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = halPtrs_.find(addr);
        if (it == halPtrs_.end()) {
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
    event->space = MemOpSpace::INVALID;
    event->device = GD_INVALID_NUM;
    event->size = 0;
    event->moduleId = INVALID_MODID;
    event->flag = FLAG_INVALID;
    event->kernelIndex = kernelLaunchRecordIndex_;
 
    Process::GetInstance(initConfig_).SendEvent(event);
 
    return true;
}

bool EventReport::ReportHalMalloc(uint64_t addr, uint64_t size, unsigned long long flag, CallStackString&& stack)
{
    // bit0~9 devId
    int32_t devId = (flag & 0x3FF);
    if (IsNeedSkip(devId)) {
        return true;
    }

    MemOpSpace space = GetMemOpSpace(flag);
    // 不采集hal接口在host申请的pin memory
    if (space == MemOpSpace::HOST) {
        return true;
    }
    int32_t moduleId = GetMallocModuleId(flag);
    std::string owner = DescribeTrace::GetInstance().GetDescribe();

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HAL;
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

    {
        if (!destroyed_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            halPtrs_.insert(addr);
        }
    }

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportHalFree(uint64_t addr, CallStackString&& stack)
{
    if (IsNeedSkip(GD_INVALID_NUM)) {
        return true;
    }

    {
        // 单例类析构之后不再访问其成员变量
        if (destroyed_.load()) {
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = halPtrs_.find(addr);
        if (it == halPtrs_.end()) {
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
    event->space = MemOpSpace::INVALID;
    event->device = GD_INVALID_NUM;
    event->size = 0;
    event->moduleId = INVALID_MODID;
    event->flag = FLAG_INVALID;
    event->kernelIndex = kernelLaunchRecordIndex_;

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

void EventReport::SetStepInfo(MarkType type, std::string msg, uint64_t rangeId)
{
    if (type == MarkType::MARK_A) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (type == MarkType::RANGE_START_A) {
        if (strcmp(msg.c_str(), "step start") != 0) {
            return;
        }
        stepInfo_.currentStepId++;
        stepInfo_.inStepRange = true;
        stepInfo_.stepMarkRangeIdList.emplace_back(rangeId);
        return;
    }

    if (type == MarkType::RANGE_END) {
        auto ret = find(stepInfo_.stepMarkRangeIdList.begin(), stepInfo_.stepMarkRangeIdList.end(), rangeId);
        if (ret == stepInfo_.stepMarkRangeIdList.end()) {
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
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    SetStepInfo(type, msg, rangeId);
    if (IsNeedSkip(devId)) {
        return true;
    }

    std::shared_ptr<MstxEvent> event = std::make_shared<MstxEvent>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = (type == MarkType::MARK_A) ? EventSubType::MSTX_MARK :
            (type == MarkType::RANGE_START_A) ? EventSubType::MSTX_RANGE_START :
            EventSubType::MSTX_RANGE_END;
    event->device = devId;
    if (Utility::CheckStrIsStartsWithInvalidChar(msg.c_str())) {
        Utility::ToSafeString(msg);
        LOG_ERROR("mstx msg %s is invalid!", msg.c_str());
        msg = "";
    }
    event->name = msg;
    event->streamId = streamId;
    event->stepId = stepInfo_.currentStepId;
    event->kernelIndex = kernelLaunchRecordIndex_;

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportAtenLaunch(const std::string& name, bool isStart, std::string&& pystack)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId)) {
        return true;
    }

    std::shared_ptr<OpLaunchEvent> event = std::make_shared<OpLaunchEvent>();
    event->eventType = EventBaseType::OP_LAUNCH;
    event->eventSubType = isStart ? EventSubType::ATEN_START : EventSubType::ATEN_END;
    event->device = devId;
    event->name = name;
    event->pyCallStack = std::move(pystack);

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportAtenAccess(const std::string& name, const std::string& attr, AccessType type,
                                   uint64_t addr, uint64_t size, std::string&& pystack)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId)) {
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::ACCESS;
    event->eventSubType = type == AccessType::READ ? EventSubType::ATEN_READ :
        type == AccessType::WRITE ? EventSubType::ATEN_WRITE : EventSubType::ATEN_READ_OR_WRITE;
    event->poolType = PoolType::PTA_CACHING;
    event->device = devId;
    event->name = name;
    event->addr = addr;
    event->size = size;
    event->attr = attr;
    event->pyCallStack = std::move(pystack);

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportKernelLaunch(const AclnnKernelMapInfo &kernelLaunchInfo)
{
    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::MALLOC) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::FREE)) {
        return true;
    }

    int32_t devId = std::get<0>(kernelLaunchInfo.taskKey);
    if (devId < 0) {
        if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
            LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
        }
    }

    if (IsNeedSkip(devId)) {
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

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportKernelExcute(const TaskKey &key, std::string &name, uint64_t time, RecordSubType type)
{
    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::KERNEL_LAUNCH)) {
        return true;
    }
    
    if (IsNeedSkip(std::get<0>(key))) {
        return true;
    }

    std::shared_ptr<KernelLaunchEvent> event = std::make_shared<KernelLaunchEvent>();
    event->eventType = EventBaseType::KERNEL_LAUNCH;
    event->eventSubType = type == RecordSubType::KERNEL_START ?
        EventSubType::KERNEL_EXECUTE_START : EventSubType::KERNEL_EXECUTE_END;
    event->device = std::get<0>(key);
    event->streamId = std::to_string(std::get<1>(key));
    event->taskId = std::to_string(std::get<2>(key));
    event->name = name;

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}
bool EventReport::ReportAclItf(RecordSubType subtype)
{
    if (IsNeedSkip(GD_INVALID_NUM)) {
        return true;
    }

    if (subtype == RecordSubType::FINALIZE) {
        KernelEventTrace::GetInstance().EndKernelEventTrace();
    }

    std::shared_ptr<SystemEvent> event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = subtype == RecordSubType::INIT ? EventSubType::ACL_INIT : EventSubType::ACL_FINI;
    event->device = GD_INVALID_NUM;
    event->name = "N/A";

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportTraceStatus(const EventTraceStatus status)
{
    if (IsNeedSkip(GD_INVALID_NUM)) {
        return true;
    }

    std::shared_ptr<SystemEvent> event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = (status == EventTraceStatus::IN_TRACING) ?
        EventSubType::TRACE_START : EventSubType::TRACE_STOP;
    event->device = GD_INVALID_NUM;
    event->name = "N/A";

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportAtbOpExecute(const char* name, size_t nameSize, const char* attr, size_t attrSize,
                                     RecordSubType type)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId)) {
        return true;
    }

    std::shared_ptr<OpLaunchEvent> event = std::make_shared<OpLaunchEvent>();
    event->eventType = EventBaseType::OP_LAUNCH;
    event->eventSubType = type == RecordSubType::ATB_START ? EventSubType::ATB_START : EventSubType::ATB_END;
    event->device = devId;
    event->name = name;
    event->attr = attr;

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportAtbKernel(const char* name, size_t nameSize, const char* attr, size_t attrSize,
                                  RecordSubType type)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId)) {
        return true;
    }
    
    std::shared_ptr<KernelLaunchEvent> event = std::make_shared<KernelLaunchEvent>();
    event->eventType = EventBaseType::KERNEL_LAUNCH;
    event->eventSubType = type == RecordSubType::KERNEL_START ?
        EventSubType::ATB_KERNEL_START : EventSubType::ATB_KERNEL_END;
    event->device = devId;
    event->name = name;
    event->attr = attr;

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportAtbAccessMemory(const char* name, size_t nameSize, const char* attr, size_t attrSize,
                                        uint64_t addr, uint64_t size, AccessType type)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId)) {
        return true;
    }

    std::shared_ptr<MemoryEvent> event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::ACCESS;
    event->eventSubType = type == AccessType::READ ? EventSubType::ATB_READ :
        type == AccessType::WRITE ? EventSubType::ATB_WRITE : EventSubType::ATB_READ_OR_WRITE;
    event->poolType = PoolType::ATB;
    event->addr = addr;
    event->size = size;
    event->device = devId;
    event->name = name;
    event->attr = attr;

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

bool EventReport::ReportMemorySnapshot(const MemorySnapshotInfo& memory_info, CallStackString&& stack)
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("[mark] RT_ERROR_INVALID_VALUE, %d", devId);
    }

    if (IsNeedSkip(devId)) {
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

    Process::GetInstance(initConfig_).SendEvent(event);

    return true;
}

void EventReport::ReportMemorySnapshotOnOOM(const CallStackString& stack)
{
    // Try to call Python's take_snapshot function to get accurate memory info
    if (Utility::IsPyInterpRepeInited()) {
        Utility::PyInterpGuard guard;
        
        try {
            // Import msmemscope module
            Utility::PythonObject msmemscope_module = Utility::PythonObject::Import("msmemscope", false, true);
            if (msmemscope_module.IsBad()) {
                LOG_ERROR("Failed to import msmemscope module for OOM snapshot");
                return;
            }
            
            // Get take_snapshot function
            Utility::PythonObject take_snapshot_func = msmemscope_module.Get("take_snapshot");
            if (take_snapshot_func.IsBad() || !take_snapshot_func.IsCallable()) {
                LOG_ERROR("Failed to get take_snapshot function for OOM snapshot");
                return;
            }
            
            // Get current device ID
            int32_t devId = GD_INVALID_NUM;
            if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
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
            
            if (!result.IsBad()) {
                LOG_INFO("OOM memory snapshot created via Python take_snapshot");
                return;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in Python take_snapshot: %s", e.what());
        }
    }
}

}