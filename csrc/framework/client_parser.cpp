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

#include "client_parser.h"

#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <unordered_map>

#include "bit_field.h"
#include "cli_logo.h"
#include "command.h"
#include "event_trace/python_trace.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#include "file.h"
#include "path.h"
#include "securec.h"
#include "string_validator.h"
#include "ustring.h"
#include "vallina_symbol.h"

namespace MemScope
{

enum class OptVal : int32_t
{
    SELECT_STEPS = 0,
    CALL_STACK,
    COMPARE,
    WATCH,
    INPUT,
    OUTPUT,
    DATA_TRACE_LEVEL,
    LOG_LEVEL,
    COLLECT_MODE,
    EVENT_TRACE_TYPE,
    DATA_FORMAT,
    ANALYSIS,
    DEVICE,
    // ↓ 以下为 CLI 规范对齐新增
    INPUT_PATH,
    OUTPUT_PATH,
    FORMAT,
    VERBOSE,
    QUIET,
    DEBUG,
    INPUT_ALIAS,
    OUTPUT_ALIAS,
    DATA_FORMAT_ALIAS,
};
constexpr uint16_t INPUT_STR_MAX_LEN = 4096;

void ShowDescription()
{
    std::cout << "msmemscope (MindStudio MemScope) — Ascend NPU memory debugging and profiling tool." << std::endl;
}

void ShowHelpInfo()
{
    std::cout << "Description:" << std::endl;
    ShowDescription();
    std::cout << std::endl
              << "Usage:" << std::endl
              << "  msmemscope [options] -- <prog> [args]" << std::endl
              << "  source msmemscope --load-api-env" << std::endl
              << "  source msmemscope --unload-api-env" << std::endl
              << std::endl
              << "Environment setup (must be sourced):" << std::endl
              << "      --load-api-env            Set LD_PRELOAD and LD_LIBRARY_PATH for Python API usage" << std::endl
              << "      --unload-api-env          Clear msmemscope entries from LD_PRELOAD and LD_LIBRARY_PATH"
              << std::endl
              << std::endl
              << "General options:" << std::endl
              << "  -h, --help                    Show this help message" << std::endl
              << "  -V, --version                 Show version information" << std::endl
              << "  -v, --verbose                 Equivalent to --log-level=debug" << std::endl
              << "  -q, --quiet                   Equivalent to --log-level=error" << std::endl
              << "      --debug                   Equivalent to --log-level=debug" << std::endl
              << std::endl
              << "Collection options:" << std::endl
              << "      --level <LEVEL>           Data trace level: op | kernel [default: op]" << std::endl
              << "      --events <EVENT>          Trace event types: alloc | free | launch | access | traceback | none"
              << std::endl
              << "                                (comma-separated, default: alloc,free,launch; none = no events)" << std::endl
              << "      --steps <STEP>            Steps to collect memory info (comma-separated, max 5)" << std::endl
              << "      --call-stack <TYPE>[:<DEPTH>]  Enable C/Python call stack (c[:depth], python[:depth],"
              << std::endl
              << "                                comma-separated, default depth: 50)" << std::endl
              << "      --collect-mode <MODE>     Collect mode: immediate | deferred [default: immediate]" << std::endl
              << "      --device <DEVICE>         Device(s): npu | npu:<SLOT> | cpu (comma-separated, default: npu)"
              << std::endl
              << std::endl
              << "Analysis options:" << std::endl
              << "      --analysis <TYPE>         Analysis methods: leaks | decompose | inefficient | oom[:K] | none"
              << std::endl
              << "                                (comma-separated, default: leaks)" << std::endl
              << "                                oom[:K]: OOM detailed analysis, K for top-K records" << std::endl
              << "                                (default: 10, range: [1,1000])" << std::endl
              << "      --compare                 Enable memory comparison mode" << std::endl
              << "      --watch <CONFIG>          Watch mode: start[:outid],end[,full-content]" << std::endl
              << std::endl
              << "Input / Output options:" << std::endl
              << "  -i, --input-path <DIR>        Path(s) to input compare files (comma-separated, used with --compare)"
              << std::endl
              << "  -o, --output-path <DIR>       Output directory [default: ./memscopeDumpResults]" << std::endl
              << "      --format <FORMAT>         Data format: csv | db [default: csv]" << std::endl
              << std::endl
              << "Other:" << std::endl
              << "      --log-level <LEVEL>       Log level: debug | info | warning | error [default: info]"
              << std::endl
              << std::endl
              << "Examples:" << std::endl
              << "  # Collect memory events with default options" << std::endl
              << "  msmemscope -- python train.py" << std::endl
              << std::endl
              << "  # Kernel-level tracing with call stacks" << std::endl
              << "  msmemscope --level=kernel --call-stack=c:20,python:10 -- python train.py" << std::endl
              << std::endl
              << "  # Memory comparison" << std::endl
              << "  msmemscope --compare --input-path=./baseline,./target" << std::endl
              << std::endl
              << "Output:" << std::endl
              << "  Results are written to <output-path> (default: ./memscopeDumpResults)." << std::endl;
}

void ShowVersion()
{
    ShowDescription();
    std::cout << std::endl << "msmemscope version " << __BUILD_VERSION__ << "-" << __MSLEAKS_COMMIT_ID__ << std::endl;
}

bool UserCommandPrecheck(const UserCommand &userCommand)
{
    if (userCommand.config.enableCompare != userCommand.config.inputCorrectPaths)
    {
        std::cout << "Please use compare command with correct input paths!" << std::endl;
        return false;
    }

    if (!userCommand.config.outputCorrectPaths)
    {
        std::cout << "Please use correct output path!" << std::endl;
        return false;
    }
    return true;
}

static void PrintDeprecation(const std::string &deprecatedField, const std::string &replacementField)
{
    // 弃用告警打印到 stderr，避免污染 stdout 的常规输出
    std::cerr << "[msmemscope] Deprecation: '" << deprecatedField << "' is deprecated, "
              << "use '" << replacementField << "' instead." << std::endl;
}

void SetEventDefaultConfig(Config &config)
{
    std::vector<std::pair<EventType, EventType>> eventsMatchLists = {{EventType::ALLOC_EVENT, EventType::FREE_EVENT},
                                                                     {EventType::FREE_EVENT, EventType::ALLOC_EVENT},
                                                                     {EventType::ACCESS_EVENT, EventType::ALLOC_EVENT},
                                                                     {EventType::ACCESS_EVENT, EventType::FREE_EVENT}};

    BitField<decltype(config.eventType)> eventTypeBit(config.eventType);
    for (auto it : eventsMatchLists)
    {
        if (eventTypeBit.checkBit(static_cast<size_t>(it.first)))
        {
            eventTypeBit.setBit(static_cast<size_t>(it.second));
        }
    }
    config.eventType = eventTypeBit.getValue();
}

void SetAnalysisDefaultConfig(Config &config)
{
    std::vector<std::pair<AnalysisType, EventType>> analysisMatchLists = {
        {AnalysisType::LEAKS_ANALYSIS, EventType::ALLOC_EVENT},
        {AnalysisType::LEAKS_ANALYSIS, EventType::FREE_EVENT},
        {AnalysisType::DECOMPOSE_ANALYSIS, EventType::ALLOC_EVENT},
        {AnalysisType::DECOMPOSE_ANALYSIS, EventType::FREE_EVENT},
        {AnalysisType::DECOMPOSE_ANALYSIS, EventType::ACCESS_EVENT},
        {AnalysisType::INEFFICIENCY_ANALYSIS, EventType::ALLOC_EVENT},
        {AnalysisType::INEFFICIENCY_ANALYSIS, EventType::FREE_EVENT},
        {AnalysisType::INEFFICIENCY_ANALYSIS, EventType::ACCESS_EVENT},
        {AnalysisType::INEFFICIENCY_ANALYSIS, EventType::LAUNCH_EVENT},
        {AnalysisType::OOM_ANALYSIS, EventType::ALLOC_EVENT},
        {AnalysisType::OOM_ANALYSIS, EventType::FREE_EVENT}};

    BitField<decltype(config.analysisType)> analysisTypeBit(config.analysisType);
    BitField<decltype(config.eventType)> eventTypeBit(config.eventType);
    for (auto it : analysisMatchLists)
    {
        if (analysisTypeBit.checkBit(static_cast<size_t>(it.first)))
        {
            eventTypeBit.setBit(static_cast<size_t>(it.second));
        }
    }
    config.eventType = eventTypeBit.getValue();
}

void SetEffectiveConfig(Config &config)
{
    // 调用SetConfig后，设置为EFFECTIVE
    // 1.状态不为空桩，使能MemScope功能
    // 2.只能设置一次的变量生效，不能在被python config接口改变
    config.isEffective = true;
    return;
}

void DoUserCommand(UserCommand userCommand)
{
    if (userCommand.printHelpInfo)
    {
        ShowHelpInfo();
        return;
    }

    if (userCommand.printVersionInfo)
    {
        PrintLogo();
        ShowVersion();
        return;
    }

    if (!UserCommandPrecheck(userCommand))
    {
        ShowHelpInfo();
        return;
    }

    PrintLogo();
    ConfigManager::Instance().SetConfig(userCommand.config);

    Command command{userCommand};
    command.Exec();
}

void ClientParser::Interpretor(int32_t argc, char **argv)
{
    auto userCommand = Parse(argc, argv);
    DoUserCommand(userCommand);
}

std::vector<option> GetLongOptArray()
{
    std::vector<option> longOpts = {
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, 'V'},  // -V 表示版本，-v 预留给 --verbose
        {"verbose", no_argument, nullptr, 'v'},  // 新增：-v 现在表示 verbose
        {"quiet", no_argument, nullptr, 'q'},    // 新增
        {"debug", no_argument, nullptr, static_cast<int32_t>(OptVal::DEBUG)},
        {"steps", required_argument, nullptr, static_cast<int32_t>(OptVal::SELECT_STEPS)},
        {"call-stack", required_argument, nullptr, static_cast<int32_t>(OptVal::CALL_STACK)},
        {"analysis", required_argument, nullptr, static_cast<int32_t>(OptVal::ANALYSIS)},
        {"compare", no_argument, nullptr, static_cast<int32_t>(OptVal::COMPARE)},
        {"watch", required_argument, nullptr, static_cast<int32_t>(OptVal::WATCH)},
        // 路径参数：新标准名注册短选项，旧名保留为隐藏别名
        {"input-path", required_argument, nullptr, 'i'},
        {"input", required_argument, nullptr, static_cast<int32_t>(OptVal::INPUT_ALIAS)},
        {"output-path", required_argument, nullptr, 'o'},
        {"output", required_argument, nullptr, static_cast<int32_t>(OptVal::OUTPUT_ALIAS)},
        // 格式参数
        {"format", required_argument, nullptr, static_cast<int32_t>(OptVal::FORMAT)},
        {"data-format", required_argument, nullptr, static_cast<int32_t>(OptVal::DATA_FORMAT_ALIAS)},
        {"level", required_argument, nullptr, static_cast<int32_t>(OptVal::DATA_TRACE_LEVEL)},
        {"events", required_argument, nullptr, static_cast<int32_t>(OptVal::EVENT_TRACE_TYPE)},
        {"log-level", required_argument, nullptr, static_cast<int32_t>(OptVal::LOG_LEVEL)},
        {"collect-mode", required_argument, nullptr, static_cast<int32_t>(OptVal::COLLECT_MODE)},
        {"device", required_argument, nullptr, static_cast<int32_t>(OptVal::DEVICE)},
        {nullptr, 0, nullptr, 0},
    };
    return longOpts;
}

std::string GetShortOptString(const std::vector<option> &longOptArray)
{
    // 根据long option string生成short option string
    // 支持大小写短选项（如 -V），required_argument 选项需追加冒号后缀
    std::string shortOpt;
    for (const auto &opt : longOptArray)
    {
        if (opt.name == nullptr)
        {
            break;
        }
        if (opt.flag == nullptr && std::isprint(opt.val))
        {
            shortOpt.append(1, static_cast<char>(opt.val));
            if (opt.has_arg == required_argument)
            {
                shortOpt.append(1, ':');
            }
        }
    }
    return shortOpt;
}

void ParseSelectSteps(const std::string &param, Config &config, bool &printHelpInfo)
{
    std::string dividePattern = "，,";
    std::vector<std::string> tokens = Utility::SplitString(param, dividePattern);
    auto it = tokens.begin();
    auto end = tokens.end();

    config.stepList.stepCount = 0;
    Utility::IntValidateRule verRule;
    verRule.minValue = 1;

    auto parseFailed = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: invalid steps input." << std::endl;
        printHelpInfo = true;
    };

    while (it != end)
    {
        SelectedStepList &stepListInfo = config.stepList;

        if (stepListInfo.stepCount >= SELECTED_STEP_MAX_NUM)
        {
            return parseFailed();
        }
        std::string step = *it;
        if (!step.empty())
        {
            if (!Utility::IsValidInteger(step, verRule))
            {
                return parseFailed();
            }
            if (!Utility::StrToUint32(stepListInfo.stepIdList[stepListInfo.stepCount], step))
            {
                return parseFailed();
            }
            stepListInfo.stepCount++;
        }

        it++;
    }

    return;
}

void ParseAnalysis(const std::string &param, Config &config, bool &printHelpInfo)
{
    std::string dividePattern = "，,";
    std::vector<std::string> tokens = Utility::SplitString(param, dividePattern);
    auto it = tokens.begin();
    auto end = tokens.end();

    auto parseFailed = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: invalid analysis type input." << std::endl;
        printHelpInfo = true;
    };

    BitField<decltype(config.analysisType)> analysisTypeBit;

    std::unordered_map<std::string, AnalysisType> analysisMp = {
        {"leaks", AnalysisType::LEAKS_ANALYSIS},
        {"decompose", AnalysisType::DECOMPOSE_ANALYSIS},
        {"inefficient", AnalysisType::INEFFICIENCY_ANALYSIS},
    };
    while (it != end)
    {
        std::string analysisMethod = *it;
        if (!analysisMethod.empty())
        {
            if (analysisMethod == "none")
            {
                // none 关键字：清空所有分析类型
                if (tokens.size() > 1)
                {
                    std::cout << "[msmemscope] Warn: 'none' mixed with other analysis types, "
                              << "all analysis cleared." << std::endl;
                }
                config.analysisType = 0;
                return;
            }
            if (analysisMethod.rfind("oom", 0) == 0)
            {
                analysisTypeBit.setBit(static_cast<size_t>(AnalysisType::OOM_ANALYSIS));
                size_t colonPos = analysisMethod.find(':');
                if (colonPos != std::string::npos)
                {
                    std::string kStr = analysisMethod.substr(colonPos + 1);
                    uint32_t kVal = 0;
                    Utility::IntValidateRule verRule;
                    verRule.minValue = 1;
                    verRule.maxValue = 1000;
                    if (kStr.empty() || !Utility::IsValidInteger(kStr, verRule) || !Utility::StrToUint32(kVal, kStr))
                    {
                        std::cout << "[msmemscope] Error: invalid OOM top-K value '" << kStr
                                  << "', valid range is [1, 1000]." << std::endl;
                        printHelpInfo = true;
                        return;
                    }
                    config.oomTopK = static_cast<uint16_t>(kVal);
                }
            }
            else
            {
                auto analysisIt = analysisMp.find(analysisMethod);
                if (analysisIt == analysisMp.end())
                {
                    return parseFailed();
                }
                analysisTypeBit.setBit(static_cast<size_t>(analysisIt->second));
            }
        }
        it++;
    }

    config.analysisType = analysisTypeBit.getValue();
    return;
}

static bool CheckIsValidDepthInfo(const std::string &param, Config &config)
{
    size_t pos = param.find(':');
    std::string callType = param.substr(0, pos);
    uint32_t depth = DEFAULT_CALL_STACK_DEPTH;

    if (pos != std::string::npos)
    {
        std::string depthStr = param.substr(pos + 1);
        Utility::IntValidateRule verRule;
        verRule.maxValue = 1000;
        if (depthStr.empty() || !Utility::IsValidInteger(depthStr, verRule) || !Utility::StrToUint32(depth, depthStr))
        {
            return false;
        }
    }

    if (callType == "python")
    {
        config.enablePyStack = true;
        config.pyStackDepth = depth;
    }
    else if (callType == "c")
    {
        config.enableCStack = true;
        config.cStackDepth = depth;
    }
    else
    {
        return false;
    }

    return true;
}

void ParseCallstack(const std::string &param, Config &config, bool &printHelpInfo)
{
    if (param == "")
    {
        config.enableCStack = false;
        config.enablePyStack = false;
        config.cStackDepth = 0;
        config.pyStackDepth = 0;
        return;
    }

    std::string dividePattern = "，,";
    std::vector<std::string> tokens = Utility::SplitString(param, dividePattern);
    auto it = tokens.begin();
    auto end = tokens.end();

    auto parseFailed = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: invalid call-stack depth input." << std::endl;
        printHelpInfo = true;
    };

    while (it != end)
    {
        std::string depthStr = *it;
        if (!depthStr.empty() && !CheckIsValidDepthInfo(depthStr, config))
        {
            return parseFailed();
        }
        it++;
    }
    return;
}

static void ParseInputPaths(const std::string param, UserCommand &userCommand)
{
    if (param.length() > INPUT_STR_MAX_LEN)
    {
        std::cout << "[msmemscope] Error: Parameter --input length exceeds the maximum length:" << INPUT_STR_MAX_LEN
                  << "." << std::endl;
        return;
    }

    std::string pattern = "，,";
    std::vector<std::string> tokens = Utility::SplitString(param, pattern);
    auto it = tokens.begin();
    auto end = tokens.end();

    while (it != end)
    {
        std::string path = *it;
        if (!path.empty() && Utility::CheckIsValidInputPath(path) && Utility::IsFileSizeSafe(path))
        {
            userCommand.inputPaths.emplace_back(path);
        }
        it++;
    }

    if (userCommand.inputPaths.size() != PATHSIZE)
    {
        std::cout << "[msmemscope] Error: invalid paths input." << std::endl;
        userCommand.printHelpInfo = true;
    }
    else
    {
        userCommand.config.inputCorrectPaths = true;
    }
}

void ParseOutputPath(const std::string param, Config &config, bool &printHelpInfo)
{
    if (param.length() > PATH_MAX)
    {
        std::cout << "[msmemscope] Error: Parameter --output length exceeds the maximum length:" << PATH_MAX
                  << " output path will be set to default(./memscopeDumpResults)." << std::endl;
        return;
    }
    if (Utility::Strip(param).length() == 0)
    {
        config.outputCorrectPaths = false;
        std::cout << "[msmemscope] Warn: empty output path." << std::endl;
        return;
    }

    auto parseFailed = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: invalid output path." << std::endl;
        std::cout << "Please use correct output path!" << std::endl;
        printHelpInfo = true;
    };

    Utility::Path path = Utility::Path{param};
    Utility::Path realPath = path.Resolved();
    std::string pathStr = realPath.ToString();

    std::string pattern = "(\\.|/|_|-|\\s|[~0-9a-zA-Z]|[\u4e00-\u9fa5])+";
    if (!Utility::CheckIsValidOutputPath(pathStr) || !Utility::IsValidOutputPath(pathStr))
    {
        return parseFailed();
    }

    if (strncpy_s(config.outputDir, sizeof(config.outputDir), pathStr.c_str(), sizeof(config.outputDir) - 1) != EOK)
    {
        std::cout << "[msmemscope] Error: strncpy dirpath FAILED" << std::endl;
        return;
    }

    config.outputDir[sizeof(config.outputDir) - 1] = '\0';
    return;
}

void ParseDataLevel(const std::string param, Config &config, bool &printHelpInfo)
{
    std::string dividePattern = "，,";
    std::vector<std::string> tokens = Utility::SplitString(param, dividePattern);
    auto it = tokens.begin();
    auto end = tokens.end();

    std::string pattern = "^(0|1|op|kernel)$";

    auto parseFailed = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: invalid data trace level input." << std::endl;
        printHelpInfo = true;
    };

    BitField<decltype(config.levelType)> levelBit;

    while (it != end)
    {
        std::string level = *it;
        if (!level.empty())
        {
            if (!Utility::IsValidDataLevel(level))
            {
                return parseFailed();
            }
            if (level == "0" || level == "1")
            {
                // 兼容旧值：0→op, 1→kernel，命中时触发弃用告警
                PrintDeprecation("--level=" + level, level == "0" ? "--level=op" : "--level=kernel");
            }
            if (level == "0" || level == "op")
            {
                levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
            }
            else if (level == "1" || level == "kernel")
            {
                levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
            }
        }
        it++;
    }

    config.levelType = levelBit.getValue();
    return;
}

void ParseEventTraceType(const std::string param, Config &config, bool &printHelpInfo)
{
    std::string dividePattern = "，,";
    std::string traceConfig = "traceback";
    bool setTraceBack = false;
    std::vector<std::string> tokens = Utility::SplitString(param, dividePattern);
    auto it = tokens.begin();
    auto end = tokens.end();

    auto parseFailed = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: invalid event trace type input." << std::endl;
        printHelpInfo = true;
    };

    BitField<decltype(config.eventType)> eventsTypeBit;

    std::unordered_map<std::string, EventType> eventsMp = {{"alloc", EventType::ALLOC_EVENT},
                                                           {"free", EventType::FREE_EVENT},
                                                           {"launch", EventType::LAUNCH_EVENT},
                                                           {"access", EventType::ACCESS_EVENT}};
    while (it != end)
    {
        std::string event = *it;
        if (event.empty())
        {
            it++;
            continue;
        }

        if (event == "none")
        {
            // none 关键字：清空所有事件类型，忽略其他 token
            if (tokens.size() > 1)
            {
                std::cout << "[msmemscope] Warn: 'none' mixed with other event types, "
                          << "all events cleared." << std::endl;
            }
            config.eventType = 0;
            config.dumpEventType = 0;
            // traceback 状态一并清理
            if (PythonTrace::GetInstance().IsTraceActive())
            {
                PythonTrace::GetInstance().Stop();
            }
            return;
        }

        if (eventsMp.count(event))
        {
            eventsTypeBit.setBit(static_cast<size_t>(eventsMp[event]));
        }
        else if (event == traceConfig)
        {
            setTraceBack = true;
            if (!PythonTrace::GetInstance().IsTraceActive())
            {
                PythonTrace::GetInstance().Start();
            }
        }
        else
        {
            return parseFailed();
        }

        it++;
    }
    // config中无traceback则判断目前是否在trace中，如在，则关闭。
    if (PythonTrace::GetInstance().IsTraceActive() && !setTraceBack)
    {
        PythonTrace::GetInstance().Stop();
    }

    config.eventType = eventsTypeBit.getValue();
    // 记录用户原始事件配置作为落盘配置，不受后续联动规则影响
    config.dumpEventType = eventsTypeBit.getValue();
    return;
}

static bool ParseWatchStartConfig(const std::string param, Config &config, size_t &pos)
{
    // 解析可选的 [start[:outid]] 部分
    size_t comma = param.find(',', pos);
    if (comma == std::string::npos)
    {
        return false;
    }

    std::string startPart = param.substr(pos, comma - pos);

    // 检查是否有 outid 部分（冒号后）
    size_t colon = startPart.find(':');
    if (colon != std::string::npos)
    {
        std::string start = startPart.substr(0, colon);
        std::string outidStr = startPart.substr(colon + 1);
        if (start.empty() || outidStr.empty())  // 出现冒号必须有start和outid
        {
            return false;
        }
        if (outidStr[0] == '0' && outidStr.size() > 1)  // outidStr不能出现前导0
        {
            return false;
        }
        auto ret =
            strncpy_s(config.watchConfig.start, WATCH_OP_DIR_MAX_LENGTH, start.c_str(), WATCH_OP_DIR_MAX_LENGTH - 1);
        if (ret != EOK)
        {
            return false;
        }
        uint32_t outId = 0;
        if (!Utility::StrToUint32(outId, outidStr))
        {
            return false;
        }
        config.watchConfig.outputId = outId;
    }
    else
    {
        // 只有 start 没有 outid
        auto ret = strncpy_s(config.watchConfig.start, WATCH_OP_DIR_MAX_LENGTH, startPart.c_str(),
                             WATCH_OP_DIR_MAX_LENGTH - 1);
        if (ret != EOK)
        {
            return false;
        }
    }

    pos = comma + 1;
    return true;
}

static bool ParseWatchEndConfig(const std::string param, Config &config, size_t &pos)
{
    // 解析必需的 end 部分
    size_t comma = param.find(',', pos);
    std::string end;
    if (comma == std::string::npos)
    {
        end = param.substr(pos);
        pos = param.length();
    }
    else
    {
        end = param.substr(pos, comma - pos);
        pos = comma + 1;
    }
    if (end.empty())
    {
        return false;
    }
    auto ret = strncpy_s(config.watchConfig.end, WATCH_OP_DIR_MAX_LENGTH, end.c_str(), WATCH_OP_DIR_MAX_LENGTH - 1);
    if (ret != EOK)
    {
        return false;
    }

    return true;
}

void ParseWatchConfig(const std::string param, Config &config, bool &printHelpInfo)
{
    size_t pos = 0;
    size_t len = param.length();

    auto parseFailed = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: invalid watch config." << std::endl;
        printHelpInfo = true;
    };

    if (!ParseWatchStartConfig(param, config, pos))
    {
        return parseFailed();
    }

    if (!ParseWatchEndConfig(param, config, pos))
    {
        return parseFailed();
    }
    // 解析可选的 full-content
    if (pos < len)
    {
        if (param.substr(pos) == "full-content")
        {
            config.watchConfig.fullContent = true;
        }
        else
        {
            return parseFailed();
        }
    }

    config.watchConfig.isWatched = true;

    return;
}

static void ParseLogLv(const std::string &param, UserCommand &userCommand)
{
    const std::map<std::string, LogLv> logLevelMap = {
        {"debug", LogLv::DEBUG}, {"info", LogLv::INFO}, {"warning", LogLv::WARN}, {"warn", LogLv::WARN},  // 兼容别名
        {"error", LogLv::ERROR},
    };
    auto it = logLevelMap.find(param);
    if (it == logLevelMap.end())
    {
        std::cout << "[msmemscope] Error: --log-level param is invalid. "
                  << "Log level: debug|info|warning|error." << std::endl;
        userCommand.printHelpInfo = true;
        return;
    }
    if (param == "warn")
    {
        PrintDeprecation("--log-level=warn", "--log-level=warning");
    }
    userCommand.config.logLevel = static_cast<uint8_t>(it->second);
}

void ParseDataFormat(const std::string &param, Config &config, bool &printHelpInfo)
{
    const std::map<std::string, DataFormat> dataFormatMap = {
        {"csv", DataFormat::CSV},
        {"db", DataFormat::DB},
    };
    auto parseFailedFormat = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: --data-format param is invalid. "
                  << "DATA_FORMAT can only be set csv,db." << std::endl;
        printHelpInfo = true;
    };
    auto it = dataFormatMap.find(param);
    if (it == dataFormatMap.end())
    {
        return parseFailedFormat();
    }
    else
    {
        auto dataFormat = it->second;
        config.dataFormat = static_cast<uint8_t>(dataFormat);
    }

    return;
}

void ParseDevice(const std::string &param, Config &config, bool &printHelpInfo)
{
    std::string dividePattern = "，,";
    std::vector<std::string> tokens = Utility::SplitString(param, dividePattern);
    auto it = tokens.begin();
    auto end = tokens.end();

    auto parseFailed = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: invalid device." << std::endl;
        printHelpInfo = true;
    };

    BitField<decltype(config.npuSlots)> slotsBit;
    config.collectAllNpu = false;
    config.collectCpu = false;

    for (; it != end; it++)
    {
        std::string device = *it;
        if (device == "npu")
        {
            config.collectAllNpu = true;
        }
        else if (device == "cpu")
        {
            config.collectCpu = true;
        }
        else if (device.substr(0, 4) == "npu:")
        {
            std::string slot = device.substr(4);
            uint32_t slotNum = 0;
            if (!Utility::StrToUint32(slotNum, slot) ||
                slotNum >= std::numeric_limits<decltype(config.npuSlots)>::digits)
            {
                return parseFailed();
            }
            slotsBit.setBit(slotNum);
        }
        else if (device.empty())
        {
            continue;
        }
        else
        {
            return parseFailed();
        }
    }

    config.npuSlots = slotsBit.getValue();
    if (!config.collectAllNpu && config.npuSlots == 0 && !config.collectCpu)
    {
        return parseFailed();
    }

    return;
}

void ParseCollectMode(const std::string &param, Config &config, bool &printHelpInfo)
{
    auto parseFailed = [&printHelpInfo](void)
    {
        std::cout << "[msmemscope] Error: --collect-mode param is invalid. "
                  << "Collect mode can only be set to immediate,deferred." << std::endl;
        printHelpInfo = true;
    };
    if (param == "immediate")
    {
        config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    }
    else if (param == "deferred")
    {
        config.collectMode = static_cast<uint8_t>(CollectMode::DEFERRED);
    }
    else
    {
        return parseFailed();
    }

    return;
}

static void ResolveLogLevel(UserCommand &userCommand)
{
    // 日志等级冲突裁决（与参数出现顺序无关）：
    // 1. 显式给出 --log-level 时以它为准，快捷开关不覆盖
    // 2. 未显式指定时，多个快捷开关并存按"可见度最高者生效"：
    //    --verbose/-v/--debug 任一出现取 debug；仅有 --quiet/-q 时取 error
    if (userCommand.logLevelExplicitSet)
    {
        return;
    }
    if (userCommand.logLevelVerboseSet)
    {
        userCommand.config.logLevel = static_cast<uint8_t>(LogLv::DEBUG);
    }
    else if (userCommand.logLevelQuietSet)
    {
        userCommand.config.logLevel = static_cast<uint8_t>(LogLv::ERROR);
    }
}

void ParseUserCommand(const int32_t &opt, const std::string &param, UserCommand &userCommand)
{
    switch (opt)
    {
        case '?':
            std::cout << "[msmemscope] Error: unrecognized command " << std::endl;
            userCommand.printHelpInfo = true;
            break;
        case 'h':  // for --help
            userCommand.printHelpInfo = true;
            break;
        case 'V':  // for --version
            userCommand.printVersionInfo = true;
            break;
        case 'v':  // for --verbose
            userCommand.logLevelVerboseSet = true;
            break;
        case 'q':  // for --quiet
            userCommand.logLevelQuietSet = true;
            break;
        case 'i':  // for --input-path
            ParseInputPaths(param, userCommand);
            break;
        case 'o':  // for --output-path
            ParseOutputPath(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::SELECT_STEPS):
            ParseSelectSteps(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::ANALYSIS):
            ParseAnalysis(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::CALL_STACK):
            ParseCallstack(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::COMPARE):
            userCommand.config.enableCompare = true;
            break;
        case static_cast<int32_t>(OptVal::WATCH):
            ParseWatchConfig(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::INPUT_PATH):
            ParseInputPaths(param, userCommand);
            break;
        case static_cast<int32_t>(OptVal::OUTPUT_PATH):
            ParseOutputPath(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::FORMAT):
            ParseDataFormat(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::VERBOSE):
            userCommand.logLevelVerboseSet = true;
            break;
        case static_cast<int32_t>(OptVal::QUIET):
            userCommand.logLevelQuietSet = true;
            break;
        case static_cast<int32_t>(OptVal::DEBUG):
            userCommand.logLevelVerboseSet = true;
            break;
        case static_cast<int32_t>(OptVal::INPUT_ALIAS):
            PrintDeprecation("--input", "--input-path");
            ParseInputPaths(param, userCommand);
            break;
        case static_cast<int32_t>(OptVal::OUTPUT_ALIAS):
            PrintDeprecation("--output", "--output-path");
            ParseOutputPath(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::DATA_FORMAT_ALIAS):
            PrintDeprecation("--data-format", "--format");
            ParseDataFormat(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::DATA_TRACE_LEVEL):
            ParseDataLevel(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::EVENT_TRACE_TYPE):
            ParseEventTraceType(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::LOG_LEVEL):
            userCommand.logLevelExplicitSet = true;
            ParseLogLv(param, userCommand);
            break;
        case static_cast<int32_t>(OptVal::DEVICE):
            ParseDevice(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::COLLECT_MODE):
            ParseCollectMode(param, userCommand.config, userCommand.printHelpInfo);
            break;
        default:;
    }
}

void SetDefaultOutputDir(Config &config)
{
    if (config.outputDir[0] == '\0')
    {
        std::string pathStr;
        Utility::SetDirPath(pathStr, std::string(OUTPUT_PATH));
        if (strncpy_s(config.outputDir, sizeof(config.outputDir), pathStr.c_str(), sizeof(config.outputDir) - 1) != EOK)
        {
            std::cout << "[msmemscope] Error: strncpy dirpath FAILED" << std::endl;
            return;
        }

        config.outputDir[sizeof(config.outputDir) - 1] = '\0';
    }
}

void ClientParser::InitialConfig(Config &config)
{
    config.stepList.stepCount = 0;
    config.enableCompare = false;
    config.enableCStack = false;
    config.enablePyStack = false;
    config.inputCorrectPaths = false;
    config.outputCorrectPaths = true;
    config.cStackDepth = 0;
    config.pyStackDepth = 0;
    config.levelType = 1;
    config.dataFormat = static_cast<uint8_t>(DataFormat::CSV);
    config.logLevel = static_cast<uint8_t>(LogLv::INFO);
    config.collectAllNpu = true;
    config.collectCpu = false;
    config.collectMode = static_cast<uint8_t>(CollectMode::IMMEDIATE);
    config.isEffective = false;

    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    eventBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.eventType = eventBit.getValue();
    config.dumpEventType = eventBit.getValue();

    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    config.analysisType = analysisBit.getValue();

    config.watchConfig.isWatched = false;
    (void)memset_s(config.watchConfig.start, WATCH_OP_DIR_MAX_LENGTH, 0, WATCH_OP_DIR_MAX_LENGTH);
    (void)memset_s(config.watchConfig.end, WATCH_OP_DIR_MAX_LENGTH, 0, WATCH_OP_DIR_MAX_LENGTH);
    config.watchConfig.outputId = UINT32_MAX;
    config.watchConfig.fullContent = false;

    (void)memset_s(config.outputDir, PATH_MAX, 0, PATH_MAX);
    SetDefaultOutputDir(config);
}

UserCommand ClientParser::Parse(int32_t argc, char **argv)
{
    UserCommand userCommand;
    InitialConfig(userCommand.config);
    int32_t optionIndex = 0;
    int32_t opt = 0;
    auto longOptions = GetLongOptArray();
    std::string shortOptions = GetShortOptString(longOptions);
    optind = 0;
    while ((opt = getopt_long(argc, argv, shortOptions.c_str(), longOptions.data(), &optionIndex)) != -1)
    {
        // somehow optionIndex is not always correct for short option.
        // match it on our own.
        for (uint32_t i = 0; i < longOptions.size(); ++i)
        {
            if (longOptions[i].val == opt)
            {
                optionIndex = static_cast<int32_t>(i);
                break;
            }
        }
        std::string param;
        if (optarg)
        {
            param = optarg;
        }
        ParseUserCommand(opt, param, userCommand);
    }
    std::vector<std::string> userBinCmd;
    for (; optind < argc; optind++)
    {
        userBinCmd.emplace_back(argv[optind]);
    }
    userCommand.cmd = userBinCmd;

    ResolveLogLevel(userCommand);
    return userCommand;
}
}  // namespace MemScope
