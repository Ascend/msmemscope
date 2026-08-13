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

#ifndef EVENT_REPORT_H
#define EVENT_REPORT_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ascend_hal.h"
#include "config_info.h"
#include "dump.h"
#include "kernel_hooks/acl_hooks.h"
#include "kernel_hooks/kernel_event_trace.h"
#include "kernel_hooks/runtime_hooks.h"
#include "process.h"
#include "record_info.h"
#include "trace_manager/event_trace_manager.h"

namespace MemScope
{

constexpr mode_t REGULAR_MODE_MASK = 0177;
constexpr char ATEN_MSG[] = "memscope-aten-";
constexpr char ATEN_BEGIN_MSG[] = "b:";
constexpr char ATEN_END_MSG[] = "e:";
constexpr char ACCESS_MSG[] = "ac:";
constexpr char SANITIZER_OP_MSG[] = "sanitizer-op:";

struct MstxStepInfo
{
    uint64_t currentStepId = 0;
    bool inStepRange = false;  // 不在mstx_range_start和mstx_range_end之间的数据，不采集

    // 暂存用mstx标记Step的RangeId,与用mstx标记host采集的RangeId区分开
    // 可能存在多线程操作，例如线程A mstx_range_start,线程B mstx_range_start,线程A mstx_range_end,线程B mstx_range_end
    // 因此是数组
    std::vector<uint64_t> stepMarkRangeIdList;
};

/*
 * EventReport类主要功能：
 * 1. 将劫持记录的信息传回到工具进程
 */
class EventReport
{
   public:
    static EventReport& Instance(MemScopeCommType type);
    bool ReportHalCreate(uint64_t addr, uint64_t size, const drv_mem_prop& prop, CallStackString&& stack);
    bool ReportHalRelease(uint64_t addr, CallStackString&& stack);
    bool ReportHalMalloc(uint64_t addr, uint64_t size, unsigned long long flag, CallStackString&& stack);
    bool ReportHalMalloc(uint64_t addr, uint64_t size, unsigned long long flag);  // shadow mode (no callstack)
    bool ReportHalFree(uint64_t addr, CallStackString&& stack);
    bool ReportHalFree(uint64_t addr);  // shadow mode (no callstack)
    bool ReportHostRegister(uint64_t addr, uint64_t size, CallStackString&& stack);
    bool ReportHostUnregister(uint64_t addr, CallStackString&& stack);
    bool ReportKernelLaunch(const AclnnKernelMapInfo& kernelLaunchInfo);
    bool ReportKernelExcute(const TaskKey& key, std::string& name, uint64_t time, RecordSubType type);
    bool ReportAclItf(RecordSubType subtype);
    bool ReportTraceStatus(const EventTraceStatus status);
    bool ReportMark(MarkType type, std::string& msg, uint32_t streamId, uint64_t rangeId);
    bool ReportMemPoolRecord(EventSubType type, const MemoryUsage& info, CallStackString&& stack);
    bool ReportAtbOpExecute(const char* name, size_t nameSize, const char* attr, size_t attrSize, RecordSubType type);
    bool ReportAtbKernel(const char* name, size_t nameSize, const char* attr, size_t attrSize, RecordSubType type);
    bool ReportAtbAccessMemory(const char* name, size_t nameSize, const char* attr, size_t attrSize, uint64_t addr,
                               uint64_t size, AccessType type);
    bool ReportAtenLaunch(const std::string& name, bool isStart, std::string&& pystack);
    bool ReportAtenAccess(const std::string& name, const std::string& attr, AccessType type, uint64_t addr,
                          uint64_t size, std::string&& pystack);
    bool ReportAddrInfo(EventSubType type, uint64_t addr,
                        const std::vector<std::pair<OwnerLevel, std::string>>& labels);
    bool ReportPyStepRecord();
    bool ReportMemorySnapshot(const MemorySnapshotInfo& memory_info, CallStackString&& stack);
    void ReportMemorySnapshotOnOOM(const CallStackString& stack = CallStackString());
    bool ReportOOMTrigger(const OOMTriggerInfo& info);
    bool ReportOOMMemRecord(const OOMMemRecord& record, EventSubType subType);
    void UpdateAnalysisType();

   private:
    void Init();
    explicit EventReport(MemScopeCommType type);
    ~EventReport();

    bool IsNeedSkip(int32_t devid);
    void SetStepInfo(MarkType type, std::string msg, uint64_t rangeId);

    // 查询整卡显存用量（dcmi_get_device_hbm_info，与 npu-smi 同源；内部已有
    // EventReportSuppressor 防递归上报）；查询失败返回 -1（不更新缓存，限频告警一次）。查询结果缓存到
    // MemoryStateManager（槽位与事件 device 同源：HAL 事件 flag 解析/池事件 TransDeviceId
    // 均归一为物理卡号）；HOST（DEVICE_ID_CPU）/无效（GD_INVALID_NUM）devId 不写缓存
    int64_t QueryDeviceUsed(int32_t devId);
    // 查询本进程显存用量（aclrtGetMemInfo(ACL_HBM_MEM) 的 total - free，ACL 池视角，
    // 与 npu-smi Process memory 对齐；内部已有 EventReportSuppressor 防递归上报）；
    // 失败返回 -1（不更新缓存，限频告警一次），缓存机制与 QueryDeviceUsed 一致
    int64_t QueryProcessUsed(int32_t devId);

   private:
    std::atomic<uint64_t> recordIndex_;
    std::atomic<uint64_t> kernelLaunchRecordIndex_;
    // python接口标识step和mstx标识step两种方式不允许同时存在
    std::atomic<uint64_t> pyStepId_;

    MstxStepInfo stepInfo_;
    std::mutex mutex_;

    // 已分配的HAL块地址 → 分配时device映射：FREE事件据此过滤未注册地址，
    // 并回填device（分配时语义，与MALLOC事件flag解析同源），保证FREE事件能按key(pid, device, addr)定位
    std::unordered_map<uint64_t, int32_t> halPtrs_;
    std::atomic<bool> destroyed_{false};

    // 查询失败限频告警标记（首个失败日志后静默）
    std::atomic<bool> deviceUsedQueryWarned_{false};
    std::atomic<bool> processUsedQueryWarned_{false};
};

class GetDeviceInfo
{
   public:
    static GetDeviceInfo& Instance();
    bool GetDeviceId(int32_t& devId);
    bool TransDeviceId(int32_t& devId);
    // 查询设备 HBM 用量（dcmi_get_device_hbm_info，与 npu-smi 同源，单位 MB 转出）；
    // 成功返回 true 并填写 usedMb/totalMb，失败（未 init/未建表/查询失败）返回 false
    bool GetDeviceHbmInfo(int32_t devId, uint64_t& usedMb, uint64_t& totalMb);
    // 查询当前设备上下文的 ACL 池显存信息（aclrtGetMemInfo(ACL_HBM_MEM)，字节单位）
    bool GetDeviceMemInfo(size_t& freeMem, size_t& totalMem);

   private:
    // dcmi_init 一次性初始化（失败则本进程内永久降级，不再重试）
    bool EnsureDcmiInit();
    // 一次性建表：acl 逻辑号 → (card_id, device_id)（dcmi_get_card_list + num_in_card + logic_id）
    bool BuildDeviceMap();

    GetDeviceInfo()
    {
        const char* visibleDeviceEnv = std::getenv("ASCEND_RT_VISIBLE_DEVICES");
        if (!visibleDeviceEnv)
        {
            LOG_DEBUG("ASCEND_RT_VISIBLE_DEVICES environment variable not found!");
            return;
        }

        std::string visibleDeviceStr(visibleDeviceEnv);
        std::vector<std::string> deviceTokens;
        std::istringstream iss(visibleDeviceStr);
        std::string token;

        while (std::getline(iss, token, ','))
        {
            // 去除首尾空格
            token.erase(0, token.find_first_not_of(" \t\n\r\f\v"));
            token.erase(token.find_last_not_of(" \t\n\r\f\v") + 1);

            if (!token.empty())
            {
                deviceTokens.push_back(token);
            }
        }

        int32_t deviceId = 0;
        for (const auto& dev : deviceTokens)
        {
            size_t pos;
            try
            {
                int32_t id = std::stoi(dev, &pos);
                if (pos != dev.length() || id < 0)
                {
                    throw std::invalid_argument("Invalid format: '" + std::string(dev) + "'");
                }
                visibleDeviceMap[deviceId] = id;
                deviceId++;
            }
            catch (const std::invalid_argument& e)
            {
                LOG_ERROR("Invalid format for ASCEND_RT_VISIBLE_DEVICES:%s", e.what());
                visibleDeviceMap.clear();
                return;
            }
        }
        setVisibleDevice = true;
        std::cout << "[msmemscope] Info: Set ASCEND_RT_VISIBLE_DEVICES successfully!" << std::endl;
    }

   private:
    ~GetDeviceInfo() = default;

    GetDeviceInfo(const GetDeviceInfo&) = delete;
    GetDeviceInfo& operator=(const GetDeviceInfo&) = delete;
    GetDeviceInfo(GetDeviceInfo&&) = delete;
    GetDeviceInfo& operator=(GetDeviceInfo&&) = delete;

    bool setVisibleDevice = false;  // 是否存在可见卡
    std::unordered_map<int32_t, int32_t> visibleDeviceMap;

    std::once_flag dcmiInitFlag_;
    bool dcmiReady_ = false;  // dcmi_init 成功后才置位
    std::once_flag dcmiMapInitFlag_;
    bool dcmiMapReady_ = false;  // 设备映射建表成功后才置位
    // acl 逻辑号 → (card_id, device_id)，建表后只读
    std::unordered_map<int32_t, std::pair<int32_t, int32_t>> devIdToDcmiMap_;
};

MemOpSpace GetMemOpSpace(unsigned long long flag);

}  // namespace MemScope
#endif
