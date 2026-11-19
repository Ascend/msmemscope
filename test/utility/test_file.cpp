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
#include <unistd.h>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
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
    auto ret = FileCreateManager::GetInstance("./testmsmemscope").CreateCsvFile(&fp, 0, "test",
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
    if (geteuid() == 0)
    {
        GTEST_SKIP() << "root bypasses file permission checks";
    }
    std::string unreadableDir = "./testmsmemscope/unreadable_dir";
    MakeDir(unreadableDir);
    chmod(unreadableDir.c_str(), 0000); // 移除所有权限
    auto ret = Utility::CheckFileBeforeCreate(unreadableDir);
    ASSERT_FALSE(ret);
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
    // ftruncate() 会把文件的逻辑大小设置为 8 GB + 1 字节，达到同样效果，避免 std::bad_alloc 可能资源不足和测试耗时过长
    ASSERT_NE(fp, nullptr);
    ASSERT_EQ(ftruncate(fileno(fp), static_cast<off_t>(MAX_INPUT_FILE_SIZE + 1)), 0);
    fclose(fp);
    
    auto ret = Utility::IsFileSizeSafe(overSizeFile);
    ASSERT_FALSE(ret);
    
    remove(overSizeFile.c_str());
}

// 测试IsFileSizeSafe：通过环境变量调整上限后，超过新上限的文件被拒绝
TEST_F(FileTest, is_file_size_safe_env_override_expect_false)
{
    setenv(MAX_INPUT_FILE_SIZE_ENV, "1", 1);
    std::string overSizeFile = "./env_over_max_size_file.txt";
    FILE* fp = fopen(overSizeFile.c_str(), "w");
    ASSERT_NE(fp, nullptr);
    ASSERT_EQ(ftruncate(fileno(fp), static_cast<off_t>((1ULL << 30) + 1)), 0);
    fclose(fp);

    EXPECT_FALSE(Utility::IsFileSizeSafe(overSizeFile));

    remove(overSizeFile.c_str());
    unsetenv(MAX_INPUT_FILE_SIZE_ENV);
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

// 测试RefreshOutputDir：outputDir变化时按新目录重算projectDir
// 回归场景：FileCreateManager在用户config()之前被提前构造（hook影子采集路径触发
// MemoryStateManager::GetInstance()的副作用），以默认outputDir固化projectDir_，
// 配置生效时经RefreshOutputDir纠正到用户output_path
TEST_F(FileTest, refresh_output_dir_to_new_dir_expect_project_dir_recalculated)
{
    auto& manager = FileCreateManager::GetInstance("./testmsmemscope");
    // 前置：解除此前用例SetProjectDir留下的钉住，回到派生态（生产场景下projectDir_始终派生自outputDir_）
    manager.projectDirPinned_ = false;
    manager.RefreshOutputDir("./testmsmemscope_fcm_refresh");
    std::string projectDir = manager.GetProjectDir();
    // projectDir按新outputDir重算: <outputDir>/msmemscope_<pid>_<时间戳>_ascend
    ASSERT_TRUE(projectDir.rfind("./testmsmemscope_fcm_refresh/msmemscope_", 0) == 0);
    const std::string suffix = "_ascend";
    ASSERT_GT(projectDir.length(), suffix.length());
    ASSERT_EQ(projectDir.substr(projectDir.length() - suffix.length()), suffix);
    // 恢复单例状态（进程级单例，测试间共享）
    manager.RefreshOutputDir("./testmsmemscope");
}

// 测试RefreshOutputDir：outputDir未变化时不重算projectDir
// （跨天运行的进程不因RefreshOutputDir中途更换落盘目录，时间戳保持首建时刻）
TEST_F(FileTest, refresh_output_dir_same_dir_expect_project_dir_kept)
{
    auto& manager = FileCreateManager::GetInstance("./testmsmemscope");
    // 先将outputDir固定为探测目录（projectDir_是否重算与本用例无关）
    manager.RefreshOutputDir("./testmsmemscope_fcm_keep");
    // 打入哨兵projectDir：若同outputDir刷新发生重算，哨兵将被覆盖
    manager.SetProjectDir("sentinel_project_dir");
    // 解除pin以隔离语义：本用例专测等值短路，哨兵仅能由"outputDir未变化"保护
    manager.projectDirPinned_ = false;
    manager.RefreshOutputDir("./testmsmemscope_fcm_keep");
    ASSERT_EQ(manager.GetProjectDir(), "sentinel_project_dir");
    // 恢复单例状态
    manager.RefreshOutputDir("./testmsmemscope");
}

// 测试RefreshOutputDir：SetProjectDir显式钉住的projectDir不被刷新覆盖
// （测试桩经SetProjectDir指定落盘目录后，SetEffectiveConfig触发的配置刷新不得将其改写为
// 派生目录<outputDir>/msmemscope_<pid>_<时间戳>_ascend；生产代码不调用SetProjectDir）
TEST_F(FileTest, refresh_output_dir_pinned_project_dir_expect_not_overwritten)
{
    auto& manager = FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir("pinned_project_dir");
    manager.RefreshOutputDir("./testmsmemscope_fcm_pin");
    ASSERT_EQ(manager.GetProjectDir(), "pinned_project_dir");
    // 恢复单例状态（解除pin并回到派生态）
    manager.projectDirPinned_ = false;
    manager.RefreshOutputDir("./testmsmemscope");
}

// ==================== 文件/目录创建失败分支补充 ====================

namespace {
constexpr int DEEP_LEVEL = 40;

// 递归创建深度超过 PATH_DEPTH_MAX(32) 的目录，用于触发 IsValidDepth 失败分支；
// created 记录从 base 到最深层的每一级路径，便于逆序清理
std::string MakeOverDepthDir(const std::string& base, std::vector<std::string>& created)
{
    std::string cur = base;
    created.push_back(cur);
    for (int i = 0; i < DEEP_LEVEL; ++i) {
        cur += "/d" + std::to_string(i);
        created.push_back(cur);
    }
    (void)MakeDir(cur);
    return cur;
}

void RemoveOverDepthDir(std::vector<std::string>& created)
{
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
        rmdir(it->c_str());
    }
    created.clear();
}
}  // namespace

// CreateCsvFile：DEVICE_ID_CPU 设备目录分支
TEST_F(FileTest, create_csv_file_cpu_device_expect_true)
{
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir("./testmsmemscope");
    FILE* fp = nullptr;
    constexpr int32_t DEVICE_ID_CPU = 10000;
    ASSERT_TRUE(manager.CreateCsvFile(&fp, DEVICE_ID_CPU, "cpu_test", MemScope::DUMP_DIR, "h\n"));
    ASSERT_NE(fp, nullptr);
    fclose(fp);

    std::string csvDir = manager.GetProjectDir() + "/device_cpu/" + MemScope::DUMP_DIR;
    std::string csvPath = csvDir + "/cpu_test" + Utility::GetDateStr() + ".csv";
    EXPECT_TRUE(Utility::FileExists(csvPath));
    remove(csvPath.c_str());
    rmdir(csvDir.c_str());
    rmdir((manager.GetProjectDir() + "/device_cpu").c_str());
}

// CreateCsvFile：目标目录超深导致 CreateFile 失败，打印错误并返回 false
TEST_F(FileTest, create_csv_file_open_failed_expect_false)
{
    std::vector<std::string> created;
    std::string deepPath = MakeOverDepthDir("./msms_deep_csv", created);
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir(deepPath);

    FILE* fp = nullptr;
    EXPECT_FALSE(manager.CreateCsvFile(&fp, 9999, "x", MemScope::DUMP_DIR, "h\n"));
    EXPECT_EQ(fp, nullptr);

    manager.SetProjectDir("./testmsmemscope");
    // CreateFile 先 MakeDir 再校验深度，dump 任务子目录已被创建，需要单独清理
    rmdir((deepPath + "/" + MemScope::DUMP_DIR).c_str());
    RemoveOverDepthDir(created);
}

// CreateDbFile：GD_INVALID_NUM 走项目根目录，表创建成功
TEST_F(FileTest, create_db_file_invalid_device_num_expect_true)
{
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir("./testmsmemscope");
    sqlite3* db = nullptr;
    const std::string createSql = "CREATE TABLE IF NOT EXISTS test_table (id INTEGER);";
    ASSERT_TRUE(manager.CreateDbFile(&db, 9999, "db_test", MemScope::DUMP_DIR, "test_table", createSql));
    ASSERT_NE(db, nullptr);
    EXPECT_TRUE(Utility::TableExists(db, "test_table"));
    sqlite3_close(db);

    std::string dbDir = manager.GetProjectDir() + "/" + MemScope::DUMP_DIR;
    std::string dbPath = dbDir + "/db_test" + manager.dbDateStr_ + ".db";
    EXPECT_TRUE(Utility::FileExists(dbPath));
    remove(dbPath.c_str());
    remove((dbPath + "-wal").c_str());
    remove((dbPath + "-shm").c_str());
    rmdir(dbDir.c_str());
}

// CreateDbFile：DEVICE_ID_CPU 设备目录分支
TEST_F(FileTest, create_db_file_cpu_device_expect_true)
{
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir("./testmsmemscope");
    sqlite3* db = nullptr;
    const std::string createSql = "CREATE TABLE IF NOT EXISTS cpu_table (id INTEGER);";
    constexpr int32_t DEVICE_ID_CPU = 10000;
    ASSERT_TRUE(manager.CreateDbFile(&db, DEVICE_ID_CPU, "cpu_db", MemScope::DUMP_DIR, "cpu_table", createSql));
    ASSERT_NE(db, nullptr);
    EXPECT_TRUE(Utility::TableExists(db, "cpu_table"));
    sqlite3_close(db);

    std::string dbDir = manager.GetProjectDir() + "/device_cpu/" + MemScope::DUMP_DIR;
    std::string dbPath = dbDir + "/cpu_db" + manager.dbDateStr_ + ".db";
    EXPECT_TRUE(Utility::FileExists(dbPath));
    remove(dbPath.c_str());
    remove((dbPath + "-wal").c_str());
    remove((dbPath + "-shm").c_str());
    rmdir(dbDir.c_str());
    rmdir((manager.GetProjectDir() + "/device_cpu").c_str());
}

// CreateDbFile：目标目录超深导致 CreateFile 失败，返回 false
TEST_F(FileTest, create_db_file_open_failed_expect_false)
{
    std::vector<std::string> created;
    std::string deepPath = MakeOverDepthDir("./msms_deep_db", created);
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir(deepPath);

    sqlite3* db = nullptr;
    const std::string createSql = "CREATE TABLE IF NOT EXISTS test_table (id INTEGER);";
    EXPECT_FALSE(manager.CreateDbFile(&db, 9999, "db_test", MemScope::DUMP_DIR, "test_table", createSql));
    EXPECT_EQ(db, nullptr);

    manager.SetProjectDir("./testmsmemscope");
    // CreateFile 先 MakeDir 再校验深度，dump 任务子目录已被创建，需要单独清理
    rmdir((deepPath + "/" + MemScope::DUMP_DIR).c_str());
    RemoveOverDepthDir(created);
}

// CreateDbFile：建表 SQL 非法，CreateDbTable 失败后关闭数据库并返回 false
TEST_F(FileTest, create_db_file_create_table_failed_expect_false)
{
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir("./testmsmemscope");
    sqlite3* db = nullptr;
    EXPECT_FALSE(manager.CreateDbFile(&db, 9999, "bad_db", MemScope::DUMP_DIR, "bad_table", "INVALID SQL"));
    EXPECT_EQ(db, nullptr);

    std::string dbDir = manager.GetProjectDir() + "/" + MemScope::DUMP_DIR;
    std::string dbPath = dbDir + "/bad_db" + manager.dbDateStr_ + ".db";
    remove(dbPath.c_str());
    remove((dbPath + "-wal").c_str());
    remove((dbPath + "-shm").c_str());
    rmdir(dbDir.c_str());
}

// CreateDbFile：句柄已存在但表不存在且建表失败，返回 false
TEST_F(FileTest, create_db_file_existing_handle_table_failed_expect_false)
{
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir("./testmsmemscope");
    sqlite3* db = nullptr;
    ASSERT_TRUE(manager.CreateDbFile(&db, 9999, "exist_db", MemScope::DUMP_DIR, "test_table",
                                     "CREATE TABLE IF NOT EXISTS test_table (id INTEGER);"));
    ASSERT_NE(db, nullptr);
    // 第二个表名不存在且建表 SQL 非法，命中“已打开句柄”分支的失败返回
    EXPECT_FALSE(manager.CreateDbFile(&db, 9999, "exist_db", MemScope::DUMP_DIR, "other_table", "INVALID SQL"));
    sqlite3_close(db);

    std::string dbDir = manager.GetProjectDir() + "/" + MemScope::DUMP_DIR;
    std::string dbPath = dbDir + "/exist_db" + manager.dbDateStr_ + ".db";
    remove(dbPath.c_str());
    remove((dbPath + "-wal").c_str());
    remove((dbPath + "-shm").c_str());
    rmdir(dbDir.c_str());
}

// CreateLogFile：目标目录超深导致 CreateFile 失败，返回 false
TEST_F(FileTest, create_log_file_open_failed_expect_false)
{
    std::vector<std::string> created;
    std::string deepPath = MakeOverDepthDir("./msms_deep_log", created);
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir(deepPath);

    FILE* fp = nullptr;
    char logPath[PATH_MAX] = {0};
    EXPECT_FALSE(manager.CreateLogFile(&fp, MemScope::DUMP_DIR, logPath, sizeof(logPath)));
    EXPECT_EQ(fp, nullptr);

    manager.SetProjectDir("./testmsmemscope");
    // CreateFile 先 MakeDir 再校验深度，dump 任务子目录已被创建，需要单独清理
    rmdir((deepPath + "/" + MemScope::DUMP_DIR).c_str());
    RemoveOverDepthDir(created);
}

// CreateLogFile：文件创建成功但日志路径回填失败（strncpy_s 目标长度不足），仍返回 true
TEST_F(FileTest, create_log_file_save_path_failed_expect_true)
{
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir("./testmsmemscope");
    FILE* fp = nullptr;
    char logPath[8] = {0};

    std::ostringstream oss;
    std::streambuf* oldBuf = std::cout.rdbuf(oss.rdbuf());
    bool ret = manager.CreateLogFile(&fp, MemScope::DUMP_DIR, logPath, 1);
    std::cout.rdbuf(oldBuf);

    EXPECT_TRUE(ret);
    ASSERT_NE(fp, nullptr);
    fclose(fp);
    EXPECT_NE(oss.str().find("Save log file path failed"), std::string::npos);

    std::string logDir = manager.GetProjectDir() + "/" + MemScope::DUMP_DIR;
    std::string logPathExpected = logDir + "/msmemscope_" + Utility::GetDateStr() + ".log";
    EXPECT_TRUE(Utility::FileExists(logPathExpected));
    remove(logPathExpected.c_str());
    rmdir(logDir.c_str());
}
