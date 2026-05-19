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

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <unordered_set>
#include "kernel_hooks/runtime_hooks.h"
#include "record_info.h"
#include "config_info.h"
#include "kernel_hooks/acl_hooks.h"
#include "kernel_hooks/kernel_event_trace.h"
#include "trace_manager/event_trace_manager.h"
#include "ascend_hal.h"

#include "process.h"
#include "dump.h"

namespace MemScope {

constexpr mode_t REGULAR_MODE_MASK = 0177;
constexpr char ATEN_MSG[] = "memscope-aten-";
constexpr char ATEN_BEGIN_MSG[] = "b:";
constexpr char ATEN_END_MSG[] = "e:";
constexpr char ACCESS_MSG[] = "ac:";

struct MstxStepInfo {
    uint64_t currentStepId = 0;
    bool inStepRange = false; // 不在mstx_range_start和mstx_range_end之间的数据，不采集

    // 暂存用mstx标记Step的RangeId,与用mstx标记host采集的RangeId区分开
    // 可能存在多线程操作，例如线程A mstx_range_start,线程B mstx_range_start,线程A mstx_range_end,线程B mstx_range_end
    // 因此是数组
    std::vector<uint64_t> stepMarkRangeIdList;
};

/*
 * EventReport类主要功能：
 * 1. 将劫持记录的信息传回到工具进程
*/
class EventReport {
public:
    static EventReport& Instance(MemScopeCommType type);
    bool ReportHalCreate(uint64_t addr, uint64_t size, const drv_mem_prop& prop, CallStackString&& stack);
    bool ReportHalRelease(uint64_t addr, CallStackString&& stack);
    bool ReportHalMalloc(uint64_t addr, uint64_t size, unsigned long long flag, CallStackString&& stack);
    bool ReportHalFree(uint64_t addr, CallStackString&& stack);
    bool ReportKernelLaunch(const AclnnKernelMapInfo &kernelLaunchInfo);
    bool ReportKernelExcute(const TaskKey &key, std::string &name, uint64_t time, RecordSubType type);
    bool ReportAclItf(RecordSubType subtype);
    bool ReportTraceStatus(const EventTraceStatus status);
    bool ReportMark(MarkType type, std::string& msg, uint32_t streamId, uint64_t rangeId);
    bool ReportMemPoolRecord(EventSubType type, const MemoryUsage& info, const std::string& owner,
                             CallStackString&& stack);
    bool ReportAtbOpExecute(const char* name, size_t nameSize, const char* attr, size_t attrSize, RecordSubType type);
    bool ReportAtbKernel(const char* name, size_t nameSize, const char* attr, size_t attrSize, RecordSubType type);
    bool ReportAtbAccessMemory(const char* name, size_t nameSize, const char* attr, size_t attrSize,
                               uint64_t addr, uint64_t size, AccessType type);
    bool ReportAtenLaunch(const std::string& name, bool isStart, std::string&& pystack);
    bool ReportAtenAccess(const std::string& name, const std::string& attr, AccessType type,
                          uint64_t addr, uint64_t size, std::string&& pystack);
    bool ReportAddrInfo(EventSubType type, uint64_t addr, std::string owner);
    bool ReportPyStepRecord();
    bool ReportMemorySnapshot(const MemorySnapshotInfo& memory_info, CallStackString&& stack);
    void ReportMemorySnapshotOnOOM(const CallStackString& stack = CallStackString());
    void UpdateAnalysisType();

private:
    void Init();
    explicit EventReport(MemScopeCommType type);
    ~EventReport();

    bool IsNeedSkip(int32_t devid);
    void SetStepInfo(MarkType type, std::string msg, uint64_t rangeId);

    // socket通信在某些场景下，client调用connect返回true并不一定代表真实连接成功
    // 这里以接收到server发过来消息为准
    bool IsConnectToServer();
private:
    std::atomic<uint64_t> recordIndex_;
    std::atomic<uint64_t> kernelLaunchRecordIndex_;
    // python接口标识step和mstx标识step两种方式不允许同时存在
    std::atomic<uint64_t> pyStepId_;

    MstxStepInfo stepInfo_;
    std::mutex mutex_;

    Config initConfig_;

    std::unordered_set<uint64_t> halPtrs_;
    std::atomic<bool> destroyed_{false};

};

class GetDeviceInfo {
public:
    static GetDeviceInfo& Instance();
    bool GetDeviceId(int32_t &devId);
    bool TransDeviceId(int32_t &devId);
    bool GetDeviceMemInfo(size_t &freeMem, size_t &totalMem);

private:
    GetDeviceInfo()
    {
        const char* visibleDeviceEnv = std::getenv("ASCEND_RT_VISIBLE_DEVICES");
        if (!visibleDeviceEnv) {
            LOG_INFO("ASCEND_RT_VISIBLE_DEVICES environment variable not found!");
            return;
        }

        std::string visibleDeviceStr(visibleDeviceEnv);
        std::vector<std::string> deviceTokens;
        std::istringstream iss(visibleDeviceStr);
        std::string token;

        while (std::getline(iss, token, ',')) {
            // 去除首尾空格
            token.erase(0, token.find_first_not_of(" \t\n\r\f\v"));
            token.erase(token.find_last_not_of(" \t\n\r\f\v") + 1);

            if (!token.empty()) {
                deviceTokens.push_back(token);
            }
        }

        int32_t deviceId = 0;
        for (const auto& dev : deviceTokens) {
            size_t pos;
            try {
                int32_t id = std::stoi(dev, &pos);
                if (pos != dev.length() || id < 0) {
                    throw std::invalid_argument("Invalid format: '" + std::string(dev) + "'");
                }
                visibleDeviceMap[deviceId] = id;
                deviceId++;
            } catch (const std::invalid_argument& e) {
                LOG_ERROR("Invalid format for ASCEND_RT_VISIBLE_DEVICES:%s", e.what());
                visibleDeviceMap.clear();
                return;
            }
        }
        setVisibleDevice = true;
        std::cout << "[msmemscope] Info: Set ASCEND_RT_VISIBLE_DEVICES successfully!"<< std::endl;
    }

private:
    ~GetDeviceInfo() = default;

    GetDeviceInfo(const GetDeviceInfo&) = delete;
    GetDeviceInfo& operator=(const GetDeviceInfo&) = delete;
    GetDeviceInfo(GetDeviceInfo&&) = delete;
    GetDeviceInfo& operator=(GetDeviceInfo&&) = delete;

    bool setVisibleDevice = false;              // 是否存在可见卡
    std::unordered_map<int32_t, int32_t> visibleDeviceMap;
};

MemOpSpace GetMemOpSpace(unsigned long long flag);

} // namespace MemScope
#endif