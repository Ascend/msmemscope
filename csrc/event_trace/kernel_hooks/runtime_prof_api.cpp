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
#include "runtime_prof_api.h"

#include <cstring>

#include "kernel_event_trace.h"
#include "securec.h"
#include "stars_common.h"
namespace MemScope
{
int32_t CompactInfoReporterCallbackImpl(uint32_t agingFlag, const void *data, uint32_t length)
{
    if (data == nullptr || length != sizeof(struct MsprofCompactInfo))
    {
        LOG_ERROR("Report Compact Info failed with nullptr.");
        return PROFAPI_ERROR;
    }

    const MsprofCompactInfo *compact = reinterpret_cast<const MsprofCompactInfo *>(data);
    // streamExpandInfo代表是否扩容,会在一开始上报一次
    if (compact && compact->level == MSPROF_REPORT_RUNTIME_LEVEL && compact->type == MSPROF_STREAM_EXPAND_SPEC_TYPE)
    {
        StarsCommon::SetStreamExpandStatus(compact->data.streamExpandInfo.expandStatus);
    }
    if (compact && compact->level == MSPROF_REPORT_RUNTIME_LEVEL && compact->type == RT_PROFILE_TYPE_TASK_TRACK)
    {
        const MsprofRuntimeTrack &runtimeTrack = compact->data.runtimeTrack;
        TaskKey taskKey = std::make_tuple(runtimeTrack.deviceId, runtimeTrack.streamId,
                                          static_cast<uint16_t>(runtimeTrack.taskInfo & 0xffff));
        RuntimeKernelLinker::GetInstance().RuntimeTaskInfoLaunch(taskKey, runtimeTrack.kernelName);
    }
    return PROFAPI_ERROR_NONE;
}

static uint64_t GetHashIdImple(const std::string &hashInfo)
{
    static const uint32_t UINT32_BITS = 32;
    uint32_t prime[2] = {29, 131};
    uint32_t hash[2] = {0};
    for (char d : hashInfo)
    {
        hash[0] = hash[0] * prime[0] + static_cast<uint32_t>(d);
        hash[1] = hash[1] * prime[1] + static_cast<uint32_t>(d);
    }
    return (((static_cast<uint64_t>(hash[0])) << UINT32_BITS) | hash[1]);
}

uint64_t GetHashIdCallBackImply(const char *hashInfo, size_t len)
{
    if (hashInfo == nullptr)
    {
        LOG_ERROR("GenHashId failed. hashInfo is nullptr.");
        return 0;
    }

    // 部分CANN版本回调的len为0，此时按NUL结尾字符串取长度，避免把空名字写入映射表；
    // len非0时按显式长度构造，避免hash计算越过回调给出的缓冲区
    size_t nameLen = len == 0 ? std::strlen(hashInfo) : len;
    std::string kernelName(hashInfo, nameLen);
    uint64_t hashId = GetHashIdImple(kernelName);
    RuntimeKernelLinker::GetInstance().SetHashInfo(hashId, kernelName);
    return hashId;
}

namespace
{
using Id2StrFunc = char *(*)(uint64_t);
// MsprofId2Str符号只解析一次：未导出该符号的CANN版本若每次kernel下发都dlsym，
// 会在RuntimeKernelLinker持锁路径上放大开销。生产路径的调用均由该锁串行化，
// 测试通过ResetKernelNameReverseLookupForTest恢复初始状态后重新注入桩。
Id2StrFunc g_id2StrFunc = nullptr;
bool g_id2StrResolved = false;
}  // namespace

std::string GetKernelNameByHashId(uint64_t hashId)
{
    if (!g_id2StrResolved)
    {
        g_id2StrFunc = VallinaSymbol<RtProfApiLoader>::Instance().Get<Id2StrFunc>("MsprofId2Str");
        g_id2StrResolved = true;
    }
    if (g_id2StrFunc == nullptr)
    {
        return "";
    }
    const char *name = g_id2StrFunc(hashId);
    return name == nullptr ? "" : std::string(name);
}

void ResetKernelNameReverseLookupForTest()
{
    g_id2StrFunc = nullptr;
    g_id2StrResolved = false;
}

int32_t ApiReporterCallbackImpl(uint32_t agingFlag, const MsprofApi *const data) { return 0; }
int32_t AdditionalInfoCallbackImpl(uint32_t agingFlag, const void *data, uint32_t len) { return 0; }

void RegisterRtProfileCallback()
{
    static const std::vector<std::pair<int, void *>> CALLBACK_FUNC_LIST = {
        {PROFILE_REPORT_GET_HASH_ID_C_CALLBACK, reinterpret_cast<void *>(GetHashIdCallBackImply)},
        {PROFILE_REPORT_COMPACT_CALLBACK, reinterpret_cast<void *>(CompactInfoReporterCallbackImpl)},
        {PROFILE_REPORT_API_C_CALLBACK, reinterpret_cast<void *>(ApiReporterCallbackImpl)},
        {PROFILE_REPORT_ADDITIONAL_CALLBACK, reinterpret_cast<void *>(AdditionalInfoCallbackImpl)},
    };

    using RegisterFunc = int32_t (*)(int32_t, void *, uint32_t);
    auto vallina = VallinaSymbol<RtProfApiLoader>::Instance().Get<RegisterFunc>("MsprofRegisterProfileCallback");
    if (vallina == nullptr)
    {
        LOG_ERROR("Get register func failed");
        return;
    }

    int32_t ret;
    for (auto iter : CALLBACK_FUNC_LIST)
    {
        ret = vallina(iter.first, iter.second, sizeof(void *));
        if (ret != 0)
        {
            LOG_ERROR("Register rtProfile callback failed, type = %d", iter.first);
        }
    }

    return;
}

void SetProfCommand(uint32_t devId)
{
    CommandHandle command;
    if (memset_s(&command, sizeof(command), 0, sizeof(command)) != EOK)
    {
        return;
    }

    command.profSwitch = PROF_TASK_TIME | PROF_RUNTIME_TRACE;
    command.profSwitchHi = 0;
    command.devNums = 1;
    command.devIdList[0] = devId;
    command.modelId = PROF_INVALID_MODE_ID;
    command.type = PROF_COMMANDHANDLE_TYPE_START;

    using ProfSetProfCommandFunc = int32_t (*)(void *, uint32_t);
    auto vallina = VallinaSymbol<RtProfApiLoader>::Instance().Get<ProfSetProfCommandFunc>("profSetProfCommand");
    if (vallina == nullptr)
    {
        LOG_ERROR("Set prof command failed with nullptr.");
        return;
    }

    int ret = vallina(static_cast<void *>(&command), sizeof(CommandHandle));
    if (ret != 0)
    {
        LOG_ERROR("Set prof command failed.");
        return;
    }

    return;
}
}  // namespace MemScope
