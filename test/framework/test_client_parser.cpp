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

#include <fcntl.h>
#include <getopt.h>
#include <sys/wait.h>
#include <unistd.h>

// 以下标准库头须先于 private->public 宏包含：python_trace.h 及其传递包含的头文件
// 不得在宏生效期内首次引入libstdc++头（否则其内部的private关键字会被宏替换）
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define private public
#include "client_parser.h"
// PythonTrace::active_ 为私有状态：解析事件的Stop分支需注入active观察覆盖，
// 沿用仓库既有 private->public 测试惯用法（布局不变，只放宽访问）
#include "event_trace/python_trace.h"
#include "log.h"
#include "bit_field.h"
#include "utility/file.h"
// MemoryCompare 单例的比对数据为私有成员：compare执行路径需在fork子进程内清空
// 残留数据(test_memory_compare.cpp同进程先行用例可能已填充)，保证走到确定分支
#include "analysis/memory_compare.h"
#undef private

using namespace MemScope;

// client_parser.cpp内部函数(未在client_parser.h声明)，按定义签名前置声明以直达边界分支
namespace MemScope
{
bool UserCommandPrecheck(const UserCommand &userCommand);
std::vector<option> GetLongOptArray();
std::string GetShortOptString(const std::vector<option> &longOptArray);
void ParseUserCommand(const int32_t &opt, const std::string &param, UserCommand &userCommand);
}  // namespace MemScope

void InvalidParamCheckHelpInfo(const char* invalidInput)
{
    std::vector<const char*> argv = {
        "msmemscope",
        invalidInput
    };
 
    optind = 1;
    ClientParser cliParser;
    testing::internal::CaptureStdout();
    cliParser.Interpretor(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStdout();
    ASSERT_NE(capture.find("Usage"), std::string::npos);
}
 
TEST(ClientParser, pass_help_parameter_expect_get_print_help_info)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--help"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, pass_help_parameter_expect_show_help_info)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--help"
    };

    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    testing::internal::CaptureStdout();
    cliParser.Interpretor(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStdout();
    ASSERT_NE(capture.find("Usage"), std::string::npos);
}

TEST(ClientParser, pass_valid_analysis_type_case_not_set)
{
    std::vector<const char*> argv = {
        "msmemscope",
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.analysisType, 1);
}

TEST(ClientParser, pass_valid_analysis_type_case_memscope)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--analysis=memscope"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.analysisType, 1);
}

TEST(ClientParser, pass_valid_analysis_type_case_decompose)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--analysis=decompose"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.analysisType, 2);
}

TEST(ClientParser, pass_valid_analysis_type_case_all)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--analysis=memscope,decompose"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 7);
}

TEST(ClientParser, pass_invalid_analysis_type_case)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--analysis=lekas"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, pass_valid_level_value_expect_level0)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--level=0"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.levelType, 1);
}

TEST(ClientParser, pass_valid_level_value_expect_level1)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--level=1"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.levelType, 2);
}

TEST(ClientParser, pass_valid_level_value_expect_level_0_and_level_1)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--level=0,1"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.levelType, 3);
}

TEST(ClientParser, pass_invalid_level_expect_show_help_info)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--level=3"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, pass_valid_event_type_case_not_set)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--level=3"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 7);
}

TEST(ClientParser, pass_valid_event_type_case_traceback)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=traceback"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
}

TEST(ClientParser, pass_valid_event_type_case_alloc)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=alloc"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 1);
}

TEST(ClientParser, pass_valid_event_type_case_free)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=free"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 2);
}

TEST(ClientParser, pass_valid_event_type_case_launch)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=launch"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 4);
}

TEST(ClientParser, pass_valid_event_type_case_access)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=access"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 8);
}

TEST(ClientParser, pass_valid_event_type_case_all)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=alloc,free,launch,access"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 15);
}

TEST(ClientParser, pass_invalid_event_type_case)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=alloc,free,launhc"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, pass_empty_prog_name_expect_get_empty_bin_cmd)
{
    std::vector<const char*> argv = {
        "msmemscope"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.cmd.empty());
}
 
TEST(ClientParser, pass_test_as_prog_expect_get_bin_cmd_test)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "test"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.cmd.empty());
    ASSERT_EQ(cmd.cmd[0], "test");
}
 
TEST(ClientParser, invalid_single_dash_option_expect_one_error)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "-log-file=log.txt"
    };
 
    ClientParser cliParser;
    testing::internal::CaptureStdout();
    cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStdout();
    const char *errorStr = "unrecognized command";
    size_t pos = capture.find(errorStr);
    ASSERT_NE(pos, std::string::npos) << capture;
    pos = capture.find(errorStr, pos + 1);
    ASSERT_EQ(pos, std::string::npos);
}

TEST(ClientParser, test_parse_select_steps)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--steps=2,3,123"
    };

    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.stepList.stepCount, 3);
    ASSERT_EQ(cmd.config.stepList.stepIdList[0], 2);
    ASSERT_EQ(cmd.config.stepList.stepIdList[1], 3);
    ASSERT_EQ(cmd.config.stepList.stepIdList[2], 123);

    argv = {
        "msmemscope",
        "--steps=2，3,234"
    };

    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.stepList.stepCount, 3);
    ASSERT_EQ(cmd.config.stepList.stepIdList[0], 2);
    ASSERT_EQ(cmd.config.stepList.stepIdList[1], 3);
    ASSERT_EQ(cmd.config.stepList.stepIdList[2], 234);

    argv = {
        "msmemscope",
        "--steps=4294967295"
    };

    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.stepList.stepCount, 1);
    ASSERT_EQ(cmd.config.stepList.stepIdList[0], 4294967295);
}

TEST(ClientParser, test_invalid_select_steps)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--steps=-1,0,3"
    };

    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    argv = {
        "msmemscope",
        "--steps=2:3.4"
    };

    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    argv = {
        "msmemscope",
        "--steps=4294967296"
    };

    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    argv = {
        "msmemscope",
        "--steps=429496729500"
    };

    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, test_exceed_five_steps_expect_print_help_info)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--steps=1,2,3,4,5,6"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, test_compare_dump_data)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--compare",
        "--input=path1,path2"
    };

    // Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.config.enableCompare);
    ASSERT_FALSE(cmd.config.inputCorrectPaths);

    argv = {
        "msmemscope",
        "--compare",
        "--input=path1，path2"
    };

    // Reset getopt states
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.config.enableCompare);
    ASSERT_FALSE(cmd.config.inputCorrectPaths);
}

TEST(ClientParser, test_invalid_compare_dump_data)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--input=path1"
    };

    // Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.config.enableCompare);
    ASSERT_FALSE(cmd.config.inputCorrectPaths);

    argv = {
        "msmemscope",
        "--compare"
    };

    // Reset getopt states
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.config.enableCompare);
    ASSERT_FALSE(cmd.config.inputCorrectPaths);
}

TEST(ClientParser, test_watch_config_set_all)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--watch=start:123,end,full-content"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(std::string(cmd.config.watchConfig.start), "start");
    ASSERT_EQ(std::string(cmd.config.watchConfig.end), "end");
    ASSERT_EQ(cmd.config.watchConfig.outputId, 123);
    ASSERT_EQ(cmd.config.watchConfig.fullContent, true);
    ASSERT_EQ(cmd.config.watchConfig.isWatched, true);
}

TEST(ClientParser, test_watch_config_set_only_end)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--watch=,end,"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(std::string(cmd.config.watchConfig.start), "");
    ASSERT_EQ(std::string(cmd.config.watchConfig.end), "end");
    ASSERT_EQ(cmd.config.watchConfig.outputId, UINT32_MAX);
    ASSERT_EQ(cmd.config.watchConfig.fullContent, false);
    ASSERT_EQ(cmd.config.watchConfig.isWatched, true);
}

TEST(ClientParser, test_watch_config_set_only_start_and_end)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--watch=start,end,"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(std::string(cmd.config.watchConfig.start), "start");
    ASSERT_EQ(std::string(cmd.config.watchConfig.end), "end");
    ASSERT_EQ(cmd.config.watchConfig.outputId, UINT32_MAX);
    ASSERT_EQ(cmd.config.watchConfig.fullContent, false);
    ASSERT_EQ(cmd.config.watchConfig.isWatched, true);
}

TEST(ClientParser, test_watch_config_set_only_start_with_id_and_end)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--watch=start:123,end,"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(std::string(cmd.config.watchConfig.start), "start");
    ASSERT_EQ(std::string(cmd.config.watchConfig.end), "end");
    ASSERT_EQ(cmd.config.watchConfig.outputId, 123);
    ASSERT_EQ(cmd.config.watchConfig.fullContent, false);
    ASSERT_EQ(cmd.config.watchConfig.isWatched, true);
}

TEST(ClientParser, test_watch_config_unset)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--level=0"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(std::string(cmd.config.watchConfig.start), "");
    ASSERT_EQ(std::string(cmd.config.watchConfig.end), "");
    ASSERT_EQ(cmd.config.watchConfig.outputId, UINT32_MAX);
    ASSERT_EQ(cmd.config.watchConfig.fullContent, false);
    ASSERT_EQ(cmd.config.watchConfig.isWatched, false);
}

TEST(ClientParser, test_watch_config_null)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--watch="
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, test_watch_config_empty_end)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--watch=,"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, test_watch_config_error_outputid)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--watch=start:error,end"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, test_watch_config_error_start_part)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--watch=:,end"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, test_watch_config_error_full_content)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--watch=start:123,end,full-contents"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, test_collect_mode_full)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--collect-mode=immediate"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.collectMode, static_cast<std::uint8_t>(CollectMode::IMMEDIATE));
}

TEST(ClientParser, test_collect_mode_custom)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--collect-mode=deferred"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.collectMode, static_cast<std::uint8_t>(CollectMode::DEFERRED));
}

TEST(ClientParser, test_collect_mode_empty)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--collect-mode="
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.collectMode, static_cast<std::uint8_t>(CollectMode::IMMEDIATE));
}

TEST(ClientParser, test_collect_mode_input_error)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--collect-mode=full2"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.collectMode, static_cast<std::uint8_t>(CollectMode::IMMEDIATE));
}

TEST(ClientParser, test_print_version)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--version"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printVersionInfo);
}

TEST(ClientParser, print_version)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--version"
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    testing::internal::CaptureStdout();
    cliParser.Interpretor(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStdout();
    ASSERT_EQ(capture.find("Usage"), std::string::npos);
}

TEST(ClientParser, test_not_set_output)
{
    std::vector<const char*> argv = {
        "msmemscope",
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.outputPath, "");
}

TEST(ClientParser, test_set_invalid_output)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--output= "
    };

    // Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.outputPath.empty());

    argv = {
        "msmemscope",
        "--output=/MyPath1/MyPath2?"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.outputPath.empty());

    argv = {
        "msmemscope",
        "--output=/MyPath1/MyPath2*/"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.outputPath.empty());

    argv = {
        "msmemscope",
        "--output=MyPath|"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.outputPath.empty());

    std::string str(256, 'A');
    str = "--output=" + str;
    argv = {
        "msmemscope",
        str.c_str()
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.outputPath.empty());
}

TEST(ClientParser, test_input_valid_log_level_expect_valid_loglv)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--log-level=warn"
    };
 
    /// Reset getopt states
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    Utility::SetLogLevel(static_cast<LogLv>(cmd.config.logLevel));
    ASSERT_EQ(Utility::Log::GetLog().lv_, LogLv::WARN);

    argv = {
        "msmemscope",
        "--log-level=info"
    };
 
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    Utility::SetLogLevel(static_cast<LogLv>(cmd.config.logLevel));
    ASSERT_EQ(Utility::Log::GetLog().lv_, LogLv::INFO);

    argv = {
        "msmemscope",
        "--log-level=error"
    };
 
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    Utility::SetLogLevel(static_cast<LogLv>(cmd.config.logLevel));
    ASSERT_EQ(Utility::Log::GetLog().lv_, LogLv::ERROR);
}

TEST(ClientParser, test_input_invalid_log_level_expect_invalid_loglv)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--log-level=test"
    };
 
    /// Reset getopt states
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    argv = {
        "msmemscope",
        "--log-level="
    };
 
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, test_parse_call_stack_expect_true)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--call-stack=c:10,python:10"
    };
    /// Reset getopt states
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    argv = {
        "msmemscope",
        "--call-stack=c:00"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
    argv = {
        "msmemscope",
        "--call-stack="
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    argv = {
        "msmemscope",
        "--call-stack=c:10,c:15"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.enableCStack, true);
    ASSERT_EQ(cmd.config.cStackDepth, 15);
    argv = {
        "msmemscope",
        "--call-stack=c:13,python:10,python:12"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.enableCStack, true);
    ASSERT_EQ(cmd.config.cStackDepth, 13);
    ASSERT_EQ(cmd.config.enablePyStack, true);
    ASSERT_EQ(cmd.config.pyStackDepth, 12);
}

TEST(ClientParser, pass_data_format_case_db)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--data-format=db",
        "--output=./testmsmemscope"
    };
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.dataFormat, 1);
}

TEST(ClientParser, usercommand_precheck_false)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--compare"
    };
    optind = 1;
    ClientParser cliParser;
    testing::internal::CaptureStdout();
    cliParser.Interpretor(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStdout();
    ASSERT_NE(capture.find("Usage"), std::string::npos);

    argv = {
        "msmemscope",
        "--output="
    };
    testing::internal::CaptureStdout();
    cliParser.Interpretor(argv.size(), const_cast<char**>(argv.data()));
    capture = testing::internal::GetCapturedStdout();
    ASSERT_NE(capture.find("Usage"), std::string::npos);
}

TEST(ClientParser, test_invalid_device_case)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--device="
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    argv = {
        "msmemscope",
        "--device=test"
    };
 
    /// Reset getopt states
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    argv = {
        "msmemscope",
        "--device=npu:100"
    };
 
    /// Reset getopt states
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    argv = {
        "msmemscope",
        "--device=npu:xx"
    };
 
    /// Reset getopt states
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, test_valid_device_case)
{
    std::vector<const char*> argv = {
        "msmemscope",
    };
 
    /// Reset getopt states
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.config.collectAllNpu);
    ASSERT_FALSE(cmd.config.collectCpu);

    argv = {
        "msmemscope",
        "--device=npu"
    };
 
    /// Reset getopt states
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.config.collectAllNpu);
    ASSERT_FALSE(cmd.config.collectCpu);

    argv = {
        "msmemscope",
        "--device=npu:0,npu:2"
    };
 
    /// Reset getopt states
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.config.collectAllNpu);
    ASSERT_FALSE(cmd.config.collectCpu);
    ASSERT_EQ(cmd.config.npuSlots, 5);

    argv = {
        "msmemscope",
        "--device=npu:0,cpu"
    };
 
    /// Reset getopt states
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.config.collectAllNpu);
    ASSERT_TRUE(cmd.config.collectCpu);
    ASSERT_EQ(cmd.config.npuSlots, 1);
}

// ==================== RFC: Precise Event Filtering Tests ====================

TEST(ClientParser, set_event_default_config_only_extends_eventType)
{
    Config config;
    memset(&config, 0, sizeof(config));
    BitField<decltype(config.eventType)> eventBit;
    eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    config.eventType = eventBit.getValue();
    config.dumpEventType = eventBit.getValue();

    SetEventDefaultConfig(config);

    // eventType: ALLOC_EVENT → ALLOC + FREE
    ASSERT_TRUE(BitPresent(config.eventType, static_cast<size_t>(EventType::ALLOC_EVENT)));
    ASSERT_TRUE(BitPresent(config.eventType, static_cast<size_t>(EventType::FREE_EVENT)));

    // dumpEventType: 仅 ALLOC_EVENT，不被联动扩展
    ASSERT_TRUE(BitPresent(config.dumpEventType, static_cast<size_t>(EventType::ALLOC_EVENT)));
    ASSERT_FALSE(BitPresent(config.dumpEventType, static_cast<size_t>(EventType::FREE_EVENT)));
}

TEST(ClientParser, set_analysis_default_config_only_extends_eventType)
{
    Config config;
    memset(&config, 0, sizeof(config));
    BitField<decltype(config.analysisType)> analysisBit;
    analysisBit.setBit(static_cast<size_t>(AnalysisType::LEAKS_ANALYSIS));
    config.analysisType = analysisBit.getValue();
    config.eventType = 0;
    config.dumpEventType = 0;

    SetAnalysisDefaultConfig(config);

    // eventType: 被扩展为 ALLOC + FREE
    ASSERT_TRUE(BitPresent(config.eventType, static_cast<size_t>(EventType::ALLOC_EVENT)));
    ASSERT_TRUE(BitPresent(config.eventType, static_cast<size_t>(EventType::FREE_EVENT)));

    // dumpEventType: 仍为 0，不被 analysis 联动扩展
    ASSERT_EQ(config.dumpEventType, 0);
}

TEST(ClientParser, pass_event_trace_type_none_expect_empty_eventType)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=none"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 0);
    ASSERT_EQ(cmd.config.dumpEventType, 0);
}

TEST(ClientParser, pass_analysis_type_none_expect_empty_analysisType)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--analysis=none"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.analysisType, 0);
}

TEST(ClientParser, pass_event_trace_type_none_mixed_expect_none_priority)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=alloc,none"
    };

    optind = 1;
    ClientParser cliParser;
    testing::internal::CaptureStdout();
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStdout();
    ASSERT_EQ(cmd.config.eventType, 0);
    ASSERT_EQ(cmd.config.dumpEventType, 0);
    // 验证有 warning 输出
    ASSERT_NE(capture.find("'none' mixed"), std::string::npos);
}

TEST(ClientParser, pass_analysis_type_none_mixed_expect_none_priority)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--analysis=leaks,none"
    };

    optind = 1;
    ClientParser cliParser;
    testing::internal::CaptureStdout();
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStdout();
    ASSERT_EQ(cmd.config.analysisType, 0);
    // 验证有 warning 输出
    ASSERT_NE(capture.find("'none' mixed"), std::string::npos);
}

TEST(ClientParser, pass_oom_analysis_topk_validation)
{
    // oom[:K]：K 可选，默认 10，合法范围 [1,1000]，越界/非法报错
    ClientParser cliParser;
    std::vector<const char*> argv;
    UserCommand cmd;

    // 无 K：默认 10，且 OOM 分析位被置位
    argv = {"msmemscope", "--analysis=oom"};
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_TRUE(BitPresent(cmd.config.analysisType, static_cast<size_t>(AnalysisType::OOM_ANALYSIS)));
    ASSERT_EQ(cmd.config.oomTopK, 10);

    // 下边界 1
    argv = {"msmemscope", "--analysis=oom:1"};
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.oomTopK, 1);

    // 上边界 1000
    argv = {"msmemscope", "--analysis=oom:1000"};
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.oomTopK, 1000);

    // 中值 5
    argv = {"msmemscope", "--analysis=oom:5"};
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.oomTopK, 5);

    // 越下界 0 → 报错
    argv = {"msmemscope", "--analysis=oom:0"};
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    // 越上界 1001 → 报错
    argv = {"msmemscope", "--analysis=oom:1001"};
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    // 非数字 → 报错
    argv = {"msmemscope", "--analysis=oom:abc"};
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);

    // 空 K → 报错
    argv = {"msmemscope", "--analysis=oom:"};
    optind = 1;
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printHelpInfo);
}

TEST(ClientParser, pass_event_trace_type_alloc_expect_dumpEventType_only_alloc)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=alloc"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    // eventType 目前还未被联动扩展（Parse 阶段尚未调用 SetEventDefaultConfig）
    // dumpEventType 记录用户原始配置
    ASSERT_EQ(cmd.config.dumpEventType, 1);  // ALLOC_EVENT = bit 0
    ASSERT_EQ(cmd.config.eventType, 1);
}

TEST(ClientParser, initial_config_sets_dumpEventType_same_as_eventType)
{
    Config config;
    memset(&config, 0, sizeof(config));

    ClientParser parser;
    parser.InitialConfig(config);

    // 默认值: alloc + free + launch (bits 0, 1, 2) = 7
    ASSERT_EQ(config.eventType, 7);
    ASSERT_EQ(config.dumpEventType, 7);
}

TEST(ClientParser, pass_events_and_analysis_none_expect_no_collection_no_analysis)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=none",
        "--analysis=none"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 0);
    ASSERT_EQ(cmd.config.dumpEventType, 0);
    ASSERT_EQ(cmd.config.analysisType, 0);
}

// ==================== RFC: CLI Standardization Tests ====================

TEST(ClientParser, pass_new_standard_format_expect_data_format_parsed)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--format=db"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.dataFormat, 1);

    argv = {
        "msmemscope",
        "--format=csv"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.dataFormat, 0);
}

TEST(ClientParser, pass_new_standard_input_path_expect_parsed)
{
    // 创建两个临时文件作为合法的 compare 输入
    const char* file1 = "cli_std_input_tmp1.csv";
    const char* file2 = "cli_std_input_tmp2.csv";
    {
        std::ofstream ofs1(file1);
        std::ofstream ofs2(file2);
        ofs1 << "ID,Event\n" << std::endl;
        ofs2 << "ID,Event\n" << std::endl;
    }

    std::string inputArg = std::string("--input-path=") + file1 + "," + file2;
    std::vector<const char*> argv = {
        "msmemscope",
        inputArg.c_str()
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    std::remove(file1);
    std::remove(file2);
    ASSERT_TRUE(cmd.config.inputCorrectPaths);
    ASSERT_EQ(cmd.inputPaths.size(), PATHSIZE);
}

TEST(ClientParser, pass_new_standard_output_path_expect_output_dir_set)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--output-path=./testmsmemscope_cli_std"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_NE(cmd.config.outputDir[0], '\0');
    ASSERT_TRUE(cmd.config.outputCorrectPaths);
}

TEST(ClientParser, pass_old_alias_expect_deprecation_warning)
{
    // --input 别名：解析行为不变，stderr 打印弃用告警
    {
        std::vector<const char*> argv = {
            "msmemscope",
            "--input=path1,path2"
        };
        optind = 1;
        ClientParser cliParser;
        testing::internal::CaptureStderr();
        UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
        std::string capture = testing::internal::GetCapturedStderr();
        ASSERT_FALSE(cmd.config.inputCorrectPaths);
        ASSERT_NE(capture.find("[msmemscope] Deprecation:"), std::string::npos);
        ASSERT_NE(capture.find("'--input' is deprecated, use '--input-path'"), std::string::npos);
    }

    // --output 别名：仍能正确解析
    {
        std::vector<const char*> argv = {
            "msmemscope",
            "--output=./testmsmemscope_cli_std"
        };
        optind = 1;
        ClientParser cliParser;
        testing::internal::CaptureStderr();
        UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
        std::string capture = testing::internal::GetCapturedStderr();
        ASSERT_NE(cmd.config.outputDir[0], '\0');
        ASSERT_NE(capture.find("'--output' is deprecated, use '--output-path'"), std::string::npos);
    }

    // --data-format 别名：仍能正确解析
    {
        std::vector<const char*> argv = {
            "msmemscope",
            "--data-format=db"
        };
        optind = 1;
        ClientParser cliParser;
        testing::internal::CaptureStderr();
        UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
        std::string capture = testing::internal::GetCapturedStderr();
        ASSERT_EQ(cmd.config.dataFormat, 1);
        ASSERT_NE(capture.find("'--data-format' is deprecated, use '--format'"), std::string::npos);
    }
}

TEST(ClientParser, pass_short_version_expect_print_version_info)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "-V"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printVersionInfo);

    argv = {
        "msmemscope",
        "--version"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_TRUE(cmd.printVersionInfo);
}

TEST(ClientParser, pass_verbose_expect_log_level_debug)
{
    const uint8_t debugLv = static_cast<uint8_t>(LogLv::DEBUG);

    std::vector<const char*> argv = {
        "msmemscope",
        "-v"
    };
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printVersionInfo);
    ASSERT_EQ(cmd.config.logLevel, debugLv);

    argv = {
        "msmemscope",
        "--verbose"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, debugLv);
}

TEST(ClientParser, pass_quiet_expect_log_level_error)
{
    const uint8_t errorLv = static_cast<uint8_t>(LogLv::ERROR);

    std::vector<const char*> argv = {
        "msmemscope",
        "-q"
    };
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, errorLv);

    argv = {
        "msmemscope",
        "--quiet"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, errorLv);
}

TEST(ClientParser, pass_debug_expect_log_level_debug)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--debug"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, static_cast<uint8_t>(LogLv::DEBUG));
}

TEST(ClientParser, pass_log_level_new_values_expect_parsed)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--log-level=debug"
    };
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.logLevel, static_cast<uint8_t>(LogLv::DEBUG));

    argv = {
        "msmemscope",
        "--log-level=warning"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    ASSERT_EQ(cmd.config.logLevel, static_cast<uint8_t>(LogLv::WARN));
}

TEST(ClientParser, pass_log_level_warn_alias_expect_warn_with_deprecation)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--log-level=warn"
    };

    optind = 1;
    ClientParser cliParser;
    testing::internal::CaptureStderr();
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStderr();
    ASSERT_EQ(cmd.config.logLevel, static_cast<uint8_t>(LogLv::WARN));
    ASSERT_NE(capture.find("'--log-level=warn' is deprecated, use '--log-level=warning'"), std::string::npos);
}

TEST(ClientParser, default_log_level_expect_info)
{
    std::vector<const char*> argv = {
        "msmemscope",
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, static_cast<uint8_t>(LogLv::INFO));
}

TEST(ClientParser, pass_legacy_level_value_expect_parsed_with_deprecation)
{
    // --level=0 → op，带弃用告警
    {
        std::vector<const char*> argv = {
            "msmemscope",
            "--level=0"
        };
        optind = 1;
        ClientParser cliParser;
        testing::internal::CaptureStderr();
        UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
        std::string capture = testing::internal::GetCapturedStderr();
        ASSERT_EQ(cmd.config.levelType, 1);
        ASSERT_NE(capture.find("'--level=0' is deprecated, use '--level=op'"), std::string::npos);
    }

    // --level=1 → kernel，带弃用告警
    {
        std::vector<const char*> argv = {
            "msmemscope",
            "--level=1"
        };
        optind = 1;
        ClientParser cliParser;
        testing::internal::CaptureStderr();
        UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
        std::string capture = testing::internal::GetCapturedStderr();
        ASSERT_EQ(cmd.config.levelType, 2);
        ASSERT_NE(capture.find("'--level=1' is deprecated, use '--level=kernel'"), std::string::npos);
    }

    // 新标准取值 op/kernel 不触发告警
    {
        std::vector<const char*> argv = {
            "msmemscope",
            "--level=op"
        };
        optind = 1;
        ClientParser cliParser;
        testing::internal::CaptureStderr();
        UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
        std::string capture = testing::internal::GetCapturedStderr();
        ASSERT_EQ(cmd.config.levelType, 1);
        ASSERT_EQ(capture.find("Deprecation"), std::string::npos);
    }
}

TEST(ClientParser, legacy_commands_regression_expect_unchanged)
{
    // 存量命令回归：事件配置 + 程序参数仍正常
    std::vector<const char*> argv = {
        "msmemscope",
        "--events=alloc,free",
        "--",
        "python",
        "train.py"
    };

    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.eventType, 3);
    ASSERT_EQ(cmd.config.dumpEventType, 3);
    ASSERT_FALSE(cmd.cmd.empty());
    ASSERT_EQ(cmd.cmd[0], "python");
    ASSERT_EQ(cmd.cmd[1], "train.py");

    // 旧别名组合命令仍可解析
    argv = {
        "msmemscope",
        "--data-format=db",
        "--output=./testmsmemscope_cli_std"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.dataFormat, 1);
    ASSERT_NE(cmd.config.outputDir[0], '\0');
}

TEST(ClientParser, show_help_info_standardized_format)
{
    std::vector<const char*> argv = {
        "msmemscope",
        "--help"
    };

    optind = 1;
    ClientParser cliParser;
    testing::internal::CaptureStdout();
    cliParser.Interpretor(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStdout();

    // Description / Usage
    ASSERT_NE(capture.find("Description:"), std::string::npos);
    ASSERT_NE(capture.find("Usage:"), std::string::npos);

    // 参数分组
    ASSERT_NE(capture.find("General options:"), std::string::npos);
    ASSERT_NE(capture.find("Collection options:"), std::string::npos);
    ASSERT_NE(capture.find("Analysis options:"), std::string::npos);
    ASSERT_NE(capture.find("Input / Output options:"), std::string::npos);

    // 标准短选项与 metavar
    ASSERT_NE(capture.find("-V, --version"), std::string::npos);
    ASSERT_NE(capture.find("-v, --verbose"), std::string::npos);
    ASSERT_NE(capture.find("-q, --quiet"), std::string::npos);
    ASSERT_NE(capture.find("--input-path <DIR>"), std::string::npos);
    ASSERT_NE(capture.find("--output-path <DIR>"), std::string::npos);
    ASSERT_NE(capture.find("--format <FORMAT>"), std::string::npos);

    // 默认值标注与示例
    ASSERT_NE(capture.find("[default: info]"), std::string::npos);
    ASSERT_NE(capture.find("Examples:"), std::string::npos);
    ASSERT_NE(capture.find("msmemscope -- python train.py"), std::string::npos);

    // 帮助信息中不应出现隐藏别名
    ASSERT_EQ(capture.find("--data-format"), std::string::npos);
}

TEST(ClientParser, log_level_conflict_shortcuts_highest_visibility_wins)
{
    const uint8_t debugLv = static_cast<uint8_t>(LogLv::DEBUG);
    const uint8_t errorLv = static_cast<uint8_t>(LogLv::ERROR);

    // 多个快捷开关并存时，按"可见度最高者生效"（debug 最高），与参数顺序无关
    std::vector<const char*> argv = {
        "msmemscope",
        "-q", "-v"
    };
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, debugLv);

    argv = {
        "msmemscope",
        "-v", "-q"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, debugLv);

    argv = {
        "msmemscope",
        "--verbose", "--quiet"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, debugLv);

    argv = {
        "msmemscope",
        "--quiet", "--debug"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, debugLv);

    // 仅有 --quiet/-q 时取 error
    argv = {
        "msmemscope",
        "-q"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, errorLv);
}

TEST(ClientParser, log_level_conflict_explicit_log_level_wins)
{
    const uint8_t debugLv = static_cast<uint8_t>(LogLv::DEBUG);
    const uint8_t infoLv = static_cast<uint8_t>(LogLv::INFO);
    const uint8_t errorLv = static_cast<uint8_t>(LogLv::ERROR);

    // 显式 --log-level 优先于快捷开关，与参数顺序无关
    std::vector<const char*> argv = {
        "msmemscope",
        "--verbose", "--log-level=error"
    };
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, errorLv);

    argv = {
        "msmemscope",
        "--log-level=error", "--verbose"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, errorLv);

    argv = {
        "msmemscope",
        "-q", "--log-level=info"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, infoLv);

    argv = {
        "msmemscope",
        "--log-level=debug", "-q"
    };
    cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.logLevel, debugLv);
}

// ==================== RFC: output_path提前构造固化回归 ====================

// 回归场景：hook影子采集路径在用户config()之前触发MemoryStateManager::GetInstance()，
// FileCreateManager随之以默认outputDir提前构造并固化projectDir_；
// SetEffectiveConfig在配置生效时须按用户outputDir刷新落盘根目录，否则output_path不生效
TEST(ClientParser, set_effective_config_expect_project_dir_follow_user_output)
{
    // InitialConfig得到默认outputDir（模拟提前构造时刻的默认配置）
    Config config;
    memset(&config, 0, sizeof(config));
    ClientParser parser;
    parser.InitialConfig(config);
    ASSERT_NE(config.outputDir[0], '\0');

    // 以默认目录完成提前构造（进程内已构造时参数被忽略，无副作用），
    // 确保后续断言不依赖测试执行顺序
    auto& fileManager = Utility::FileCreateManager::GetInstance(config.outputDir);
    // 前置：projectDir_处于派生态（未被SetProjectDir显式钉住），生产场景下始终如此
    fileManager.projectDirPinned_ = false;

    // 模拟用户config(output_path=...)后配置生效
    const std::string userOutput = "./testmsmemscope_effective_config";
    snprintf(config.outputDir, sizeof(config.outputDir), "%s", userOutput.c_str());
    SetEffectiveConfig(config);

    // 落盘根目录按用户output_path重算: <outputDir>/msmemscope_<pid>_<时间戳>_ascend
    std::string projectDir = fileManager.GetProjectDir();
    ASSERT_TRUE(projectDir.rfind(userOutput + "/msmemscope_", 0) == 0);
    ASSERT_TRUE(config.isEffective);

    // 恢复单例状态（进程级单例，测试间共享）
    fileManager.RefreshOutputDir("./testmsmemscope");
    fileManager.SetProjectDir("");
}

// ==================== 进程外控制通道: --pid/--command 解析与互斥校验 ====================

// --pid解析: 长/短选项均落config.attachPid,不与采集配置耦合
TEST(ClientParser, pid_parses_positive_integer)
{
    std::vector<const char*> argv = {"msmemscope", "--pid", "12345"};
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.attachPid, 12345u);
    ASSERT_FALSE(cmd.printHelpInfo);
}

// --pid短选项+--command单发控制字
TEST(ClientParser, pid_short_option_and_command)
{
    std::vector<const char*> argv = {"msmemscope", "-p", "999", "-c", "display memory summary"};
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
    ASSERT_EQ(cmd.config.attachPid, 999u);
    ASSERT_EQ(cmd.attachCommand, "display memory summary");
    ASSERT_FALSE(cmd.printHelpInfo);
}

// --pid非法值: 空/非数字/混合/0/负/超INT32_MAX → 报错+attachPid不落
TEST(ClientParser, pid_rejects_invalid_values)
{
    const char* invalid[] = {"", "abc", "12x", "0", "-5", "99999999999"};
    for (const char* bad : invalid)
    {
        std::vector<const char*> argv = {"msmemscope", "--pid", bad};
        optind = 1;
        ClientParser cliParser;
        testing::internal::CaptureStdout();
        UserCommand cmd = cliParser.Parse(argv.size(), const_cast<char**>(argv.data()));
        std::string capture = testing::internal::GetCapturedStdout();
        ASSERT_TRUE(cmd.printHelpInfo) << "should reject --pid " << bad;
        ASSERT_EQ(cmd.config.attachPid, 0u);
        ASSERT_NE(capture.find("Error: --pid requires a positive integer"), std::string::npos) << bad;
    }
}

// 互斥校验(UserCommandPrecheck): --pid不与--compare/启动命令共存;--command须配--pid。
// 错误路径经Interpretor: precheck失败→ShowHelpInfo,不触发attach执行(成功路径由IT覆盖)
TEST(ClientParser, pid_mutual_exclusion_errors)
{
    // --pid + --compare: 需要两份真实输入文件越过compare自身校验(enableCompare==inputCorrectPaths)
    const char* p1 = "pid_mutex_a.csv";
    const char* p2 = "pid_mutex_b.csv";
    {
        std::ofstream f1(p1);
        f1 << "x\n";
    }
    {
        std::ofstream f2(p2);
        f2 << "x\n";
    }
    std::vector<const char*> argv = {"msmemscope", "--pid", "12345", "--compare",
                                     "--input-path=./pid_mutex_a.csv,./pid_mutex_b.csv"};
    optind = 1;
    ClientParser cliParser;
    testing::internal::CaptureStdout();
    cliParser.Interpretor(argv.size(), const_cast<char**>(argv.data()));
    std::string capture = testing::internal::GetCapturedStdout();
    std::remove(p1);
    std::remove(p2);
    ASSERT_NE(capture.find("Error: --pid cannot be combined with --compare"), std::string::npos);

    // --pid + 启动命令
    argv = {"msmemscope", "--pid", "12345", "--", "python", "train.py"};
    optind = 1;
    testing::internal::CaptureStdout();
    cliParser.Interpretor(argv.size(), const_cast<char**>(argv.data()));
    capture = testing::internal::GetCapturedStdout();
    ASSERT_NE(capture.find("Error: --pid cannot be combined with a launch command"), std::string::npos);

    // --command 无 --pid
    argv = {"msmemscope", "--command", "start"};
    optind = 1;
    testing::internal::CaptureStdout();
    cliParser.Interpretor(argv.size(), const_cast<char**>(argv.data()));
    capture = testing::internal::GetCapturedStdout();
    ASSERT_NE(capture.find("Error: --command requires --pid"), std::string::npos);
}

// ============================================================================
// 覆盖率补充用例：目标 framework/client_parser.cpp 未覆盖行(选项解析边界、
// UserCommandPrecheck成功路径、ParseUserCommand直达分支、attach/compare执行路径)。
// Interpretor 成功路径会改动 ConfigManager/PythonTrace 等跨用例单例，故在 fork
// 子进程内执行：父进程 COW 不受影响，子进程正常退出以落盘 gcov。
// ============================================================================
namespace
{
// OptVal 为 client_parser.cpp 内部枚举(不在头文件)，按定义顺序取数值：
// 仅用于直达 switch 分支——INPUT_PATH/OUTPUT_PATH/VERBOSE/QUIET 这些 val
// 在 long option 表中无对应项，CLI(Parse)路径不可达
constexpr int32_t kOptValInputPath = 13;
constexpr int32_t kOptValOutputPath = 14;
constexpr int32_t kOptValVerbose = 16;
constexpr int32_t kOptValQuiet = 17;

std::string MakeTempFilePath(const char *tag)
{
    char tmpl[128] = {0};
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/msmemscope_cp_ut_%s_XXXXXX", tag);
    const int fd = ::mkstemp(tmpl);
    if (fd < 0)
    {
        return std::string();
    }
    ::close(fd);
    std::ofstream ofs(tmpl);
    ofs << "x\n";
    return std::string(tmpl);
}

std::string MakeTempDirPath(const char *tag)
{
    char tmpl[128] = {0};
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/msmemscope_cp_ut_%s_XXXXXX", tag);
    char *dir = ::mkdtemp(tmpl);
    return dir == nullptr ? std::string() : std::string(dir);
}

struct ChildRunResult
{
    int status{-1};
    std::string output;
};

// 子进程执行上限：attach/compare 路径均应在秒级完成；超时强制回收，
// 避免被测代码回归(卡锁/阻塞写)时无界 waitpid 拖死整个 gtest 进程
constexpr int kChildTimeoutMs = 30000;
constexpr int kChildPollStepMs = 50;

// 有界等待子进程退出：期限内轮询 WNOHANG，超时 SIGKILL 后阻塞回收。
// 超时强杀返回的 status 为“被信号终止”，调用点的 WIFEXITED 断言必然失败并
// 打印已捕获输出，不会静默通过
void WaitChildExitBounded(pid_t pid, int *status)
{
    for (int waitedMs = 0; waitedMs < kChildTimeoutMs; waitedMs += kChildPollStepMs)
    {
        const pid_t ret = ::waitpid(pid, status, WNOHANG);
        if (ret == pid)
        {
            return;
        }
        if (ret < 0 && errno != EINTR)
        {
            return;
        }
        ::usleep(kChildPollStepMs * 1000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, status, 0);
}

// fork隔离执行：子进程stdout重定向到临时文件、stderr丢弃；body正常返回后
// std::exit(0)以落盘gcov。body内自行std::exit(attach路径)时同样保留已写输出
ChildRunResult RunInFork(const std::function<void()> &body)
{
    char tmpl[] = "/tmp/msmemscope_cp_fork_XXXXXX";
    const int pathFd = ::mkstemp(tmpl);
    if (pathFd < 0)
    {
        return ChildRunResult();
    }
    ::close(pathFd);
    const std::string path(tmpl);
    ChildRunResult result;
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0)
        {
            ::dup2(fd, STDOUT_FILENO);
            ::close(fd);
        }
        const int nullFd = ::open("/dev/null", O_WRONLY);
        if (nullFd >= 0)
        {
            ::dup2(nullFd, STDERR_FILENO);
            ::close(nullFd);
        }
        body();
        std::exit(0);  // 正常退出以落盘gcov
    }
    if (pid < 0)
    {
        ::unlink(path.c_str());
        return result;
    }
    WaitChildExitBounded(pid, &result.status);
    std::ifstream in(path);
    result.output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    in.close();
    ::unlink(path.c_str());
    return result;
}

// UserCommandPrecheck成功返回true(L210)：默认/attach/compare三种合法组合
TEST(ClientParser, usercommand_precheck_valid_commands_expect_true)
{
    ClientParser cliParser;

    UserCommand defaultCmd{};
    cliParser.InitialConfig(defaultCmd.config);
    EXPECT_TRUE(UserCommandPrecheck(defaultCmd));

    // attach模式：--pid + --command 合法
    UserCommand attachCmd{};
    cliParser.InitialConfig(attachCmd.config);
    attachCmd.config.attachPid = 12345;
    attachCmd.attachCommand = "display memory summary";
    EXPECT_TRUE(UserCommandPrecheck(attachCmd));

    // compare模式：enableCompare 与 inputCorrectPaths 一致即可
    UserCommand compareCmd{};
    cliParser.InitialConfig(compareCmd.config);
    compareCmd.config.enableCompare = true;
    compareCmd.config.inputCorrectPaths = true;
    EXPECT_TRUE(UserCommandPrecheck(compareCmd));
}

// GetShortOptString(L364-385)：标准表结果 + 自定义表(flag非空/不可打印val/终止项截断)
TEST(ClientParser, get_short_opt_string_expect_short_options)
{
    const std::vector<option> longOpts = GetLongOptArray();
    EXPECT_EQ(GetShortOptString(longOpts), "hVvqi:o:p:c:");  // -c/--command 为 required_argument

    int32_t flagValue = 0;
    const std::vector<option> customOpts = {
        {"alpha", no_argument, nullptr, 'a'},
        {"beta", required_argument, nullptr, 'b'},
        {"flag", no_argument, &flagValue, 300},  // flag非空：不生成短选项
        {"ctrl", no_argument, nullptr, 1},       // 不可打印val：跳过
        {nullptr, 0, nullptr, 0},                // 终止项
        {"after", no_argument, nullptr, 'z'},    // 终止项之后不再扫描
    };
    EXPECT_EQ(GetShortOptString(customOpts), "ab:");
}

// ParseAnalysis(L447-452/L518)：decompose/inefficient/host-leaks/leaks 映射
TEST(ClientParser, parse_analysis_maps_all_types)
{
    bool printHelp = false;
    Config config{};
    ParseAnalysis("decompose,inefficient", config, printHelp);
    EXPECT_FALSE(printHelp);
    const uint8_t expectedDecompose =
        static_cast<uint8_t>(1u << static_cast<uint8_t>(AnalysisType::DECOMPOSE_ANALYSIS));
    const uint8_t expectedInefficient =
        static_cast<uint8_t>(1u << static_cast<uint8_t>(AnalysisType::INEFFICIENCY_ANALYSIS));
    EXPECT_EQ(config.analysisType, static_cast<uint8_t>(expectedDecompose | expectedInefficient));

    printHelp = false;
    Config hostLeakConfig{};
    ParseAnalysis("host-leaks", hostLeakConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(hostLeakConfig.analysisType,
              static_cast<uint8_t>(1u << static_cast<uint8_t>(AnalysisType::HOST_LEAK_ANALYSIS)));

    printHelp = false;
    Config leaksConfig{};
    ParseAnalysis("leaks", leaksConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(leaksConfig.analysisType,
              static_cast<uint8_t>(1u << static_cast<uint8_t>(AnalysisType::LEAKS_ANALYSIS)));
}

// ParseAnalysis(L504-516)：host-leaks 与其余分析项互斥，失败时保持原配置(末尾赋值被跳过)
TEST(ClientParser, parse_analysis_host_leaks_mutually_exclusive)
{
    const uint8_t leaksOnly = static_cast<uint8_t>(1u << static_cast<uint8_t>(AnalysisType::LEAKS_ANALYSIS));
    Config config{};
    config.analysisType = leaksOnly;
    bool printHelp = false;
    testing::internal::CaptureStdout();
    ParseAnalysis("host-leaks,decompose", config, printHelp);
    const std::string capture = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(printHelp);
    EXPECT_EQ(config.analysisType, leaksOnly);
    EXPECT_NE(capture.find("'host-leaks' is mutually exclusive"), std::string::npos) << capture;
}

// ParseCallstack/CheckIsValidDepthInfo(L551)：未知类型报错；省略深度取默认值50
TEST(ClientParser, parse_callstack_unknown_type_and_default_depth)
{
    bool printHelp = false;
    Config invalidConfig{};
    testing::internal::CaptureStdout();
    ParseCallstack("java:10", invalidConfig, printHelp);
    const std::string capture = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(printHelp);
    EXPECT_FALSE(invalidConfig.enableCStack);
    EXPECT_FALSE(invalidConfig.enablePyStack);
    EXPECT_NE(capture.find("invalid call-stack depth input"), std::string::npos) << capture;

    printHelp = false;
    Config config{};
    ParseCallstack("c", config, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_TRUE(config.enableCStack);
    EXPECT_EQ(config.cStackDepth, static_cast<uint32_t>(DEFAULT_CALL_STACK_DEPTH));
}

// ParseInputPaths(L591-598)：参数长度超过4096提前返回(不置printHelpInfo)
TEST(ClientParser, parse_input_path_too_long_expect_early_return)
{
    UserCommand cmd{};
    const std::string tooLong(4097, 'a');  // INPUT_STR_MAX_LEN = 4096
    testing::internal::CaptureStdout();
    ParseUserCommand('i', tooLong, cmd);
    const std::string capture = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(cmd.printHelpInfo);
    EXPECT_TRUE(cmd.inputPaths.empty());
    EXPECT_FALSE(cmd.config.inputCorrectPaths);
    EXPECT_NE(capture.find("Parameter --input length exceeds the maximum length:4096"), std::string::npos) << capture;
}

// ParseOutputPath(L626-633)：参数长度超过PATH_MAX提前返回，保留默认输出目录
TEST(ClientParser, parse_output_path_too_long_expect_default_kept)
{
    Config config{};
    ClientParser cliParser;
    cliParser.InitialConfig(config);
    const std::string defaultOutputDir(config.outputDir);
    bool printHelp = false;
    const std::string tooLong(static_cast<size_t>(PATH_MAX) + 1, 'a');
    testing::internal::CaptureStdout();
    ParseOutputPath(tooLong, config, printHelp);
    const std::string capture = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(printHelp);
    EXPECT_TRUE(config.outputCorrectPaths);
    EXPECT_EQ(std::string(config.outputDir), defaultOutputDir);
    EXPECT_NE(capture.find("Parameter --output length exceeds the maximum length:4096"), std::string::npos) << capture;
    EXPECT_NE(capture.find("output path will be set to default(./memscopeDumpResults)"), std::string::npos)
        << capture;
}

// ParseEventTraceType(L739-743)：空token跳过，不影响后续token解析
TEST(ClientParser, parse_events_skips_empty_tokens)
{
    bool printHelp = false;
    Config config{};
    ParseEventTraceType("alloc,,free", config, printHelp);
    EXPECT_FALSE(printHelp);
    const uint8_t allocBit = static_cast<uint8_t>(1u << static_cast<uint8_t>(EventType::ALLOC_EVENT));
    const uint8_t freeBit = static_cast<uint8_t>(1u << static_cast<uint8_t>(EventType::FREE_EVENT));
    EXPECT_EQ(config.eventType, static_cast<uint8_t>(allocBit | freeBit));
    EXPECT_EQ(config.dumpEventType, config.eventType);

    printHelp = false;
    Config onlyEmptyConfig{};
    ParseEventTraceType(",", onlyEmptyConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(onlyEmptyConfig.eventType, 0u);
    EXPECT_EQ(onlyEmptyConfig.dumpEventType, 0u);
}

// ParseWatchStartConfig(L815-819)：outid前导0被拒绝；单个0合法
TEST(ClientParser, parse_watch_output_id_leading_zero_rejected)
{
    bool printHelp = false;
    Config config{};
    testing::internal::CaptureStdout();
    ParseWatchConfig("start:0123,end", config, printHelp);
    const std::string capture = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(printHelp);
    EXPECT_FALSE(config.watchConfig.isWatched);
    EXPECT_NE(capture.find("invalid watch config"), std::string::npos) << capture;

    printHelp = false;
    Config zeroIdConfig{};
    ParseWatchConfig("start:0,end", zeroIdConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_TRUE(zeroIdConfig.watchConfig.isWatched);
    EXPECT_EQ(zeroIdConfig.watchConfig.outputId, 0u);
    EXPECT_STREQ(zeroIdConfig.watchConfig.start, "start");
    EXPECT_STREQ(zeroIdConfig.watchConfig.end, "end");
}

// ParseDataFormat(L934-957)：非法值报错且保持CSV；db合法
TEST(ClientParser, parse_data_format_invalid_value_rejected)
{
    bool printHelp = false;
    Config config{};
    testing::internal::CaptureStdout();
    ParseDataFormat("json", config, printHelp);
    const std::string capture = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(printHelp);
    EXPECT_EQ(config.dataFormat, static_cast<uint8_t>(DataFormat::CSV));
    EXPECT_NE(capture.find("--data-format param is invalid"), std::string::npos) << capture;
    EXPECT_NE(capture.find("DATA_FORMAT can only be set csv,db"), std::string::npos) << capture;

    printHelp = false;
    Config dbConfig{};
    ParseDataFormat("db", dbConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(dbConfig.dataFormat, static_cast<uint8_t>(DataFormat::DB));
}

// ParseDevice(L977-1016)：空token跳过，npu/cpu标志仍正确
TEST(ClientParser, parse_device_skips_empty_tokens)
{
    bool printHelp = false;
    Config config{};
    ParseDevice("npu,,cpu", config, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_TRUE(config.collectAllNpu);
    EXPECT_TRUE(config.collectCpu);
    EXPECT_EQ(config.npuSlots, 0u);
}

// ParseHostLeakMode(L1044-1066)：event/summary 合法；非法值报错且保持原值
TEST(ClientParser, parse_host_leak_mode_values)
{
    bool printHelp = false;
    Config eventConfig{};
    ParseHostLeakMode("event", eventConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(eventConfig.hostLeakMode, static_cast<uint8_t>(HostLeakMode::EVENT));

    printHelp = false;
    Config summaryConfig{};
    ParseHostLeakMode("summary", summaryConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(summaryConfig.hostLeakMode, static_cast<uint8_t>(HostLeakMode::SUMMARY));

    printHelp = false;
    Config invalidConfig{};
    invalidConfig.hostLeakMode = static_cast<uint8_t>(HostLeakMode::SUMMARY);
    testing::internal::CaptureStdout();
    ParseHostLeakMode("verbose", invalidConfig, printHelp);
    const std::string capture = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(printHelp);
    EXPECT_EQ(invalidConfig.hostLeakMode, static_cast<uint8_t>(HostLeakMode::SUMMARY));
    EXPECT_NE(capture.find("Host leak mode can only be set to event,summary"), std::string::npos) << capture;
}

// ParseBlockSizeThreshold(L1069-1083)：0与uint32上界合法；前导0/空/非数字/超界非法且保持原值
TEST(ClientParser, parse_block_size_threshold_values)
{
    bool printHelp = false;
    Config zeroConfig{};
    ParseBlockSizeThreshold("0", zeroConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(zeroConfig.blockSizeThreshold, 0u);

    printHelp = false;
    Config maxConfig{};
    ParseBlockSizeThreshold("4294967295", maxConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(maxConfig.blockSizeThreshold, 4294967295ULL);

    const char *invalidValues[] = {"abc", "", "0123", "4294967296"};
    for (const char *invalid : invalidValues)
    {
        printHelp = false;
        Config invalidConfig{};
        invalidConfig.blockSizeThreshold = 7;
        testing::internal::CaptureStdout();
        ParseBlockSizeThreshold(invalid, invalidConfig, printHelp);
        const std::string capture = testing::internal::GetCapturedStdout();
        EXPECT_TRUE(printHelp) << invalid;
        EXPECT_EQ(invalidConfig.blockSizeThreshold, 7u) << invalid;
        EXPECT_NE(capture.find("Threshold must be a non-negative integer"), std::string::npos) << invalid;
    }
}

// ParseSampleRate(L1087-1101)：1与uint32上界合法；0/非数字/超界非法且保持原值
TEST(ClientParser, parse_sample_rate_values)
{
    bool printHelp = false;
    Config oneConfig{};
    ParseSampleRate("1", oneConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(oneConfig.sampleRate, 1u);

    printHelp = false;
    Config maxConfig{};
    ParseSampleRate("4294967295", maxConfig, printHelp);
    EXPECT_FALSE(printHelp);
    EXPECT_EQ(maxConfig.sampleRate, 0xFFFFFFFFu);

    const char *invalidValues[] = {"0", "abc", "4294967296"};
    for (const char *invalid : invalidValues)
    {
        printHelp = false;
        Config invalidConfig{};
        invalidConfig.sampleRate = 7;
        testing::internal::CaptureStdout();
        ParseSampleRate(invalid, invalidConfig, printHelp);
        const std::string capture = testing::internal::GetCapturedStdout();
        EXPECT_TRUE(printHelp) << invalid;
        EXPECT_EQ(invalidConfig.sampleRate, 7u) << invalid;
        EXPECT_NE(capture.find("Rate must be a positive integer within uint32"), std::string::npos) << invalid;
    }
}

// ParseUserCommand的HOST_LEAK_MODE/BLOCK_SIZE_THRESHOLD/SAMPLE_RATE分支(L1210-1218)
TEST(ClientParser, parse_host_leak_options_via_cli)
{
    std::vector<const char *> argv = {"msmemscope", "--host-leak-mode=event", "--block-size-threshold=4096",
                                      "--sample-rate=2"};
    optind = 1;
    ClientParser cliParser;
    UserCommand cmd = cliParser.Parse(static_cast<int32_t>(argv.size()), const_cast<char **>(argv.data()));
    ASSERT_FALSE(cmd.printHelpInfo);
    EXPECT_EQ(cmd.config.hostLeakMode, static_cast<uint8_t>(HostLeakMode::EVENT));
    EXPECT_EQ(cmd.config.blockSizeThreshold, 4096u);
    EXPECT_EQ(cmd.config.sampleRate, 2u);
}

// DoUserCommand attach分支(L305-311)：--pid目标不存在时 attach 流程执行并以1退出。
// AttachController::Run 内部 std::exit，必须在 fork 子进程中执行
TEST(ClientParser, interpretor_attach_mode_runs_attach_and_exits)
{
    const ChildRunResult result = RunInFork(
        []()
        {
            // 大于Linux pid上限(PID_MAX_LIMIT)的合法参数值，保证目标进程必然不存在
            std::vector<const char *> argv = {"msmemscope", "--pid", "2147483647"};
            optind = 1;
            ClientParser cliParser;
            cliParser.Interpretor(static_cast<int32_t>(argv.size()), const_cast<char **>(argv.data()));
        });
    ASSERT_TRUE(WIFEXITED(result.status)) << result.output;
    EXPECT_EQ(WEXITSTATUS(result.status), 1) << result.output;
    EXPECT_NE(result.output.find("does not exist"), std::string::npos) << result.output;
}

// DoUserCommand非attach分支(L313-318)：compare流程执行(PrintLogo/SetConfig/Command::Exec)。
// ConfigManager属跨用例单例，在fork子进程内执行；输出目录指向临时目录避免污染工作区
TEST(ClientParser, interpretor_compare_mode_runs_comparison)
{
    const std::string fileA = MakeTempFilePath("compare_a");
    const std::string fileB = MakeTempFilePath("compare_b");
    const std::string outDir = MakeTempDirPath("compare_out");
    ASSERT_FALSE(fileA.empty());
    ASSERT_FALSE(fileB.empty());
    ASSERT_FALSE(outDir.empty());

    const ChildRunResult result = RunInFork(
        [fileA, fileB, outDir]()
        {
            // 清空同进程先行用例可能残留的比对数据(非csv路径ReadFile不改写这些容器)，
            // 保证走到"无有效数据→打印Memory comparison failed!"的确定分支
            MemoryCompare &comparer = MemoryCompare::GetInstance();
            comparer.baseFileOriginData_.clear();
            comparer.compareFileOriginData_.clear();
            comparer.result_.clear();
            comparer.deviceIdSet_.clear();

            const std::string inputArg = "--input-path=" + fileA + "," + fileB;
            const std::string outputArg = "--output-path=" + outDir;
            std::vector<std::string> args = {"msmemscope", "--compare", inputArg, outputArg};
            std::vector<const char *> argv;
            for (const std::string &arg : args)
            {
                argv.push_back(arg.c_str());
            }
            optind = 1;
            ClientParser cliParser;
            cliParser.Interpretor(static_cast<int32_t>(argv.size()), const_cast<char **>(argv.data()));
        });
    ASSERT_TRUE(WIFEXITED(result.status)) << result.output;
    // 非csv输入使对比失败，但非attach流程正常走完(返回后子进程std::exit(0))
    EXPECT_EQ(WEXITSTATUS(result.status), 0) << result.output;
    EXPECT_NE(result.output.find("Memory comparison failed!"), std::string::npos) << result.output;

    ::unlink(fileA.c_str());
    ::unlink(fileB.c_str());
    ::rmdir(outDir.c_str());
}
}  // namespace
