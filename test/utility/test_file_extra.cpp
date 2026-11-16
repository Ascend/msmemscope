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
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <gtest/gtest.h>

#include "file.h"
#include "utility/path.h"
#include "utility/file_write_manager.h"

using namespace Utility;

namespace {
// 恢复工作目录的 RAII 守卫：用例会删除当前工作目录以构造 getcwd 失败场景
class CwdGuard {
public:
    CwdGuard()
    {
        cwdOk_ = getcwd(original_, sizeof(original_)) != nullptr;
    }
    ~CwdGuard()
    {
        if (cwdOk_) {
            (void)chdir(original_);
        }
    }
    bool IsValid() const { return cwdOk_; }
private:
    char original_[PATH_MAX] = {0};
    bool cwdOk_ = false;
};

// 创建唯一的临时目录，成功返回绝对路径，失败返回空串
std::string CreateTempDir(const char* prefix)
{
    std::string tmpl = std::string("/tmp/") + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = mkdtemp(buf.data());
    if (dir == nullptr) {
        return std::string();
    }
    return std::string(dir);
}
}  // namespace

TEST(FileExtraTest, set_dir_path_normal_expect_absolute_dir)
{
    std::string dirPath;
    SetDirPath(dirPath, ".");
    ASSERT_FALSE(dirPath.empty());
    EXPECT_EQ(dirPath[0], '/');
}

// 当前工作目录被删除后 getcwd 失败，SetDirPath 应保留空路径不写入。
// 默认值必须传绝对路径：相对路径在 Path::Absolute 失败后与空cwd合并时，错误标记会随
// 右操作数被丢弃（见 path.cpp 中 operator/ 的 absolute_ 分支），无法触发提前返回
TEST(FileExtraTest, set_dir_path_getcwd_failed_expect_empty)
{
    CwdGuard guard;
    ASSERT_TRUE(guard.IsValid());
    const std::string deletedDir = "./msms_file_extra_deleted_cwd";
    ASSERT_TRUE(MakeDir(deletedDir));
    // chdir 前先取绝对路径，删除当前工作目录使后续 getcwd 失败
    std::string absDir = Utility::Path(deletedDir).Absolute().ToString();
    ASSERT_EQ(chdir(absDir.c_str()), 0);
    ASSERT_EQ(rmdir(absDir.c_str()), 0);

    std::string dirPath;
    SetDirPath(dirPath, absDir);
    EXPECT_TRUE(dirPath.empty());
}

// 父路径为悬空软链接时，mkdir 失败应返回 false
TEST(FileExtraTest, make_dir_dangling_symlink_expect_false)
{
    std::string baseDir = CreateTempDir("msms_file_extra_link_");
    ASSERT_FALSE(baseDir.empty());
    std::string linkPath = baseDir + "/ut_dangling";
    ASSERT_EQ(symlink("./ut_missing_target", linkPath.c_str()), 0);

    EXPECT_FALSE(MakeDir(linkPath + "/sub"));

    ASSERT_EQ(unlink(linkPath.c_str()), 0);
    ASSERT_EQ(rmdir(baseDir.c_str()), 0);
}

// 父路径是普通文件时，末级 mkdir 返回 ENOTDIR，应返回 false
TEST(FileExtraTest, make_dir_under_regular_file_expect_false)
{
    std::string baseDir = CreateTempDir("msms_file_extra_file_");
    ASSERT_FALSE(baseDir.empty());
    std::string blocker = baseDir + "/ut_blocker_file";
    FILE* fp = fopen(blocker.c_str(), "w");
    ASSERT_NE(fp, nullptr);
    ASSERT_EQ(fclose(fp), 0);

    EXPECT_FALSE(MakeDir(blocker + "/sub"));

    ASSERT_EQ(unlink(blocker.c_str()), 0);
    ASSERT_EQ(rmdir(baseDir.c_str()), 0);
}

// 以只读方式打开的流写入失败，fprintf 返回负值
TEST(FileExtraTest, fprintf_read_only_file_expect_false)
{
    char tmpl[] = "/tmp/msms_file_extra_fprintf_XXXXXX";
    int fd = mkstemp(tmpl);
    ASSERT_NE(fd, -1);
    ASSERT_EQ(close(fd), 0);

    FILE* fp = fopen(tmpl, "r");
    ASSERT_NE(fp, nullptr);
    EXPECT_FALSE(Fprintf(fp, "%s", "data"));
    ASSERT_EQ(fclose(fp), 0);
    ASSERT_EQ(unlink(tmpl), 0);
}

TEST(FileExtraTest, fprintf_normal_file_expect_true)
{
    char tmpl[] = "/tmp/msms_file_extra_fprintf_ok_XXXXXX";
    int fd = mkstemp(tmpl);
    ASSERT_NE(fd, -1);
    FILE* fp = fdopen(fd, "w");
    ASSERT_NE(fp, nullptr);
    EXPECT_TRUE(Fprintf(fp, "%s-%d", "value", 1));
    ASSERT_EQ(fclose(fp), 0);
    ASSERT_EQ(unlink(tmpl), 0);
}

TEST(FileExtraTest, write_manager_lock_stable_per_device)
{
    Utility::FileWriteManager& manager = Utility::FileWriteManager::GetInstance();
    EXPECT_EQ(&manager, &Utility::FileWriteManager::GetInstance());

    std::mutex& lock0a = manager.GetLock(0);
    std::mutex& lock0b = manager.GetLock(0);
    std::mutex& lock1 = manager.GetLock(1);
    EXPECT_EQ(&lock0a, &lock0b);
    EXPECT_NE(&lock0a, &lock1);

    // 获取到的锁可正常加解锁
    {
        std::lock_guard<std::mutex> guard(lock0a);
    }
    EXPECT_TRUE(lock0a.try_lock());
    lock0a.unlock();
}
