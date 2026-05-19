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

#include "tensor_dumper.h"

#include <fstream>
#include <vector>
#include "utils.h"
#include "file.h"
#include "event_report.h"
#include "calculate_data_check_sum.h"
#include "tensor_monitor.h"
#include "memory_watch.h"

namespace MemScope {
void CleanFileName(std::string& fileName)
{
    for (size_t i = 0; i < fileName.size(); i++) {
        if (fileName[i] == '/') {
            fileName[i] = '.';
        }
    }
    return;
}

TensorDumper::TensorDumper() : fullContent_(GetConfig().watchConfig.fullContent),
    dumpDir_(Utility::FileCreateManager::GetInstance(GetConfig().outputDir).GetProjectDir())
{
}

TensorDumper::~TensorDumper()
{
    if (csvFile_ != nullptr) {
        fclose(csvFile_);
        csvFile_ = nullptr;
    }
}

bool TensorDumper::IsDumpFullContent() const
{
    return fullContent_;
}

bool TensorDumper::DumpTensorBinary(const std::vector<uint8_t> &hostData, std::string& fileName)
{
    CleanFileName(fileName);

    int32_t devId = GD_INVALID_NUM;
    if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
        LOG_ERROR("DumpTensorBinary get device id failed, %d", devId);
    }
    std::string binOutDir = dumpDir_ + "/" + "device_" + std::to_string(devId) + "/" + WATCH_DUMP_DIR;
    // 判断watch_dump的目录是否存在，如不存在则提示用户将要创建。
    if (access(binOutDir.c_str(), F_OK) != 0) {
        std::cout << "[msmemscope] Info: Created watch_dump directory at " << binOutDir << "." << std::endl;
    }
    if (!Utility::MakeDir(binOutDir)) {
        LOG_ERROR("Make DumpTensorBinary dir failed.");
        return false;
    }
    Utility::UmaskGuard guard{Utility::DEFAULT_UMASK_FOR_BIN_FILE};
    std::string outpath = binOutDir + "/" + fileName;
    std::ofstream outFile(outpath, std::ios::binary);
    if (!outFile) {
        LOG_ERROR("outFile false.");
        return false;
    }

    outFile.write(reinterpret_cast<const char*>(hostData.data()), hostData.size());

    if (!outFile.good()) {
        LOG_ERROR("outFile write false.");
        return false;
    }

    outFile.close();

    return true;
}

bool TensorDumper::DumpTensorHashValue(const std::vector<uint8_t> &hostData, std::string& fileName)
{
    if (csvFile_ == nullptr) {
        int32_t devId = GD_INVALID_NUM;
        if (!GetDeviceInfo::Instance().GetDeviceId(devId) || devId == GD_INVALID_NUM) {
            LOG_ERROR("DumpTensorHashValue get device id failed, %d", devId);
        }
        if (!Utility::FileCreateManager::GetInstance(GetConfig().outputDir).CreateCsvFile(&csvFile_,
            devId, WATCH_CSV_FILE_PREFIX, WATCH_DUMP_DIR, WATCH_HASH_HEADERS)) {
            LOG_ERROR("DumpTensorHashValue create csv file failed.");
            return false;
        }
    }

    auto hashValue = CalculateDataCheckSum64(hostData);
    if (!Utility::Fprintf(csvFile_, "%s,%s\n", fileName.c_str(), hashValue.c_str())) {
        LOG_ERROR("Write tensor data check sum info failed.");
        return false;
    }
    return true;
}

bool TensorDumper::DumpOneTensor(const MonitoredTensor& tensor, std::string& fileName)
{
    if (!Utility::MakeDir(dumpDir_)) {
        LOG_ERROR("Make dir failed.");
        return false;
    }
    using AclrtMemcpy = decltype(&aclrtMemcpy);
    auto vallina = VallinaSymbol<AclLibLoader>::Instance().Get<AclrtMemcpy>("aclrtMemcpy");
    if (vallina == nullptr) {
        return false;
    }

    std::vector<uint8_t> hostData(tensor.dataSize);
    aclError ret = vallina(hostData.data(), tensor.dataSize, tensor.data, tensor.dataSize, ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != ACL_SUCCESS) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsDumpFullContent()) {
        return DumpTensorBinary(hostData, fileName);
    }

    return DumpTensorHashValue(hostData, fileName);
}

std::string TensorDumper::GetFileName(const std::string &op, std::string watchedOpName,
    uint64_t index, bool isWatchStart)
{
    std::string type = isWatchStart ? "before" : "after";
    std::string name = op + "-" + watchedOpName + "_" + std::to_string(index) + "_" + type + ".bin";
    return name;
}

void TensorDumper::SynchronizeStream(aclrtStream stream)
{
    using AclrtSynchronizeStream = decltype(&aclrtSynchronizeStream);
    static auto vallina = VallinaSymbol<AclLibLoader>::Instance().Get<AclrtSynchronizeStream>("aclrtSynchronizeStream");
    if (vallina == nullptr) {
        LOG_ERROR("Gey aclrtSynchronizeStream func ptr failed");
        return;
    }
 
    int ret = vallina(stream);
    if (ret != ACL_SUCCESS) {
        LOG_ERROR("Dump tensor synchronize stream failed, ret is %d", ret);
        return;
    }
    return;
}

void TensorDumper::Dump(aclrtStream stream, const std::string &op, bool isWatchStart)
{
    SynchronizeStream(stream);
    std::unordered_map<uint64_t, MonitoredTensor> tensorsMap = TensorMonitor::GetInstance().GetCmdWatchedTensorsMap();
    uint64_t index = 0;
    for (const auto& tensorPair : tensorsMap) {
        std::string watchedOpName = MemoryWatch::GetInstance().GetWatchedTargetName();
        auto outputId = TensorMonitor::GetInstance().GetCmdWatchedOutputId();
        auto fileName = GetFileName(op, watchedOpName, outputId != UINT32_MAX ? outputId : index, isWatchStart);
        auto result = DumpOneTensor(tensorPair.second, fileName);
        if (!result) {
            LOG_ERROR("Dump tensor failed, current op: %s, watched op: %s", op.c_str(), watchedOpName.c_str());
        }
        ++index;
    }
    index = 0;
    std::unordered_map<uint64_t, MonitoredTensor> tensors = TensorMonitor::GetInstance().GetPythonWatchedTensorsMap();
    for (const auto& tensorPair : tensors) {
        uint64_t ptr = static_cast<uint64_t>((std::uintptr_t)(tensorPair.second.data));
        std::string watchedOpName = GetDumpName(ptr);
        auto fileName = GetFileName(op, watchedOpName, index, isWatchStart);
        auto dumpNums = GetDumpNums(ptr);
        if (dumpNums == 0) {
            break;
        }
        auto result = DumpOneTensor(tensorPair.second, fileName);
        if (!result) {
            LOG_ERROR("Dump tensor failed, current op: %s, watched op: %s", op.c_str(), watchedOpName.c_str());
        }
        if (dumpNums > 0) {
            SetDumpNums(ptr, dumpNums-1);
        }
    }
    return;
}

void TensorDumper::SetDumpNums(uint64_t ptr, int32_t dumpNums)
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = dumpNumsMap_.find(ptr);
    if (it != dumpNumsMap_.end()) {
        dumpNumsMap_[ptr] = dumpNums;
    } else {
        dumpNumsMap_.insert({ptr, dumpNums});
    }
}

int32_t TensorDumper::GetDumpNums(uint64_t ptr)
{
    auto it = dumpNumsMap_.find(ptr);
    if (it != dumpNumsMap_.end()) {
        return dumpNumsMap_[ptr];
    }
    return -1;
}

void TensorDumper::DeleteDumpNums(uint64_t ptr)
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = dumpNumsMap_.find(ptr);
    if (it != dumpNumsMap_.end()) {
        dumpNumsMap_.erase(ptr);
    }
}

void TensorDumper::SetDumpName(uint64_t ptr, std::string name)
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = dumpNameMap_.find(ptr);
    if (it != dumpNameMap_.end()) {
        dumpNameMap_[ptr] = name;
    } else {
        dumpNameMap_.insert({ptr, name});
    }
}

std::string TensorDumper::GetDumpName(uint64_t ptr)
{
    auto it = dumpNameMap_.find(ptr);
    if (it != dumpNameMap_.end()) {
        return dumpNameMap_[ptr];
    }
    return "UNKNWON";
}

void TensorDumper::DeleteDumpName(uint64_t ptr)
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = dumpNameMap_.find(ptr);
    if (it != dumpNameMap_.end()) {
        dumpNameMap_.erase(ptr);
    }
}

}