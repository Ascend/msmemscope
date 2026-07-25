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

#ifndef CONFIG_INFO_H
#define CONFIG_INFO_H

#include <linux/limits.h>

#include <string>
#include <vector>

namespace MemScope
{

constexpr uint8_t SELECTED_STEP_MAX_NUM = 5;  // 先设定最多指定5个step的信息采集
constexpr uint8_t DEFAULT_CALL_STACK_DEPTH = 50;
constexpr uint8_t SKIP_DEPTH = 2;
constexpr const char *MEMSCOPE_HEADERS =
    "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,"
    "Ptr,Attr,Call Stack(Python),Call Stack(C)\n";
constexpr const char *STEP_INTER_HEADERS =
    ",,,Base,Compare\nEvent,Name,Device Id,Allocated Memory(byte),"
    "Allocated Memory(byte),Diff Memory(byte)\n";
constexpr const char *TRACE_HEADERS = "FuncInfo,StartTime(ns),EndTime(ns),Thread Id,Process Id\n";
constexpr const char *WATCH_HASH_HEADERS = "Tensor info,Check data sum\n";
constexpr const char *OUTPUT_PATH = "memscopeDumpResults";
constexpr const char *TRACE_FILE = "trace";
constexpr const char *DUMP_DIR = "dump";
constexpr const char *WATCH_DUMP_DIR = "watch_dump";
constexpr const char *LOG_DIR = "msmemscope_logs";
constexpr const char *CONFIG_FILE = "config";
constexpr const char *COMPARE_DIR = "compare";
constexpr uint16_t WATCH_OP_DIR_MAX_LENGTH = 255;
constexpr const char *CSV_FILE_PREFIX = "memscope_dump_";
constexpr const char *PYTHON_TRACE_FILE_PREFIX = "python_trace_";
constexpr const char *MEMORY_COMPARE_FILE_PREFIX = "memory_compare_";
constexpr const char *WATCH_CSV_FILE_PREFIX = "watch_dump_data_check_sum_";
constexpr int SQLITE_TIME_OUT = 5000;
constexpr const char *EMPTY_DEVID = "";

// level type可以多选，每一种type占一个bit位
enum class LevelType : uint8_t
{
    LEVEL_OP = 0,
    LEVEL_KERNEL = 1,
};

// event type可以多选，每一种type占一个bit位
enum class EventType : uint8_t
{
    ALLOC_EVENT = 0,
    FREE_EVENT = 1,
    LAUNCH_EVENT = 2,
    ACCESS_EVENT = 3,
};

enum class DataFormat : uint8_t
{
    CSV = 0,
    DB = 1,
};

// analysis type可以多选，每一种type占一个bit位
enum class AnalysisType : uint8_t
{
    LEAKS_ANALYSIS = 0,
    DECOMPOSE_ANALYSIS = 1,
    INEFFICIENCY_ANALYSIS = 2,
    OOM_ANALYSIS = 3,
};

enum class LogLv : uint8_t
{
    DEBUG = 0,
    INFO,
    WARN,
    ERROR,
    COUNT,
};

enum class CollectMode : uint8_t
{
    IMMEDIATE = 0,
    DEFERRED,
};

struct SelectedStepList
{
    uint32_t stepIdList[SELECTED_STEP_MAX_NUM];
    uint8_t stepCount;
};

struct WatchConfig
{
    bool isWatched;
    bool fullContent;
    char start[WATCH_OP_DIR_MAX_LENGTH];
    char end[WATCH_OP_DIR_MAX_LENGTH];
    uint32_t outputId;
};

// 内存分析算法配置
struct Config
{
    SelectedStepList stepList;
    bool enableCompare;
    bool enableCStack;
    bool enablePyStack;
    uint32_t cStackDepth;
    uint32_t pyStackDepth;
    bool inputCorrectPaths;
    bool outputCorrectPaths;
    WatchConfig watchConfig;
    uint8_t levelType;
    uint8_t eventType;
    uint8_t dumpEventType;
    uint8_t analysisType;
    uint16_t oomTopK = 10;
    uint8_t logLevel;
    uint8_t collectMode;
    char outputDir[PATH_MAX];
    uint8_t dataFormat;
    bool collectAllNpu;
    bool collectCpu;
    /* 当前单机最多16卡，用32bits表示足够了，后续有需要再扩充 */
    uint32_t npuSlots;
    bool isEffective;
};

// 用于承载用户命令行参数的解析结果
struct UserCommand
{
    bool printHelpInfo{false};
    bool printVersionInfo{false};
    Config config;
    std::vector<std::string> cmd;
    std::vector<std::string> inputPaths;
    std::string outputPath;
};

}  // namespace MemScope
#endif
