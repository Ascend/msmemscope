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
#include <gtest/internal/gtest-port.h>
#include <string>
#include <memory>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "config_info.h"
#include "record_info.h"
#include "utils.h"
#include "file.h"
#include "securec.h"
#include "utility/log.h"
#include "constant.h"
#include "event.h"
#include "python_trace_event.h"
// 注入用例需要直接操作 handler 的文件句柄/语句句柄（置空、替换为 /dev/full、保留未 finalize 语句）
#define private public
#include "data_handler.h"
#undef private

using namespace MemScope;
extern bool g_isDlsymNullptr;
int32_t devId = 0;

class DataHandlerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("./testmsmemscope");
    }

    void TearDown() override
    {
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("");
        rmdir("./testmsmemscope");
    }
};

TEST_F(DataHandlerTest, CsvHandler_Write_LeakRecord)
{
    Config config{};
    config.dataFormat = static_cast<uint8_t>(DataFormat::CSV);
    config.enableCStack = false;
    config.enablePyStack = false;
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);

    CsvHandler handler(config, DataType::MEMORY_EVENT, devId);
    handler.Init();
    std::shared_ptr<EventBase> data1 = std::make_shared<EventBase>();
    data1->id = 1;
    data1->eventType = EventBaseType::MALLOC;
    data1->eventSubType = EventSubType::PTA_CACHING;
    data1->name = "obj1";
    data1->timestamp = 123456789;
    data1->pid = 1234;
    data1->tid = 5678;
    data1->device = 1;
    data1->addr = 0x1234;
    data1->attr = "size=1024";
    ASSERT_TRUE(handler.Write(data1));

    config.enablePyStack = true;
    CsvHandler handlerPy(config, DataType::MEMORY_EVENT, devId);
    handlerPy.Init();
    CallStackString stack_;
    data1->pyCallStack = "call_stack_py";
    ASSERT_TRUE(handlerPy.Write(data1));

    CsvHandler handler_(config, DataType::PYTHON_TRACE_EVENT, devId);
    handler_.Init();
    std::shared_ptr<TraceEvent> data2 = std::make_shared<TraceEvent>();
    data2->startTs = 1000;
    data2->endTs = 2000;
    data2->tid = 123;
    data2->pid = 456;
    data2->info = "function_call";
    data2->hash = "hash123";
    ASSERT_TRUE(handler_.Write(data2));
}

TEST_F(DataHandlerTest, Sqlite3_open)
{
    g_isDlsymNullptr = false;
    sqlite3* db = nullptr;
    std::string path = "./testmsmemscope/test.db";
    int rc = sqlite3_open(path.c_str(), &db);
    EXPECT_EQ(rc, 0);
    sqlite3_errmsg(db);
}

TEST_F(DataHandlerTest, DbHandler_Write_LeakRecord)
{
    g_isDlsymNullptr = false;
    Config config{};
    config.dataFormat = static_cast<uint8_t>(DataFormat::DB);
    config.enableCStack = true;
    config.enablePyStack = true;
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);

    std::unique_ptr<DataHandler> handler = MakeDataHandler(config, DataType::MEMORY_EVENT, devId);
    handler->Init();
    std::shared_ptr<EventBase> data1 = std::make_shared<EventBase>();
    data1->id = 1;
    data1->eventType = EventBaseType::MALLOC;
    data1->eventSubType = EventSubType::PTA_CACHING;
    data1->name = "obj1";
    data1->timestamp = 123456789;
    data1->pid = 1234;
    data1->tid = 5678;
    data1->device = 1;
    data1->addr = 0x1234;
    data1->attr = "size=1024";
    data1->cCallStack = "call_stack_c";
    data1->pyCallStack = "call_stack_py";
    ASSERT_TRUE(handler->Write(data1));

    config.enableCStack = false;
    config.enablePyStack = false;
    DbHandler handler_(config, DataType::PYTHON_TRACE_EVENT, devId);
    handler_.Init();
    std::shared_ptr<TraceEvent> data2 = std::make_shared<TraceEvent>();
    data2->startTs = 1000;
    data2->endTs = 2000;
    data2->tid = 123;
    data2->pid = 456;
    data2->info = "function_call";
    data2->hash = "hash123";
    ASSERT_TRUE(handler_.Write(data2));
}

TEST_F(DataHandlerTest, CsvHandler_InitSetParm_Default)
{
    Config config{};
    config.dataFormat = static_cast<uint8_t>(DataFormat::CSV);
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);
    CsvHandler handler(config, static_cast<DataType>(999), devId);
    EXPECT_TRUE(true);
}

TEST_F(DataHandlerTest, CsvHandler_Write_NullData)
{
    Config config{};
    config.dataFormat = static_cast<uint8_t>(DataFormat::CSV);
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);
    CsvHandler handler(config, DataType::MEMORY_EVENT, devId);
    handler.Init();
    ASSERT_FALSE(handler.Write(nullptr));
}

TEST_F(DataHandlerTest, DbHandler_InitSetParm_Default)
{
    Config config{};
    config.dataFormat = static_cast<uint8_t>(DataFormat::DB);
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);
    DbHandler handler(config, static_cast<DataType>(999), devId);
}

TEST_F(DataHandlerTest, DbHandler_Write_NullData)
{
    g_isDlsymNullptr = false;
    Config config{};
    config.dataFormat = static_cast<uint8_t>(DataFormat::DB);
    config.enableCStack = false;
    config.enablePyStack = false;
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);
    DbHandler handler(config, DataType::MEMORY_EVENT, devId);
    handler.Init();
    ASSERT_FALSE(handler.Write(nullptr));
}

TEST_F(DataHandlerTest, MakeDataHandler_FALSE)
{
    Config config{};
    config.dataFormat = 2;
    auto handler = MemScope::MakeDataHandler(config, static_cast<DataType>(999), devId);
    EXPECT_EQ(handler, nullptr);
}

TEST_F(DataHandlerTest, DataHandler_Write_Type_False)
{
    g_isDlsymNullptr = false;
    std::shared_ptr<DataBase> data = std::make_shared<DataBase>(static_cast<DataType>(999));
    Config config{};
    config.dataFormat = static_cast<uint8_t>(DataFormat::DB);
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);
    DbHandler handler(config, DataType::MEMORY_EVENT, devId);
    EXPECT_FALSE(handler.Write(data));
    config.dataFormat = static_cast<uint8_t>(DataFormat::CSV);
    CsvHandler handler_(config, DataType::MEMORY_EVENT, devId);
    EXPECT_FALSE(handler_.Write(data));
}

TEST_F(DataHandlerTest, DataHandler_FixJson)
{
    std::string input = "\"{addr:20616937226752,size:28160,total:2097152,used:1617920}\"";
    std::string expected = R"({"addr":"20616937226752","size":"28160","total":"2097152","used":"1617920"})";
    EXPECT_EQ(FixJson(input), expected);
}

// ==================== 文件创建失败 / 类型不匹配 / 写库失败分支补充 ====================

namespace {
constexpr int DH_DEEP_LEVEL = 40;

// 构造深度超限目录，使 FileCreateManager::CreateCsvFile/CreateDbFile 的 CreateFile 失败
std::string MakeDhOverDepthDir(const std::string& base, std::vector<std::string>& created)
{
    std::string cur = base;
    created.push_back(cur);
    for (int i = 0; i < DH_DEEP_LEVEL; ++i) {
        cur += "/d" + std::to_string(i);
        created.push_back(cur);
    }
    (void)Utility::MakeDir(cur);
    return cur;
}

void RemoveDhOverDepthDir(std::vector<std::string>& created)
{
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
        rmdir(it->c_str());
    }
    created.clear();
}

void FillTestConfig(Config& config)
{
    config.dataFormat = static_cast<uint8_t>(DataFormat::CSV);
    config.enableCStack = false;
    config.enablePyStack = false;
    strncpy_s(config.outputDir, sizeof(config.outputDir) - 1, "./testmsmemscope", sizeof(config.outputDir) - 1);
}
}  // namespace

// CsvHandler：创建 csv 文件失败时 file_ 为空，Write 打印错误并返回 false
TEST_F(DataHandlerTest, DataHandlerExtra_CsvCreateFailedExpectWriteFalse)
{
    std::vector<std::string> created;
    std::string deepPath = MakeDhOverDepthDir("./msms_dh_deep_csv", created);
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir(deepPath);

    Config config{};
    FillTestConfig(config);
    CsvHandler handler(config, DataType::MEMORY_EVENT, devId);

    manager.SetProjectDir("./testmsmemscope");
    EXPECT_EQ(handler.file_, nullptr);
    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::MALLOC;
    EXPECT_FALSE(handler.Write(event));

    // CreateFile 先 MakeDir 再校验深度，device_0/dump 目录已被创建，需要逐级清理
    rmdir((deepPath + "/device_" + std::to_string(devId) + "/" + MemScope::DUMP_DIR).c_str());
    rmdir((deepPath + "/device_" + std::to_string(devId)).c_str());
    RemoveDhOverDepthDir(created);
}

// DbHandler：内存事件建库失败时 dataFileDb_ 为空，Write 打印错误并返回 false
TEST_F(DataHandlerTest, DataHandlerExtra_DbMemCreateFailedExpectWriteFalse)
{
    std::vector<std::string> created;
    std::string deepPath = MakeDhOverDepthDir("./msms_dh_deep_db", created);
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir(deepPath);

    Config config{};
    FillTestConfig(config);
    DbHandler handler(config, DataType::MEMORY_EVENT, devId);

    manager.SetProjectDir("./testmsmemscope");
    EXPECT_EQ(handler.dataFileDb_, nullptr);
    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::MALLOC;
    EXPECT_FALSE(handler.Write(event));

    // CreateFile 先 MakeDir 再校验深度，device_0/dump 目录已被创建，需要逐级清理
    rmdir((deepPath + "/device_" + std::to_string(devId) + "/" + MemScope::DUMP_DIR).c_str());
    rmdir((deepPath + "/device_" + std::to_string(devId)).c_str());
    RemoveDhOverDepthDir(created);
}

// DbHandler：python trace 建库失败时 dataFileDb_ 为空，Write 打印错误并返回 false
TEST_F(DataHandlerTest, DataHandlerExtra_DbTraceCreateFailedExpectWriteFalse)
{
    std::vector<std::string> created;
    std::string deepPath = MakeDhOverDepthDir("./msms_dh_deep_trace", created);
    auto& manager = Utility::FileCreateManager::GetInstance("./testmsmemscope");
    manager.SetProjectDir(deepPath);

    Config config{};
    FillTestConfig(config);
    DbHandler handler(config, DataType::PYTHON_TRACE_EVENT, devId);

    manager.SetProjectDir("./testmsmemscope");
    EXPECT_EQ(handler.dataFileDb_, nullptr);
    auto trace = std::make_shared<TraceEvent>();
    trace->info = "func";
    EXPECT_FALSE(handler.Write(trace));

    // CreateFile 先 MakeDir 再校验深度，device_0/dump 目录已被创建，需要逐级清理
    rmdir((deepPath + "/device_" + std::to_string(devId) + "/" + MemScope::DUMP_DIR).c_str());
    rmdir((deepPath + "/device_" + std::to_string(devId)).c_str());
    RemoveDhOverDepthDir(created);
}

// CsvHandler：底层 fprintf 写失败（/dev/full 无缓冲）时 WriteDumpRecord 返回 false
TEST_F(DataHandlerTest, DataHandlerExtra_CsvFprintfFailedExpectFalse)
{
    Config config{};
    FillTestConfig(config);
    CsvHandler handler(config, DataType::MEMORY_EVENT, devId);
    ASSERT_NE(handler.file_, nullptr);
    fclose(handler.file_);
    handler.file_ = fopen("/dev/full", "w");
    ASSERT_NE(handler.file_, nullptr);
    ASSERT_EQ(setvbuf(handler.file_, nullptr, _IONBF, 0), 0);

    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::MALLOC;
    EXPECT_FALSE(handler.Write(event));
}

// CsvHandler：python trace 事件写失败时 WriteTraceEvent 返回 false
TEST_F(DataHandlerTest, DataHandlerExtra_CsvTraceFprintfFailedExpectFalse)
{
    Config config{};
    FillTestConfig(config);
    CsvHandler handler(config, DataType::PYTHON_TRACE_EVENT, devId);
    ASSERT_NE(handler.file_, nullptr);
    fclose(handler.file_);
    handler.file_ = fopen("/dev/full", "w");
    ASSERT_NE(handler.file_, nullptr);
    ASSERT_EQ(setvbuf(handler.file_, nullptr, _IONBF, 0), 0);

    auto trace = std::make_shared<TraceEvent>();
    trace->info = "func";
    EXPECT_FALSE(handler.Write(trace));
}

// DbHandler：MSTX 事件写库成功；name 加引号、指针列 N/A、CPU 设备映射为 cpu
TEST_F(DataHandlerTest, DataHandlerExtra_DbMstxEventFieldsExpectWritten)
{
    Config config{};
    FillTestConfig(config);
    DbHandler handler(config, DataType::MEMORY_EVENT, devId);
    ASSERT_NE(handler.dataFileDb_, nullptr);

    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::MSTX;
    event->eventSubType = EventSubType::MSTX_MARK;
    event->device = DEVICE_ID_CPU;
    event->name = "mark";
    ASSERT_TRUE(handler.Write(event));

    sqlite3_stmt* stmt = nullptr;
    // 同一进程内多个用例复用同一 db 文件，按 rowid 倒序取本次刚写入的那条
    ASSERT_EQ(sqlite3_prepare_v2(handler.dataFileDb_,
                                 "SELECT \"Name\",\"Ptr\",\"Device Id\" FROM memscope_dump ORDER BY rowid DESC LIMIT 1;",
                                 -1, &stmt, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), "\"mark\"");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)), "N/A");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)), "cpu");
    sqlite3_finalize(stmt);
}

// DbHandler：底层表被删除导致 insert 执行失败，WriteDumpRecord 返回 false
TEST_F(DataHandlerTest, DataHandlerExtra_DbInsertStepFailedExpectFalse)
{
    Config config{};
    FillTestConfig(config);
    DbHandler handler(config, DataType::MEMORY_EVENT, devId);
    ASSERT_NE(handler.dataFileDb_, nullptr);
    ASSERT_EQ(sqlite3_exec(handler.dataFileDb_, "DROP TABLE memscope_dump;", nullptr, nullptr, nullptr), SQLITE_OK);

    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::MALLOC;
    EXPECT_FALSE(handler.Write(event));
}

// DbHandler：trace 表被删除导致 insert 执行失败，WriteTraceEvent 返回 false
TEST_F(DataHandlerTest, DataHandlerExtra_DbTraceStepFailedExpectFalse)
{
    Config config{};
    FillTestConfig(config);
    DbHandler handler(config, DataType::PYTHON_TRACE_EVENT, devId);
    ASSERT_NE(handler.dataFileDb_, nullptr);
    std::string tableName = PYTHON_TRACE_TABLE + std::to_string(Utility::GetPid());
    std::string dropSql = "DROP TABLE " + tableName + ";";
    ASSERT_EQ(sqlite3_exec(handler.dataFileDb_, dropSql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);

    auto trace = std::make_shared<TraceEvent>();
    trace->info = "func";
    EXPECT_FALSE(handler.Write(trace));
}
