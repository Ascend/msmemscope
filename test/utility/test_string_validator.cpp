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

#include "utility/string_validator.h"

using namespace Utility;

TEST(StringValidatorTest, split_string_multi_delimiters)
{
    std::vector<std::string> tokens = SplitString("a,b;c", ",;");
    ASSERT_EQ(tokens.size(), 3U);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "b");
    EXPECT_EQ(tokens[2], "c");

    // 连续分隔符会产生空 token
    tokens = SplitString("a,,b", ",");
    ASSERT_EQ(tokens.size(), 3U);
    EXPECT_EQ(tokens[1], "");

    // 无分隔符与末尾分隔符
    tokens = SplitString("abc", ",");
    ASSERT_EQ(tokens.size(), 1U);
    EXPECT_EQ(tokens[0], "abc");
    tokens = SplitString("a,", ",");
    ASSERT_EQ(tokens.size(), 1U);
    EXPECT_EQ(tokens[0], "a");
}

TEST(StringValidatorTest, chinese_character_boundaries)
{
    // U+4E2D “中”：合法基本汉字
    const unsigned char zhong[] = {0xE4, 0xB8, 0xAD};
    EXPECT_TRUE(IsChineseCharacter(zhong));

    // 续字节不符合 10xxxxxx 格式
    const unsigned char badContinuation[] = {0xE4, 0x41, 0x41};
    EXPECT_FALSE(IsChineseCharacter(badContinuation));

    // 起始字节不是三字节前缀
    const unsigned char badLead[] = {0xC3, 0xA9, 0x00};
    EXPECT_FALSE(IsChineseCharacter(badLead));

    // U+4000：低于基本汉字下界
    const unsigned char belowRange[] = {0xE4, 0x80, 0x80};
    EXPECT_FALSE(IsChineseCharacter(belowRange));

    // U+9FA6：高于基本汉字上界
    const unsigned char aboveRange[] = {0xE9, 0xBE, 0xA6};
    EXPECT_FALSE(IsChineseCharacter(aboveRange));
}

TEST(StringValidatorTest, valid_output_path)
{
    EXPECT_TRUE(IsValidOutputPath("valid_dir-1/file.txt~"));
    // 中文目录 + 中文文件名
    EXPECT_TRUE(IsValidOutputPath("中文目录/文件.txt"));
    // 空路径
    EXPECT_FALSE(IsValidOutputPath(""));
    // 非法 ASCII 字符
    EXPECT_FALSE(IsValidOutputPath("dir:name"));
}

TEST(StringValidatorTest, invalid_output_path_multibyte)
{
    // 以汉字起始字节开头但后续不是合法汉字
    std::string fakeChinese("\xE4\x41\x41", 3);
    EXPECT_FALSE(IsValidOutputPath(fakeChinese));

    // 双字节 UTF-8（é）不在允许范围内
    std::string twoByte("\xC3\xA9", 2);
    EXPECT_FALSE(IsValidOutputPath(twoByte));

    // 基本汉字范围之外的合法三字节字符 U+9FA6
    std::string outOfRange("\xE9\xBE\xA6", 3);
    EXPECT_FALSE(IsValidOutputPath(outOfRange));
}

TEST(StringValidatorTest, data_level)
{
    EXPECT_TRUE(IsValidDataLevel("0"));
    EXPECT_TRUE(IsValidDataLevel("1"));
    EXPECT_TRUE(IsValidDataLevel("op"));
    EXPECT_TRUE(IsValidDataLevel("kernel"));
    EXPECT_FALSE(IsValidDataLevel("2"));
    EXPECT_FALSE(IsValidDataLevel(""));
}

TEST(StringValidatorTest, integer_rule)
{
    EXPECT_FALSE(IsValidInteger(""));
    EXPECT_TRUE(IsValidInteger("123"));
    EXPECT_FALSE(IsValidInteger("abc"));
    EXPECT_FALSE(IsValidInteger("0123"));
    EXPECT_FALSE(IsValidInteger("12a"));

    IntValidateRule allowZero;
    allowZero.allowLeadingZero = true;
    EXPECT_TRUE(IsValidInteger("0123", allowZero));

    IntValidateRule bounded;
    bounded.minValue = 10;
    bounded.maxValue = 100;
    EXPECT_TRUE(IsValidInteger("10", bounded));
    EXPECT_TRUE(IsValidInteger("100", bounded));
    EXPECT_FALSE(IsValidInteger("9", bounded));
    EXPECT_FALSE(IsValidInteger("101", bounded));
}
