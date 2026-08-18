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
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#define private public
#include "client_parser.h"
#include "log.h"
#include "bit_field.h"
#undef private

using namespace MemScope;

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