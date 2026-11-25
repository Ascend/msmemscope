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

#ifndef RUNTIME_PROF_API_H
#define RUNTIME_PROF_API_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "log.h"
#include "vallina_symbol.h"

namespace MemScope
{
// 和获取驱动上报信息接口配套使用
struct RtProfApiLoader
{
    static void *Load(void) { return MemScope::LibLoad("libprofapi.so"); }
};

enum ProfilerCallbackType
{
    PROFILE_CTRL_CALLBACK = 0,
    PROFILE_DEVICE_STATE_CALLBACK,
    PROFILE_REPORT_API_CALLBACK,
    PROFILE_REPORT_EVENT_CALLBACK,
    PROFILE_REPORT_COMPACT_CALLBACK,
    PROFILE_REPORT_ADDITIONAL_CALLBACK,
    PROFILE_REPORT_REG_TYPE_INFO_CALLBACK,
    PROFILE_REPORT_GET_HASH_ID_CALLBACK,
    PROFILE_HOST_FREQ_IS_ENABLE_CALLBACK,
    PROFILE_REPORT_API_C_CALLBACK,
    PROFILE_REPORT_EVENT_C_CALLBACK,
    PROFILE_REPORT_REG_TYPE_INFO_C_CALLBACK,
    PROFILE_REPORT_GET_HASH_ID_C_CALLBACK,
    PROFILE_HOST_FREQ_IS_ENABLE_C_CALLBACK,
};

constexpr uint16_t MSPROF_DATA_HEAD_MAGIC_NUM = 0x5a5a;
constexpr uint64_t MSPROF_EVENT_FLAG = 0xFFFFFFFFFFFFFFFFULL;
constexpr uint32_t PROF_INVALID_MODE_ID = 0xFFFFFFFFUL;
constexpr size_t MAX_DEV_NUM = 64;

constexpr uint64_t PROF_ACL_API = 0x00000001ULL;
constexpr uint64_t PROF_TASK_TIME = 0x00000800ULL;
constexpr uint64_t PROF_RUNTIME_TRACE = 0x0000004000000ULL;

constexpr uint16_t MSPROF_STREAM_EXPAND_SPEC_TYPE = 804U;
constexpr uint16_t MSPROF_REPORT_RUNTIME_LEVEL = 5000U;
constexpr uint16_t MSPROF_REPORT_NNOPBASE_LEVEL = 10000U;
constexpr uint16_t MSPROF_REPORT_ACL_LEVEL = 20000U;

struct MsprofApi
{  // for MsprofReportApi
    uint16_t magicNumber = MSPROF_DATA_HEAD_MAGIC_NUM;
    uint16_t level;
    uint32_t type;
    uint32_t threadId;
    uint32_t reserve;
    uint64_t beginTime;
    uint64_t endTime;
    uint64_t itemId;
};

struct MsprofEvent
{  // for MsprofReportEvent
    uint16_t magicNumber = MSPROF_DATA_HEAD_MAGIC_NUM;
    uint16_t level;
    uint32_t type;
    uint32_t threadId;
    uint32_t requestId;  // 0xFFFF means single event
    uint64_t timestamp;
    uint64_t reserve = MSPROF_EVENT_FLAG;
    uint64_t itemId;
};

struct MsprofRuntimeTrack
{
    uint16_t deviceId;
    uint16_t streamId;
    uint32_t taskInfo;
    uint64_t taskType;
    uint64_t kernelName;
};

struct MsprofNodeBasicInfo
{
    uint64_t opName;
    uint32_t taskType;
    uint64_t opType;
    uint32_t blockDim;
    uint32_t opFlag;
    uint8_t opState;
};

struct MsprofAttrInfo
{
    uint64_t opName;
    uint32_t attrType;
    uint64_t hashId;
};

#pragma pack(1)
struct MsprofHcclOPInfo
{  // for MsprofReportCompactInfo buffer data
    uint8_t relay : 1;
    uint8_t retry : 1;
    uint8_t dataType;
    uint64_t algType;  // 通信算子使用的算法,hash的key,其值是以"-"分隔的字符串
    uint64_t count;
    uint64_t groupName;
};
#pragma pack()

#pragma pack(1)
struct MsprofStreamExpandSpecInfo
{                          // for MsprofReportCompactInfo buffer data
    uint8_t expandStatus;  // 1: 扩容场景 0: 非扩容场景
    uint8_t reserved1;
    uint16_t reserved2;
};
#pragma pack()

enum TsTaskType
{
    TS_TASK_TYPE_KERNEL_AICORE = 0,
    TS_TASK_TYPE_KERNEL_AICPU = 1,
    TS_TASK_TYPE_KERNEL_AIVEC = 66,
    TS_TASK_TYPE_FLIP = 98,
    TS_TASK_TYPE_KERNEL_MIX_AIC = 108,
    TS_TASK_TYPE_KERNEL_MIX_AIV = 109,
    TS_TASK_TYPE_RESERVED,
};

enum RtProfileDataType
{
    RT_PROFILE_TYPE_TASK_BEGIN = 0,
    RT_PROFILE_TYPE_TASK_END = 800,
    RT_PROFILE_TYPE_MEMCPY_INFO = 801,
    RT_PROFILE_TYPE_TASK_TRACK = 802,
    RT_PROFILE_TYPE_API_BEGIN = 1000,
    RT_PROFILE_TYPE_API_END = 2000,
    RT_PROFILE_TYPE_MAX
};

const uint16_t MSPROF_COMPACT_INFO_DATA_LENGTH = 40;
struct MsprofCompactInfo
{
    uint16_t magicNumber = MSPROF_DATA_HEAD_MAGIC_NUM;
    uint16_t level;
    uint32_t type;
    uint32_t threadId;
    uint32_t dataLen;
    uint64_t timestamp;
    union
    {
        uint8_t info[MSPROF_COMPACT_INFO_DATA_LENGTH];
        MsprofRuntimeTrack runtimeTrack;
        MsprofNodeBasicInfo nodeBasicInfo;
        MsprofAttrInfo nodeAttrInfo;
        MsprofHcclOPInfo hcclopInfo;
        MsprofStreamExpandSpecInfo streamExpandInfo;
    } data;
};

constexpr size_t PATH_LEN_MAX = 1023;
constexpr size_t PARAM_LEN_MAX = 4095;

struct CommandHandleParams
{
    uint32_t pathLen;
    uint32_t storageLimit;  // MB
    uint32_t profDataLen;
    char path[PATH_LEN_MAX + 1];
    char profData[PARAM_LEN_MAX + 1];
};

struct CommandHandle
{
    uint64_t profSwitch;
    uint64_t profSwitchHi;
    uint32_t devNums;
    uint32_t devIdList[MAX_DEV_NUM];
    uint32_t modelId;
    uint32_t type;
    uint32_t cacheFlag;
    struct CommandHandleParams params;
};

enum CommandHandleType
{
    PROF_COMMANDHANDLE_TYPE_INIT = 0,
    PROF_COMMANDHANDLE_TYPE_START,
    PROF_COMMANDHANDLE_TYPE_STOP,
    PROF_COMMANDHANDLE_TYPE_FINALIZE,
};

enum ProfApiErrorCode
{
    PROFAPI_ERROR_NONE = 0,
    PROFAPI_ERROR_MEM_NOT_ENOUGH,
    PROFAPI_ERROR_GET_ENV,
    PROFAPI_ERROR_CONFIG_INVALID,
    PROFAPI_ERROR_ACL_JSON_OFF,
    PROFAPI_ERROR,
    PROFAPI_ERROR_UNINITIALIZE,
};

void RegisterRtProfileCallback();
int32_t CompactInfoReporterCallbackImpl(uint32_t agingFlag, const void *data, uint32_t length);
uint64_t GetHashIdCallBackImply(const char *hashInfo, size_t len);
void SetProfCommand(uint32_t devId);
// 通过libprofapi的MsprofId2Str反查hashId对应的kernel名：
// 部分CANN版本的MsprofGetHashId不回调工具注册的GET_HASH_ID回调，但会由libmsprof
// 用同一套DoubleHash算法算出hash并保存名字，此时可经该接口取回；符号缺失时返回空串
std::string GetKernelNameByHashId(uint64_t hashId);
// 仅测试用：清除MsprofId2Str符号解析缓存，便于按用例重新注入桩
void ResetKernelNameReverseLookupForTest();
}  // namespace MemScope
#endif
