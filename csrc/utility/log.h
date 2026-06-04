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
#ifndef LOG_H
#define LOG_H

#include <mutex>
#include <string>
#include <type_traits>

#include "config_info.h"
#include "cstring"
#include "file.h"
#include "umask_guard.h"
#include "utils.h"

namespace Utility
{

constexpr uint16_t LOG_BUF_SIZE = 32;
constexpr int16_t DOUBLE = 2;

inline bool operator<(MemScope::LogLv a, MemScope::LogLv b)
{
    using underlying = typename std::underlying_type<MemScope::LogLv>::type;
    return static_cast<underlying>(a) < static_cast<underlying>(b);
}

class Log
{
   public:
    static Log& GetLog(void);

    template <typename... Args>
    inline void Printf(const char* format, MemScope::LogLv lv, const char* fileName, const uint32_t line,
                       const Args&... args);
    void SetLogLevel(const MemScope::LogLv& logLevel);

   private:
    Log(void) = default;
    ~Log(void);
    Log(Log const&) = delete;
    Log& operator=(Log const&) = delete;
    void GetTimeStr(char* buf, size_t size) const;
    void CreateLogFile();
    const char* LvToString(MemScope::LogLv lv) const;
    inline int64_t LogSize() const
    {
        if (fp_ == nullptr)
        {
            return 0;
        }
        int rt = fseeko(fp_, 0L, SEEK_END);
        if (rt != 0)
        {
            return -1;
        }
        int64_t size = ftello64(fp_);
        return size;
    }

   private:
    MemScope::LogLv lv_{MemScope::LogLv::WARN};
    FILE* fp_{nullptr};
    char logFilePath_[PATH_MAX];
    mutable std::mutex mtx_;
    int64_t maxLogSize_ = 100L * 1024L * 1024L;  // 100M
};

inline const char* GetLogSourceFileName(const char* path)
{
    return (strrchr(path, '/')) ? (strrchr(path, '/') + 1) : path;
}

template <typename... Args>
void Log::Printf(const char* format, MemScope::LogLv lv, const char* fileName, const uint32_t line, const Args&... args)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (fp_ == nullptr)
    {
        CreateLogFile();
    }

    if (lv < lv_)
    {
        return;
    }

    char buf[LOG_BUF_SIZE];
    buf[LOG_BUF_SIZE - 1] = '\0';
    GetTimeStr(buf, LOG_BUF_SIZE);

    if (LogSize() > maxLogSize_)
    {
        std::cout << "[msmemscope] Warn: Log file size is too large, please check: " << logFilePath_ << std::endl;
        maxLogSize_ *= DOUBLE;
    }

    FILE* fp = fp_ != nullptr ? fp_ : stderr;
    fprintf(fp, "%s %s [%s:%u] ", buf, LvToString(lv), GetLogSourceFileName(fileName), line);
    fprintf(fp, format, args...);
    fprintf(fp, "\n");
    fflush(fp);
}

#define LOG_DEBUG(format, ...) \
    Utility::Log::GetLog().Printf(format, MemScope::LogLv::DEBUG, __FILE__, __LINE__, ##__VA_ARGS__)

#define LOG_INFO(format, ...) \
    Utility::Log::GetLog().Printf(format, MemScope::LogLv::INFO, __FILE__, __LINE__, ##__VA_ARGS__)

#define LOG_WARN(format, ...) \
    Utility::Log::GetLog().Printf(format, MemScope::LogLv::WARN, __FILE__, __LINE__, ##__VA_ARGS__)

#define LOG_ERROR(format, ...) \
    Utility::Log::GetLog().Printf(format, MemScope::LogLv::ERROR, __FILE__, __LINE__, ##__VA_ARGS__)

inline void SetLogLevel(const MemScope::LogLv& logLevel) { Log::GetLog().SetLogLevel(logLevel); }
}  // namespace Utility

#endif
