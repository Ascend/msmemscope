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

#include "file.h"

#include <dlfcn.h>
#include <securec.h>

#include "utils.h"

namespace Utility
{

// Log创建之前不要使用LOG_***
bool CheckFileBeforeCreate(const std::string& path)
{
    Path curPath = Path(path).Absolute();
    std::string curPathStr = curPath.ToString();
    std::string users;

    if (!curPath.Exists())
    {
        std::cout << "[msmemscope] Error: The path " << curPathStr << " do not exist." << std::endl;
        return false;
    }
    if (CheckStrIsStartsWithInvalidChar(curPathStr.c_str()))
    {
        std::cout << "[msmemscope] Error: The path " << curPathStr << " is invalid." << std::endl;
        return false;
    }
    if (!curPath.IsReadable())
    {
        std::cout << "[msmemscope] Error: The file path " << curPathStr << " is not readable." << std::endl;
        return false;
    }
    if (!curPath.IsValidLength())
    {
        std::cout << "[msmemscope] Error: The length of file path " << curPathStr << " exceeds the maximum length."
                  << std::endl;
        return false;
    }
    if (!curPath.IsValidDepth())
    {
        std::cout << "[msmemscope] Error: The depth of file path " << curPathStr << " exceeds the maximum depth."
                  << std::endl;
        return false;
    }
    curPath.DeclarePermissionRisk();
    return true;
}

bool FileExists(const std::string& filePath)
{
    struct stat st;
    return stat(filePath.c_str(), &st) == 0;
}

bool TableExists(sqlite3* filefp, std::string tableName)
{
    std::string sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(filefp, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return false;
    }
    rc = sqlite3_bind_text(stmt, 1, tableName.c_str(), -1, SQLITE_STATIC);
    if (rc != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_busy_timeout(filefp, MemScope::SQLITE_TIME_OUT);
    rc = sqlite3_step(stmt);
    bool exists = (rc == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

namespace
{
// 落盘根目录命名规则: <outputDir>/msmemscope_<pid>_<时间戳>_ascend（构造与RefreshOutputDir共用）
std::string MakeProjectDir(const std::string& outputDir)
{
    return outputDir + "/" + "msmemscope_" + std::to_string(GetPid()) + "_" + GetDateStr() + "_ascend";
}
}  // namespace

FileCreateManager& FileCreateManager::GetInstance(const std::string& outputDir)
{
    static FileCreateManager instance(outputDir);
    return instance;
}

FileCreateManager::FileCreateManager(const std::string& outputDir)
    : outputDir_(outputDir), projectDir_(MakeProjectDir(outputDir))
{
}

void FileCreateManager::RefreshOutputDir(const std::string& outputDir)
{
    // 空outputDir为无效配置（生产两条SetConfig路径均保证非空），不刷新，
    // 避免落盘根目录漂移到"/msmemscope_..."这类根路径
    if (outputDir.empty())
    {
        return;
    }
    // outputDir未变化时不重算:projectDir_时间戳保持首建时刻,避免跨天运行的进程中途更换目录
    if (outputDir == outputDir_)
    {
        return;
    }
    outputDir_ = outputDir;
    // SetProjectDir显式钉住的目录优先于配置派生值（生产代码不调用SetProjectDir，不受影响），
    // 此处仅同步outputDir_，不覆盖目录
    if (projectDirPinned_)
    {
        return;
    }
    projectDir_ = MakeProjectDir(outputDir);
}

FILE* FileCreateManager::CreateFileWithUmask(const std::string& path, const std::string& mode, mode_t mask)
{
    if (path.empty())
    {
        return nullptr;
    }
    UmaskGuard guard{mask};
    FILE* fp = fopen(path.c_str(), mode.c_str());
    return fp;
}

FILE* FileCreateManager::CreateFile(const std::string& dir, const std::string& name, mode_t mask)
{
    if (!MakeDir(dir))
    {
        return nullptr;
    }
    std::string filePath = dir + "/" + name;
    if (!CheckFileBeforeCreate(dir))
    {
        return nullptr;
    }
    return CreateFileWithUmask(filePath, "a", mask);
}

bool FileCreateManager::CreateDir()
{
    if (!MakeDir(projectDir_))
    {
        std::cerr << "[msmemscope] Error: Failed to create directory: " << projectDir_ << std::endl;
        return false;
    }
    if (!CheckFileBeforeCreate(projectDir_))
    {
        return false;
    }
    return true;
}

bool FileCreateManager::CreateCsvFile(FILE** filefp, int32_t devId, const std::string& filePrefix,
                                      const std::string& taskDir, const std::string& headers)
{
    std::lock_guard<std::mutex> lock(createCsvFileMutex_);
    if (*filefp == nullptr)
    {
        std::string fileName = filePrefix + GetDateStr() + ".csv";
        std::string dirPath;
        constexpr const int32_t GD_INVALID_NUM = 9999;
        constexpr const int32_t DEVICE_ID_CPU = 10000;
        if (devId == GD_INVALID_NUM)
        {
            dirPath = projectDir_ + "/" + taskDir;
        }
        else if (devId == DEVICE_ID_CPU)
        {
            dirPath = projectDir_ + "/device_cpu/" + taskDir;
        }
        else
        {
            dirPath = projectDir_ + "/device_" + std::to_string(devId) + "/" + taskDir;
        }
        std::string filePath = dirPath + "/" + fileName;
        FILE* fp = CreateFile(dirPath, fileName, DEFAULT_UMASK_FOR_CSV_FILE);
        if (fp != nullptr)
        {
            std::cout << "[msmemscope] Info: create file " << filePath << "." << std::endl;
            fprintf(fp, "%s", headers.c_str());
            *filefp = fp;
        }
        else
        {
            std::cout << "[msmemscope] Error: open file " << filePath << " failed." << std::endl;
            return false;
        }
    }
    return true;
}

bool FileCreateManager::CreateDbFile(sqlite3** filefp, int32_t devId, const std::string& filePrefix,
                                     const std::string& taskDir, const std::string& tableName,
                                     const std::string& tableCreateSql)
{
    std::lock_guard<std::mutex> lock(createDbFileMutex_);
    if (*filefp == nullptr)
    {
        if (dbDateStr_ == "")
        {
            dbDateStr_ = GetDateStr();
        }
        std::string fileName = filePrefix + dbDateStr_ + ".db";
        std::string dirPath;
        constexpr const int32_t GD_INVALID_NUM = 9999;
        constexpr const int32_t DEVICE_ID_CPU = 10000;
        if (devId == GD_INVALID_NUM)
        {
            dirPath = projectDir_ + "/" + taskDir;
        }
        else if (devId == DEVICE_ID_CPU)
        {
            dirPath = projectDir_ + "/device_cpu/" + taskDir;
        }
        else
        {
            dirPath = projectDir_ + "/device_" + std::to_string(devId) + "/" + taskDir;
        }
        std::string filePath = dirPath + "/" + fileName;
        // 在sqlite3_open前先创建好db文件
        FILE* fp = CreateFile(dirPath, fileName, DEFAULT_UMASK_FOR_DB_FILE);
        if (fp == nullptr)
        {
            std::cout << "[msmemscope] Error: open file " << filePath << " failed." << std::endl;
            return false;
        }
        else
        {
            std::cout << "[msmemscope] Info: create dbfile " << filePath << "." << std::endl;
        }
        sqlite3* db = nullptr;
        int rc = sqlite3_open(filePath.c_str(), &db);
        if (rc != SQLITE_OK)
        {
            std::cout << "[msmemscope] Error: Cannot open database: " << sqlite3_errmsg(db) << std::endl;
            if (db != nullptr)
            {
                sqlite3_close(db);
            }
            return false;
        }
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        if (CreateDbTable(db, tableCreateSql))
        {
            std::cout << "[msmemscope] Info: create dbtable " << tableName << " in " << filePath << "." << std::endl;
            *filefp = db;
        }
        else
        {
            sqlite3_close(db);
            return false;
        }
    }
    else if (!TableExists(*filefp, tableName) && !CreateDbTable(*filefp, tableCreateSql))
    {
        return false;
    }
    return true;
}

bool FileCreateManager::CreateLogFile(FILE** filefp, const char* taskDir, char* logFilePath, size_t size)
{
    std::lock_guard<std::mutex> lock(createLogFileMutex_);
    if (*filefp == nullptr)
    {
        std::string fileName = "msmemscope_" + GetDateStr() + ".log";
        std::string dirPath = projectDir_ + "/" + taskDir;
        std::string filePath = dirPath + "/" + fileName;
        FILE* fp = CreateFile(dirPath, fileName, DEFAULT_UMASK_FOR_LOG_FILE);
        if (fp == nullptr)
        {
            std::cout << "[msmemscope] Error: Create log file failed: " << filePath << std::endl;
            return false;
        }
        else
        {
            std::cout << "[msmemscope] Info: logging into file " << filePath << std::endl;
            *filefp = fp;
            if (strncpy_s(logFilePath, size, filePath.c_str(), filePath.length()) != EOK)
            {
                std::cout << "[msmemscope] Error: Save log file path failed: " << filePath << std::endl;
            }
        }
    }
    return true;
}

bool FileCreateManager::CreateConfigFile(FILE** filefp, const std::string& fileName, std::string& configFilePath)
{
    std::lock_guard<std::mutex> lock(createConfigFileMutex_);
    if (*filefp == nullptr)
    {
        std::string realName = fileName + ".json";
        configFilePath = projectDir_ + "/" + realName;
        FILE* fp = CreateFile(projectDir_, realName, DEFAULT_UMASK_FOR_CONFIG_FILE);
        if (fp == nullptr)
        {
            std::cout << "[msmemscope] Error: Create config file failed: " << configFilePath << std::endl;
            return false;
        }
        else
        {
            std::cout << "[msmemscope] Info: Config into file " << configFilePath << std::endl;
            *filefp = fp;
        }
    }
    return true;
}

bool FileCreateManager::CreateDbTable(sqlite3* filefp, const std::string& tableCreateSql)
{
    sqlite3_busy_timeout(filefp, MemScope::SQLITE_TIME_OUT);
    int rc = sqlite3_exec(filefp, tableCreateSql.c_str(), nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cout << "[msmemscope] Error: Create SQLTable error: " << sqlite3_errmsg(filefp) << std::endl;
        return false;
    }
    return true;
}

std::string FileCreateManager::GetProjectDir() const { return projectDir_; }

void FileCreateManager::SetProjectDir(std::string dirPath)
{
    projectDir_ = std::move(dirPath);
    // 显式指定的目录钉住projectDir_，后续RefreshOutputDir不再重算（见RefreshOutputDir注释）
    projectDirPinned_ = true;
}

}  // namespace Utility
