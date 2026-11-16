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

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

#include "vallina_symbol.h"

using namespace MemScope;

namespace {
// 保存/恢复ASCEND_HOME_PATH，保证用例不影响进程内其他测试
class AscendHomeGuard {
public:
    AscendHomeGuard() : had_(false)
    {
        const char *value = std::getenv("ASCEND_HOME_PATH");
        if (value != nullptr) {
            had_ = true;
            value_ = value;
        }
    }
    ~AscendHomeGuard()
    {
        if (had_) {
            setenv("ASCEND_HOME_PATH", value_.c_str(), 1);
        } else {
            unsetenv("ASCEND_HOME_PATH");
        }
    }
    AscendHomeGuard(const AscendHomeGuard &) = delete;
    AscendHomeGuard &operator=(const AscendHomeGuard &) = delete;

private:
    bool had_;
    std::string value_;
};
}  // namespace

TEST(VallinaSymbolTEST, lib_load_get_empty_expect_failed)
{
    std::string emptyLibName = "";
    EXPECT_EQ(MemScope::LibLoad(emptyLibName), nullptr);
}

// 未设置ASCEND_HOME_PATH时打印告警并按裸库名直接加载
TEST(VallinaSymbolTEST, lib_load_without_ascend_home_path_uses_direct_load)
{
    AscendHomeGuard guard;
    ASSERT_EQ(unsetenv("ASCEND_HOME_PATH"), 0);

    int savedStdout = dup(STDOUT_FILENO);
    ASSERT_NE(savedStdout, -1);
    char tmpl[] = "/tmp/msms_val_sym_XXXXXX";
    int fd = mkstemp(tmpl);
    ASSERT_NE(fd, -1);
    ASSERT_NE(dup2(fd, STDOUT_FILENO), -1);
    ASSERT_EQ(close(fd), 0);

    void *handle = MemScope::LibLoad("libut_missing.so");
    std::fflush(stdout);

    ASSERT_NE(dup2(savedStdout, STDOUT_FILENO), -1);
    ASSERT_EQ(close(savedStdout), 0);

    std::ifstream in(tmpl);
    std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    ASSERT_EQ(unlink(tmpl), 0);

    EXPECT_NE(content.find("Failed to acquire ASCEND_HOME_PATH"), std::string::npos);
    EXPECT_NE(content.find("libut_missing.so"), std::string::npos);
    // 直接加载分支返回dlopen结果（测试桩环境下非空）
    EXPECT_NE(handle, nullptr);
}