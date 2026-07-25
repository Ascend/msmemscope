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
};
constexpr uint16_t INPUT_STR_MAX_LEN = 4096;

void ShowDescription()
{
    std::cout << "msmemscope(MindStudio MemScope) is part of MindStudio Memory analysis Tools." << std::endl;
}

void ShowHelpInfo()
{
    ShowDescription();
    std::cout
        << std::endl
        << "Usage: msmemscope <option(s)> prog-and-args" << std::endl
        << std::endl
        << "  environment setup (run from shell, must be sourced):" << std::endl
        << "    source msmemscope --load-api-env          Set env variables for API usage." << std::endl
        << "    source msmemscope --unload-api-env        Clear msmemscope env entries." << std::endl
        << std::endl
        << "  basic user options, with default in [ ]:" << std::endl
        << "    -h --help                                Show this message." << std::endl
        << "    -v --version                             Show version." << std::endl
        << "    --level=<level>                          Set the data trace level." << std::endl
        << "                                             Level [1] for kernel, Level [0] for op(default:0)."
        << std::endl
        << "                                             You can choose both separated by, or ，." << std::endl
        << "    --events=event1,event2                   Set the trace event type." << std::endl
        << "                                             You can combine any of the following options:" << std::endl
        << "                                             alloc,free,launch,access,traceback" << std::endl
        << "                                             Use 'none' to disable all event types." << std::endl
        << "                                             (default:alloc,free,launch)." << std::endl
        << "    --steps=1,2,3,...                        Select the steps to collect memory information." << std::endl
        << "                                             The input step numbers need to be separated by, or ，."
        << std::endl
        << "                                             The maximum number of steps is 5." << std::endl
        << "    --call-stack=<c/python>[:<Depth>],...    Enable C,Python call stack collection for memory event."
        << std::endl
        << "                                             Select the maximum depth of the collected call stack([0,1000])"
        << std::endl
        << "                                             If no depth is specified, use default depth(50)." << std::endl
        << "                                             e.g. --call-stack=c:20,python:10" << std::endl
        << "                                                  --call-stack=python" << std::endl
        << "                                             The input params need to be separated by, or ，." << std::endl
        << "    --analysis                               Specify the analysis method(s) to enable (optional)."
        << std::endl
        << "                                             Available options:" << std::endl
        << "                                               - leaks : Enables memory leak detection (default)"
        << std::endl
        << "                                               - decompose : Enables memory categorization" << std::endl
        << "                                               - inefficient : Enables inefficient memory recognition"
        << std::endl
        << "                                               - oom[:K] : Enables OOM detailed analysis," << std::endl
        << "                                                 K for top-K records (default:10, range:[1,1000])"
        << std::endl
        << "                                               - none : Disables all analysis features" << std::endl
        << "                                             Leave empty to disable all analysis features." << std::endl
        << "    --compare                                Enable memory data comparison." << std::endl
        << "    --watch                                  Enable watch ability." << std::endl
        << "                                             e.g. [start[:outid]],end[,full-content]" << std::endl
        << "                                             The content within [] is optional" << std::endl
        << "    --input=path1,path2                      Paths to compare files, valid with compare command on."
        << std::endl
        << "                                             The input paths need to be separated by, or ，." << std::endl
        << "    --output=path                            The path to store the generated files." << std::endl
        << "    --log-level                              Set log level to <level> [warn]." << std::endl
        << "    --data-format=<db|csv>                   Set data format to <format> (default:csv)." << std::endl
        << "    --device=<npu|npu:x|cpu>,...             Set device(s) to collect, 'npu' for all npu," << std::endl
        << "                                             'npu:x' for npu in slot x, and 'cpu' for" << std::endl
        << "                                             host cpu (default:npu). Fields separated by,or，." << std::endl
        << "    --collect-mode=<immediate|deferred>      Set data collect mode. Default: immediate." << std::endl;
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
        {"version", no_argument, nullptr, 'v'},
        {"steps", required_argument, nullptr, static_cast<int32_t>(OptVal::SELECT_STEPS)},
        {"call-stack", required_argument, nullptr, static_cast<int32_t>(OptVal::CALL_STACK)},
        {"analysis", required_argument, nullptr, static_cast<int32_t>(OptVal::ANALYSIS)},
        {"compare", no_argument, nullptr, static_cast<int32_t>(OptVal::COMPARE)},
        {"watch", required_argument, nullptr, static_cast<int32_t>(OptVal::WATCH)},
        {"input", required_argument, nullptr, static_cast<int32_t>(OptVal::INPUT)},
        {"output", required_argument, nullptr, static_cast<int32_t>(OptVal::OUTPUT)},
        {"level", required_argument, nullptr, static_cast<int32_t>(OptVal::DATA_TRACE_LEVEL)},
        {"events", required_argument, nullptr, static_cast<int32_t>(OptVal::EVENT_TRACE_TYPE)},
        {"log-level", required_argument, nullptr, static_cast<int32_t>(OptVal::LOG_LEVEL)},
        {"collect-mode", required_argument, nullptr, static_cast<int32_t>(OptVal::COLLECT_MODE)},
        {"data-format", required_argument, nullptr, static_cast<int32_t>(OptVal::DATA_FORMAT)},
        {"device", required_argument, nullptr, static_cast<int32_t>(OptVal::DEVICE)},
        {nullptr, 0, nullptr, 0},
    };
    return longOpts;
}

std::string GetShortOptString(const std::vector<option> &longOptArray)
{
    // 根据long option string生成short option string
    std::string shortOpt;
    for (const auto &opt : longOptArray)
    {
        if (opt.name == nullptr)
        {
            break;
        }
        if ((opt.flag == nullptr) && (opt.val >= 'a') && (opt.val <= 'z'))
        {
            shortOpt.append(1, static_cast<char>(opt.val));
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
                    if (kStr.empty() || !Utility::IsValidInteger(kStr, verRule) ||
                        !Utility::StrToUint32(kVal, kStr))
                    {
                        std::cout << "[msmemscope] Warn: invalid OOM top-K value '" << kStr
                                  << "', using default 10." << std::endl;
                        config.oomTopK = 10;
                    }
                    else
                    {
                        config.oomTopK = static_cast<uint16_t>(kVal);
                    }
                }
            }
            else if (analysisMp.count(analysisMethod))
            {
                analysisTypeBit.setBit(static_cast<size_t>(analysisMp[analysisMethod]));
            }
            else
            {
                return parseFailed();
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
        {"info", LogLv::INFO},
        {"warn", LogLv::WARN},
        {"error", LogLv::ERROR},
    };
    auto it = logLevelMap.find(param);
    if (it == logLevelMap.end())
    {
        std::cout << "[msmemscope] Error: --log-level param is invalid. "
                  << "LOG_LEVEL can only be set info,warn,error." << std::endl;
        userCommand.printHelpInfo = true;
    }
    else
    {
        auto logLevel = it->second;
        userCommand.config.logLevel = static_cast<uint8_t>(logLevel);
    }
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
        case 'v':  // for --version
            userCommand.printVersionInfo = true;
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
        case static_cast<int32_t>(OptVal::INPUT):
            ParseInputPaths(param, userCommand);
            break;
        case static_cast<int32_t>(OptVal::OUTPUT):
            ParseOutputPath(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::DATA_TRACE_LEVEL):
            ParseDataLevel(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::EVENT_TRACE_TYPE):
            ParseEventTraceType(param, userCommand.config, userCommand.printHelpInfo);
            break;
        case static_cast<int32_t>(OptVal::LOG_LEVEL):
            ParseLogLv(param, userCommand);
            break;
        case static_cast<int32_t>(OptVal::DATA_FORMAT):
            ParseDataFormat(param, userCommand.config, userCommand.printHelpInfo);
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
    config.logLevel = static_cast<uint8_t>(LogLv::WARN);
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
        // 打印help或者version不进行其他操作
        if (userCommand.printHelpInfo || userCommand.printVersionInfo)
        {
            return userCommand;
        }
    }
    std::vector<std::string> userBinCmd;
    for (; optind < argc; optind++)
    {
        userBinCmd.emplace_back(argv[optind]);
    }
    userCommand.cmd = userBinCmd;

    return userCommand;
}
}  // namespace MemScope
