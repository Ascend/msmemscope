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

#include "memory_compare.h"
#include <fstream>
#include <sstream>
#include "file.h"
#include "utils.h"
#include "config_info.h"
#include "record_info.h"
#include "ustring.h"
#include "bit_field.h"

namespace MemScope {

MemoryCompare& MemoryCompare::GetInstance(Config config)
{
    static MemoryCompare instance(config);
    return instance;
}

MemoryCompare::MemoryCompare(Config config)
{
    config_ = config;
}

// 用此方式依次读取CSV的每一行，不会被单格数据的逗号干扰
std::string MemoryCompare::ReadQuotedField(std::stringstream& ss)
{
    std::string field;
    if (ss.peek() == '"') {  // 检查是否以引号开头，如果是就跳过（因为其中可能存在逗号）
        ss.get();
        std::getline(ss, field, '"');

        // 处理转义引号
        size_t pos = 0;
        while ((pos = field.find("\"\"", pos)) != std::string::npos) {
            field.replace(pos, 2, "\"");
            pos += 1;
        }
        // 跳过可能的分隔符（逗号）
        if (ss.peek() == ',') {
            ss.get();
        }
    } else {
        std::getline(ss, field, ',');  // 普通字段
    }
    return field;
}

bool Compare(const std::unordered_map<std::string, std::string> &a,
    const std::unordered_map<std::string, std::string> &b)
{
    uint64_t compareA;
    uint64_t compareB;

    if (!Utility::StrToUint64(compareA, a.at("Timestamp(ns)"))) {
        LOG_WARN("StrToUint64 failed, the str is %s.", a.at("Timestamp(ns)").c_str());
        compareA = UINT64_MAX;
    }
    if (!Utility::StrToUint64(compareB, b.at("Timestamp(ns)"))) {
        LOG_WARN("StrToUint64 failed, the str is %s.", b.at("Timestamp(ns)").c_str());
        compareB = UINT64_MAX;
    }

    return compareA < compareB;
}

void MemoryCompare::ReadFile(std::string &path, std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> &data)
{
    std::vector<std::string> fileName;
    Utility::Split(path, std::back_inserter(fileName), ".");
    if (fileName.size() > 0 && fileName.back() == "csv") {
        LOG_INFO("Read csv file: %s.", path.c_str());
        ReadCsvFile(path, data);
        for (const auto& pair : data) {
            uint64_t deviceId = pair.first;
            // 需要根据timestamp排序保证顺序
            sort(data[deviceId].begin(), data[deviceId].end(), Compare);
        }
    } else {
        LOG_ERROR("The file %s is an unsupported format.", path.c_str());
    }
}

bool MemoryCompare::CheckCsvHeader(std::string &path, std::ifstream& file, std::vector<std::string> &headerData)
{
    if (!file.is_open()) {
        LOG_ERROR("The path: %s open failed!", path.c_str());
        return false;
    }
    std::string line;
    getline(file, line);

    std::string normalizedLine = NormalizeString(line);
    if (normalizedLine + "\n" != std::string(MEMSCOPE_HEADERS)) {
        return false;
    }

    Utility::Split(normalizedLine, std::back_inserter(headerData), ",");

    return true;
}


std::string MemoryCompare::NormalizeString(const std::string& line)
{
    std::string result = line;
    // 清除header的多余\r或者\n
    result.erase(
        std::remove_if(result.begin(), result.end(), [](unsigned char c) {
            return c == '\r' || c == '\n';
        }),
        result.end()
    );

    // 清除header多余前导和后缀空格
    auto start = result.begin();
    auto end = result.end();

    while (start != end && std::isspace(*start)) ++start;
    while (start != end && std::isspace(*(end - 1))) --end;

    return std::string(start, end);
}


bool IsSupportedFramework(const std::string& name)
{
    static const std::unordered_set<std::string> supportedFrameworks = {"PTA", "MINDSPORE"}; // 暂不支持PTA_WORKSPACE的比对
    return supportedFrameworks.find(name) != supportedFrameworks.end();
}

void MemoryCompare::ReadCsvFile(std::string &path, std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> &data)
{
    std::ifstream csvFile(path, std::ios::in);
    std::vector<std::string> headerData;
    if (!CheckCsvHeader(path, csvFile, headerData)) {
        LOG_ERROR("The headers of %s file is illegal!", path.c_str());
        return ;
    }
    std::string line;
    uint64_t countLine = 1;
    while (getline(csvFile, line)) {
        ++countLine;
        std::vector<std::string> lineData;
        std::stringstream ss(line);
        while (ss.good()) {
            std::string singleValue = ReadQuotedField(ss);
            Utility::ToSafeString(singleValue);
            lineData.emplace_back(singleValue);
        }
        if (lineData.size() != headerData.size()) {
            LOG_ERROR("The file %s on line %d is invalid!", path.c_str(), countLine);
            data.clear();
            return ;
        }
        std::unordered_map<std::string, std::string> tempLine;
        for (size_t index = 0; index < headerData.size(); ++index) {
            tempLine.insert({headerData[index], lineData[index]});
        }
        if (IsSupportedFramework(tempLine["Event Type"])) {
            if (framework_.empty()) {
                framework_ = tempLine["Event Type"];
            }
            if (framework_ != tempLine["Event Type"]) {
                LOG_ERROR("The content of the file %s is invalid.", path.c_str());
                data.clear();
                return ;
            }
        }
        uint64_t deviceId;
        if (tempLine["Device Id"] == std::to_string(GD_INVALID_NUM) || tempLine["Device Id"] == "host" ||
            tempLine["Device Id"] == "N/A") {
            continue;
        }
        if (!Utility::StrToUint64(deviceId, tempLine["Device Id"])) {
            LOG_WARN("StrToUint64 failed, the str is %s.", tempLine["Device Id"].c_str());
            continue;
        }
        data[deviceId].emplace_back(tempLine);
    }
    csvFile.close();
}

void MemoryCompare::ReadNameIndexData(const ORIGINAL_FILE_DATA &originData, NAME_WITH_INDEX &dataList)
{
    LOG_DEBUG("Read kernelLaunch/op data.");
    std::unordered_set<std::string> eventMap;
    BitField<decltype(config_.levelType)> levelType(config_.levelType);
    if (levelType.checkBit(static_cast<size_t>(LevelType::LEVEL_OP))) {
        if (framework_ == "MINDSPORE") {
            LOG_ERROR("Comparison of the MindSpore framework under the op level is not supported.");
            return ;
        }
        eventMap.insert("ATB_END");
        eventMap.insert("ATEN_END");
    }
    if (levelType.checkBit(static_cast<size_t>(LevelType::LEVEL_KERNEL))) {
        eventMap.insert("KERNEL_LAUNCH");
    }
    for (size_t index = 0; index < originData.size(); ++index) {
        auto lineData = originData[index];
        if (eventMap.find(lineData["Event Type"]) != eventMap.end()) {
            if (Utility::CheckStrIsStartsWithInvalidChar(lineData["Name"].c_str())) {
                LOG_ERROR("Name %s is invalid!", lineData["Name"].c_str());
                dataList.clear();
                return ;
            }
            dataList.emplace_back(std::make_tuple(lineData["Name"], lineData["Event"], index));
        }
    }
}

void MemoryCompare::GetMemoryUsage(size_t index, const ORIGINAL_FILE_DATA &data, int64_t &memDiff)
{
    LOG_DEBUG("Get memorypool usage.");
    std::unordered_map<std::string, std::string> frameworkMemory;
    for (size_t i = index; i < data.size(); ++i) {
        auto lineData = data[i];
        if (lineData["Event Type"] == framework_) {
            frameworkMemory = lineData;
            break;
        }
    }

    if (frameworkMemory.empty()) {
        memDiff = 0;
        return ;
    }

    std::string attrKey = "size";
    std::string attrValue = Utility::ExtractAttrValueByKey(frameworkMemory["Attr"], attrKey);
    if (attrValue.empty()) {
        LOG_WARN("Attr has no \"size\" value");
        return ;
    }
    if (!Utility::StrToInt64(memDiff, attrValue)) {
        LOG_WARN("Alloc Size to int64_t failed!");
    }
}

bool MemoryCompare::WriteCompareDataToCsv()
{
    LOG_DEBUG("Write compare result data to csv file.");
    if (result_.empty()) {
        LOG_WARN("Empty comparison result data!");
        return false;
    }

    if (!Utility::FileCreateManager::GetInstance(config_.outputDir).CreateCsvFile(&compareFile_,
        GD_INVALID_NUM, MEMORY_COMPARE_FILE_PREFIX, COMPARE_DIR, std::string(STEP_INTER_HEADERS))) {
        LOG_ERROR("Create comparison csv file failed!");
        return false;
    }

    for (const auto& pair : result_) {
        uint64_t deviceId = pair.first;
        std::reverse(result_[deviceId].begin(), result_[deviceId].end());

        for (const auto& str : result_[deviceId]) {
            int fpRes = fprintf(compareFile_, "%s\n", str.c_str());
            if (fpRes < 0) {
                std::cout << "[msmemscope] Error: Fail to write data to csv file, errno:" << fpRes << std::endl;
                return false;
            }
        }
    }

    return true;
}

void MemoryCompare::CalcuMemoryDiff(const DEVICEID deviceId,
    const std::tuple<std::string, std::string, size_t> &baseData,
    const std::tuple<std::string, std::string, size_t> &compareData)
{
    std::string temp;
    std::string name;
    std::string event;
    int64_t baseAllocSize = 0;
    int64_t compareAllocSize = 0;

    std::string baseMemDiff;
    if (!std::get<0>(baseData).empty()) {
        name = std::get<0>(baseData);
        event = std::get<1>(baseData);
        GetMemoryUsage(std::get<2>(baseData), baseFileOriginData_[deviceId], baseAllocSize);
        baseMemDiff = std::to_string(baseAllocSize);
    } else {
        baseMemDiff = "N/A";
    }

    std::string compareMemDiff;
    if (!std::get<0>(compareData).empty()) {
        name = std::get<0>(compareData);
        event = std::get<1>(compareData);
        GetMemoryUsage(std::get<2>(compareData), compareFileOriginData_[deviceId], compareAllocSize);
        compareMemDiff = std::to_string(compareAllocSize);
    } else {
        compareMemDiff = "N/A";
    }

    temp += event;
    temp = temp + "," + name + "," + std::to_string(deviceId) + "," + baseMemDiff + "," + compareMemDiff;

    int64_t diffAllocSize = Utility::GetSubResult(compareAllocSize, baseAllocSize);
    temp = temp + "," + std::to_string(diffAllocSize);
    result_[deviceId].emplace_back(temp);
}

std::shared_ptr<PathNode> MemoryCompare::BuildPath(const NAME_WITH_INDEX &baseLists,
    const NAME_WITH_INDEX &compareLists)
{
    LOG_DEBUG("Start to build myers path.");
    const int64_t n = static_cast<int64_t>(baseLists.size());
    const int64_t m = static_cast<int64_t>(compareLists.size());
    const int64_t max = m + n + 1;
    const int64_t size = 1 + 2 * max;
    const int64_t middle = size / 2;
    std::vector<std::shared_ptr<PathNode>> diagonal(size, nullptr); // 存储每一步的最优路径位置
    diagonal[middle + 1] = std::make_shared<Snake>(0, -1);
    auto start_time = Utility::GetTimeMicroseconds();
    for (int64_t d = 0; d < max; ++d) {
        for (int64_t k = -d; k <= d; k += KSTEPSIZE) {
            auto end_time = Utility::GetTimeMicroseconds();
            if ((end_time - start_time) >= MAXLOOPTIME) {
                LOG_ERROR("Memory comparison build path failed! Reaching maximum loop time limit!");
                break;
            }
            int64_t kmiddle = middle + k;
            int64_t kplus = kmiddle + 1;
            int64_t kminus = kmiddle - 1;
            int64_t i;
            std::shared_ptr<PathNode> prev;
            if ((k == -d) || (k != d && diagonal[kminus]->i < diagonal[kplus]->i)) { // 最优路径为从上往下走
                i = diagonal[kplus]->i;
                prev = diagonal[kplus];
            } else { // 最优路径为从左往右走
                i = diagonal[kminus]->i + 1;
                prev = diagonal[kminus];
            }
            int64_t j = i - k;
            diagonal[kminus] = nullptr;
            std::shared_ptr<PathNode> node = std::make_shared<DiffNode>(i, j, prev);
            // 判断两个name是否相同
            while (i < n && j < m && (std::get<0>(baseLists[i]) == std::get<0>(compareLists[j]))) {
                ++i;
                ++j;
            }
            if (i > node->i) { // 对角线节点更新为snake
                node = std::make_shared<Snake>(i, j, node);
            }
            diagonal[kmiddle] = node;
            if (i >= n && j >= m) { // 达到终点，返回节点
                return diagonal[kmiddle];
            }
        }
    }
    return nullptr;
}

void MemoryCompare::BuildDiff(std::shared_ptr<PathNode> path, const DEVICEID deviceId,
    const NAME_WITH_INDEX &baseLists, const NAME_WITH_INDEX &compareLists)
{
    LOG_DEBUG("Start to build myers diff.");
    if (path == nullptr) {
        LOG_WARN("Empty myers path!");
        return ;
    }
    auto start_time = Utility::GetTimeMicroseconds();
    while (path && path->prev && path->prev->j >= 0) {
        auto end_time = Utility::GetTimeMicroseconds();
        if ((end_time - start_time) >= MAXLOOPTIME) {
                LOG_ERROR("Memory compare build diff failed! Reaching maximum loop time limit!");
                break;
            }
        if (path->IsSnake()) { // base name = compare name
            int endi = path->i;

            int endj = path->j;
            int beginj = path->prev->j;
            for (int i = endi - 1, j = endj - 1; j >= beginj; --i, --j) {
                CalcuMemoryDiff(deviceId, baseLists[i], compareLists[j]);
            }
        } else {
            int i = path->i;
            int j = path->j;
            int prei = path->prev->i;
            if (prei < i) { // base name diff
                CalcuMemoryDiff(deviceId, baseLists[i - 1], {});
            } else { // compare name diff
                CalcuMemoryDiff(deviceId, {}, compareLists[j - 1]);
            }
        }
        path = path->prev;
    }
}

void MemoryCompare::MyersDiff(const DEVICEID deviceId, const NAME_WITH_INDEX &baseLists,
    const NAME_WITH_INDEX &compareLists)
{
    LOG_DEBUG("Start to compare with Myers algorithm.");
    if (baseLists.empty() && compareLists.empty()) {
        LOG_WARN("Device %s has empty kernelLaunch/op data!", std::to_string(deviceId).c_str());
        return ;
    } else {
        auto pathNode = BuildPath(baseLists, compareLists);
        BuildDiff(pathNode, deviceId, baseLists, compareLists);
    }
}

void MemoryCompare::RunComparison(const std::vector<std::string> &paths)
{
    LOG_INFO("Start to compare memory data.");
    auto start_time = Utility::GetTimeMicroseconds();
    // 已在命令行输入处校验path长度
    std::string pathBase = paths[0];
    std::string pathCompare = paths[1];
    
    ReadFile(pathBase, baseFileOriginData_);
    ReadFile(pathCompare, compareFileOriginData_);

    if (baseFileOriginData_.empty() || compareFileOriginData_.empty()) {
        std::cout << "[msmemscope] ERROR: Memory comparison failed!" << std::endl;
        return ;
    }

    for (const auto& pair : baseFileOriginData_) {
        deviceIdSet_.insert(pair.first);
    }
    for (const auto& pair : compareFileOriginData_) {
        deviceIdSet_.insert(pair.first);
    }

    for (const auto& deviceId : deviceIdSet_) {
        NAME_WITH_INDEX baseLists {};
        NAME_WITH_INDEX compareLists {};
        ReadNameIndexData(baseFileOriginData_[deviceId], baseLists);
        ReadNameIndexData(compareFileOriginData_[deviceId], compareLists);
        MyersDiff(deviceId, baseLists, compareLists);
    }

    if (!WriteCompareDataToCsv()) {
        std::cout << "[msmemscope] ERROR: Memory comparison failed!" << std::endl;
    } else {
        auto end_time = Utility::GetTimeMicroseconds();
        LOG_INFO("The memory comparison has been completed "
            "in a total time of %.6f(s)", (end_time-start_time) / MICROSEC);
    }
    return ;
}

MemoryCompare::~MemoryCompare()
{
    if (compareFile_ != nullptr) {
        std::fclose(compareFile_);
        compareFile_ = nullptr;
    }
}
}
