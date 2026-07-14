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

extern "C" void test_kernel_do(uint32_t blockDim, void *l2ctrl, void *stream, uint8_t *gm);

int main(void)
{
    CHECK_ACL(aclInit(nullptr));
    aclrtContext context;
    int32_t deviceId = 0;
    CHECK_ACL(aclrtSetDevice(deviceId));
    CHECK_ACL(aclrtCreateContext(&context, deviceId));
    aclrtStream stream = nullptr;
    CHECK_ACL(aclrtCreateStream(&stream));

    uint8_t *gm = nullptr;
    CHECK_ACL(aclrtMalloc((void **)&gm, 256, ACL_MEM_MALLOC_HUGE_FIRST));

    uint64_t blockDim = 1UL;

    uint64_t id_1 = mstxRangeStartA("step start", nullptr);  // 使用"step start"的mstxRangeStartA接口标识step开始
    uint64_t id_2 = mstxRangeStartA("step start", nullptr);  // 这里仅作模拟，请按实际情况打点

    test_kernel_do(blockDim, nullptr, stream, gm);
    CHECK_ACL(aclrtSynchronizeStream(stream));
    mstxRangeEnd(id_2);  // 使用mstxRangeEnd接口标识step结束
    // CHECK_ACL(aclrtFree(gm)); // 这里不释放模拟HAL内存泄漏
    CHECK_ACL(aclrtDestroyStream(stream));
    CHECK_ACL(aclrtDestroyContext(context));
    mstxRangeEnd(id_1);
    CHECK_ACL(aclrtResetDevice(deviceId));
    CHECK_ACL(aclFinalize());
    return 0;
}
