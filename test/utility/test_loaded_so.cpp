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

#include <fstream>
#include <string>
#include <gtest/gtest.h>

#include "utility/loaded_so.h"

namespace {
// 从 /proc/self/maps 中取一个必然存在的映射名，用于构造"已加载"场景
std::string PickLoadedMappingName()
{
    std::ifstream mapsFile("/proc/self/maps");
    std::string line;
    while (std::getline(mapsFile, line)) {
        size_t slashPos = line.find('/');
        if (slashPos == std::string::npos) {
            continue;
        }
        std::string fullPath = line.substr(slashPos);
        size_t namePos = fullPath.rfind('/');
        if (namePos != std::string::npos && namePos + 1 < fullPath.size()) {
            return fullPath.substr(namePos + 1);
        }
    }
    // 常规 Linux 进程均存在 vdso 映射
    return "[vdso]";
}
}  // namespace

TEST(LoadedSoTest, host_hook_so_name)
{
    ASSERT_NE(MemScope::HOST_HOOK_SO_NAME, nullptr);
    // 钩子文件名是部署契约的一部分：LD_PRELOAD 依赖精确名字
    EXPECT_STREQ(MemScope::HOST_HOOK_SO_NAME, "libmsmemscope_host_mem_hook.so");
}

TEST(LoadedSoTest, loaded_mapping_expect_true)
{
    std::string loadedName = PickLoadedMappingName();
    ASSERT_FALSE(loadedName.empty());
    EXPECT_TRUE(MemScope::IsSoLoaded(loadedName.c_str()));
}

TEST(LoadedSoTest, absent_so_expect_false)
{
    EXPECT_FALSE(MemScope::IsSoLoaded("libut_definitely_absent_msmemscope.so"));
}
