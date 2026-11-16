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
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "calculate_data_check_sum.h"

#define private public
#include "event_trace/memory_watch/tensor_dumper.h"
#undef private
#include "event_trace/memory_watch/memory_watch.h"
#include "event_trace/memory_watch/tensor_monitor.h"
#include "file.h"

// 符号桩表定义在 test_kernel_hooks.cpp，此处借dlsym桩注入aclrtMemcpy/aclrtSynchronizeStream
extern std::unordered_map<std::string, void *> g_funcMocks;
extern bool g_isDlsymNullptr;

using namespace MemScope;

TEST(TesnorDumperTest, dump_tensor_binary_expect_success)
{
    TensorDumper::GetInstance().dumpDir_ = "./testmsmemscope/watch_dump";
    Utility::MakeDir(TensorDumper::GetInstance().dumpDir_);
    std::string fileName = "test";
    std::vector<uint8_t> hostData(fileName.begin(), fileName.end());
    auto ret = TensorDumper::GetInstance().DumpTensorBinary(hostData, fileName);
    ASSERT_TRUE(ret);
}

TEST(TesnorDumperTest, dump_expect_success)
{
    std::string fileName = "test";
    MonitoredTensor tensor = {};
    uint64_t ptr = 6666;
    tensor.data = reinterpret_cast<void *>(ptr);
    tensor.dataSize = 100;
    TensorDumper::GetInstance().SetDumpName(ptr, "test_op");
    TensorDumper::GetInstance().SetDumpNums(ptr, 2);
    TensorMonitor::GetInstance().pythonWatchedTensorsMap_.insert({ptr, tensor});
    TensorDumper::GetInstance().Dump(nullptr, fileName, true);
    EXPECT_EQ(TensorDumper::GetInstance().GetDumpNums(ptr), 1);

    TensorDumper::GetInstance().SetDumpNums(ptr, 0);
    TensorDumper::GetInstance().Dump(nullptr, fileName, true);
}

TEST(TesnorDumperTest, set_dump_name_expect_success)
{
    std::string name = "test";
    uint64_t ptr = 6666;
    TensorDumper::GetInstance().SetDumpName(ptr, name);
    EXPECT_EQ(TensorDumper::GetInstance().GetDumpName(ptr), name);
}

TEST(TesnorDumperTest, delete_dump_name_expect_success)
{
    std::string name = "test";
    uint64_t ptr = 6666;
    TensorDumper::GetInstance().SetDumpName(ptr, name);
    TensorDumper::GetInstance().DeleteDumpName(ptr);
    EXPECT_EQ(TensorDumper::GetInstance().GetDumpName(ptr), "UNKNWON");
}

TEST(TesnorDumperTest, set_dump_nums_expect_success)
{
    int32_t dumpNums = 100;
    uint64_t ptr = 6666;
    TensorDumper::GetInstance().SetDumpNums(ptr, dumpNums);
    EXPECT_EQ(TensorDumper::GetInstance().GetDumpNums(ptr), dumpNums);
}

TEST(TesnorDumperTest, delete_dump_nums_expect_success)
{
    int32_t dumpNums = 100;
    uint64_t ptr = 6666;
    TensorDumper::GetInstance().SetDumpNums(ptr, dumpNums);
    TensorDumper::GetInstance().DeleteDumpNums(ptr);
    EXPECT_EQ(TensorDumper::GetInstance().GetDumpNums(ptr), -1);
}

namespace
{
constexpr const char* kSyncChildEnv = "MSMEMSCOPE_TENSOR_DUMPER_SYNC_CHILD";
constexpr int kChildTimeoutMs = 20000;

// aclrtMemcpy/aclrtSynchronizeStream桩的调用计数与返回值（用例内自归一）
int g_mockMemcpyCalls = 0;
int g_mockMemcpyReturn = ACL_SUCCESS;
int g_mockSyncCalls = 0;
int g_mockSyncReturn = ACL_SUCCESS;

extern "C"
{
aclError MockMemcpyFill(void *dst, size_t destMax, const void *src, size_t count, aclrtMemcpyKind kind)
{
    (void)destMax;
    (void)src;
    (void)kind;
    ++g_mockMemcpyCalls;
    if (g_mockMemcpyReturn == ACL_SUCCESS && dst != nullptr && count > 0)
    {
        (void)std::memset(dst, 0x5A, count);
    }
    return g_mockMemcpyReturn;
}

aclError MockSynchronizeStream(aclrtStream stream)
{
    (void)stream;
    ++g_mockSyncCalls;
    return g_mockSyncReturn;
}
}

// RAII方式注入/还原dlsym桩表中的符号，避免影响其他用例
class ScopedFuncMock
{
   public:
    ScopedFuncMock(const std::string& symbol, void *func) : symbol_(symbol)
    {
        auto it = g_funcMocks.find(symbol_);
        if (it != g_funcMocks.end())
        {
            hadOld_ = true;
            oldFunc_ = it->second;
            it->second = func;
        }
        else
        {
            g_funcMocks[symbol_] = func;
        }
    }
    ~ScopedFuncMock()
    {
        if (hadOld_)
        {
            g_funcMocks[symbol_] = oldFunc_;
        }
        else
        {
            g_funcMocks.erase(symbol_);
        }
    }
    ScopedFuncMock(const ScopedFuncMock&) = delete;
    ScopedFuncMock& operator=(const ScopedFuncMock&) = delete;

   private:
    std::string symbol_;
    bool hadOld_ = false;
    void *oldFunc_ = nullptr;
};

bool IsSyncChild()
{
    const char* value = std::getenv(kSyncChildEnv);
    return value != nullptr && std::string(value) == "1";
}

std::string MakeTempDir()
{
    char pathTemplate[] = "/tmp/msms_tensor_dumper_XXXXXX";
    char* dir = ::mkdtemp(pathTemplate);
    return dir == nullptr ? std::string() : std::string(dir);
}

void RemoveDirTree(const std::string& path)
{
    struct stat st;
    if (path.empty() || ::lstat(path.c_str(), &st) != 0)
    {
        return;
    }
    if (!S_ISDIR(st.st_mode))
    {
        (void)::unlink(path.c_str());
        return;
    }
    DIR* dir = ::opendir(path.c_str());
    if (dir != nullptr)
    {
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(dir)) != nullptr)
        {
            const std::string name(entry->d_name);
            if (name == "." || name == "..")
            {
                continue;
            }
            RemoveDirTree(path + "/" + name);
        }
        (void)::closedir(dir);
    }
    (void)::rmdir(path.c_str());
}

bool WriteTextFile(const std::string& path, const std::string& content)
{
    std::ofstream ofs(path);
    if (!ofs.is_open())
    {
        return false;
    }
    ofs << content;
    ofs.close();
    return true;
}

std::string ReadTextFile(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open())
    {
        return std::string();
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return content;
}

std::string ReadFromFilePtr(std::FILE* fp)
{
    if (fp == nullptr)
    {
        return std::string();
    }
    (void)::fflush(fp);
    const int fd = ::fileno(fp);
    if (fd < 0 || ::lseek(fd, 0, SEEK_SET) < 0)
    {
        return std::string();
    }
    std::string content;
    char buffer[1024];
    ssize_t len = 0;
    while ((len = ::read(fd, buffer, sizeof(buffer))) > 0)
    {
        content.append(buffer, static_cast<size_t>(len));
    }
    return content;
}

// 递归查找文件名前缀命中的首个文件（CreateCsvFile的文件名带日期，无法写死）
std::string FindFileWithPrefix(const std::string& root, const std::string& prefix)
{
    DIR* dir = ::opendir(root.c_str());
    if (dir == nullptr)
    {
        return std::string();
    }
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr)
    {
        const std::string name(entry->d_name);
        if (name == "." || name == "..")
        {
            continue;
        }
        const std::string path = root + "/" + name;
        struct stat st;
        if (::lstat(path.c_str(), &st) != 0)
        {
            continue;
        }
        if (S_ISDIR(st.st_mode))
        {
            const std::string found = FindFileWithPrefix(path, prefix);
            if (!found.empty())
            {
                (void)::closedir(dir);
                return found;
            }
            continue;
        }
        if (name.compare(0, prefix.size(), prefix) == 0)
        {
            (void)::closedir(dir);
            return path;
        }
    }
    (void)::closedir(dir);
    return std::string();
}

int32_t ResolveDeviceId()
{
    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM)
    {
        devId = GD_INVALID_NUM;
    }
    return devId;
}

std::string SelfExePath()
{
    char path[4096] = {0};
    const ssize_t len = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0)
    {
        return std::string();
    }
    return std::string(path, static_cast<size_t>(len));
}

std::string ReadAllFd(int fd)
{
    std::string content;
    char buffer[4096];
    ssize_t len = 0;
    while ((len = ::read(fd, buffer, sizeof(buffer))) > 0)
    {
        content.append(buffer, static_cast<size_t>(len));
    }
    return content;
}

bool WaitChildExit(pid_t pid, int timeoutMs, int* status)
{
    constexpr int kPollStepMs = 50;
    for (int waited = 0; waited < timeoutMs; waited += kPollStepMs)
    {
        const pid_t ret = ::waitpid(pid, status, WNOHANG);
        if (ret == pid)
        {
            return true;
        }
        ::usleep(kPollStepMs * 1000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, status, 0);
    return false;
}

pid_t SpawnChild(const std::string& filter, int stdoutFd)
{
    const std::string exePath = SelfExePath();
    const std::string filterArg = "--gtest_filter=" + filter;
    pid_t pid = ::fork();
    if (pid != 0)
    {
        return pid;
    }
    (void)::dup2(stdoutFd, STDOUT_FILENO);
    ::setenv(kSyncChildEnv, "1", 1);
    ::execl(exePath.c_str(), exePath.c_str(), filterArg.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
}

// 用例自归一：保存并注入临时落盘目录/TensorDumper字段，用例结束逐项恢复
class TensorDumperLocalTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        tmpDir_ = MakeTempDir();
        ASSERT_FALSE(tmpDir_.empty());

        auto& fileManager = Utility::FileCreateManager::GetInstance("");
        savedOutputDir_ = fileManager.outputDir_;
        savedProjectDir_ = fileManager.projectDir_;
        savedProjectDirPinned_ = fileManager.projectDirPinned_;
        fileManager.outputDir_ = tmpDir_;
        fileManager.projectDir_ = tmpDir_;
        fileManager.projectDirPinned_ = true;

        auto& dumper = TensorDumper::GetInstance();
        savedFullContent_ = dumper.fullContent_;
        savedDumpDir_ = dumper.dumpDir_;
        savedCsvFile_ = dumper.csvFile_;
        savedDlsymNullptr_ = g_isDlsymNullptr;
        // dlsym桩的全局开关可能被其他用例置位，这里显式归一，保证注入的符号可被解析
        g_isDlsymNullptr = false;
        dumpDirForTest_ = tmpDir_ + "/watch_dump";
        dumper.fullContent_ = false;
        dumper.dumpDir_ = dumpDirForTest_;
        dumper.csvFile_ = nullptr;
    }

    void TearDown() override
    {
        if (tmpDir_.empty())
        {
            return;
        }
        auto& dumper = TensorDumper::GetInstance();
        if (dumper.csvFile_ != nullptr && dumper.csvFile_ != savedCsvFile_)
        {
            (void)std::fclose(dumper.csvFile_);
        }
        dumper.csvFile_ = savedCsvFile_;
        dumper.fullContent_ = savedFullContent_;
        dumper.dumpDir_ = savedDumpDir_;
        g_isDlsymNullptr = savedDlsymNullptr_;

        auto& fileManager = Utility::FileCreateManager::GetInstance("");
        fileManager.outputDir_ = savedOutputDir_;
        fileManager.projectDir_ = savedProjectDir_;
        fileManager.projectDirPinned_ = savedProjectDirPinned_;

        RemoveDirTree(tmpDir_);
    }

    std::string BinOutDir(int32_t devId) const
    {
        return dumpDirForTest_ + "/device_" + std::to_string(devId) + "/" + WATCH_DUMP_DIR;
    }

    std::string tmpDir_;
    std::string dumpDirForTest_;
    bool savedFullContent_ = false;
    std::string savedDumpDir_;
    std::FILE* savedCsvFile_ = nullptr;
    std::string savedOutputDir_;
    std::string savedProjectDir_;
    bool savedProjectDirPinned_ = false;
    bool savedDlsymNullptr_ = false;
};

// 析构函数：csvFile_非空时关闭文件句柄（用局部对象触发，避免依赖进程退出时序）
TEST_F(TensorDumperLocalTest, destructor_closes_open_csv_file)
{
    alignas(TensorDumper) unsigned char storage[sizeof(TensorDumper)];
    TensorDumper* dumper = new (storage) TensorDumper();

    const std::string csvPath = tmpDir_ + "/dtor.csv";
    dumper->csvFile_ = std::fopen(csvPath.c_str(), "w");
    ASSERT_NE(dumper->csvFile_, nullptr);
    const int fd = ::fileno(dumper->csvFile_);
    ASSERT_GE(fd, 0);

    dumper->~TensorDumper();

    errno = 0;
    const int fdFlags = ::fcntl(fd, F_GETFD);
    const int fdErrno = errno;
    EXPECT_EQ(fdFlags, -1);
    EXPECT_EQ(fdErrno, EBADF);
    EXPECT_EQ(::access(csvPath.c_str(), F_OK), 0);
}

// DumpTensorBinary：父路径是普通文件导致MakeDir失败，返回false且不产生输出文件
TEST_F(TensorDumperLocalTest, dump_tensor_binary_make_dir_failure)
{
    auto& dumper = TensorDumper::GetInstance();
    const std::string blocker = tmpDir_ + "/binary_blocker";
    ASSERT_TRUE(WriteTextFile(blocker, "blocker"));
    dumper.dumpDir_ = blocker;

    std::vector<uint8_t> hostData(16, 0x01);
    std::string fileName = "make_dir_fail.bin";
    EXPECT_FALSE(dumper.DumpTensorBinary(hostData, fileName));
    const std::string outPath = blocker + "/device_" + std::to_string(ResolveDeviceId()) + "/" +
                                WATCH_DUMP_DIR + "/make_dir_fail.bin";
    EXPECT_NE(::access(outPath.c_str(), F_OK), 0);
}

// DumpTensorBinary：目标文件路径被目录占用导致打开失败，返回false
TEST_F(TensorDumperLocalTest, dump_tensor_binary_open_failure)
{
    auto& dumper = TensorDumper::GetInstance();
    dumper.dumpDir_ = tmpDir_ + "/watch_dump";
    const std::string outDir = tmpDir_ + "/watch_dump/device_" + std::to_string(ResolveDeviceId()) + "/" +
                               WATCH_DUMP_DIR;
    ASSERT_TRUE(Utility::MakeDir(outDir));
    const std::string outPath = outDir + "/open_fail.bin";
    ASSERT_EQ(::mkdir(outPath.c_str(), 0750), 0);

    std::vector<uint8_t> hostData(16, 0x02);
    std::string fileName = "open_fail.bin";
    EXPECT_FALSE(dumper.DumpTensorBinary(hostData, fileName));
}

// DumpTensorBinary：底层写失败（/dev/full）时返回false
TEST_F(TensorDumperLocalTest, dump_tensor_binary_write_failure)
{
    if (::access("/dev/full", F_OK) != 0)
    {
        GTEST_SKIP() << "/dev/full not available";
    }
    auto& dumper = TensorDumper::GetInstance();
    dumper.dumpDir_ = tmpDir_ + "/watch_dump";
    const std::string outDir = tmpDir_ + "/watch_dump/device_" + std::to_string(ResolveDeviceId()) + "/" +
                               WATCH_DUMP_DIR;
    ASSERT_TRUE(Utility::MakeDir(outDir));
    const std::string outPath = outDir + "/write_fail.bin";
    if (::symlink("/dev/full", outPath.c_str()) != 0)
    {
        GTEST_SKIP() << "symlink to /dev/full failed";
    }

    std::vector<uint8_t> hostData(1024 * 1024, 0x03);
    std::string fileName = "write_fail.bin";
    EXPECT_FALSE(dumper.DumpTensorBinary(hostData, fileName));
}

// DumpTensorHashValue：首次调用创建csv并写入“文件名,校验和”，内容与公共校验和实现一致
TEST_F(TensorDumperLocalTest, dump_tensor_hash_value_success_writes_check_sum_csv)
{
    auto& dumper = TensorDumper::GetInstance();
    ASSERT_EQ(dumper.csvFile_, nullptr);
    const std::vector<uint8_t> hostData = {0x10, 0x20, 0x30, 0x40, 0x50};
    std::string fileName = "hash_case.bin";

    ASSERT_TRUE(dumper.DumpTensorHashValue(hostData, fileName));
    ASSERT_NE(dumper.csvFile_, nullptr);
    // CreateCsvFile/Fprintf写入的是FILE*缓冲，先flush再读取落盘内容
    ASSERT_EQ(std::fflush(dumper.csvFile_), 0);

    const std::string csvPath = FindFileWithPrefix(tmpDir_, WATCH_CSV_FILE_PREFIX);
    ASSERT_FALSE(csvPath.empty());
    const std::string csvContent = ReadTextFile(csvPath);
    const std::string expectedLine = fileName + "," + CalculateDataCheckSum64(hostData) + "\n";
    EXPECT_NE(csvContent.find(WATCH_HASH_HEADERS), std::string::npos);
    EXPECT_NE(csvContent.find(expectedLine), std::string::npos);
}

// DumpTensorHashValue：落盘根目录被普通文件占用导致CreateCsvFile失败，返回false
TEST_F(TensorDumperLocalTest, dump_tensor_hash_value_create_csv_failure)
{
    auto& dumper = TensorDumper::GetInstance();
    auto& fileManager = Utility::FileCreateManager::GetInstance("");
    const std::string blocker = tmpDir_ + "/project_blocker";
    ASSERT_TRUE(WriteTextFile(blocker, "blocker"));
    fileManager.projectDir_ = blocker;
    ASSERT_EQ(dumper.csvFile_, nullptr);

    const std::vector<uint8_t> hostData = {0x01, 0x02};
    std::string fileName = "create_fail.bin";
    EXPECT_FALSE(dumper.DumpTensorHashValue(hostData, fileName));
    EXPECT_EQ(dumper.csvFile_, nullptr);
}

// DumpOneTensor：MakeDir失败时直接返回false
TEST_F(TensorDumperLocalTest, dump_one_tensor_make_dir_failure)
{
    auto& dumper = TensorDumper::GetInstance();
    const std::string blocker = tmpDir_ + "/one_tensor_blocker";
    ASSERT_TRUE(WriteTextFile(blocker, "blocker"));
    // dumpDir_的父路径是普通文件：MakeDir需真正创建末级目录，必然失败
    dumper.dumpDir_ = blocker + "/sub_dir";

    MonitoredTensor tensor = {};
    tensor.data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x3000));
    tensor.dataSize = 32;
    std::string fileName = "make_dir_fail_after.bin";
    EXPECT_FALSE(dumper.DumpOneTensor(tensor, fileName));
}

// DumpOneTensor：aclrtMemcpy成功后按fullContent_选择二进制落盘或校验和落盘
TEST_F(TensorDumperLocalTest, dump_one_tensor_memcpy_success_both_dump_modes)
{
    auto& dumper = TensorDumper::GetInstance();
    ScopedFuncMock memcpyMock("aclrtMemcpy", reinterpret_cast<void*>(&MockMemcpyFill));
    g_mockMemcpyCalls = 0;
    g_mockMemcpyReturn = ACL_SUCCESS;

    MonitoredTensor tensor = {};
    tensor.data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4000));
    tensor.dataSize = 64;

    // 校验和模式：写入csv
    dumper.csvFile_ = std::tmpfile();
    ASSERT_NE(dumper.csvFile_, nullptr);
    dumper.fullContent_ = false;
    std::string hashFileName = "op1_after.bin";
    ASSERT_TRUE(dumper.DumpOneTensor(tensor, hashFileName));
    const std::string csvContent = ReadFromFilePtr(dumper.csvFile_);
    const std::vector<uint8_t> filledData(64, 0x5A);
    const std::string expectedLine = hashFileName + "," + CalculateDataCheckSum64(filledData) + "\n";
    EXPECT_NE(csvContent.find(expectedLine), std::string::npos);

    // 全量内容模式：bin文件内容即桩写入的数据
    dumper.fullContent_ = true;
    dumper.dumpDir_ = tmpDir_ + "/watch_dump";
    std::string binFileName = "op1_before.bin";
    ASSERT_TRUE(dumper.DumpOneTensor(tensor, binFileName));
    EXPECT_EQ(g_mockMemcpyCalls, 2);
    const std::string binPath = BinOutDir(ResolveDeviceId()) + "/op1_before.bin";
    ASSERT_EQ(::access(binPath.c_str(), F_OK), 0);
    EXPECT_EQ(ReadTextFile(binPath), std::string(64, static_cast<char>(0x5A)));
}

// DumpOneTensor：aclrtMemcpy返回错误时直接返回false，不落盘
TEST_F(TensorDumperLocalTest, dump_one_tensor_memcpy_failure)
{
    auto& dumper = TensorDumper::GetInstance();
    ScopedFuncMock memcpyMock("aclrtMemcpy", reinterpret_cast<void*>(&MockMemcpyFill));
    g_mockMemcpyCalls = 0;
    g_mockMemcpyReturn = ACL_ERROR_INTERNAL_ERROR;

    MonitoredTensor tensor = {};
    tensor.data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5000));
    tensor.dataSize = 16;
    std::string fileName = "memcpy_fail_after.bin";
    EXPECT_FALSE(dumper.DumpOneTensor(tensor, fileName));
    EXPECT_EQ(g_mockMemcpyCalls, 1);
    EXPECT_FALSE(Utility::Exist(BinOutDir(ResolveDeviceId()) + "/" + fileName));
}

// 子进程专用：全新进程内首次调用SynchronizeStream才解析静态函数指针，桩注入后才可覆盖成功/失败分支
TEST(TensorDumperSynchronizeChild, covers_cached_func_ptr_paths)
{
    if (!IsSyncChild())
    {
        GTEST_SKIP() << "only driven by TensorDumperSynchronizeLauncher";
    }
    g_isDlsymNullptr = false;
    ScopedFuncMock syncMock("aclrtSynchronizeStream", reinterpret_cast<void*>(&MockSynchronizeStream));
    g_mockSyncCalls = 0;
    g_mockSyncReturn = ACL_SUCCESS;

    TensorDumper::GetInstance().SynchronizeStream(nullptr);
    EXPECT_EQ(g_mockSyncCalls, 1);

    // 静态指针已缓存，第二次调用仍命中同一桩；返回值非0走失败日志分支
    g_mockSyncReturn = ACL_ERROR_INTERNAL_ERROR;
    TensorDumper::GetInstance().SynchronizeStream(nullptr);
    EXPECT_EQ(g_mockSyncCalls, 2);
}

class TensorDumperSynchronizeLauncher : public ::testing::Test
{
   protected:
    void TearDown() override
    {
        if (stdoutFd_ >= 0)
        {
            (void)::close(stdoutFd_);
            stdoutFd_ = -1;
        }
        if (!stdoutPath_.empty())
        {
            (void)::unlink(stdoutPath_.c_str());
            stdoutPath_.clear();
        }
    }

    int MakeTempFile(std::string& path)
    {
        char tmpl[] = "/tmp/msms_tensor_dumper_sync_XXXXXX";
        int fd = ::mkstemp(tmpl);
        if (fd >= 0)
        {
            path.assign(tmpl);
        }
        return fd;
    }

    int stdoutFd_ = -1;
    std::string stdoutPath_;
};

TEST_F(TensorDumperSynchronizeLauncher, default_synchronize_stream_paths)
{
    if (IsSyncChild())
    {
        GTEST_SKIP();
    }
    stdoutFd_ = MakeTempFile(stdoutPath_);
    ASSERT_GE(stdoutFd_, 0);

    const pid_t pid = SpawnChild("TensorDumperSynchronizeChild.covers_cached_func_ptr_paths", stdoutFd_);
    ASSERT_GT(pid, 0);
    int status = 0;
    const bool finished = WaitChildExit(pid, kChildTimeoutMs, &status);

    std::string output;
    (void)::lseek(stdoutFd_, 0, SEEK_SET);
    output = ReadAllFd(stdoutFd_);

    ASSERT_TRUE(finished) << "child timeout, output:\n" << output;
    ASSERT_TRUE(WIFEXITED(status)) << "child exited abnormally, output:\n" << output;
    EXPECT_EQ(WEXITSTATUS(status), 0) << output;
    EXPECT_NE(output.find("[  PASSED  ] 1 test"), std::string::npos) << output;
}
}  // namespace
