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

#include "utility/utils.h"

using namespace Utility;

TEST(Utils, GetAddResult_Overflow_Test)
{
    uint8_t a = 255;
    uint8_t b = 100;
    auto result = GetAddResult(a, b);
    ASSERT_EQ(result, a);
}

TEST(Utils, GetSubResult_underflow_Test)
{
    uint8_t a = 100;
    uint8_t b = 255;
    auto result = GetSubResult(a, b);
    ASSERT_EQ(result, a);
}
TEST(Utils, GetAddResult_underflow_Test)
{
    int8_t a = -100;
    int8_t b = -127;
    auto result = GetAddResult(a, b);
    ASSERT_EQ(result, a);
}
TEST(Utils, GetSubResult_Overflow_Test)
{
    int8_t a = 100;
    int8_t b = -127;
    auto result = GetSubResult(a, b);
    ASSERT_EQ(result, a);
}

TEST(Utils, StrToInt64_Failed_Test)
{
    std::string str = "";
    int64_t dest = 0;
    auto ret = StrToInt64(dest, str);
    EXPECT_FALSE(ret);

    str = "6.0";
    ret = StrToInt64(dest, str);
    EXPECT_FALSE(ret);
}

TEST(Utils, StrToUInt64_Failed_Test)
{
    std::string str = "";
    uint64_t dest = 0;
    auto ret = StrToUint64(dest, str);
    EXPECT_FALSE(ret);

    str = "6.0";
    ret = StrToUint64(dest, str);
    EXPECT_FALSE(ret);
}

TEST(Utils, StrToUInt32_Failed_Test)
{
    std::string str = "";
    uint32_t dest = 0;
    auto ret = StrToUint32(dest, str);
    EXPECT_FALSE(ret);

    str = "6.0";
    ret = StrToUint32(dest, str);
    EXPECT_FALSE(ret);
}

TEST(Utils, StrToInt64_Overflow_Test)
{
    // 超出 int64_t 表示范围，std::stoll 抛异常后被捕获
    std::string str(30, '9');
    int64_t dest = 0;
    EXPECT_FALSE(StrToInt64(dest, str));

    str = "-" + std::string(30, '9');
    EXPECT_FALSE(StrToInt64(dest, str));

    // 合法边界值应成功
    str = "9223372036854775807";
    EXPECT_TRUE(StrToInt64(dest, str));
    EXPECT_EQ(dest, 9223372036854775807LL);
}

TEST(Utils, GetProcessVmRss_Test)
{
    uint64_t vmrss = GetProcessVmRss();
    EXPECT_GT(vmrss, 0);
}
