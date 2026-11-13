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

#include "utility/ustring.h"

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <iterator>

using namespace Utility;

TEST(UString, join_empty_list_expect_empty_string)
{
    std::vector<std::string> items;
    std::string joined = Join(items.cbegin(), items.cend());
    ASSERT_TRUE(joined.empty());
}

TEST(UString, join_non_empty_list_expect_correct_string)
{
    std::vector<std::string> items = {
        "aaa",
        "bbb",
        "ccc"
    };
    std::string joined = Join(items.cbegin(), items.cend(), "::");
    ASSERT_EQ(joined, "aaa::bbb::ccc");
}

TEST(UString, strip_empty_string_expect_equal_to_self)
{
    std::string str = "";
    ASSERT_EQ(Strip(str), str);
}

TEST(UString, strip_string_with_target_expect_correct_string)
{
    std::string str = "::abc::";
    ASSERT_EQ(Strip(str, ":"), "abc");
}

TEST(UString, strip_string_without_target_expect_equal_to_self)
{
    std::string str = "abc::def";
    ASSERT_EQ(Strip(str, ":"), str);
}

TEST(UString, strip_string_twice_expect_equal_to_strip_once)
{
    std::string str = "::abc::";
    ASSERT_EQ(Strip(str, ":"), Strip(Strip(str, ":"), ":"));
}

TEST(UString, split_empty_string_expect_list_of_one_empty_string)
{
    std::vector<std::string> items;
    Split("", std::back_inserter(items));
    ASSERT_EQ(items.size(), 1UL);
    ASSERT_EQ(items[0], "");
}

TEST(UString, split_string_without_delims_expect_list_of_one_string)
{
    std::vector<std::string> items;
    Split("aaa", std::back_inserter(items));
    ASSERT_EQ(items.size(), 1UL);
    ASSERT_EQ(items[0], "aaa");
}

TEST(UString, split_string_start_with_delims_expect_list_start_with_empty_string)
{
    std::vector<std::string> items;
    Split(":aaa", std::back_inserter(items), ":");
    ASSERT_EQ(items.size(), 2UL);
    ASSERT_EQ(items[0], "");
    ASSERT_EQ(items[1], "aaa");
}

TEST(UString, split_string_end_with_delims_expect_list_end_with_empty_string)
{
    std::vector<std::string> items;
    Split("aaa:", std::back_inserter(items), ":");
    ASSERT_EQ(items.size(), 2UL);
    ASSERT_EQ(items[0], "aaa");
    ASSERT_EQ(items[1], "");
}

TEST(UString, split_string_with_muti_delims_test_strict_except_correct_list)
{
    std::vector<std::string> items;
    Split("aaa::bbb", std::back_inserter(items), ":", true);
    ASSERT_EQ(items.size(), 3UL);
    ASSERT_EQ(items[0], "aaa");
    ASSERT_EQ(items[1], "");
    ASSERT_EQ(items[2], "bbb");
}

TEST(UString, split_string_with_several_delims_expect_correct_list)
{
    std::vector<std::string> items;
    Split(":aaa:bbb:ccc:", std::back_inserter(items), ":");
    ASSERT_EQ(items.size(), 5UL);
    ASSERT_EQ(items[0], "");
    ASSERT_EQ(items[1], "aaa");
    ASSERT_EQ(items[2], "bbb");
    ASSERT_EQ(items[3], "ccc");
    ASSERT_EQ(items[4], "");
}

TEST(UString, extract_attr_value_by_key_expect_correct_value)
{
    std::string attrKey = "size";
    std::string str = "{addr:1000,size:123,owner:,MID:3}";
    std::string attrValue = ExtractAttrValueByKey(str, attrKey);
    ASSERT_EQ(attrValue, "123");
}

TEST(UString, extract_attr_value_by_key_expect_empty_value)
{
    std::string attrKey = "size";
    std::string str = "test";
    std::string attrValue = ExtractAttrValueByKey(str, attrKey);
    ASSERT_EQ(attrValue, "");
}

TEST(VersionClass, compare_version_test)
{
    Version ver1("3.11");
    Version ver2("3");
    Version ver3("4");
    EXPECT_TRUE(ver1 > ver2);
    EXPECT_TRUE(ver1 >= ver2);
    EXPECT_FALSE(ver1 == ver2);
    EXPECT_TRUE(ver2 <= ver3);
    EXPECT_TRUE(ver2 < ver3);
}
 
TEST(ToSafeStringTest, log_safe_string_test)
{
    std::string testStr = "test\t1";
    ToSafeString(testStr);
    ASSERT_EQ(testStr, "test\\t1");

    testStr = "test1";
    ToSafeString(testStr);
    ASSERT_EQ(testStr, "test1");
}

TEST(UString, extract_attr_value_by_key_prefix_without_colon)
{
    // key 首次出现仅为其他 key 的前缀，应继续向后查找真正带冒号的键
    std::string str = "{nameX,y=1,name:ut_value}";
    EXPECT_EQ(ExtractAttrValueByKey(str, "name"), "ut_value");

    // key 出现在串尾且后面没有冒号，循环应正常结束并返回空
    std::string tail = "{addr:1000,name";
    EXPECT_EQ(ExtractAttrValueByKey(tail, "name"), "");

    // 命中冒号但后面紧跟 '}'，值为空
    std::string emptyValue = "{name:}";
    EXPECT_EQ(ExtractAttrValueByKey(emptyValue, "name"), "");
}

TEST(VersionClass, compare_size_when_common_prefix_equal)
{
    Version shortVer("3");
    Version longVer("3.11");
    // 公共前缀相同，按段数比较
    EXPECT_TRUE(shortVer < longVer);
    EXPECT_FALSE(longVer < shortVer);
    EXPECT_TRUE(longVer > shortVer);
    EXPECT_FALSE(shortVer > longVer);
    EXPECT_TRUE(shortVer <= longVer);
    EXPECT_TRUE(longVer >= shortVer);
}

TEST(ToSafeStringTest, all_invalid_chars_escaped)
{
    std::string testStr = "a\nb\fc\rd\be\tf\vg\x7Fh";
    ToSafeString(testStr);
    ASSERT_EQ(testStr, "a\\nb\\fc\\rd\\be\\tf\\vg\\u007Fh");
}
