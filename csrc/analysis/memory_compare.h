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

#ifndef STEPINTER_ANALYZER_H
#define STEPINTER_ANALYZER_H

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "framework/config_info.h"
#include "log.h"
#include "ustring.h"

namespace MemScope
{
constexpr uint32_t KSTEPSIZE = 2;
constexpr uint32_t MAXLOOPTIME = 60 * 1000000;  // 最大处理时间1分钟
constexpr double MICROSEC = 1000000.0;

using DEVICEID = uint64_t;
using ORIGINAL_FILE_DATA = std::vector<std::unordered_map<std::string, std::string>>;
using NAME_WITH_INDEX = std::vector<std::tuple<std::string, std::string, size_t>>;

/* PathNode基类
 * 1. 存储最优节点的坐标(i , j)，以及前驱节点prev
 * 2. 使用IsSnake函数标记是否为对角线节点
 */
class PathNode
{
   public:
    int i, j;
    std::shared_ptr<PathNode> prev;

    PathNode(int i, int j, std::shared_ptr<PathNode> prev = nullptr) : i(i), j(j), prev(std::move(prev)) {}

    virtual ~PathNode() = default;

    virtual bool IsSnake() const = 0;
};

/* Snake类
 * 1. 存储最优节点的坐标(i , j)，以及前驱节点prev
 * 2. 该节点为对角线节点
 */
class Snake : public PathNode
{
   public:
    Snake(int i, int j, std::shared_ptr<PathNode> prev = nullptr) : PathNode(i, j, std::move(prev)) {}

    bool IsSnake() const override { return true; }
};

/* DiffNode类
 * 1. 存储最优节点的坐标(i , j)，以及前驱节点prev
 * 2. 该节点不是对角线节点
 */
class DiffNode : public PathNode
{
   public:
    DiffNode(int i, int j, std::shared_ptr<PathNode> prev = nullptr) : PathNode(i, j, std::move(prev)) {}

    bool IsSnake() const override { return false; }
};

class MemoryCompare
{
   public:
    static MemoryCompare &GetInstance();
    void RunComparison(const std::vector<std::string> &paths);

   private:
    explicit MemoryCompare();
    ~MemoryCompare();
    void SetDirPath();
    std::vector<std::string> SplitLineData(std::string line);
    std::string ReadQuotedField(std::stringstream &ss);
    void ReadCsvFile(std::string &path, std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> &data);
    std::shared_ptr<PathNode> BuildPath(const NAME_WITH_INDEX &baseLists, const NAME_WITH_INDEX &compareLists);
    void BuildDiff(std::shared_ptr<PathNode> path, const DEVICEID deviceId, const NAME_WITH_INDEX &baseLists,
                   const NAME_WITH_INDEX &compareLists);
    bool WriteCompareDataToCsv();
    void MyersDiff(const DEVICEID deviceId, const NAME_WITH_INDEX &baseLists, const NAME_WITH_INDEX &compareLists);
    void ReadNameIndexData(const ORIGINAL_FILE_DATA &data, NAME_WITH_INDEX &dataLists);
    void GetMemoryUsage(size_t index, const ORIGINAL_FILE_DATA &data, int64_t &memDiff);
    void CalcuMemoryDiff(const DEVICEID deviceId, const std::tuple<std::string, std::string, size_t> &baseData,
                         const std::tuple<std::string, std::string, size_t> &compareData);
    void ReadFile(std::string &path, std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> &data);
    bool CheckCsvHeader(std::string &path, std::ifstream &file, std::vector<std::string> &headerData);
    std::string NormalizeString(const std::string &line);

   private:
    FILE *compareFile_ = nullptr;
    std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> baseFileOriginData_;
    std::unordered_map<DEVICEID, ORIGINAL_FILE_DATA> compareFileOriginData_;
    std::unordered_map<DEVICEID, std::vector<std::string>> result_;
    std::unordered_set<DEVICEID> deviceIdSet_;
    std::string fileNamePrefix_ = "memory_compare_";
    std::string framework_;
    std::mutex fileMutex_;
};

}  // namespace MemScope

#endif
