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
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "config_info.h"
#include "file.h"
#include "bit_field.h"
#include "event_trace/trace_manager/event_trace_manager.h"

#define private public
#include "memory_compare.h"
#undef private

using namespace MemScope;

class MemoryCompareTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        // 保存全局配置，用例内修改 levelType 后由 TearDown 恢复，避免影响其他用例
        savedConfig_ = GetConfig();
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("./testmsmemscope");
    }

    void TearDown() override
    {
        ConfigManager::Instance().SetConfig(savedConfig_);
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("");
        rmdir("./testmsmemscope");
    }

    Config savedConfig_;
};

void CreateCsvData(ORIGINAL_FILE_DATA &data)
{
    std::unordered_map<std::string, std::string> temp;
    temp["Event"] = "MALLOC";
    temp["Event Type"] = "PTA";
    temp["Attr"] = "{addr:1000,size:123,owner:,MID:3}";
    temp["Device Id"] = "0";
    temp["Timestamp(ns)"] = "1";
    data.emplace_back(temp);
    temp["Event"] = "KERNEL_LAUNCH";
    temp["Event Type"] = "KERNEL_LAUNCH";
    temp["Name"] = "matmul_v1";
    temp["Timestamp(ns)"] = "2";
    data.emplace_back(temp);
    temp["Event"] = "KERNEL_LAUNCH";
    temp["Event Type"] = "KERNEL_LAUNCH";
    temp["Name"] = "matmul_v2";
    temp["Timestamp(ns)"] = "3";
    data.emplace_back(temp);
    temp["Event"] = "MALLOC";
    temp["Event Type"] = "PTA";
    temp["Attr"] = "{addr:1001,size:124,owner:,MID:3}";
    temp["Timestamp(ns)"] = "4";
    data.emplace_back(temp);
}

TEST_F(MemoryCompareTest, do_read_csv_file_expect_read_correct_data)
{
    Utility::UmaskGuard umaskGuard(Utility::DEFAULT_UMASK_FOR_CSV_FILE);
    FILE *fp = fopen("test_memscope.csv", "w");
    std::string testHeader = std::string(MEMSCOPE_HEADERS);
    fprintf(fp, testHeader.c_str());

    for (int index = 0; index < 10; ++index) {
        fprintf(fp, "1,MALLOC,PTA,N/A,%d,123,234,0,0,{size:124},,\n", index);
    }

    for (int index = 0; index < 5; ++index) {
        fprintf(fp, "2,KERNEL_LAUNCH,KERNEL_LAUNCH,null,%d,123,234,0,0,N/A,,\n", index+10);
    }

    for (int index = 0; index < 5; ++index) {
        fprintf(fp, "3,MALLOC,PTA,N/A,%d,123,234,2,0,N/A,,\n", index+2);
    }

    for (int index = 4; index >= 0; --index) {
        fprintf(fp, "4,KERNEL_LAUNCH,KERNEL_LAUNCH,null,%d,123,234,2,0,{size:224},,\n", index+1);
    }

    fclose(fp);
    Config config;
    config.enableCStack = false;
    config.enablePyStack = false;
    MemoryCompare memCompare;
    std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> data;
    std::string str = "test_memscope.csv";
    memCompare.ReadCsvFile(str, data);
    ASSERT_EQ(data.size(), 2);
    ASSERT_EQ(data[0].size(), 15);
    ASSERT_EQ(data[2].size(), 10);
    remove("test_memscope.csv");
}

TEST_F(MemoryCompareTest, do_read_invalid_csv_file_expect_empty_data)
{
    Utility::UmaskGuard umaskGuard(Utility::DEFAULT_UMASK_FOR_CSV_FILE);
    Config config;
    FILE *fp = fopen("test_memscope.csv", "w");
    std::string headers = "testheader1,testheader2\n";
    fprintf(fp, headers.c_str());

    fclose(fp);
    MemoryCompare memCompare;
    std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> data;
    std::string str = "test_memscope.csv";
    memCompare.ReadCsvFile(str, data);
    ASSERT_EQ(data.size(), 0);
    remove("test_memscope.csv");
    str = "test.csv";
    memCompare.ReadCsvFile(str, data);
    ASSERT_EQ(data.size(), 0);

    fp = fopen("test_memscope.csv", "w");
    std::string testHeader = std::string(MEMSCOPE_HEADERS);
    fprintf(fp, testHeader.c_str());
    fprintf(fp, "1,0,pytorch,malloc,123,234,0,0,N/A,N/A,0\n");
    fclose(fp);
    // 上面写入的是合法12列表头+11列数据行，需读回该文件才能进入"列数不匹配"分支
    // (此处若沿用上面的"test.csv"会因文件不存在走!is_open分支，断言形同虚设)
    str = "test_memscope.csv";
    memCompare.ReadCsvFile(str, data);
    ASSERT_EQ(data.size(), 0);
    remove("test_memscope.csv");
}

TEST_F(MemoryCompareTest, do_read_kernelLaunch_data_expect_return_true_and_correct_data)
{
    Config config;
    BitField<decltype(config.levelType)> levelBit;
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    config.levelType = levelBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    ORIGINAL_FILE_DATA data;
    CreateCsvData(data);
    MemoryCompare memCompare;
    NAME_WITH_INDEX result;
    memCompare.ReadNameIndexData(data, result);
    ASSERT_EQ(result.size(), 2);
    ASSERT_EQ(std::get<0>(result[0]), "matmul_v1");
    ASSERT_EQ(std::get<2>(result[0]), 1);
    ASSERT_EQ(std::get<0>(result[1]), "matmul_v2");
    ASSERT_EQ(std::get<2>(result[1]), 2);
}

TEST_F(MemoryCompareTest, do_read_no_kernelLaunch_data_expect_return_false_and_empty_data)
{
    Config config;
    ORIGINAL_FILE_DATA data;
    MemoryCompare memCompare;
    NAME_WITH_INDEX result;
    memCompare.ReadNameIndexData(data, result);
    ASSERT_EQ(result.size(), 0);
}

TEST_F(MemoryCompareTest, do_read_invalid_kernelLaunch_data_expect_falseand_empty_data)
{
    Config config;
    ORIGINAL_FILE_DATA data;
    CreateCsvData(data);
    data[1]["Name"] = "+test";
    MemoryCompare memCompare;
    NAME_WITH_INDEX result;
    memCompare.ReadNameIndexData(data, result);
    ASSERT_EQ(result.size(), 0);
}

TEST_F(MemoryCompareTest, do_get_kernel_memory_diff_expect_correct_data)
{
    Config config;
    ORIGINAL_FILE_DATA data;
    CreateCsvData(data);
    MemoryCompare memCompare;
    int64_t result;
    memCompare.framework_ = "PTA";
    memCompare.GetMemoryUsage(1, data, result);
    ASSERT_EQ(result, 124);

    memCompare.GetMemoryUsage(2, data, result);
    ASSERT_EQ(result, 124);

    data[3]["Attr"] = "test";
    memCompare.GetMemoryUsage(1, data, result);

    data[3]["Event Type"] = "KERNEL_LAUNCH";
    memCompare.GetMemoryUsage(1, data, result);
    ASSERT_EQ(result, 0);
}

TEST_F(MemoryCompareTest, do_write_compare_data_to_csv_expect_true)
{
    Config config;
    MemoryCompare memCompare;
    std::string temp;
    temp = "matmul,0,10,0,11,20,0,21,10,0,10\n";
    memCompare.result_[0].emplace_back(temp);
    Utility::UmaskGuard umaskGuard(Utility::DEFAULT_UMASK_FOR_CSV_FILE);
    FILE *fp = fopen("test_memscope.csv", "w");
    memCompare.compareFile_ = fp;
    ASSERT_TRUE(memCompare.WriteCompareDataToCsv());
    remove("test_memscope.csv");
}

TEST_F(MemoryCompareTest, do_empty_compare_data_to_csv_expect_false)
{
    Config config;
    Utility::UmaskGuard umaskGuard(Utility::DEFAULT_UMASK_FOR_CSV_FILE);
    MemoryCompare memCompare;
    FILE *fp = fopen("test_memscope.csv", "w");
    memCompare.compareFile_ = fp;
    ASSERT_FALSE(memCompare.WriteCompareDataToCsv());
    remove("test_memscope.csv");
}

TEST_F(MemoryCompareTest, do_save_compare_kernel_memory_expect_correct_data)
{
    Config config;
    ORIGINAL_FILE_DATA data;
    CreateCsvData(data);
    MemoryCompare memCompare;
    memCompare.framework_ = "PTA";
    memCompare.baseFileOriginData_[0] = data;
    data[3]["Attr"] = "{addr:20623378087936,size:130,owner:,total:0,used:2822144}";
    memCompare.compareFileOriginData_[0] = data;

    std::string temp;
    temp = "KERNEL_LAUNCH,matmul_v1,0,124,130,6";

    memCompare.CalcuMemoryDiff(0, {"matmul_v1", "KERNEL_LAUNCH", 1}, {"matmul_v1", "KERNEL_LAUNCH", 1});
    ASSERT_EQ(memCompare.result_[0][0], temp);

    memCompare.CalcuMemoryDiff(0, {"matmul_v2", "KERNEL_LAUNCH", 2}, {"matmul_v2", "KERNEL_LAUNCH", 2});
    temp = "KERNEL_LAUNCH,matmul_v2,0,124,130,6";
    ASSERT_EQ(memCompare.result_[0][1], temp);
}

TEST_F(MemoryCompareTest, do_build_path_expect_coorrect_data)
{
    NAME_WITH_INDEX kernelIndexMap {};
    kernelIndexMap.emplace_back(std::make_tuple("matmul_v1", "KERNEL_LAUNCH", 1));
    kernelIndexMap.emplace_back(std::make_tuple("matmul_v2", "KERNEL_LAUNCH", 2));
    NAME_WITH_INDEX kernelIndexCompareMap {};
    kernelIndexCompareMap.emplace_back(std::make_tuple("mul", "KERNEL_LAUNCH", 1));
    kernelIndexCompareMap.emplace_back(std::make_tuple("matmul_v2", "KERNEL_LAUNCH", 2));

    std::shared_ptr<PathNode> pathNode1 = std::make_shared<DiffNode>(0, 0, nullptr);
    std::shared_ptr<PathNode> pathNode2 = std::make_shared<DiffNode>(1, 0, pathNode1);
    std::shared_ptr<PathNode> pathNode3 = std::make_shared<DiffNode>(1, 1, pathNode2);
    std::shared_ptr<PathNode> pathNode4 = std::make_shared<Snake>(2, 2, pathNode3);

    Config config;
    MemoryCompare memCompare;
    auto pathNode = memCompare.BuildPath(kernelIndexMap, kernelIndexCompareMap);
    ASSERT_EQ(pathNode->i, pathNode4->i);
    ASSERT_EQ(pathNode->j, pathNode4->j);
    pathNode = pathNode->prev;
    ASSERT_EQ(pathNode->i, pathNode3->i);
    ASSERT_EQ(pathNode->j, pathNode3->j);
    pathNode = pathNode->prev;
    ASSERT_EQ(pathNode->i, pathNode2->i);
    ASSERT_EQ(pathNode->j, pathNode2->j);
    pathNode = pathNode->prev;
    ASSERT_EQ(pathNode->i, pathNode1->i);
    ASSERT_EQ(pathNode->j, pathNode1->j);
}

TEST_F(MemoryCompareTest, do_empty_path_build_diff_expect_empty_data)
{
    Config config;
    std::shared_ptr<PathNode> pathNode;
    NAME_WITH_INDEX kernelIndexMap {};
    NAME_WITH_INDEX kernelIndexCompareMap {};
    MemoryCompare memCompare;
    memCompare.BuildDiff(pathNode, 0, kernelIndexMap, kernelIndexCompareMap);
    ASSERT_EQ(memCompare.result_.size(), 0);
}

TEST_F(MemoryCompareTest, do_build_diff_expect_correct_data)
{
    std::shared_ptr<PathNode> pathNode1 = std::make_shared<DiffNode>(0, 0, nullptr);
    std::shared_ptr<PathNode> pathNode2 = std::make_shared<DiffNode>(1, 0, pathNode1);
    std::shared_ptr<PathNode> pathNode3 = std::make_shared<DiffNode>(1, 1, pathNode2);
    std::shared_ptr<PathNode> pathNode4 = std::make_shared<Snake>(2, 2, pathNode3);

    Config config;
    ORIGINAL_FILE_DATA data;
    CreateCsvData(data);
    MemoryCompare memCompare;
    memCompare.baseFileOriginData_[0] = data;
    data[1]["name"] = "mul";
    memCompare.compareFileOriginData_[0] = data;
    NAME_WITH_INDEX kernelIndexMap {};
    kernelIndexMap.emplace_back(std::make_tuple("matmul_v1", "KERNEL_LAUNCH", 1));
    kernelIndexMap.emplace_back(std::make_tuple("matmul_v2", "KERNEL_LAUNCH", 2));
    NAME_WITH_INDEX kernelIndexCompareMap {};
    kernelIndexCompareMap.emplace_back(std::make_tuple("mul", "KERNEL_LAUNCH", 1));
    kernelIndexCompareMap.emplace_back(std::make_tuple("matmul_v2", "KERNEL_LAUNCH", 2));

    memCompare.BuildDiff(pathNode4, 0, kernelIndexMap, kernelIndexCompareMap);
    ASSERT_EQ(memCompare.result_[0].size(), 3);
}

TEST_F(MemoryCompareTest, do_myersdiff_input_kernelLaunch_data)
{
    Config config;
    NAME_WITH_INDEX kernelIndexMap {};
    NAME_WITH_INDEX kernelIndexCompareMap {};
    MemoryCompare memCompare;
    memCompare.MyersDiff(0, kernelIndexMap, kernelIndexCompareMap);
    ASSERT_EQ(memCompare.result_[0].size(), 0);

    ORIGINAL_FILE_DATA data;
    CreateCsvData(data);
    data[1]["name"] = "mul";
    kernelIndexMap.clear();
    kernelIndexMap.emplace_back(std::make_tuple("matmul_v1", "KERNEL_LAUNCH", 1));
    kernelIndexMap.emplace_back(std::make_tuple("matmul_v2", "KERNEL_LAUNCH", 2));
    kernelIndexCompareMap.clear();
    kernelIndexCompareMap.emplace_back(std::make_tuple("mul", "KERNEL_LAUNCH", 1));
    kernelIndexCompareMap.emplace_back(std::make_tuple("matmul_v2", "KERNEL_LAUNCH", 2));

    memCompare.MyersDiff(0, kernelIndexMap, kernelIndexCompareMap);
    ASSERT_EQ(memCompare.result_[0].size(), 3);
}

TEST_F(MemoryCompareTest, do_stepinter_compare_input_invalid_path_return_empty_data)
{
    Config config;
    std::vector<std::string> paths;
    paths.emplace_back("test_path1");
    paths.emplace_back("test_path2");

    MemoryCompare memCompare;
    memCompare.RunComparison(paths);
    ASSERT_EQ(memCompare.baseFileOriginData_.size(), 0);
}

TEST_F(MemoryCompareTest, do_kernel_launch_compare)
{
    Config config;
    BitField<decltype(config.levelType)> levelBit;
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_KERNEL));
    config.levelType = levelBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    config.enableCStack = false;
    config.enablePyStack = false;
    Utility::UmaskGuard umaskGuard(Utility::DEFAULT_UMASK_FOR_CSV_FILE);
    FILE *fp = fopen("test_memscope.csv", "w");
    std::string testHeader = std::string(MEMSCOPE_HEADERS);
    fprintf(fp, testHeader.c_str());

    for (int index = 0; index < 10; ++index) {
        fprintf(fp, "1,MALLOC,PTA,N/A,%d,123,234,0,0,{size:1024},,\n", index);
    }

    for (int index = 0; index < 5; ++index) {
        fprintf(fp, "2,KERNEL_LAUNCH,KERNEL_LAUNCH,null,%d,123,234,0,0,N/A,,\n", index+10);
    }

    for (int index = 0; index < 5; ++index) {
        fprintf(fp, "3,MALLOC,PTA,N/A,%d,123,234,2,0,{size:1024},,\n", index+2);
    }

    for (int index = 4; index >= 0; --index) {
        fprintf(fp, "4,KERNEL_LAUNCH,KERNEL_LAUNCH,null,%d,123,234,2,0,N/A,,\n", index+1);
    }

    fprintf(fp, "5,MALLOC,PTA,N/A,5,123,234,2,0,{size:1024},,\n");

    fclose(fp);
    std::vector<std::string> paths;
    paths.emplace_back("test_memscope.csv");
    paths.emplace_back("test_memscope.csv");

    MemoryCompare memCompare;
    memCompare.RunComparison(paths);
    ASSERT_EQ(memCompare.result_.size(), 2);
    remove("test_memscope.csv");
}

// ==================== 引用字段 / 排序 / 非法数据行补充 ====================

namespace MemScope
{
// memory_compare.cpp 内部自由函数（供 sort 使用），此处仅作外部声明以直接驱动
bool Compare(const std::unordered_map<std::string, std::string> &a,
             const std::unordered_map<std::string, std::string> &b);
}  // namespace MemScope

namespace {
// 按 MEMSCOPE_HEADERS 生成一行 12 列数据；deviceId 传入非数字可构造解析失败场景
std::string MakeCompareCsvRow(const std::string &eventType, const std::string &deviceId)
{
    return "1," + eventType + "," + eventType + ",name,1,123,234," + deviceId + ",0,{size:124},,\n";
}

// 构造 40 层超限目录，覆盖创建 compare csv 失败的路径
std::string MakeCompareOverDepthDir(const std::string &base, std::vector<std::string> &created)
{
    std::string cur = base;
    created.push_back(cur);
    for (int i = 0; i < 40; ++i) {
        cur += "/d" + std::to_string(i);
        created.push_back(cur);
    }
    (void)Utility::MakeDir(cur);
    return cur;
}
}  // namespace

// ReadQuotedField：引号包裹的字段整体读出（字段内逗号不作为分隔符）并跳过字段后的分隔逗号，
// 后续普通字段仍按逗号切分
TEST_F(MemoryCompareTest, read_quoted_field_with_comma_expect_whole_field)
{
    MemoryCompare memCompare;
    std::stringstream ss("\"mat, mul, v\",123");
    EXPECT_EQ(memCompare.ReadQuotedField(ss), "mat, mul, v");
    EXPECT_EQ(memCompare.ReadQuotedField(ss), "123");
}

// Compare：时间戳无法解析时按 UINT64_MAX 处理，排序结果仍可比较
TEST_F(MemoryCompareTest, compare_invalid_timestamp_expect_uint64_max)
{
    std::unordered_map<std::string, std::string> valid;
    valid["Timestamp(ns)"] = "10";
    std::unordered_map<std::string, std::string> invalid;
    invalid["Timestamp(ns)"] = "abc";

    EXPECT_TRUE(Compare(valid, invalid));   // 10 < UINT64_MAX
    EXPECT_FALSE(Compare(invalid, valid));  // UINT64_MAX < 10 为假
    EXPECT_FALSE(Compare(valid, valid));
}

// ReadCsvFile：数据行列数与表头不一致时清空已读数据并返回
TEST_F(MemoryCompareTest, read_csv_invalid_line_expect_data_cleared)
{
    Utility::UmaskGuard umaskGuard(Utility::DEFAULT_UMASK_FOR_CSV_FILE);
    const std::string path = "msms_cmp_bad_line.csv";
    FILE *fp = fopen(path.c_str(), "w");
    ASSERT_NE(fp, nullptr);
    fprintf(fp, "%s", MEMSCOPE_HEADERS);
    fprintf(fp, "1,2\n");
    fclose(fp);

    MemoryCompare memCompare;
    std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> data;
    data[0].emplace_back(std::unordered_map<std::string, std::string>{});
    std::string filePath = path;
    memCompare.ReadCsvFile(filePath, data);

    EXPECT_TRUE(data.empty());
    remove(path.c_str());
}

// ReadCsvFile：同一文件内出现两种受支持框架时判为非法并清空数据
TEST_F(MemoryCompareTest, read_csv_framework_mismatch_expect_data_cleared)
{
    Utility::UmaskGuard umaskGuard(Utility::DEFAULT_UMASK_FOR_CSV_FILE);
    const std::string path = "msms_cmp_framework.csv";
    FILE *fp = fopen(path.c_str(), "w");
    ASSERT_NE(fp, nullptr);
    fprintf(fp, "%s", MEMSCOPE_HEADERS);
    fprintf(fp, "%s", MakeCompareCsvRow("PTA", "0").c_str());
    fprintf(fp, "%s", MakeCompareCsvRow("MINDSPORE", "0").c_str());
    fclose(fp);

    MemoryCompare memCompare;
    std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> data;
    std::string filePath = path;
    memCompare.ReadCsvFile(filePath, data);

    EXPECT_TRUE(data.empty());
    remove(path.c_str());
}

// ReadCsvFile：host/N/A/无效设备号(9999)与不可解析设备号的行被跳过，仅保留有效设备数据
TEST_F(MemoryCompareTest, read_csv_invalid_device_id_skipped)
{
    Utility::UmaskGuard umaskGuard(Utility::DEFAULT_UMASK_FOR_CSV_FILE);
    const std::string path = "msms_cmp_device.csv";
    FILE *fp = fopen(path.c_str(), "w");
    ASSERT_NE(fp, nullptr);
    fprintf(fp, "%s", MEMSCOPE_HEADERS);
    fprintf(fp, "%s", MakeCompareCsvRow("PTA", "host").c_str());
    fprintf(fp, "%s", MakeCompareCsvRow("PTA", "N/A").c_str());
    fprintf(fp, "%s", MakeCompareCsvRow("PTA", "9999").c_str());
    fprintf(fp, "%s", MakeCompareCsvRow("PTA", "abc").c_str());
    fprintf(fp, "%s", MakeCompareCsvRow("PTA", "1").c_str());
    fclose(fp);

    MemoryCompare memCompare;
    std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> data;
    std::string filePath = path;
    memCompare.ReadCsvFile(filePath, data);

    ASSERT_EQ(data.size(), 1);
    EXPECT_EQ(data[1].size(), 1);
    remove(path.c_str());
}

// ReadNameIndexData：op 级别下 MINDSPORE 框架不支持比对，直接返回空结果
TEST_F(MemoryCompareTest, read_name_index_mindspore_op_level_expect_empty)
{
    Config opConfig{};
    BitField<decltype(opConfig.levelType)> levelBit;
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    opConfig.levelType = levelBit.getValue();
    ConfigManager::Instance().SetConfig(opConfig);

    ORIGINAL_FILE_DATA data;
    CreateCsvData(data);
    // 追加一条 op 级可比对记录：若未走 MINDSPORE 早退，该记录会被收集，使空结果断言具备区分度
    std::unordered_map<std::string, std::string> row;
    row["Event"] = "ATB_END";
    row["Event Type"] = "ATB_END";
    row["Name"] = "op_atb";
    data.emplace_back(row);
    MemoryCompare memCompare;
    memCompare.framework_ = "MINDSPORE";
    NAME_WITH_INDEX result;
    memCompare.ReadNameIndexData(data, result);

    EXPECT_TRUE(result.empty());
}

// ReadNameIndexData：op 级别下 PTA 框架收集 ATB_END / ATEN_END 事件
TEST_F(MemoryCompareTest, read_name_index_pta_op_level_expect_atb_and_aten)
{
    Config opConfig{};
    BitField<decltype(opConfig.levelType)> levelBit;
    levelBit.setBit(static_cast<size_t>(LevelType::LEVEL_OP));
    opConfig.levelType = levelBit.getValue();
    ConfigManager::Instance().SetConfig(opConfig);

    ORIGINAL_FILE_DATA data;
    std::unordered_map<std::string, std::string> row;
    row["Event"] = "ATB_END";
    row["Event Type"] = "ATB_END";
    row["Name"] = "op_atb";
    data.emplace_back(row);
    row["Event"] = "ATEN_END";
    row["Event Type"] = "ATEN_END";
    row["Name"] = "op_aten";
    data.emplace_back(row);

    MemoryCompare memCompare;
    memCompare.framework_ = "PTA";
    NAME_WITH_INDEX result;
    memCompare.ReadNameIndexData(data, result);

    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(std::get<0>(result[0]), "op_atb");
    EXPECT_EQ(std::get<0>(result[1]), "op_aten");
}

// GetMemoryUsage：Attr 中 size 非数字时保留调用方传入的 memDiff
TEST_F(MemoryCompareTest, get_memory_usage_invalid_size_expect_diff_unchanged)
{
    ORIGINAL_FILE_DATA data;
    CreateCsvData(data);
    data[3]["Event Type"] = "PTA";
    data[3]["Attr"] = "{addr:1001,size:notanumber,owner:,MID:3}";
    MemoryCompare memCompare;
    memCompare.framework_ = "PTA";

    int64_t memDiff = 777;
    memCompare.GetMemoryUsage(1, data, memDiff);
    EXPECT_EQ(memDiff, 777);
}

// WriteCompareDataToCsv：fprintf 写文件失败（/dev/full 无缓冲）时打印错误并返回 false
TEST_F(MemoryCompareTest, write_compare_csv_fprintf_failed_expect_false)
{
    MemoryCompare memCompare;
    // compareFile_ 非空时 CreateCsvFile 直接返回成功，保留注入的句柄
    memCompare.compareFile_ = fopen("/dev/full", "w");
    ASSERT_NE(memCompare.compareFile_, nullptr);
    ASSERT_EQ(setvbuf(memCompare.compareFile_, nullptr, _IONBF, 0), 0);
    memCompare.result_[0].emplace_back("KERNEL_LAUNCH,k,0,1,1,0");

    std::ostringstream oss;
    std::streambuf *oldBuf = std::cout.rdbuf(oss.rdbuf());
    bool ret = memCompare.WriteCompareDataToCsv();
    std::cout.rdbuf(oldBuf);

    EXPECT_FALSE(ret);
    EXPECT_NE(oss.str().find("Fail to write data to csv file"), std::string::npos);
    fclose(memCompare.compareFile_);
    memCompare.compareFile_ = nullptr;
}
