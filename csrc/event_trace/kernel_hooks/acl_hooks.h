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

#ifndef ACL_HOOKS_H
#define ACL_HOOKS_H

#include <cstdint>
#include <cstddef>
#include <dlfcn.h>
#include "vallina_symbol.h"
#include "atb_hooks/atb_stub.h"

namespace MemScope {
constexpr int ACL_SUCCESS = 0;
constexpr int ACL_ERROR_INTERNAL_ERROR = 500000;
const constexpr int ACL_ERROR_RT_FAILURE = 500003;
 
struct AclLibLoader {
    static void *Load(void)
    {
        return LibLoad("libascendcl.so");
    }
};

// DCMI 设备管理库加载器：libdcmi.so 由 LD_LIBRARY_PATH 提供（driver lib64），直接 dlopen
struct DcmiLibLoader {
    static void *Load(void)
    {
        return dlopen("libdcmi.so", RTLD_NOW | RTLD_GLOBAL);
    }
};

// DCMI HBM 信息（与驱动 dcmi_interface_api.h 对齐，单位 MB；手动声明保持仓库自包含）
struct dcmi_hbm_info {
    unsigned long long memory_size;   // HBM 总容量（MB）
    unsigned int freq;
    unsigned long long memory_usage;  // HBM 已使用（MB）
    int temp;
    unsigned int bandwith_util_rate;
};

// 进程显存占用（与驱动 dcmi_interface_api.h 对齐；手动声明保持仓库自包含）
struct dcmi_proc_mem_info {
    int proc_id;                // 进程号（pid）
    unsigned long proc_mem_usage;  // 该进程在设备上的显存占用（字节，内核 malloc+sharepool+alloc_page 累计）
};
}

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#ifdef FUNC_VISIBILITY
#define ACL_FUNC_VISIBILITY _declspec(dllexport)
#else
#define ACL_FUNC_VISIBILITY
#endif
#else
#ifdef FUNC_VISIBILITY
#define ACL_FUNC_VISIBILITY __attribute__((visibility("default")))
#else
#define ACL_FUNC_VISIBILITY
#endif
#endif

typedef int aclError;
typedef void *aclrtStream;

typedef enum aclrtMemcpyKind {
    ACL_MEMCPY_HOST_TO_HOST,
    ACL_MEMCPY_HOST_TO_DEVICE,
    ACL_MEMCPY_DEVICE_TO_HOST,
    ACL_MEMCPY_DEVICE_TO_DEVICE,
} aclrtMemcpyKind;

ACL_FUNC_VISIBILITY aclError aclInit(const char *configPath);
ACL_FUNC_VISIBILITY aclError aclFinalize();

aclError aclrtMemcpy(void *dst, size_t destMax, const void *src, size_t count, aclrtMemcpyKind kind);
aclError aclrtSynchronizeStream(aclrtStream stream);

#ifdef __cplusplus
}
#endif

#endif