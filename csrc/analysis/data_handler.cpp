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

#include <sstream>
#include "file.h"
#include "utils.h"
#include "data_handler.h"

namespace MemScope {

DataHandler::DataHandler(const Config config)
{
    config_ = config;
}

// csv handler
CsvHandler::CsvHandler(const Config config, DataType dataType, int32_t devId) : DataHandler(config),
    dataType_(dataType), devId_(devId)
{
    InitSetParm();
}

void CsvHandler::InitSetParm()
{
    switch (dataType_) {
        case DataType::MEMORY_EVENT: {
            prefix_ = CSV_FILE_PREFIX;
            csvHeader_ = MEMSCOPE_HEADERS;
            break;
        }
        case DataType::PYTHON_TRACE_EVENT:
            prefix_ = PYTHON_TRACE_FILE_PREFIX;
            csvHeader_ = TRACE_HEADERS;
            break;
        default:
            LOG_ERROR("Unsupported data type : %d\n", static_cast<int>(dataType_));
            break;
    }
    if(!Init()){
        LOG_ERROR("Create csv file failed.");
    }
}

bool CsvHandler::Init()
{
    return Utility::FileCreateManager::GetInstance(config_.outputDir).CreateCsvFile(&file_, devId_, prefix_,
        DUMP_DIR, csvHeader_);
}

bool CsvHandler::Write(std::shared_ptr<DataBase> data)
{
    if (!data) {
        LOG_ERROR("Null data pointer");
        return false;
    }

    if (!file_) {
        LOG_ERROR("Create csv file failed.");
        return false;
    }

    switch (data->GetDataType()) {
        case DataType::MEMORY_EVENT: {
            auto event = std::dynamic_pointer_cast<EventBase>(data);
            if (event) {
                return WriteDumpRecord(event);
            }
            break;
        }
        case DataType::PYTHON_TRACE_EVENT: {
            auto event = std::dynamic_pointer_cast<TraceEvent>(data);
            if (event) {
                return WriteTraceEvent(event);
            }
            break;
        }
        default:
            LOG_ERROR("Unsupported data type : %d\n", static_cast<int>(data->GetDataType()));
            return false;
    }
    return false;
}

bool CsvHandler::WriteDumpRecord(std::shared_ptr<EventBase>& event)
{
    std::lock_guard<std::mutex> lock(dumpFileMutex_);
    std::string pid = event->pid == INVALID_PROCESSID ? "N/A" : std::to_string(event->pid);
    std::string tid = event->tid == INVALID_THREADID ? "N/A" : std::to_string(event->tid);
    std::string eventType = EVENT_BASE_TYPE_MAP.find(event->eventType) == EVENT_BASE_TYPE_MAP.end()
        ? "N/A" : EVENT_BASE_TYPE_MAP.at(event->eventType);
    std::string eventSubType = EVENT_SUB_TYPE_MAP.find(event->eventSubType) == EVENT_SUB_TYPE_MAP.end()
        ? "N/A" : EVENT_SUB_TYPE_MAP.at(event->eventSubType);
    std::string addr = (event->eventType == EventBaseType::MALLOC
        || event->eventType == EventBaseType::FREE
        || event->eventType == EventBaseType::ACCESS) ? Uint64ToHexString(event->addr) : "N/A";
    std::string devId = event->device == GD_INVALID_NUM ? "N/A" : std::to_string(event->device);
    if (event->eventType == EventBaseType::MSTX) {
        event->name = "\"" + event->name + "\"";
    }
    if (!Utility::Fprintf(file_, "%lu,%s,%s,%s,%lu,%s,%s,%s,%s,%s,%s,%s",
        event->id, eventType.c_str(), eventSubType.c_str(), event->name.c_str(), event->timestamp,
        pid.c_str(), tid.c_str(), devId.c_str(), addr.c_str(), event->attr.c_str(),
        event->pyCallStack.c_str(), event->cCallStack.c_str())) {
        return false;
    }

    if (!Utility::Fprintf(file_, "\n")) {
        return false;
    }
    return true;
}

bool CsvHandler::WriteTraceEvent(std::shared_ptr<TraceEvent>& event)
{
    std::lock_guard<std::mutex> lock(traceFileMutex_);
    std::string startTime = event->startTs ? std::to_string(event->startTs) : "N/A";
    std::string endTime = event->endTs ? std::to_string(event->endTs) : "N/A";
    if (!Utility::Fprintf(file_, "%s,%s,%s,%lu,%lu\n", event->info.c_str(), startTime.c_str(),
        endTime.c_str(), event->tid, event->pid)) {
        return false;
    }
    return true;
}

void CsvHandler::FflushFile()
{
    if (file_ != nullptr) {
        fflush(file_);
    }
}

CsvHandler::~CsvHandler()
{
    FflushFile();
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

DbHandler::DbHandler(const Config config, DataType dataType, int32_t devId) : DataHandler(config),
    dataType_(dataType), devId_(devId)
{
    InitSetParm();
}

void DbHandler::InitSetParm()
{
    switch (dataType_) {
        case DataType::MEMORY_EVENT: {
            std::vector<std::pair<std::string, std::string>> schema = DUMP_RECORD_TABLE_SQL;
            eventColumns_ = ParserHeader(DUMP_RECORD_TABLE_SQL);

            schema.emplace_back("Call Stack(Python)", "TEXT");
            eventColumns_.push_back("Call Stack(Python)");
            schema.emplace_back("Call Stack(C)", "TEXT");
            eventColumns_.push_back("Call Stack(C)");

            tableName_ = DUMP_RECORD_TABLE;
            dbHeader_ = BuildCreateStatement(tableName_, schema);
            if (!Init()) {
                LOG_ERROR("Sqlite create error: %s", sqlite3_errmsg(dataFileDb_));
                break;
            }
            std::string insertSql = BuildInsertStatement(tableName_, eventColumns_);
            int resultCount1 = sqlite3_prepare_v2(dataFileDb_, insertSql.c_str(), -1, &insertEventStmt_, nullptr);
            if (resultCount1 != SQLITE_OK) {
                LOG_ERROR("Sqlite prepare error: %s", sqlite3_errmsg(dataFileDb_));
                insertEventStmt_ = nullptr;
            }
            break;
        }
        case DataType::PYTHON_TRACE_EVENT: {
            tableName_ = PYTHON_TRACE_TABLE + std::to_string(Utility::GetPid());
            dbHeader_ = BuildCreateStatement(tableName_, PYTHON_TRACE_TABLE_SQL);
            traceColumns_ = ParserHeader(PYTHON_TRACE_TABLE_SQL);
            if (!Init()) {
                LOG_ERROR("Sqlite create error: %s", sqlite3_errmsg(dataFileDb_));
                break;
            }
            std::string insertSql = BuildInsertStatement(tableName_, traceColumns_);
            int resultCount2 = sqlite3_prepare_v2(dataFileDb_, insertSql.c_str(), -1, &insertTraceStmt_, nullptr);
            if (resultCount2 != SQLITE_OK) {
                LOG_ERROR("Sqlite prepare error: %s", sqlite3_errmsg(dataFileDb_));
                insertTraceStmt_ = nullptr;
            }
            break;
        }
        default:
            LOG_ERROR("Unsupported data type : %d\n", static_cast<int>(dataType_));
            break;
    }
}

bool DbHandler::Init()
{
    return Utility::FileCreateManager::GetInstance(config_.outputDir).CreateDbFile(&dataFileDb_, devId_,
        CSV_FILE_PREFIX, DUMP_DIR, tableName_, dbHeader_);
}

bool DbHandler::Write(std::shared_ptr<DataBase> data)
{
    if (!data) {
        LOG_ERROR("Null data pointer");
        return false;
    }

    if (!dataFileDb_) {
        LOG_ERROR("Create db file failed.");
        return false;
    }

    switch (data->GetDataType()) {
        case DataType::MEMORY_EVENT: {
            auto event = std::dynamic_pointer_cast<EventBase>(data);
            if (event) {
                return WriteDumpRecord(event);
            }
            break;
        }
        case DataType::PYTHON_TRACE_EVENT: {
            auto event = std::dynamic_pointer_cast<TraceEvent>(data);
            if (event) {
                return WriteTraceEvent(event, tableName_);
            }
            break;
        }
        default:
            LOG_ERROR("Unsupported data type : %d\n", static_cast<int>(data->GetDataType()));
            return false;
    }
    return false;
}

bool DbHandler::WriteDumpRecord(std::shared_ptr<EventBase>& event)
{
    std::lock_guard<std::mutex> lock(dumpFileMutex_);
    std::string eventType = EVENT_BASE_TYPE_MAP.find(event->eventType) == EVENT_BASE_TYPE_MAP.end()
        ? "N/A" : EVENT_BASE_TYPE_MAP.at(event->eventType);
    std::string eventSubType = EVENT_SUB_TYPE_MAP.find(event->eventSubType) == EVENT_SUB_TYPE_MAP.end()
        ? "N/A" : EVENT_SUB_TYPE_MAP.at(event->eventSubType);
    std::string addr = (event->eventType == EventBaseType::MALLOC
        || event->eventType == EventBaseType::FREE
        || event->eventType == EventBaseType::ACCESS) ? Uint64ToHexString(event->addr) : "N/A";
    std::string attrJson = FixJson(event->attr);
    std::string devId = event->device == GD_INVALID_NUM ? "N/A" : std::to_string(event->device);
    int paramIndex = 1;
    if (!insertEventStmt_) {
        LOG_ERROR("Sqlite prepare failed.");
        return false;
    }
    if (event->eventType == EventBaseType::MSTX) {
        event->name = "\"" + event->name + "\"";
    }
    sqlite3_bind_int64(insertEventStmt_, paramIndex++, event->id);
    sqlite3_bind_text(insertEventStmt_, paramIndex++, eventType.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(insertEventStmt_, paramIndex++, eventSubType.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(insertEventStmt_, paramIndex++, event->name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(insertEventStmt_, paramIndex++, event->timestamp);
    sqlite3_bind_int(insertEventStmt_, paramIndex++, event->pid);
    sqlite3_bind_int(insertEventStmt_, paramIndex++, event->tid);
    sqlite3_bind_text(insertEventStmt_, paramIndex++, devId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(insertEventStmt_, paramIndex++, addr.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(insertEventStmt_, paramIndex++, attrJson.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(insertEventStmt_, paramIndex++, event->pyCallStack.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(insertEventStmt_, paramIndex++, event->cCallStack.c_str(), -1, SQLITE_STATIC);
    sqlite3_busy_timeout(dataFileDb_, SQLITE_TIME_OUT);
    int rc = sqlite3_step(insertEventStmt_);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("Sqlite insert error in memscope dump: %s  %d", sqlite3_errmsg(dataFileDb_), getpid());
        sqlite3_reset(insertEventStmt_);
        return false;
    }
    sqlite3_reset(insertEventStmt_);
    return true;
}

bool DbHandler::WriteTraceEvent(std::shared_ptr<TraceEvent>& event, const std::string &tableName)
{
    std::lock_guard<std::mutex> lock(dumpFileMutex_);
    if (!insertTraceStmt_) {
        LOG_ERROR("Sqlite prepare failed.");
        return false;
    }
    std::string startTime = event->startTs ? std::to_string(event->startTs) : "N/A";
    std::string endTime = event->endTs ? std::to_string(event->endTs) : "N/A";
    int paramIndex = 1;
    sqlite3_bind_text(insertTraceStmt_, paramIndex++, event->info.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(insertTraceStmt_, paramIndex++, startTime.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(insertTraceStmt_, paramIndex++, endTime.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(insertTraceStmt_, paramIndex++, event->tid);
    sqlite3_bind_int64(insertTraceStmt_, paramIndex++, event->pid);

    sqlite3_busy_timeout(dataFileDb_, SQLITE_TIME_OUT);
    int rc = sqlite3_step(insertTraceStmt_);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("Sqlite insert error in python trace: %s", sqlite3_errmsg(dataFileDb_));
        sqlite3_reset(insertTraceStmt_);
        return false;
    }
    sqlite3_reset(insertTraceStmt_);
    return true;
}

void DbHandler::FflushFile()
{
    if (dataFileDb_ != nullptr) {
        sqlite3_exec(dataFileDb_, "PRAGMA wal_checkpoint(FULL);", nullptr, nullptr, nullptr);
        // 提交任何未完成的事务
        sqlite3_exec(dataFileDb_, "COMMIT;", nullptr, nullptr, nullptr);
    }
}

DbHandler::~DbHandler()
{
    FflushFile();
    if (dataFileDb_ != nullptr) {
        if (insertEventStmt_ != nullptr) {
            sqlite3_finalize(insertEventStmt_);
            insertEventStmt_ = nullptr;
        }
        if (insertTraceStmt_ != nullptr) {
            sqlite3_finalize(insertTraceStmt_);
            insertTraceStmt_ = nullptr;
        }
        int rc = sqlite3_close(dataFileDb_);
        if (rc != SQLITE_OK) {
            LOG_ERROR("Sqlite close error: %s", sqlite3_errmsg(dataFileDb_));
        }
        dataFileDb_ = nullptr;
    }
}

std::string BuildInsertStatement(const std::string& table, const std::vector<std::string>& columns)
{
    std::ostringstream oss;
    oss << "INSERT INTO " << table << " (";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << columns[i] << "\"";
    }
    oss << ") VALUES (";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "?";
    }
    oss << ");";
    return oss.str();
}

std::string BuildCreateStatement(const std::string& table,
    const std::vector<std::pair<std::string, std::string>>& columns)
{
    std::ostringstream oss;
    oss << "CREATE TABLE IF NOT EXISTS " << table << " (";

    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "\"" << columns[i].first << "\" " << columns[i].second;
    }

    oss << ");";
    return oss.str();
}

std::unique_ptr<DataHandler> MakeDataHandler(Config config, DataType data, int32_t devId)
{
    switch (config.dataFormat) {
        case static_cast<uint8_t>(DataFormat::CSV):
            return std::unique_ptr<DataHandler>(new CsvHandler(config, data, devId));
            break;
        case static_cast<uint8_t>(DataFormat::DB):
            return std::unique_ptr<DataHandler>(new DbHandler(config, data, devId));
            break;
        default:
            LOG_ERROR("Unsupported format: %lu", config.dataFormat);
            return nullptr;
    }
}

// 将"\"{addr:20616937226752,size:28160,...}\"" 转成标准JSON字符串{"addr":"20616937226752","size":"28160",...}
std::string FixJson(const std::string& input)
{
    std::string str = input;
    uint32_t minSize = 4;
    if (str.size() >= minSize) {
        str = str.substr(2, str.length() - minSize);
    }
    size_t pos = 0;
    std::vector<std::string> parts;

    while (pos < str.length()) {
        size_t colonPos = str.find(':', pos);
        if (colonPos == std::string::npos) {
            parts.push_back(str.substr(pos, str.length() - pos));
            break;
        }
        size_t lastCommaPos = str.rfind(',', colonPos);
        if (lastCommaPos == std::string::npos || lastCommaPos < pos) {
            // 没有找到逗号或者逗号在pos之前
            parts.push_back(str.substr(pos, colonPos - pos));
        } else {
            parts.push_back(str.substr(pos, lastCommaPos - pos));
            parts.push_back(str.substr(lastCommaPos + 1, colonPos - lastCommaPos - 1));
        }
        pos = colonPos + 1;
    }

    std::ostringstream oss;
    auto partsNum = parts.size();
    oss << "{";
    for (size_t i = 0; i < partsNum; i++) {
        oss << "\"" << parts[i] << "\":";
        i++;
        if (i >= partsNum) {
            oss << "\"\"";
            break;
        }
        oss << "\"" << parts[i] << "\"";
        if (i != partsNum - 1) {
            oss << ",";
        }
    }
    oss << "}";
    return oss.str();
}

std::vector<std::string> ParserHeader(const std::vector<std::pair<std::string, std::string>>& header)
{
    std::vector<std::string> result;
    result.reserve(header.size());
    for (const auto& item : header) {
        result.push_back(item.first);
    }
    return result;
}

std::string Uint64ToHexString(uint64_t value)
{
    std::stringstream ss;
    ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return ss.str();
}
};
