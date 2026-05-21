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
#include <gtest/gtest.h>
#include "event_trace/hal_hooks/hal_hooks.h"
#include "event_trace/event_report.h"
#include "bit_field.h"

TEST(HalHooksTest, GetMallocMemTypeTest) {
    unsigned long long flag = 2377900603261207558;
    MemScope::MemOpSpace result = MemScope::GetMemOpSpace(flag);
    EXPECT_EQ(result, MemScope::MemOpSpace::DEVICE);
}


TEST(HalHooksTest, HalMemAllocTest) {
    void* ptr = nullptr;               // 指针初始化为空指针
    unsigned long long size = 1024;    // 分配的大小
    unsigned long long flag = 0x1234;  // 测试 flag

    drvError_t result = halMemAlloc(&ptr, size, flag);
    EXPECT_EQ(result, DRV_ERROR_NOT_SUPPORT);

    halMemFree(ptr);
}

// 3. 测试 halMemFree 函数
TEST(HalHooksTest, HalMemFreeTest) {
    void* ptr = malloc(1024); // 手动分配内存，以便测试 halMemFree

    drvError_t result = halMemFree(ptr);
    EXPECT_EQ(result, DRV_ERROR_NOT_SUPPORT);
}

// 测试新的驱动接口
TEST(HalHooksTest, HalMemCreate)
{
    drv_mem_handle_t* ptr = nullptr;
    drv_mem_prop *prop = nullptr;
    unsigned long long size = 1024;
    unsigned long long flag = 0;
 
    drvError_t result = halMemCreate(&ptr, size, prop, flag);
    EXPECT_EQ(result, DRV_ERROR_RESERVED);
}
 
TEST(HalHooksTest, halMemReleaseInner)
{
    drv_mem_handle_t* ptr = nullptr;
 
    drvError_t result = halMemRelease(ptr);
    EXPECT_EQ(result, DRV_ERROR_RESERVED);
}