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

#include "hal_hooks.h"

#include <dlfcn.h>

#include <iostream>
#include <string>

#include "call_stack.h"
#include "log.h"
#include "oom_handler.h"
#include "record_info.h"
#include "trace_manager/event_trace_manager.h"

using namespace MemScope;

// 通用OOM错误处理函数
void HandleOOM(size_t size, uint64_t flag, int ret)
{
    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::MALLOC) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::FREE))
    {
        return;
    }
    // 在OOM时直接获取C和Python调用栈，不依赖配置
    CallStackString stack;
    Utility::GetCCallstack(MemScope::DEFAULT_CALL_STACK_DEPTH, stack.cStack, MemScope::SKIP_DEPTH);
    Utility::GetPythonCallstack(MemScope::DEFAULT_CALL_STACK_DEPTH, stack.pyStack);

    // 将调用栈保存到OOMHandler实例中，并触发OOM快照
    OOMHandler::Instance().SetOOMStack(stack);
    EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportMemorySnapshotOnOOM(stack);
}

drvError_t halMemAlloc(void **pp, unsigned long long size, unsigned long long flag)
{
    static auto inner_func =
        reinterpret_cast<drvError_t (*)(void **pp, unsigned long long size, unsigned long long flag)>(
            dlsym(RTLD_DEFAULT, "halMemAllocInner"));
    if (inner_func == nullptr)
    {
        LOG_ERROR("HAL memory alloc func not found");
        return DRV_ERROR_NOT_SUPPORT;
    }
    drvError_t ret = inner_func(pp, size, flag);
    if (ret != DRV_ERROR_NONE)
    {
        // Check for OOM errors
        if (ret == DRV_ERROR_OUT_OF_MEMORY)
        {
            HandleOOM(size, flag, ret);
        }
        return ret;
    }
    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::MALLOC) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::FREE))
    {
        return ret;
    }

    CallStackString stack;
    Utility::GetCallstack(stack);

    // report to memscope here
    uintptr_t addr = reinterpret_cast<uintptr_t>(*pp);
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
             .ReportHalMalloc(reinterpret_cast<uint64_t>(addr), size, flag, std::move(stack)))
    {
        LOG_ERROR("halMemAlloc report failed");
    }

    return ret;
}

drvError_t halMemFree(void *pp)
{
    static auto inner_func = reinterpret_cast<drvError_t (*)(void *pp)>(dlsym(RTLD_DEFAULT, "halMemFreeInner"));
    if (inner_func == nullptr)
    {
        LOG_ERROR("HAL memory free func not found");
        return DRV_ERROR_NOT_SUPPORT;
    }
    drvError_t ret = inner_func(pp);
    if (ret != DRV_ERROR_NONE)
    {
        return ret;
    }

    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::MALLOC) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::FREE))
    {
        return ret;
    }

    CallStackString stack;
    Utility::GetCallstack(stack);

    uintptr_t addr = reinterpret_cast<uintptr_t>(pp);
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
             .ReportHalFree(reinterpret_cast<uint64_t>(addr), std::move(stack)))
    {
        LOG_ERROR("halMemFree report failed");
    }

    return ret;
}

drvError_t halMemCreate(drv_mem_handle_t **handle, size_t size, const struct drv_mem_prop *prop, uint64_t flag)
{
    static auto inner_func =
        reinterpret_cast<drvError_t (*)(drv_mem_handle_t **, size_t, const struct drv_mem_prop *, uint64_t)>(
            dlsym(RTLD_DEFAULT, "halMemCreateInner"));

    drvError_t ret = DRV_ERROR_NONE;

    if (inner_func)
    {
        // 驱动新包，含有halMemCreateInner实现
        ret = inner_func(handle, size, prop, flag);
    }
    else
    {
        // 老驱动包：查找原始halMemCreate
        static auto original_func =
            reinterpret_cast<drvError_t (*)(drv_mem_handle_t **, size_t, const struct drv_mem_prop *, uint64_t)>(
                dlsym(RTLD_NEXT, "halMemCreate"));
        if (original_func == nullptr)
        {
            ret = DRV_ERROR_RESERVED;
        }
        else
        {
            ret = original_func(handle, size, prop, flag);
        }
    }

    if (ret != DRV_ERROR_NONE)
    {
        // Check for OOM errors
        if (ret == DRV_ERROR_OUT_OF_MEMORY)
        {
            HandleOOM(size, flag, ret);
        }
        else
        {
            LOG_ERROR("halMemCreate excute failed");
        }
        return ret;
    }

    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::MALLOC) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::FREE))
    {
        return ret;
    }

    if (prop == nullptr)
    {
        LOG_ERROR("Driver memory property pointer is null");
        return ret;
    }

    CallStackString stack;
    Utility::GetCallstack(stack);

    uintptr_t addr = reinterpret_cast<uintptr_t>(*handle);
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
             .ReportHalCreate(reinterpret_cast<uint64_t>(addr), size, *prop, std::move(stack)))
    {
        LOG_ERROR("halMemCreate report failed");
    }

    return ret;
}

drvError_t halMemRelease(drv_mem_handle_t *handle)
{
    static auto inner_func =
        reinterpret_cast<drvError_t (*)(drv_mem_handle_t *)>(dlsym(RTLD_DEFAULT, "halMemReleaseInner"));

    drvError_t ret = DRV_ERROR_NONE;

    if (inner_func)
    {
        // 驱动新包，含有halMemReleaseInner实现
        ret = inner_func(handle);
    }
    else
    {
        // 老驱动包：查找原始halMemRelease
        static auto original_func =
            reinterpret_cast<drvError_t (*)(drv_mem_handle_t *)>(dlsym(RTLD_NEXT, "halMemRelease"));
        if (original_func == nullptr)
        {
            ret = DRV_ERROR_RESERVED;
        }
        else
        {
            ret = original_func(handle);
        }
    }

    if (ret != DRV_ERROR_NONE)
    {
        LOG_ERROR("halMemRelease excute failed");
        return ret;
    }

    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::MALLOC) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::FREE))
    {
        return ret;
    }

    CallStackString stack;
    Utility::GetCallstack(stack);

    uintptr_t addr = reinterpret_cast<uintptr_t>(handle);
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
             .ReportHalRelease(reinterpret_cast<uint64_t>(addr), std::move(stack)))
    {
        LOG_ERROR("halMemRelease report failed");
    }

    return ret;
}

#define ACL_HOST_REG_PINNED 0X10000000UL

aclError aclrtHostRegisterV2(void *ptr, uint64_t size, uint32_t flag)
{
    static auto original_func =
        reinterpret_cast<drvError_t (*)(void *, uint64_t, uint32_t)>(dlsym(RTLD_NEXT, "aclrtHostRegisterV2"));
    if (original_func == nullptr)
    {
        return ACL_ERROR_INTERNAL_ERROR;
    }

    aclError ret = original_func(ptr, size, flag);
    if (ret != ACL_SUCCESS)
    {
        return ret;
    }

    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::MALLOC) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::FREE))
    {
        return ret;
    }

    if ((flag & static_cast<uint32_t>(ACL_HOST_REG_PINNED)) == 0)
    {
        return ret;
    }

    CallStackString stack;
    Utility::GetCallstack(stack);
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
             .ReportHostRegister(reinterpret_cast<uint64_t>(addr), size, std::move(stack)))
    {
        LOG_ERROR("aclrtHostRegisterV2 report failed");
    }

    return ret;
}

aclError aclrtHostUnregister(void *ptr)
{
    static auto original_func = reinterpret_cast<drvError_t (*)(void *)>(dlsym(RTLD_NEXT, "aclrtHostUnregister"));
    if (original_func == nullptr)
    {
        return ACL_ERROR_INTERNAL_ERROR;
    }

    aclError ret = original_func(ptr);
    if (ret != ACL_SUCCESS)
    {
        return ret;
    }

    if (!EventTraceManager::Instance().IsNeedTrace(EventBaseType::MALLOC) &&
        !EventTraceManager::Instance().IsNeedTrace(EventBaseType::FREE))
    {
        return ret;
    }

    CallStackString stack;
    Utility::GetCallstack(stack);
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
             .ReportHostUnregister(reinterpret_cast<uint64_t>(addr), std::move(stack)))
    {
        LOG_ERROR("aclrtHostUnregister report failed");
    }

    return ret;
}
