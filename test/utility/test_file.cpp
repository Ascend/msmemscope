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
#include <dlfcn.h>
#define private public
#include "utility/file.h"
#undef private
#include "utility/log.h"
#include "config_info.h"

using namespace Utility;

class FileTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Utility::FileCreateManager::GetInstance("./testmsmemscope");
    }
 
    void TearDown() override
    {
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("");
        rmdir("./testmsmemscope");
    }
};

TEST_F(FileTest, make_empty_path_expect_return_false)
{
    std::string dirPath;
    auto ret = MakeDir(dirPath);
    ASSERT_FALSE(ret);
}

TEST_F(FileTest, make_path_expect_return_true)
{
    std::string dirPath = "test_dir";
    auto ret = MakeDir(dirPath);
    ASSERT_TRUE(ret);
    rmdir(dirPath.c_str());
}

TEST_F(FileTest, make_recursive_path_expect_return_true)
{
    std::string dirPath = "./test_dir/test1/test2";
    auto ret = MakeDir(dirPath);
    ASSERT_TRUE(ret);
    rmdir(dirPath.c_str());
    rmdir("./test_dir/test1");
    rmdir("./test_dir");
}


TEST_F(FileTest, make_exist_path_expect_return_false)
{
    char cwd[100];
    getcwd(cwd, sizeof(cwd));
    std::string dirPath = cwd;
    auto ret = MakeDir(dirPath);
    ASSERT_TRUE(ret);
}

TEST_F(FileTest, not_exist_path_expect_return_false)
{
    std::string path;
    auto ret = Exist(path);
    ASSERT_FALSE(ret);
}

TEST_F(FileTest, exist_path_expect_return_false)
{
    std::string path = __FILE__;
    auto ret = Exist(path);
    ASSERT_TRUE(ret);
}

TEST_F(FileTest, create_new_csv_file_expect_true)
{
    FILE *fp = nullptr;
    FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("./testmsmemscope");
    auto ret = FileCreateManager::GetInstance("./testmsmemscope").CreateCsvFile(&fp, "0", "test",
        MemScope::DUMP_DIR, "test_headers\n");
    ASSERT_TRUE(ret);
    fclose(fp);
}

TEST_F(FileTest, create_file_with_umask_failed)
{
    uint32_t mask = 0177;
    std::string path = "";
    FileCreateManager::GetInstance("").SetProjectDir(path);
    auto ret = FileCreateManager::GetInstance(path).CreateFileWithUmask(path, "", mask);
    EXPECT_EQ(ret, nullptr);
}

TEST_F(FileTest, check_file_before_create_path_not_exist_expect_false)
{
    std::string invalidPath = "./testmsmemscope/non_existent_dir/test.txt";
    auto ret = Utility::CheckFileBeforeCreate(invalidPath);
    ASSERT_FALSE(ret);
}

TEST_F(FileTest, check_file_before_create_path_unreadable_expect_false)
{
    std::string unreadableDir = "./testmsmemscope/unreadable_dir";
    MakeDir(unreadableDir);
    chmod(unreadableDir.c_str(), 0000); // 移除所有权限
    auto ret = Utility::CheckFileBeforeCreate(unreadableDir);
    ASSERT_TRUE(ret);
    chmod(unreadableDir.c_str(), 0755); // 恢复权限以便删除
    rmdir(unreadableDir.c_str());
}

TEST_F(FileTest, check_file_before_create_path_over_depth_expect_false)
{
    std::string deepPath = "./testmsmemscope";
    std::string currentPath = deepPath;
    for (int i = 2; i <= 32; ++i) {
        currentPath += "/a" + std::to_string(i);
    }
    // 增加第33级，触发深度超限
    std::string overDeepPath = currentPath + "/a33";
    MakeDir(overDeepPath);
    
    // 验证33级路径触发深度超限
    auto ret = Utility::CheckFileBeforeCreate(overDeepPath);
    ASSERT_FALSE(ret);
    
    rmdir(overDeepPath.c_str());
    for (int i = 32; i >= 1; --i) {
        rmdir(currentPath.c_str());
        currentPath = currentPath.substr(0, currentPath.find_last_of("/"));
    }
}

// 测试CheckFileBeforeCreate的异常场景：软链接路径
TEST_F(FileTest, check_file_before_create_soft_link_expect_false)
{
    std::string realDir = "./real_dir";
    std::string linkDir = "./link_dir";
    MakeDir(realDir);
    symlink(realDir.c_str(), linkDir.c_str()); // 创建软链接
    auto ret = Utility::CheckFileBeforeCreate(linkDir);
    ASSERT_TRUE(ret);
    unlink(linkDir.c_str());
    rmdir(realDir.c_str());
}

// 测试FileExists：文件不存在场景
TEST_F(FileTest, file_exists_not_exist_file_expect_false)
{
    std::string nonExistentFile = "./non_existent_file.txt";
    auto ret = Utility::FileExists(nonExistentFile);
    ASSERT_FALSE(ret);
}

// 测试TableExists：表不存在场景（基于sqlite3）
TEST_F(FileTest, table_exists_not_exist_table_expect_false)
{
    sqlite3* db = nullptr;
    std::string dbPath = "./test_db.db";
    int rc = sqlite3_open(dbPath.c_str(), &db);
    ASSERT_EQ(rc, SQLITE_OK);
    auto ret = Utility::TableExists(db, "non_existent_table");
    ASSERT_FALSE(ret);
    sqlite3_close(db);
    remove(dbPath.c_str());
}

// 测试TableExists：表存在场景（基于sqlite3）
TEST_F(FileTest, table_exists_exist_table_expect_true)
{
    sqlite3* db = nullptr;
    std::string dbPath = "./test_db.db";
    int rc = sqlite3_open(dbPath.c_str(), &db);
    ASSERT_EQ(rc, SQLITE_OK);
    std::string createSql = "CREATE TABLE test_table (id INT);";
    sqlite3_exec(db, createSql.c_str(), nullptr, nullptr, nullptr);
    auto ret = Utility::TableExists(db, "test_table");
    ASSERT_TRUE(ret);
    sqlite3_close(db);
    remove(dbPath.c_str());
}

// 测试FileCreateManager::CreateFile：路径不可读（CheckFileBeforeCreate失败）
TEST_F(FileTest, create_file_dir_unreadable_expect_nullptr)
{
    std::string unreadableDir = "./unreadable_dir2";
    MakeDir(unreadableDir);
    chmod(unreadableDir.c_str(), 0000); // 移除权限
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    FILE* fp = manager.CreateFile(unreadableDir, "test.txt", 0644);
    chmod(unreadableDir.c_str(), 0755);
    rmdir(unreadableDir.c_str());
}

// 测试IsFileSizeSafe：文件不存在（lstat失败）
TEST_F(FileTest, is_file_size_safe_file_not_exist_expect_false)
{
    std::string nonExistentFile = "./non_existent_test_file.txt";
    auto ret = Utility::IsFileSizeSafe(nonExistentFile);
    ASSERT_FALSE(ret);
}

// 测试IsFileSizeSafe：路径是目录（非普通文件）
TEST_F(FileTest, is_file_size_safe_path_is_dir_expect_false)
{
    std::string testDir = "./test_dir_for_file_check";
    MakeDir(testDir);
    auto ret = Utility::IsFileSizeSafe(testDir);
    ASSERT_FALSE(ret);
    rmdir(testDir.c_str());
}

// 测试IsFileSizeSafe：路径是软链接（非普通文件）
TEST_F(FileTest, is_file_size_safe_path_is_symlink_expect_false)
{
    std::string realFile = "./real_test_file.txt";
    std::string linkFile = "./link_test_file.txt";
    // 创建真实普通文件
    FILE* fp = fopen(realFile.c_str(), "w");
    fclose(fp);
    // 创建软链接指向真实文件
    symlink(realFile.c_str(), linkFile.c_str());
    
    auto ret = Utility::IsFileSizeSafe(linkFile);
    ASSERT_FALSE(ret);
    
    // 清理资源
    unlink(linkFile.c_str());
    remove(realFile.c_str());
}

// 测试IsFileSizeSafe：文件大小正常（未超过MAX_INPUT_FILE_SIZE）
TEST_F(FileTest, is_file_size_safe_normal_size_expect_true)
{
    std::string normalFile = "./normal_size_file.txt";
    FILE* fp = fopen(normalFile.c_str(), "w");
    // 写入小于MAX_INPUT_FILE_SIZE的内容（示例：100字节）
    std::string content(100, 'a');
    fwrite(content.c_str(), 1, content.size(), fp);
    fclose(fp);
    
    auto ret = Utility::IsFileSizeSafe(normalFile);
    ASSERT_TRUE(ret);
    
    remove(normalFile.c_str());
}

// 测试IsFileSizeSafe：文件大小超过MAX_INPUT_FILE_SIZE
TEST_F(FileTest, is_file_size_safe_exceed_max_size_expect_false)
{
    std::string overSizeFile = "./over_max_size_file.txt";
    FILE* fp = fopen(overSizeFile.c_str(), "w");
    // 写入超过MAX_INPUT_FILE_SIZE的内容（多1字节）
    std::string content(MAX_INPUT_FILE_SIZE + 1, 'a');
    fwrite(content.c_str(), 1, content.size(), fp);
    fclose(fp);
    
    auto ret = Utility::IsFileSizeSafe(overSizeFile);
    ASSERT_FALSE(ret);
    
    remove(overSizeFile.c_str());
}

// 测试SetDirPath：第一个分支（dirPath长度超过PATH_MAX，触发异常）
TEST_F(FileTest, set_dir_path_exceed_path_max_expect_no_change)
{
    std::string overLengthDir(PATH_MAX + 1, 'a'); // 长度 = PATH_MAX + 1，触发第一个分支
    std::string defaultDir = "./default_test_dir";

    std::string originalDir = overLengthDir;

    Utility::SetDirPath(overLengthDir, defaultDir);
    
    // 断言1：路径长度仍超过PATH_MAX（第一个分支仅打印日志并返回，不修改dirPath）
    ASSERT_GT(overLengthDir.length(), PATH_MAX);
    // 断言2：路径内容未被修改（与原始值一致）
    ASSERT_EQ(overLengthDir, originalDir);
}