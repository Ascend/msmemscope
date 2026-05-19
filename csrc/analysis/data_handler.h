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

#ifndef DATA_HANDLER_H
#define DATA_HANDLER_H

#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include "sqlite3.h"
#include "config_info.h"
#include "record_info.h"
#include "utils.h"
#include "file.h"
#include "log.h"
#include "constant.h"
#include "python_trace_event.h"
#include "event.h"

namespace MemScope {

// DumpHandler类主要用于将analyzer分析的数据dump至csv或者db文件
class DataHandler {
public:
    virtual ~DataHandler() = default;
    virtual bool Init() = 0;
    virtual bool Write(std::shared_ptr<DataBase> data) = 0;
    virtual void FflushFile() = 0;

protected:
    explicit DataHandler(const Config config);
    Config config_;

private:
    DataHandler(const DataHandler&) = delete;
    DataHandler& operator=(const DataHandler&) = delete;
    DataHandler(DataHandler&& other) = delete;
    DataHandler& operator=(DataHandler&& other) = delete;
};

class CsvHandler : public DataHandler {
public:
    ~CsvHandler() override;
    explicit CsvHandler(const Config config, DataType dataType, int32_t devId);
    bool Init() override;
    bool Write(std::shared_ptr<DataBase> data) override;
    void FflushFile() override;

private:
    void InitSetParm();
    bool WriteDumpRecord(std::shared_ptr<EventBase>& event);
    bool WriteTraceEvent(std::shared_ptr<TraceEvent>& event);
    FILE *file_ = nullptr;
    std::string csvHeader_;
    std::string prefix_;
    DataType dataType_;
    int32_t devId_;
    std::mutex dumpFileMutex_;
    std::mutex traceFileMutex_;
};

class DbHandler : public DataHandler {
public:
    explicit DbHandler(const Config config, DataType dataType, int32_t devId);
    ~DbHandler() override;
    bool Init() override;
    bool Write(std::shared_ptr<DataBase> data) override;
    void FflushFile() override;

private:
    void InitSetParm();
    bool WriteDumpRecord(std::shared_ptr<EventBase>& event);
    bool WriteTraceEvent(std::shared_ptr<TraceEvent>& event, const std::string &tableName);
    sqlite3 *dataFileDb_ = nullptr;
    sqlite3_stmt *insertEventStmt_ = nullptr;
    sqlite3_stmt *insertTraceStmt_ = nullptr;
    std::vector<std::string> eventColumns_;
    std::vector<std::string> traceColumns_;
    std::string dbHeader_;
    std::string tableName_;
    DataType dataType_;
    int32_t devId_;
    std::mutex dumpFileMutex_;
};

std::string Uint64ToHexString(uint64_t value);
std::string BuildInsertStatement(const std::string& table, const std::vector<std::string>& columns);
std::string BuildCreateStatement(const std::string& table,
    const std::vector<std::pair<std::string, std::string>>& columns);

std::unique_ptr<DataHandler> MakeDataHandler(Config config, DataType data, int32_t devId);
std::string FixJson(const std::string& input);
std::vector<std::string> ParserHeader(const std::vector<std::pair<std::string, std::string>>& header);
}
#endif