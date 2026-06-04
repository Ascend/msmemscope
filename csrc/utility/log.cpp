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
#include "log.h"

#include <chrono>
#include <ctime>
#include <map>
#include <type_traits>

#include "file.h"
#include "json_manager.h"
#include "trace_manager/event_trace_manager.h"

namespace Utility
{

const char *Log::LvToString(MemScope::LogLv lv) const
{
    using underlying = typename std::underlying_type<MemScope::LogLv>::type;
    constexpr const char *lvString[static_cast<underlying>(MemScope::LogLv::COUNT)] = {"[DEBUG]", "[INFO] ", "[WARN] ",
                                                                                       "[ERROR]"};
    return lv < MemScope::LogLv::COUNT ? lvString[static_cast<underlying>(lv)] : "N";
}

Log &Log::GetLog(void)
{
    static Log instance;
    return instance;
}

Log::~Log()
{
    if (fp_ != nullptr)
    {
        fclose(fp_);
        fp_ = nullptr;
    }
}
void Log::GetTimeStr(char *buf, size_t size) const
{
    if (buf == nullptr)
    {
        return;
    }

    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm *tm = std::localtime(&time);
    std::strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm);
    return;
}

void Log::SetLogLevel(const MemScope::LogLv &logLevel) { lv_ = logLevel; }

void Log::CreateLogFile()
{
    if (!MemScope::ConfigManager::HasInited())
    {
        return;
    }
    MemScope::Config config = MemScope::GetConfig();
    Utility::FileCreateManager::GetInstance(config.outputDir)
        .CreateLogFile(&fp_, MemScope::LOG_DIR, logFilePath_, sizeof(logFilePath_));
}

}  // namespace Utility
