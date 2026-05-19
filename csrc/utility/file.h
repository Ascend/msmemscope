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

#ifndef FILE_H
#define FILE_H

#include <unistd.h>
#include <sys/stat.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <mutex>
#include <linux/limits.h>
#include "sqlite3.h"
#include "path.h"
#include "config_info.h"
#include "umask_guard.h"
#include "ustring.h"

namespace Utility {
    constexpr uint32_t DIRMOD = 0750;
    constexpr uint32_t DEFAULT_UMASK_FOR_CSV_FILE = 0177;
    constexpr uint32_t DEFAULT_UMASK_FOR_DB_FILE = 0177;
    constexpr uint32_t DEFAULT_UMASK_FOR_BIN_FILE = 0177;
    constexpr uint32_t DEFAULT_UMASK_FOR_LOG_FILE = 0177;
    constexpr uint32_t DEFAULT_UMASK_FOR_CONFIG_FILE = 0137;
    constexpr uint64_t MAX_INPUT_FILE_SIZE = 1UL << 33; // 8GB

    class FileCreateManager {
    public:
        static FileCreateManager& GetInstance(const std::string outputDir);

        // 多线程情况下调用,需加锁保护,这里已经在内部进行加锁保护，外部调用无需加锁
        bool CreateCsvFile(FILE **filefp, int32_t devId, std::string filePrefix, std::string taskDir,
            std::string headers);
        bool CreateDbFile(sqlite3 **filefp, int32_t devId, std::string filePrefix, std::string taskDir,
            std::string tableName, std::string tableCreateSql);
        bool CreateLogFile(FILE **filefp, const char* taskDir, char* logFilePath, size_t size);
        bool CreateConfigFile(FILE **filefp, std::string fileName, std::string& configFilePath);

        /// 创建文件
        FILE* CreateFileWithUmask(const std::string &path, const std::string &mode, mode_t mask);
        FILE* CreateFile(const std::string &dir, const std::string &name, mode_t mask);
        bool CreateDbTable(sqlite3 *filefp, std::string tableCreateSql);
        bool CreateDir();
        
        std::string GetProjectDir() const;
        void SetProjectDir(std::string dirPath);
    private:
        explicit FileCreateManager(const std::string outputDir);
        std::string projectDir_;
        std::string dbDateStr_{""};
        std::mutex createCsvFileMutex_;
        std::mutex createDbFileMutex_; 
        std::mutex createLogFileMutex_; 
        std::mutex createConfigFileMutex_; 
    };

    inline void SetDirPath(std::string& dirPath, const std::string& defaultDirPath)
    {
        if (dirPath.length() > PATH_MAX) {
            std::cout << "[msmemscope] Error: Path " << dirPath << " length exceeds the maximum length:"
                      << PATH_MAX << "." << std::endl;
            return;
        }
        if (dirPath.empty()) {
            Utility::Path path = Utility::Path{defaultDirPath};
            Utility::Path realPath = path.Resolved();
            if (realPath.ErrorOccured()) { return; }
            dirPath = realPath.ToString();
        }
    }

    inline bool MakeDir(const std::string& dirPath)
    {
        if (dirPath.empty()) {
            std::cout << "[msmemscope] Error: The directory path is empty." << std::endl;
            return false;
        }

        if (access(dirPath.c_str(), F_OK) != -1) {
            return true;
        }
        
        size_t pos = dirPath[0] != '/' ? 0 : 1;
        std::string tempPath = dirPath;
        
        // 遍历路径中的每一部分，递归创建父目录
        while ((pos = tempPath.find('/', pos)) != std::string::npos) {
            std::string partPath = tempPath.substr(0, pos);
            pos++;
            if (access(partPath.c_str(), F_OK) != -1) {
                continue;
            }
            if (mkdir(partPath.c_str(), DIRMOD) != 0) {
                std::cout << "[msmemscope] Error: Cannot create dir " << partPath << " ." << std::endl;
                return false;
            }
        }

        if (mkdir(dirPath.c_str(), DIRMOD) != 0) {
            std::cout << "[msmemscope] Error: Cannot create dir " << dirPath << " ." << std::endl;
            return false;
        }
        return true;
    }

    inline bool Exist(const std::string &path)
    {
        if (path.empty()) {
            std::cout << "[msmemscope] Error: The file path is empty." << std::endl;
            return false;
        }
        return access(path.c_str(), F_OK) == 0;
    }

    template <typename... Args>
    inline bool Fprintf(FILE* fp, const std::string &format, const Args& ...args)
    {
        if (fp == nullptr) {
            std::cout << "[msmemscope] Error: Fail to write data to file, fp is NULL" << std::endl;
            return false;
        }
        int fpRes = fprintf(fp, format.c_str(), args...);
        if (fpRes < 0) {
            std::cout << "[msmemscope] Error: Fail to write data to file, errno: " << fpRes << std::endl;
            return false;
        }
        return true;
    }

    // 输入+输出文件专属校验：文件大小
    inline bool IsFileSizeSafe(const std::string& path)
    {
        struct stat buffer;
        if (lstat(path.c_str(), &buffer) != 0) {
            std::cout << "[msmemscope] Error: Error getting file state for " << path << "." << std::endl;
            return false;
        }

        if (!S_ISREG(buffer.st_mode)) {
            std::cout << "[msmemscope] Error: File " << path << " is not a regular file." << std::endl;
            return false;
        }

        if (buffer.st_size > static_cast<int64_t>(MAX_INPUT_FILE_SIZE)) {
            std::cout << "[msmemscope] Error: File " << path << " exceeds maximum size ("
                      << MAX_INPUT_FILE_SIZE << " bytes)." << std::endl;
            return false;
        }
        return true;
    }

    /// 输出文件校验
    bool CheckFileBeforeCreate(const std::string &path);
    bool FileExists(const std::string& filePath);
    bool TableExists(sqlite3 *filefp, std::string tableName);

}

#endif