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

#include <cstdint>
#include <string>
#include <vector>
#include <gtest/gtest.h>

#include "utility/calculate_data_check_sum.h"

namespace {
constexpr size_t CHECKSUM_HEX_LEN = 16;
}

// 空数据：初始值与异或输出均为全1，结果应为全0
TEST(CalculateDataCheckSum64Test, empty_data_expect_all_zero)
{
    std::vector<uint8_t> data;
    std::string checksum = CalculateDataCheckSum64(data);
    EXPECT_EQ(checksum, "0000000000000000");
    EXPECT_EQ(checksum.size(), CHECKSUM_HEX_LEN);
}

// 标准校验数据"123456789"，期望值由独立实现按同一多项式推算得出
TEST(CalculateDataCheckSum64Test, standard_vector_expect_known_value)
{
    const std::string input = "123456789";
    std::vector<uint8_t> data(input.begin(), input.end());
    EXPECT_EQ(CalculateDataCheckSum64(data), "b86883e6fa710a9f");
}

// 单字节数据：不同字节应产生不同校验值，且长度固定为16
TEST(CalculateDataCheckSum64Test, single_byte_values_are_distinct)
{
    std::vector<uint8_t> zero{0x00};
    std::vector<uint8_t> one{0x01};
    std::string zeroSum = CalculateDataCheckSum64(zero);
    std::string oneSum = CalculateDataCheckSum64(one);
    EXPECT_EQ(zeroSum, "eb16d7a787b7faa0");
    EXPECT_EQ(oneSum, "d72daf4f0f6ff541");
    EXPECT_NE(zeroSum, oneSum);
    EXPECT_EQ(zeroSum.size(), CHECKSUM_HEX_LEN);
    EXPECT_EQ(oneSum.size(), CHECKSUM_HEX_LEN);
}

// 多次调用结果稳定（覆盖查找表已初始化后的提前返回分支）
TEST(CalculateDataCheckSum64Test, repeated_call_is_stable)
{
    std::vector<uint8_t> data{0xDE, 0xAD, 0xBE, 0xEF};
    std::string first = CalculateDataCheckSum64(data);
    std::string second = CalculateDataCheckSum64(data);
    EXPECT_EQ(first, "997205fda63e7f1a");
    EXPECT_EQ(first, second);
    // 追加一个字节后校验值必须变化
    data.push_back(0x00);
    EXPECT_NE(CalculateDataCheckSum64(data), first);
}
