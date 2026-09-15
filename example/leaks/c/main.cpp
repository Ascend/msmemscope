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

#include <iostream>

#include "acl/acl.h"
#define MSTX_NO_IMPL            // 避免包含mstx的内部头文件
#include "mstx/ms_tools_ext.h"  // 使用mstx打点

#define ACL_ERROR_NONE 0

#define CHECK_ACL(x)                                                                        \
    do                                                                                      \
    {                                                                                       \
        aclError __ret = x;                                                                 \
        if (__ret != ACL_ERROR_NONE)                                                        \
        {                                                                                   \
            std::cerr << __FILE__ << ":" << __LINE__ << " aclError:" << __ret << std::endl; \
        }                                                                                   \
    } while (0);

constexpr uint32_t DATA_LENGTH = 1024;

extern "C" void add_kernel_do(uint32_t blockDim, void *l2ctrl, void *stream, uint8_t *x, uint8_t *y, uint8_t *z);

int main(void)
{
    CHECK_ACL(aclInit(nullptr));
    aclrtContext context;
    int32_t deviceId = 0;
    CHECK_ACL(aclrtSetDevice(deviceId));
    CHECK_ACL(aclrtCreateContext(&context, deviceId));
    aclrtStream stream = nullptr;
    CHECK_ACL(aclrtCreateStream(&stream));

    size_t bufferSize = DATA_LENGTH * sizeof(float);
    float *x = nullptr;
    float *y = nullptr;
    float *z = nullptr;
    CHECK_ACL(aclrtMalloc((void **)&x, bufferSize, ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK_ACL(aclrtMalloc((void **)&y, bufferSize, ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK_ACL(aclrtMalloc((void **)&z, bufferSize, ACL_MEM_MALLOC_HUGE_FIRST));

    uint64_t blockDim = 1UL;

    uint64_t id_1 = mstxRangeStartA("step start", nullptr);  // 使用"step start"的mstxRangeStartA接口标识step开始
    uint64_t id_2 = mstxRangeStartA("step start", nullptr);  // 这里仅作模拟，请按实际情况打点

    add_kernel_do(blockDim, nullptr, stream, (uint8_t *)x, (uint8_t *)y, (uint8_t *)z);
    CHECK_ACL(aclrtSynchronizeStream(stream));
    mstxRangeEnd(id_2);  // 使用mstxRangeEnd接口标识step结束
    // CHECK_ACL(aclrtFree(z)); // 这里不释放模拟HAL内存泄漏
    CHECK_ACL(aclrtFree(x));
    CHECK_ACL(aclrtFree(y));
    CHECK_ACL(aclrtDestroyStream(stream));
    CHECK_ACL(aclrtDestroyContext(context));
    mstxRangeEnd(id_1);
    CHECK_ACL(aclrtResetDevice(deviceId));
    CHECK_ACL(aclFinalize());
    return 0;
}
