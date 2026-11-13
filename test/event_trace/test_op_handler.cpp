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
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "utility/cpython.h"

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

// ============================================================================
// 隔离子进程用例
// TriggerKernelLaunch 依赖 Python 解释器以及 torch_npu 侧 sanitizer 模块：
//  - Handle 的“解释器未初始化”早退分支要求进程内确实没有解释器；
//  - 其余分支需要解释器已初始化，且导入结果必须可控（真机上 torch_npu 是否存在、
//    其 event_handler 是否就绪都不确定）。
// 因此这两类路径都改由 fork+exec 拉起本测试二进制的子进程执行，--gtest_filter
// 保证子进程只跑目标用例；子进程内先向 sys.modules 注入假 torch_npu/torch 顶层模块，
// 阻断真实第三方包的导入，再按阶段构造 sanitizer 模块的不同残缺形态。
// 假 _handle_kernel_launch 把收到的参数写入标记文件，供主线程侧断言参数个数与内容。
// ============================================================================
bool RunMemScopeTestChild(const char *gtestFilter);

namespace
{
constexpr const char *OP_HANDLER_MARKER = "msmemscope_ut_op_handler_marker.txt";
constexpr const char *OP_MSG_WITH_ACCESS =
    "name=my_op;read=ra:0x1000:4,rb:0x2000:8;write=ra:0x1000:4,wc:0x3000:12";

void RemoveOpMarker()
{
    std::remove(OP_HANDLER_MARKER);
}

bool OpMarkerExists()
{
    return access(OP_HANDLER_MARKER, F_OK) == 0;
}

std::string ReadOpMarker()
{
    std::ifstream ifs(OP_HANDLER_MARKER);
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

std::string OpMarkerFirstLine()
{
    std::ifstream ifs(OP_HANDLER_MARKER);
    std::string line;
    std::getline(ifs, line);
    return line;
}

// 在主线程已持GIL的前提下，嵌套获取GIL是幂等的；统一封装便于失败时清理错误指示器
bool RunPythonCode(const char *code)
{
    PyGILState_STATE state = PyGILState_Ensure();
    const int rc = PyRun_SimpleString(code);
    PyGILState_Release(state);
    return rc == 0;
}

void ClearPythonError()
{
    PyGILState_STATE state = PyGILState_Ensure();
    PyErr_Clear();
    PyGILState_Release(state);
}

void ExpectMarkerArgs(const std::string &expectedCount, const char *message)
{
    ASSERT_TRUE(OpMarkerExists()) << message;
    EXPECT_EQ(OpMarkerFirstLine(), expectedCount) << message;
}
}  // namespace

// 子进程用例：解释器未初始化时，Handle 完成消息解析后 TriggerKernelLaunch 直接返回
TEST(OpHandlerChild, NoPythonInterpreter)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    // 子进程仅执行本用例，未调用 Py_Initialize：保证走的是解释器未初始化分支
    ASSERT_FALSE(Utility::IsPyInterpRepeInited());

    SanitizerOpHandler &handler = SanitizerOpHandler::GetInstance();
    SanitizerOpHandler::SetEnabled(true);
    EXPECT_TRUE(SanitizerOpHandler::IsEnabled());
    // 使能且 name/read/write 均合法：解析完成后 TriggerKernelLaunch 早退
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    SanitizerOpHandler::SetEnabled(false);
    EXPECT_FALSE(SanitizerOpHandler::IsEnabled());
}

// 子进程用例：解释器初始化后的 TriggerKernelLaunch 全分支
TEST(OpHandlerChild, FullPythonPath)
{
    if (getenv("MSMEMSCOPE_UT_CHILD") == nullptr)
    {
        GTEST_SKIP() << "仅在RunMemScopeTestChild拉起的子进程中执行";
    }
    RemoveOpMarker();
    Py_Initialize();
    ASSERT_NE(Py_IsInitialized(), 0);
    ASSERT_TRUE(Utility::IsPyInterpRepeInited());

    SanitizerOpHandler &handler = SanitizerOpHandler::GetInstance();
    SanitizerOpHandler::SetEnabled(true);

    // 注入假顶层模块，阻断真实 torch_npu/torch 的导入；此时叶子模块均不存在
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys, types
sys.modules['torch_npu'] = types.ModuleType('torch_npu')
sys.modules['torch'] = types.ModuleType('torch')
)PY"));

    // 阶段1：sanitizer 模块导入失败
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    EXPECT_FALSE(OpMarkerExists());

    // 阶段2：sanitizer 存在，但 _stream_check 导入失败
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys, types
m = types.ModuleType('torch_npu.npu._sanitizer')
m.npu_sanitizer = object()
sys.modules['torch_npu.npu._sanitizer'] = m
sys.modules.pop('torch_npu.npu._stream_check', None)
)PY"));
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    EXPECT_FALSE(OpMarkerExists());

    // 阶段3：两个模块都在，但缺少 npu_sanitizer 实例
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys, types
san = sys.modules['torch_npu.npu._sanitizer']
if hasattr(san, 'npu_sanitizer'):
    del san.npu_sanitizer
sys.modules['torch_npu.npu._stream_check'] = types.ModuleType('torch_npu.npu._stream_check')
)PY"));
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    EXPECT_FALSE(OpMarkerExists());

    // 阶段4：npu_sanitizer 存在，但缺少 event_handler
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys, types
san = sys.modules['torch_npu.npu._sanitizer']
san.npu_sanitizer = types.SimpleNamespace()
)PY"));
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    EXPECT_FALSE(OpMarkerExists());

    // 阶段5：event_handler 存在，但缺少 _handle_kernel_launch 方法
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys, types
san = sys.modules['torch_npu.npu._sanitizer']
san.npu_sanitizer = types.SimpleNamespace(event_handler=types.SimpleNamespace())
)PY"));
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    EXPECT_FALSE(OpMarkerExists());

    // 阶段6：完整 handler；_stream_check 无 NPURecordStreamHandler（旧版本，6参数）
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys, types
san = sys.modules['torch_npu.npu._sanitizer']
def _handle_v1(*args):
    with open('msmemscope_ut_op_handler_marker.txt', 'w') as f:
        f.write('%d\n' % len(args))
        f.write(repr(args))
    return []
san.npu_sanitizer = types.SimpleNamespace(
    event_handler=types.SimpleNamespace(_handle_kernel_launch=_handle_v1))
sc = sys.modules['torch_npu.npu._stream_check']
if hasattr(sc, 'NPURecordStreamHandler'):
    del sc.NPURecordStreamHandler
)PY"));
    RemoveOpMarker();
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    ExpectMarkerArgs("6", "旧版本 _handle_kernel_launch 应收到6个参数");
    {
        const std::string content = ReadOpMarker();
        // 参数0为stream；operator名与read/write解析出的别名、地址都应进入参数
        EXPECT_NE(content.find("(11, "), std::string::npos);
        EXPECT_NE(content.find("'my_op'"), std::string::npos);
        EXPECT_NE(content.find("'ra'"), std::string::npos);
        EXPECT_NE(content.find("'rb'"), std::string::npos);
        EXPECT_NE(content.find("'wc'"), std::string::npos);
        EXPECT_NE(content.find("8192"), std::string::npos);   // read_only 独有地址
        EXPECT_NE(content.find("4096"), std::string::npos);   // read+write 同一地址
        EXPECT_NE(content.find("12288"), std::string::npos);  // write 独有地址
    }

    // 不含 read/write 字段：两个列表为空，仍构造6参数并调用
    RemoveOpMarker();
    handler.Handle("name=no_access", 7);
    ExpectMarkerArgs("6", "缺少read/write时应构造空列表");
    {
        const std::string content = ReadOpMarker();
        EXPECT_NE(content.find("(7, set(), set(), set(), 'no_access', {})"), std::string::npos);
    }

    // 阶段7：_stream_check 有 NPURecordStreamHandler（新版本，7参数），返回 None
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys, types
sc = sys.modules['torch_npu.npu._stream_check']
sc.NPURecordStreamHandler = type('NPURecordStreamHandler', (), {})
san = sys.modules['torch_npu.npu._sanitizer']
def _handle_v2(*args):
    with open('msmemscope_ut_op_handler_marker.txt', 'w') as f:
        f.write('%d\n' % len(args))
        f.write(repr(args))
    return None
san.npu_sanitizer.event_handler._handle_kernel_launch = _handle_v2
)PY"));
    RemoveOpMarker();
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    ExpectMarkerArgs("7", "新版本 _handle_kernel_launch 应收到7个参数");
    EXPECT_NE(ReadOpMarker().find("'my_op'"), std::string::npos);

    // 阶段8：返回非空错误列表，torch.cuda._sanitizer 不可导入（打印错误后返回）
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys, types
sys.modules.pop('torch.cuda._sanitizer', None)
san = sys.modules['torch_npu.npu._sanitizer']
def _handle_err(*args):
    with open('msmemscope_ut_op_handler_marker.txt', 'w') as f:
        f.write('%d\n' % len(args))
        f.write(repr(args))
    return ['err_one', 'err_two']
san.npu_sanitizer.event_handler._handle_kernel_launch = _handle_err
)PY"));
    RemoveOpMarker();
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    ExpectMarkerArgs("7", "返回错误列表前应完成调用");

    // 阶段9：torch.cuda._sanitizer 存在但缺少 CUDASanitizerErrors 类
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys, types
sys.modules['torch.cuda._sanitizer'] = types.ModuleType('torch.cuda._sanitizer')
)PY"));
    RemoveOpMarker();
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    ExpectMarkerArgs("7", "缺少异常类时应仅告警后返回");

    // 阶段10：异常类存在，构造实例并置位Python错误指示器
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys
m = sys.modules['torch.cuda._sanitizer']
m.CUDASanitizerErrors = type('CUDASanitizerErrors', (Exception,), {})
)PY"));
    RemoveOpMarker();
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    ExpectMarkerArgs("7", "返回错误列表时应尝试抛出CUDASanitizerErrors");
    // 用例侧清理业务代码有意置位的异常，避免影响后续Python调用
    ClearPythonError();

    // 阶段11：_handle_kernel_launch 自身抛异常 → Call 返回无效对象，仅告警后返回
    ASSERT_TRUE(RunPythonCode(R"PY(
import sys
san = sys.modules['torch_npu.npu._sanitizer']
def _handle_boom(*args):
    raise RuntimeError('boom')
san.npu_sanitizer.event_handler._handle_kernel_launch = _handle_boom
)PY"));
    RemoveOpMarker();
    handler.Handle(OP_MSG_WITH_ACCESS, 11);
    EXPECT_FALSE(OpMarkerExists());

    SanitizerOpHandler::SetEnabled(false);
    RemoveOpMarker();
}

// 父进程驱动用例：拉起隔离子进程并断言其正常退出（退出码非0说明子用例失败）
TEST(OpHandlerIsolation, NoPythonInterpreterChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("OpHandlerChild.NoPythonInterpreter"));
}

TEST(OpHandlerIsolation, FullPythonPathChildProcess)
{
    EXPECT_TRUE(RunMemScopeTestChild("OpHandlerChild.FullPythonPath"));
}
