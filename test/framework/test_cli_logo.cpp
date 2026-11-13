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

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <gtest/gtest.h>

#include "framework/cli_logo.h"

namespace MemScope {
// 内部函数：非tty时返回false，测试据此确定性进入纯文本分支
bool ShouldUseColorLogo();
}

namespace {
constexpr const char* UT_LOGO_TEMP = "/tmp/msms_cli_logo_XXXXXX";
}

// stderr重定向到普通文件后不是tty，PrintLogo应输出无ANSI转义的纯文本横幅
TEST(CliLogoTest, print_logo_plain_text_when_not_tty)
{
    int savedStderr = dup(STDERR_FILENO);
    ASSERT_NE(savedStderr, -1);

    // mkstemp 需要可写的模板缓冲区：从常量拷贝到 std::string（C++11 起连续存储，可写）
    std::string tmpl = UT_LOGO_TEMP;
    int fd = mkstemp(&tmpl[0]);
    ASSERT_NE(fd, -1);
    ASSERT_NE(dup2(fd, STDERR_FILENO), -1);
    ASSERT_EQ(close(fd), 0);

    EXPECT_FALSE(MemScope::ShouldUseColorLogo());
    MemScope::PrintLogo();

    ASSERT_NE(dup2(savedStderr, STDERR_FILENO), -1);
    ASSERT_EQ(close(savedStderr), 0);

    std::ifstream in(tmpl.c_str());
    std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    ASSERT_EQ(unlink(tmpl.c_str()), 0);

    EXPECT_NE(content.find("MindStudio"), std::string::npos);
    EXPECT_NE(content.find("===="), std::string::npos);
    EXPECT_NE(content.find("END-TO-END TOOLCHAIN"), std::string::npos);
    // 纯文本分支不应包含颜色控制序列
    EXPECT_EQ(content.find("\033["), std::string::npos);
}
