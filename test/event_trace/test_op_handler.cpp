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
#include <cstring>

#define private public
#include "op_handler.h"
#undef private

using namespace MemScope;

class OpHandlerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        SanitizerOpHandler::SetEnabled(false);
    }

    void TearDown() override
    {
        SanitizerOpHandler::SetEnabled(false);
    }
};

// ============================================================================
// ExtractField 测试
// ============================================================================

TEST_F(OpHandlerTest, ExtractField_ValidKeyValue_ReturnsTrue)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "name=add;read=tensor:0x1000:4;write=tensor:0x2000:8";

    EXPECT_TRUE(handler.ExtractField(msg, "name", value));
    EXPECT_EQ(value, "add");
}

TEST_F(OpHandlerTest, ExtractField_KeyNotFound_ReturnsFalse)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "name=add;read=tensor:0x1000:4";

    EXPECT_FALSE(handler.ExtractField(msg, "nonexistent", value));
    EXPECT_TRUE(value.empty());
}

TEST_F(OpHandlerTest, ExtractField_ValueAtEndWithoutSemicolon_ReturnsTrue)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "name=matmul";

    EXPECT_TRUE(handler.ExtractField(msg, "name", value));
    EXPECT_EQ(value, "matmul");
}

TEST_F(OpHandlerTest, ExtractField_EmptyMessage_ReturnsFalse)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "";

    EXPECT_FALSE(handler.ExtractField(msg, "name", value));
}

TEST_F(OpHandlerTest, ExtractField_LastFieldInMessage_ReturnsTrue)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "name=add;read=t0:0x1000:4;write=t1:0x2000:8";

    EXPECT_TRUE(handler.ExtractField(msg, "write", value));
    EXPECT_EQ(value, "t1:0x2000:8");
}

TEST_F(OpHandlerTest, ExtractField_KeyPrefixOfAnother_ExtractsExactMatch)
{
    // "read" 是 "read_only" 的前缀，应精确匹配 "read=" 而非 "read_only="
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "read=t0:0x1000:4;read_only=t1:0x2000:8";

    EXPECT_TRUE(handler.ExtractField(msg, "read", value));
    EXPECT_EQ(value, "t0:0x1000:4");
}

TEST_F(OpHandlerTest, ExtractField_ValueContainsEquals_ReturnsTrue)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "name=add;extra=key=val";

    EXPECT_TRUE(handler.ExtractField(msg, "extra", value));
    EXPECT_EQ(value, "key=val");
}

TEST_F(OpHandlerTest, ExtractField_EmptyValue_ReturnsTrue)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "name=;read=t0:0x1000:4";

    EXPECT_TRUE(handler.ExtractField(msg, "name", value));
    EXPECT_EQ(value, "");
}

TEST_F(OpHandlerTest, ExtractField_SemicolonOnlySeparator_NoMatchForPartial)
{
    // key=value 以分号分隔，不以其他字符分隔
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "name=add,read=t0:0x1000:4";

    EXPECT_TRUE(handler.ExtractField(msg, "name", value));
    EXPECT_EQ(value, "add,read=t0:0x1000:4");
}

TEST_F(OpHandlerTest, ExtractField_KeyAtStartOfMessage_ReturnsTrue)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "name=add;read=t0:0x1000:4;write=t1:0x2000:8";

    EXPECT_TRUE(handler.ExtractField(msg, "name", value));
    EXPECT_EQ(value, "add");
}

TEST_F(OpHandlerTest, ExtractField_KeyNotFoundButSimilarKeyExists_ReturnsFalse)
{
    // "op" 不存在，但 "operation" 存在，应返回 false
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    std::string value;
    const char* msg = "operation=add";

    EXPECT_FALSE(handler.ExtractField(msg, "op", value));
}

// ============================================================================
// ParseAccessItem 测试
// ============================================================================

TEST_F(OpHandlerTest, ParseAccessItem_ValidFormatDecimal_ReturnsTrue)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    EXPECT_TRUE(handler.ParseAccessItem("tensor_a:4096:128", item));
    EXPECT_STREQ(item.alias, "tensor_a");
    EXPECT_EQ(item.ptr, 4096u);
    EXPECT_EQ(item.size, 128u);
}

TEST_F(OpHandlerTest, ParseAccessItem_ValidFormatHex_ReturnsTrue)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    EXPECT_TRUE(handler.ParseAccessItem("t0:0x1000:0x200", item));
    EXPECT_STREQ(item.alias, "t0");
    EXPECT_EQ(item.ptr, 0x1000u);
    EXPECT_EQ(item.size, 0x200u);
}

TEST_F(OpHandlerTest, ParseAccessItem_MissingFirstColon_ReturnsFalse)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    EXPECT_FALSE(handler.ParseAccessItem("tensor_addr_size", item));
}

TEST_F(OpHandlerTest, ParseAccessItem_MissingSecondColon_ReturnsFalse)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    EXPECT_FALSE(handler.ParseAccessItem("tensor:0x1000", item));
}

TEST_F(OpHandlerTest, ParseAccessItem_EmptyString_ReturnsFalse)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    EXPECT_FALSE(handler.ParseAccessItem("", item));
}

TEST_F(OpHandlerTest, ParseAccessItem_EmptyAlias_ReturnsFalse)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    // 第一个冒号在位置0，意味着alias为空
    EXPECT_FALSE(handler.ParseAccessItem(":0x1000:0x200", item));
}

TEST_F(OpHandlerTest, ParseAccessItem_InvalidAddr_ReturnsFalse)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    EXPECT_FALSE(handler.ParseAccessItem("tensor:not_a_number:128", item));
}

TEST_F(OpHandlerTest, ParseAccessItem_InvalidSize_ReturnsFalse)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    EXPECT_FALSE(handler.ParseAccessItem("tensor:0x1000:xyz", item));
}

TEST_F(OpHandlerTest, ParseAccessItem_AliasExactly31Chars_NotTruncated)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    std::string alias31(31, 'a');
    std::string input = alias31 + ":4096:128";

    EXPECT_TRUE(handler.ParseAccessItem(input, item));
    EXPECT_EQ(std::strlen(item.alias), 31u);
    EXPECT_EQ(std::string(item.alias), alias31);
}

TEST_F(OpHandlerTest, ParseAccessItem_AliasLongerThan31Chars_Truncated)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    std::string alias40(40, 'b');
    std::string input = alias40 + ":4096:128";

    EXPECT_TRUE(handler.ParseAccessItem(input, item));
    EXPECT_EQ(std::strlen(item.alias), 31u);
    EXPECT_EQ(std::string(item.alias), std::string(31, 'b'));
}

TEST_F(OpHandlerTest, ParseAccessItem_SecondColonAtEnd_ReturnsFalse)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    // secondColon == item.length() - 1 即 size 为空
    EXPECT_FALSE(handler.ParseAccessItem("tensor:4096:", item));
}

TEST_F(OpHandlerTest, ParseAccessItem_MultipleColons_UsesFirstTwo)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    // alias = "a", addr = "b", size = "c:d"
    EXPECT_FALSE(handler.ParseAccessItem("a:b:c:d", item));
}

TEST_F(OpHandlerTest, ParseAccessItem_AliasWithSpecialChars_Accepted)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    MemoryAccessItem item;
    std::memset(&item, 0, sizeof(item));

    EXPECT_TRUE(handler.ParseAccessItem("tensor_0.attr:123:456", item));
    EXPECT_STREQ(item.alias, "tensor_0.attr");
    EXPECT_EQ(item.ptr, 123u);
    EXPECT_EQ(item.size, 456u);
}

// ============================================================================
// ParseAccessList 测试
// ============================================================================

TEST_F(OpHandlerTest, ParseAccessList_EmptyString_ReturnsEmpty)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();

    auto result = handler.ParseAccessList("");
    EXPECT_TRUE(result.empty());
}

TEST_F(OpHandlerTest, ParseAccessList_SingleItem_ReturnsOne)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();

    auto result = handler.ParseAccessList("t0:0x1000:4");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_STREQ(result[0].alias, "t0");
    EXPECT_EQ(result[0].ptr, 0x1000u);
    EXPECT_EQ(result[0].size, 4u);
}

TEST_F(OpHandlerTest, ParseAccessList_TwoItems_ReturnsBoth)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();

    auto result = handler.ParseAccessList("t0:0x1000:4,t1:0x2000:8");
    ASSERT_EQ(result.size(), 2u);
    EXPECT_STREQ(result[0].alias, "t0");
    EXPECT_EQ(result[0].ptr, 0x1000u);
    EXPECT_EQ(result[0].size, 4u);
    EXPECT_STREQ(result[1].alias, "t1");
    EXPECT_EQ(result[1].ptr, 0x2000u);
    EXPECT_EQ(result[1].size, 8u);
}

TEST_F(OpHandlerTest, ParseAccessList_MultipleItems_ReturnsAll)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();

    auto result = handler.ParseAccessList("a:4096:128,b:8192:256,c:16384:512");
    ASSERT_EQ(result.size(), 3u);
    EXPECT_STREQ(result[0].alias, "a");
    EXPECT_STREQ(result[1].alias, "b");
    EXPECT_STREQ(result[2].alias, "c");
}

TEST_F(OpHandlerTest, ParseAccessList_MixedValidInvalid_SkipsInvalid)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();

    // 第二个缺少addr:size分隔符，应跳过
    auto result = handler.ParseAccessList("t0:0x1000:4,invalid_item,t2:0x3000:12");
    ASSERT_EQ(result.size(), 2u);
    EXPECT_STREQ(result[0].alias, "t0");
    EXPECT_STREQ(result[1].alias, "t2");
}

TEST_F(OpHandlerTest, ParseAccessList_ItemWithEmptyAlias_Skipped)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();

    auto result = handler.ParseAccessList(":0x1000:4,t1:0x2000:8");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_STREQ(result[0].alias, "t1");
}

TEST_F(OpHandlerTest, ParseAccessList_ItemWithInvalidAddr_Skipped)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();

    auto result = handler.ParseAccessList("t0:invalid:4,t1:0x2000:8");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_STREQ(result[0].alias, "t1");
}

TEST_F(OpHandlerTest, ParseAccessList_AllInvalid_ReturnsEmpty)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();

    auto result = handler.ParseAccessList("bad1,bad2,bad3");
    EXPECT_TRUE(result.empty());
}

// ============================================================================
// GetInstance / Singleton 测试
// ============================================================================

TEST_F(OpHandlerTest, GetInstance_ReturnsSameInstance)
{
    SanitizerOpHandler& inst1 = SanitizerOpHandler::GetInstance();
    SanitizerOpHandler& inst2 = SanitizerOpHandler::GetInstance();

    EXPECT_EQ(&inst1, &inst2);
}

// ============================================================================
// SetEnabled / IsEnabled 测试
// ============================================================================

TEST_F(OpHandlerTest, IsEnabled_DefaultFalse)
{
    EXPECT_FALSE(SanitizerOpHandler::IsEnabled());
}

TEST_F(OpHandlerTest, SetEnabled_True_IsEnabledReturnsTrue)
{
    SanitizerOpHandler::SetEnabled(true);
    EXPECT_TRUE(SanitizerOpHandler::IsEnabled());
}

TEST_F(OpHandlerTest, SetEnabled_False_IsEnabledReturnsFalse)
{
    SanitizerOpHandler::SetEnabled(true);
    SanitizerOpHandler::SetEnabled(false);
    EXPECT_FALSE(SanitizerOpHandler::IsEnabled());
}

// ============================================================================
// Handle 测试 (基本路径，不依赖 Python 解释器)
// ============================================================================

TEST_F(OpHandlerTest, Handle_NullMessage_ReturnsEarly)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    SanitizerOpHandler::SetEnabled(true);

    // 不应崩溃，应直接返回
    handler.Handle(nullptr, 0);
}

TEST_F(OpHandlerTest, Handle_NotEnabled_ReturnsEarly)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    SanitizerOpHandler::SetEnabled(false);

    // 未使能，应直接返回
    handler.Handle("name=add;read=t0:0x1000:4", 0);
}

TEST_F(OpHandlerTest, Handle_MissingNameField_ReturnsEarly)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    SanitizerOpHandler::SetEnabled(true);

    // 缺 name 字段，应直接返回
    handler.Handle("read=t0:0x1000:4;write=t1:0x2000:8", 0);
}

TEST_F(OpHandlerTest, Handle_EmptyNameField_ReturnsEarly)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    SanitizerOpHandler::SetEnabled(true);

    // name 字段为空，应直接返回
    handler.Handle("name=;read=t0:0x1000:4", 0);
}

TEST_F(OpHandlerTest, Handle_EmptyMessage_ReturnsEarly)
{
    SanitizerOpHandler& handler = SanitizerOpHandler::GetInstance();
    SanitizerOpHandler::SetEnabled(true);

    // 空消息中找不到 name，应直接返回
    handler.Handle("", 0);
}
