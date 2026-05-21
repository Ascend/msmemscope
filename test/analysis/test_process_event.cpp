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
#include <string>
#include <unordered_map>
#include <memory>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <iostream>
#define private public
#include "dump.h"
#include "decompose_analyzer.h"
#include "data_handler.h"
#include "process.h"
#include "memory_state_manager.h"
#undef private
#include "record_info.h"
#include "config_info.h"
#include "securec.h"
#include "file.h"
#include "event.h"
#include "event_dispatcher.h"
 
using namespace MemScope;
 
std::unordered_map<std::string, std::shared_ptr<EventBase>> eventMap;
int32_t testdDevId = 0;
Config config;
 
static void ResetSingleton()
{
    // 全局变量初始化
    DecomposeAnalyzer::GetInstance();
    Dump::GetInstance(config);
 
    // 取消数据订阅与部分config参数
    EventDispatcher::GetInstance().UnSubscribe(SubscriberId::DECOMPOSE_ANALYZER);
}

static bool RemoveDir(const std::string& dirPath)
{
    DIR* dir = opendir(dirPath.c_str());
    if (dir == nullptr) {
        return false;
    }
 
    // 清除路径下所有文件
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
 
        std::string fullPath = dirPath + "/" + entry->d_name;
 
        struct stat statBuf;
        if (stat(fullPath.c_str(), &statBuf) != 0) {
            closedir(dir);
            return false;
        }
 
        if (S_ISDIR(statBuf.st_mode)) {
            if (!RemoveDir(fullPath)) {
                closedir(dir);
                return false;
            }
        } else {
            if (unlink(fullPath.c_str()) != 0) {
                closedir(dir);
                return false;
            }
        }
    }
 
    closedir(dir);
 
    // 删除空目录
    if (rmdir(dirPath.c_str()) != 0) {
        return false;
    }
 
    return true;
}
 
// 定义测试夹具
class TestProcess : public ::testing::Test {
protected:
    void SetUp() override
    {
        config = Config{};
        RemoveDir("./testmsmemscope");
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("./testmsmemscope");
        MemoryStateManager::GetInstance().poolsMap_.clear();
        // 初始化单例类的参数
        ResetSingleton();
 
        AddHalDeviceMemoryEvent();
        AddAccessEvent();
 
        AddPtaCachingMemoryEvent();
        AddPtaWorkspaceMemoryEvent();
        AddAtbMemoryEvent();
        AddMindsporeMemoryEvent();
 
        AddPtaMemoryOwnerEvent();
 
        AddPtaOpLaunchEvent();
        AddAtbOpLaunchEvent();
 
        AddKernelLaunchEvent();
        AddAtbKernelLaunchEvent();
 
        AddMstxEvent();
 
        AddSystemEvent();
 
        AddCleanUpEvent();
    }
 
    void TearDown() override
    {
        // 清空设置
        config = Config{};
        Dump::GetInstance(config).config_ = Config{};
        Dump::GetInstance(config).handlerMap_.clear();
        Dump::GetInstance(config).sharedEventLists_.clear();
        std::unordered_map<std::string, std::shared_ptr<EventBase>>().swap(eventMap);
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("");
        MemoryStateManager::GetInstance().poolsMap_.clear();
        RemoveDir("./testmsmemscope");
    }
 
private:
    void AddHalDeviceMemoryEvent()
    {
        auto event1 = std::make_shared<MemoryEvent>();
        event1->poolType = PoolType::HAL;
        event1->id = 3;
        event1->timestamp = 3;
        event1->pid = 123;
        event1->tid = 1234;
        event1->addr = 0x0000000000003039;
        event1->device = 0;
        event1->name = "N/A";
        event1->eventType = EventBaseType::MALLOC;
        event1->eventSubType = EventSubType::HAL;
        event1->size = 10;
        event1->moduleId = 1;
        eventMap["HalDeviceMallocEvent"] = event1;
 
        auto event2 = std::make_shared<MemoryEvent>();
        event2->poolType = PoolType::HAL;
        event2->id = 54;
        event2->timestamp = 54;
        event2->pid = 123;
        event2->tid = 1234;
        event2->addr = 0x0000000000003039;
        event2->device = GD_INVALID_NUM;
        event2->name = "N/A";
        event2->eventType = EventBaseType::FREE;
        event2->eventSubType = EventSubType::HAL;
        event2->moduleId = 1;
        eventMap["HalDeviceFreeEvent"] = event2;

        auto event3 = std::make_shared<MemoryEvent>();
        event3->poolType = PoolType::HAL;
        event3->id = 0;
        event3->timestamp = 0;
        event3->pid = 123;
        event3->tid = 1234;
        event3->addr = 0x0000000000003039;
        event3->device = 0;
        event3->name = "N/A";
        event3->eventType = EventBaseType::MALLOC;
        event3->eventSubType = EventSubType::HAL;
        event3->size = 10;
        eventMap["HalUnknownMallocEvent"] = event3;
    }
 
    void AddAccessEvent()
    {
        auto event1 = std::make_shared<MemoryEvent>();
        event1->poolType = PoolType::PTA_CACHING;
        event1->id = 13;
        event1->timestamp = 13;
        event1->pid = 123;
        event1->tid = 1234;
        event1->addr = 0x0000000000003039;
        event1->device = 0;
        event1->name = "aten.add";
        event1->eventType = EventBaseType::ACCESS;
        event1->eventSubType = EventSubType::ATEN_READ_OR_WRITE;
        event1->size = 10;
        event1->attr = "dtype:torch.float16,shape:torch.Size([1,5])";
        eventMap["PtaAccessEvent"] = event1;
 
        auto event2 = std::make_shared<MemoryEvent>();
        event2->poolType = PoolType::ATB;
        event2->id = 14;
        event2->timestamp = 14;
        event2->pid = 123;
        event2->tid = 1234;
        event2->addr = 0x000000000000303e;
        event2->device = 0;
        event2->name = "addOperation";
        event2->eventType = EventBaseType::ACCESS;
        event2->eventSubType = EventSubType::ATB_READ_OR_WRITE;
        event2->size = 5;
        event2->attr = "dtype:FLOAT,format:NZD,shape:[1,5,]";
        eventMap["AtbAccessEvent"] = event2;

        auto event3 = std::make_shared<MemoryEvent>();
        event3->poolType = PoolType::ATB;
        event3->id = 15;
        event3->timestamp = 14;
        event3->pid = 1234;
        event3->tid = 1234;
        event3->addr = 0x000000000000303e;
        event3->device = 0;
        event3->name = "addOperation";
        event3->eventType = EventBaseType::ACCESS;
        event3->eventSubType = EventSubType::ATB_READ_OR_WRITE;
        event3->size = 5;
        event3->attr = "dtype:FLOAT,format:NZD,shape:[1,5,]";
        eventMap["AtbAccessEventInDifferentPid"] = event3;
    }
 
    void AddPtaCachingMemoryEvent()
    {
        auto event1 = std::make_shared<MemoryEvent>();
        event1->poolType = PoolType::PTA_CACHING;
        event1->id = 3;
        event1->timestamp = 3;
        event1->pid = 123;
        event1->tid = 1234;
        event1->addr = 0x0000000000003039;
        event1->device = 0;
        event1->name = "N/A";
        event1->eventType = EventBaseType::MALLOC;
        event1->eventSubType = EventSubType::PTA_CACHING;
        event1->size = 10;
        event1->total = 10;
        event1->used = 10;
        event1->cCallStack = "";
        event1->pyCallStack = "";
        eventMap["PtaCachingMallocEvent"] = event1;
 
        auto event2 = std::make_shared<MemoryEvent>();
        event2->poolType = PoolType::PTA_CACHING;
        event2->id = 54;
        event2->timestamp = 54;
        event2->pid = 123;
        event2->tid = 1234;
        event2->addr = 0x0000000000003039;
        event2->device = 0;
        event2->name = "N/A";
        event2->eventType = EventBaseType::FREE;
        event2->eventSubType = EventSubType::PTA_CACHING;
        event2->size = 10;
        event2->total = 0;
        event2->used = 0;
        eventMap["PtaCachingFreeEvent"] = event2;
    }

    void AddPtaWorkspaceMemoryEvent()
    {
        auto event1 = std::make_shared<MemoryEvent>();
        event1->poolType = PoolType::PTA_WORKSPACE;
        event1->id = 3;
        event1->timestamp = 3;
        event1->pid = 123;
        event1->tid = 1234;
        event1->addr = 0x0000000000003039;
        event1->device = 0;
        event1->name = "N/A";
        event1->eventType = EventBaseType::MALLOC;
        event1->eventSubType = EventSubType::PTA_WORKSPACE;
        event1->size = 10;
        event1->total = 10;
        event1->used = 10;
        event1->cCallStack = "";
        event1->pyCallStack = "";
        eventMap["PtaWorkspaceMallocEvent"] = event1;
 
        auto event2 = std::make_shared<MemoryEvent>();
        event2->poolType = PoolType::PTA_WORKSPACE;
        event2->id = 54;
        event2->timestamp = 54;
        event2->pid = 123;
        event2->tid = 1234;
        event2->addr = 0x0000000000003039;
        event2->device = 0;
        event2->name = "N/A";
        event2->eventType = EventBaseType::FREE;
        event2->eventSubType = EventSubType::PTA_WORKSPACE;
        event2->size = 10;
        event2->total = 0;
        event2->used = 0;
        eventMap["PtaWorkspaceFreeEvent"] = event2;
    }
 
    void AddAtbMemoryEvent()
    {
        auto event1 = std::make_shared<MemoryEvent>();
        event1->poolType = PoolType::ATB;
        event1->id = 3;
        event1->timestamp = 3;
        event1->pid = 123;
        event1->tid = 1234;
        event1->addr = 0x0000000000003039;
        event1->device = 0;
        event1->name = "N/A";
        event1->eventType = EventBaseType::MALLOC;
        event1->eventSubType = EventSubType::ATB;
        event1->size = 10;
        event1->total = 10;
        event1->used = 10;
        event1->cCallStack = "";
        event1->pyCallStack = "";
        eventMap["AtbMallocEvent"] = event1;
 
        auto event2 = std::make_shared<MemoryEvent>();
        event2->poolType = PoolType::ATB;
        event2->id = 54;
        event2->timestamp = 54;
        event2->pid = 123;
        event2->tid = 1234;
        event2->addr = 0x0000000000003039;
        event2->device = 0;
        event2->name = "N/A";
        event2->eventType = EventBaseType::FREE;
        event2->eventSubType = EventSubType::ATB;
        event2->size = 10;
        event2->total = 0;
        event2->used = 0;
        eventMap["AtbFreeEvent"] = event2;
    }
 
    void AddMindsporeMemoryEvent()
    {
        auto event1 = std::make_shared<MemoryEvent>();
        event1->poolType = PoolType::MINDSPORE;
        event1->id = 3;
        event1->timestamp = 3;
        event1->pid = 123;
        event1->tid = 1234;
        event1->addr = 0x0000000000003039;
        event1->device = 0;
        event1->name = "N/A";
        event1->eventType = EventBaseType::MALLOC;
        event1->eventSubType = EventSubType::MINDSPORE;
        event1->size = 10;
        event1->total = 10;
        event1->used = 10;
        event1->cCallStack = "";
        event1->pyCallStack = "";
        eventMap["MindsporeMallocEvent"] = event1;
 
        auto event2 = std::make_shared<MemoryEvent>();
        event2->poolType = PoolType::MINDSPORE;
        event2->id = 54;
        event2->timestamp = 54;
        event2->pid = 123;
        event2->tid = 1234;
        event2->addr = 0x0000000000003039;
        event2->device = 0;
        event2->name = "N/A";
        event2->eventType = EventBaseType::FREE;
        event2->eventSubType = EventSubType::MINDSPORE;
        event2->size = 10;
        event2->total = 0;
        event2->used = 0;
        eventMap["MindsporeFreeEvent"] = event2;
    }
 
    void AddPtaMemoryOwnerEvent()
    {
        auto event1 = std::make_shared<MemoryOwnerEvent>();
        event1->poolType = PoolType::PTA_CACHING;
        event1->id = 23;
        event1->timestamp = 23;
        event1->pid = 123;
        event1->tid = 1234;
        event1->addr = 0x0000000000003039;
        event1->device = GD_INVALID_NUM;
        event1->name = "N/A";
        event1->eventType = EventBaseType::MEMORY_OWNER;
        event1->eventSubType = EventSubType::DESCRIBE_OWNER;
        event1->owner = "@memscope";
        eventMap["DescribeOwnerEvent"] = event1;
 
        auto event2 = std::make_shared<MemoryOwnerEvent>();
        event2->poolType = PoolType::PTA_CACHING;
        event2->id = 24;
        event2->timestamp = 24;
        event2->pid = 123;
        event2->tid = 1234;
        event2->addr = 0x0000000000003039;
        event2->device = GD_INVALID_NUM;
        event2->name = "N/A";
        event2->eventType = EventBaseType::MEMORY_OWNER;
        event2->eventSubType = EventSubType::TORCH_OPTIMIZER_STEP_OWNER;
        event2->owner = "@model@gradient";
        eventMap["TorchStepOwnerEvent"] = event2;
    }
 
    void AddPtaOpLaunchEvent()
    {
        auto event1 = std::make_shared<OpLaunchEvent>();
        event1->id = 1;
        event1->timestamp = 12;
        event1->pid = 123;
        event1->tid = 1234;
        event1->device = 0;
        event1->name = "aten.add";
        event1->eventType = EventBaseType::OP_LAUNCH;
        event1->eventSubType = EventSubType::ATEN_START;
        eventMap["AtenOpStartEvent"] = event1;
 
        auto event2 = std::make_shared<OpLaunchEvent>();
        event2->id = 2;
        event2->timestamp = 13;
        event2->pid = 123;
        event2->tid = 1234;
        event2->device = 0;
        event2->name = "aten.add";
        event2->eventType = EventBaseType::OP_LAUNCH;
        event2->eventSubType = EventSubType::ATEN_END;
        eventMap["AtenOpEndEvent"] = event2;
    }
 
    void AddAtbOpLaunchEvent()
    {
        auto event1 = std::make_shared<OpLaunchEvent>();
        event1->id = 1;
        event1->timestamp = 12;
        event1->pid = 123;
        event1->tid = 1234;
        event1->device = 0;
        event1->name = "operation";
        event1->eventType = EventBaseType::OP_LAUNCH;
        event1->eventSubType = EventSubType::ATB_START;
        event1->attr = "path:0/0_123/operation,workspace_ptr:0x12313,workspace_size:12";
        eventMap["AtbOpStartEvent"] = event1;
 
        auto event2 = std::make_shared<OpLaunchEvent>();
        event2->id = 2;
        event2->timestamp = 13;
        event2->pid = 123;
        event2->tid = 1234;
        event2->device = 0;
        event2->name = "operation";
        event2->eventType = EventBaseType::OP_LAUNCH;
        event2->eventSubType = EventSubType::ATB_END;
        event2->attr = "path:0/0_123/operation,workspace_ptr:0x12313,workspace_size:12";
        eventMap["AtbOpEndEvent"] = event2;
    }
 
    void AddKernelLaunchEvent()
    {
        auto event1 = std::make_shared<KernelLaunchEvent>();
        event1->id = 1;
        event1->timestamp = 12;
        event1->pid = 123;
        event1->tid = 1234;
        event1->device = 0;
        event1->name = "add01";
        event1->eventType = EventBaseType::KERNEL_LAUNCH;
        event1->eventSubType = EventSubType::KERNEL_LAUNCH;
        event1->streamId = "111";
        event1->taskId = "222";
        eventMap["KernelLaunchEvent"] = event1;
 
        auto event2 = std::make_shared<KernelLaunchEvent>();
        event2->id = 2;
        event2->timestamp = 13;
        event2->pid = INVALID_PROCESSID;
        event2->tid = INVALID_THREADID;
        event2->device = 0;
        event2->name = "add01";
        event2->eventType = EventBaseType::KERNEL_LAUNCH;
        event2->eventSubType = EventSubType::KERNEL_EXECUTE_START;
        event2->streamId = "111";
        event2->taskId = "222";
        eventMap["KernelExecuteStartEvent"] = event2;
 
        auto event3 = std::make_shared<KernelLaunchEvent>();
        event3->id = 3;
        event3->timestamp = 14;
        event3->pid = INVALID_PROCESSID;
        event3->tid = INVALID_THREADID;
        event3->device = 0;
        event3->name = "add01";
        event3->eventType = EventBaseType::KERNEL_LAUNCH;
        event3->eventSubType = EventSubType::KERNEL_EXECUTE_END;
        event3->streamId = "111";
        event3->taskId = "222";
        eventMap["KernelExecuteEndEvent"] = event3;
    }
 
    void AddAtbKernelLaunchEvent()
    {
        auto event1 = std::make_shared<KernelLaunchEvent>();
        event1->id = 1;
        event1->timestamp = 12;
        event1->pid = 123;
        event1->tid = 1234;
        event1->device = 0;
        event1->name = "add01";
        event1->eventType = EventBaseType::KERNEL_LAUNCH;
        event1->eventSubType = EventSubType::ATB_KERNEL_START;
        event1->attr = "path:0/0_123/add";
        eventMap["AtbKernelStartEvent"] = event1;
 
        auto event2 = std::make_shared<KernelLaunchEvent>();
        event2->id = 2;
        event2->timestamp = 13;
        event2->pid = 123;
        event2->tid = 1234;
        event2->device = 0;
        event2->name = "add01";
        event2->eventType = EventBaseType::KERNEL_LAUNCH;
        event2->eventSubType = EventSubType::ATB_KERNEL_END;
        event2->attr = "path:0/0_123/add";
        eventMap["AtbKernelEndEvent"] = event2;
    }
 
    void AddMstxEvent()
    {
        auto event1 = std::make_shared<MstxEvent>();
        event1->id = 1;
        event1->timestamp = 12;
        event1->pid = 123;
        event1->tid = 1234;
        event1->device = 0;
        event1->name = "++++++ test mstx mark +++++";
        event1->eventType = EventBaseType::MSTX;
        event1->eventSubType = EventSubType::MSTX_MARK;
        eventMap["MstxMarkEvent"] = event1;
 
        auto event2 = std::make_shared<MstxEvent>();
        event2->id = 2;
        event2->timestamp = 13;
        event2->pid = 123;
        event2->tid = 1234;
        event2->device = 0;
        event2->name = "step start";
        event2->eventType = EventBaseType::MSTX;
        event2->eventSubType = EventSubType::MSTX_RANGE_START;
        eventMap["MstxRangeStartEvent"] = event2;
 
        auto event3 = std::make_shared<MstxEvent>();
        event3->id = 3;
        event3->timestamp = 14;
        event3->pid = 123;
        event3->tid = 1234;
        event3->device = 0;
        event3->name = "";
        event3->eventType = EventBaseType::MSTX;
        event3->eventSubType = EventSubType::MSTX_RANGE_END;
        eventMap["MstxRangeEndEvent"] = event3;
    }
 
    void AddSystemEvent()
    {
        auto event1 = std::make_shared<SystemEvent>();
        event1->id = 1;
        event1->timestamp = 12;
        event1->pid = 123;
        event1->tid = 1234;
        event1->device = GD_INVALID_NUM;
        event1->name = "N/A";
        event1->eventType = EventBaseType::SYSTEM;
        event1->eventSubType = EventSubType::ACL_INIT;
        eventMap["AclInitEvent"] = event1;
 
        auto event2 = std::make_shared<SystemEvent>();
        event2->id = 2;
        event2->timestamp = 13;
        event2->pid = 123;
        event2->tid = 1234;
        event2->device = GD_INVALID_NUM;
        event2->name = "N/A";
        event2->eventType = EventBaseType::SYSTEM;
        event2->eventSubType = EventSubType::ACL_FINI;
        eventMap["AclFinalEvent"] = event2;

        auto event3 = std::make_shared<SystemEvent>();
        event3->id = 1;
        event3->timestamp = 12;
        event3->pid = 123;
        event3->tid = 1234;
        event3->device = GD_INVALID_NUM;
        event3->name = "N/A";
        event3->eventType = EventBaseType::SYSTEM;
        event3->eventSubType = EventSubType::TRACE_START;
        eventMap["TraceStartEvent"] = event3;
 
        auto event4 = std::make_shared<SystemEvent>();
        event4->id = 2;
        event4->timestamp = 13;
        event4->pid = 123;
        event4->tid = 1234;
        event4->device = GD_INVALID_NUM;
        event4->name = "N/A";
        event4->eventType = EventBaseType::SYSTEM;
        event4->eventSubType = EventSubType::TRACE_STOP;
        eventMap["TraceStopEvent"] = event4;
    }
 
    void AddCleanUpEvent()
    {
        auto event1 = std::make_shared<CleanUpEvent>(PoolType::HAL, 123, 0x0000000000003039);
        eventMap["HalCleanUpEvent"] = event1;
        auto event2 = std::make_shared<CleanUpEvent>(PoolType::PTA_CACHING, 123, 0x0000000000003039);
        eventMap["PtaCleanUpEvent"] = event2;
    }
};
 
static bool ReadFile(std::string filePath, std::string &content)
{
    filePath = Utility::FileCreateManager::GetInstance(filePath).GetProjectDir() + "/device_" +
               std::to_string(testdDevId) + "/" + MemScope::DUMP_DIR;
    DIR* dir = opendir(filePath.c_str());
    if (!dir) {
        return false;
    }
 
    struct dirent* entry;
    std::string name;
    while ((entry = readdir(dir)) != nullptr) {
        std::string tmpName = entry->d_name;
        if (tmpName != "." && tmpName != "..") {
            closedir(dir);
            name = tmpName;  // 第一个有效文件名
            break;
        }
    }
 
    if (name.size() < 10) {
        return false;
    }
    filePath += "/" + name;
 
    std::ifstream file(filePath);
    if (!file.is_open()) {
        return false;
    }
 
    std::string line;
    while (std::getline(file, line)) {
        content += line + "\n";
    }
 
    file.close();
    return true;
}
 
static void CleanUpEventInMemoryStateManager()
{
    for (auto& state : MemoryStateManager::GetInstance().GetAllStateKeys()) {
        std::shared_ptr<EventBase> event = std::make_shared<CleanUpEvent>(state.first, state.second.pid, state.second.addr);
        EventHandler(event);
    }
}

static void CleanUpDumpSharedEvents()
{
    for (auto& event : Dump::GetInstance(config).sharedEventLists_) {
        for (auto& handler : Dump::GetInstance(config).handlerMap_) {
            handler.second->Write(event);
        }
    }
}

TEST_F(TestProcess, process_hal_device_memory_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["HalDeviceMallocEvent"]);
    EventHandler(eventMap["HalDeviceFreeEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,HAL,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,page_type:normal,alloc_type:create}\",,\n"
"54,FREE,HAL,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_pta_caching_memory_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["PtaCachingMallocEvent"]);
    EventHandler(eventMap["PtaAccessEvent"]);
    EventHandler(eventMap["DescribeOwnerEvent"]);
    EventHandler(eventMap["TorchStepOwnerEvent"]);
    EventHandler(eventMap["PtaCachingFreeEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10}\",,\n"
"13,ACCESS,UNKNOWN,aten.add,13,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,type:PTA,"
"dtype:torch.float16,shape:torch.Size([1,5])}\",,\n"
"54,FREE,PTA,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}

TEST_F(TestProcess, process_pta_workspace_memory_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["PtaWorkspaceMallocEvent"]);
    EventHandler(eventMap["PtaWorkspaceFreeEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA_WORKSPACE,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10}\",,\n"
"54,FREE,PTA_WORKSPACE,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_atb_memory_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["AtbMallocEvent"]);
    EventHandler(eventMap["AtbAccessEvent"]);
    EventHandler(eventMap["AtbAccessEventInDifferentPid"]);
    EventHandler(eventMap["AtbFreeEvent"]);
    CleanUpEventInMemoryStateManager();
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,ATB,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10}\",,\n"
"14,ACCESS,UNKNOWN,addOperation,14,123,1234,0,0x000000000000303e,\"{allocation_id:1,addr:0x000000000000303e,size:5,type:ATB,"
"dtype:FLOAT,format:NZD,shape:[1,5,]}\",,\n"
"54,FREE,ATB,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n"
"15,ACCESS,UNKNOWN,addOperation,14,1234,1234,0,0x000000000000303e,\"{allocation_id:2,addr:0x000000000000303e,size:5,type:ATB,"
"dtype:FLOAT,format:NZD,shape:[1,5,]}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_mindspore_memory_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["MindsporeMallocEvent"]);
    EventHandler(eventMap["MindsporeFreeEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
    ",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,MINDSPORE,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10}\",,\n"
"54,FREE,MINDSPORE,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_aten_op_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["AtenOpStartEvent"]);
    EventHandler(eventMap["AtenOpEndEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"1,OP_LAUNCH,ATEN_START,aten.add,12,123,1234,0,N/A,,,\n"
"2,OP_LAUNCH,ATEN_END,aten.add,13,123,1234,0,N/A,,,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_atb_op_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["AtbOpStartEvent"]);
    EventHandler(eventMap["AtbOpEndEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"1,OP_LAUNCH,ATB_START,operation,12,123,1234,0,N/A,\"{path:0/0_123/operation,workspace_ptr:0x12313,workspace_size:12}\",,\n"
"2,OP_LAUNCH,ATB_END,operation,13,123,1234,0,N/A,\"{path:0/0_123/operation,workspace_ptr:0x12313,workspace_size:12}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_kernel_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["KernelLaunchEvent"]);
    EventHandler(eventMap["KernelExecuteStartEvent"]);
    EventHandler(eventMap["KernelExecuteEndEvent"]);
 
    EventHandler(eventMap["AtbKernelStartEvent"]);
    EventHandler(eventMap["AtbKernelEndEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"1,KERNEL_LAUNCH,KERNEL_LAUNCH,add01,12,123,1234,0,N/A,\"{streamId:111,taskId:222}\",,\n"
"2,KERNEL_LAUNCH,KERNEL_EXECUTE_START,add01,13,N/A,N/A,0,N/A,\"{streamId:111,taskId:222}\",,\n"
"3,KERNEL_LAUNCH,KERNEL_EXECUTE_END,add01,14,N/A,N/A,0,N/A,\"{streamId:111,taskId:222}\",,\n"
"1,KERNEL_LAUNCH,KERNEL_START,add01,12,123,1234,0,N/A,\"{path:0/0_123/add}\",,\n"
"2,KERNEL_LAUNCH,KERNEL_END,add01,13,123,1234,0,N/A,\"{path:0/0_123/add}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_mstx_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["MstxMarkEvent"]);
    EventHandler(eventMap["MstxRangeStartEvent"]);
    EventHandler(eventMap["MstxRangeEndEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"1,MSTX,Mark,\"++++++ test mstx mark +++++\",12,123,1234,0,N/A,,,\n"
"2,MSTX,Range_start,\"step start\",13,123,1234,0,N/A,,,\n"
"3,MSTX,Range_end,\"\",14,123,1234,0,N/A,,,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_system_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["AclInitEvent"]);
    EventHandler(eventMap["AclFinalEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"1,SYSTEM,ACL_INIT,N/A,12,123,1234,N/A,N/A,,,\n"
"2,SYSTEM,ACL_FINI,N/A,13,123,1234,N/A,N/A,,,\n";

    CleanUpDumpSharedEvents();
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_clean_up_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["PtaCachingMallocEvent"]);
    EventHandler(eventMap["PtaAccessEvent"]);
    EventHandler(eventMap["PtaCleanUpEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10}\",,\n"
"13,ACCESS,UNKNOWN,aten.add,13,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,type:PTA,"
"dtype:torch.float16,shape:torch.Size([1,5])}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, dump_event_before_malloc)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["HalUnknownMallocEvent"]);
    EventHandler(eventMap["HalDeviceMallocEvent"]);
    CleanUpEventInMemoryStateManager();
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"0,MALLOC,HAL,N/A,0,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,page_type:normal,alloc_type:create}\",,\n"
"3,MALLOC,HAL,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:2,addr:0x0000000000003039,size:10,page_type:normal,alloc_type:create}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, dump_two_malloc_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["HalDeviceMallocEvent"]);
    EventHandler(eventMap["HalDeviceFreeEvent"]);
    EventHandler(eventMap["HalUnknownMallocEvent"]);
    EventHandler(eventMap["HalCleanUpEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,HAL,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,page_type:normal,alloc_type:create}\",,\n"
"54,FREE,HAL,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10}\",,\n"
"0,MALLOC,HAL,N/A,0,123,1234,0,0x0000000000003039,\"{allocation_id:2,addr:0x0000000000003039,size:10,page_type:normal,alloc_type:create}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, clean_up_event_failed)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["HalCleanUpEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_memory_owner_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    auto func = std::bind(&DecomposeAnalyzer::EventHandle, &DecomposeAnalyzer::GetInstance(),
        std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::MEMORY_OWNER};
    EventDispatcher::GetInstance().Subscribe(
        SubscriberId::DECOMPOSE_ANALYZER, eventList, EventDispatcher::Priority::High, func);
 
    EventHandler(eventMap["PtaCachingMallocEvent"]);
    EventHandler(eventMap["PtaAccessEvent"]);
    EventHandler(eventMap["DescribeOwnerEvent"]);
    EventHandler(eventMap["TorchStepOwnerEvent"]);
    EventHandler(eventMap["PtaCachingFreeEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10,"
"owner:PTA@model@gradient@memscope}\",,\n"
"13,ACCESS,UNKNOWN,aten.add,13,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,type:PTA,"
"dtype:torch.float16,shape:torch.Size([1,5])}\",,\n"
"54,FREE,PTA,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_memory_owner_event_in_torch_step)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    auto func = std::bind(&DecomposeAnalyzer::EventHandle, &DecomposeAnalyzer::GetInstance(),
        std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::MEMORY_OWNER};
    EventDispatcher::GetInstance().Subscribe(
        SubscriberId::DECOMPOSE_ANALYZER, eventList, EventDispatcher::Priority::High, func);
 
    EventHandler(eventMap["PtaCachingMallocEvent"]);
    EventHandler(eventMap["TorchStepOwnerEvent"]);
    EventHandler(eventMap["PtaCachingFreeEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10,"
"owner:PTA@model@gradient}\",,\n"
"54,FREE,PTA,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, process_memory_owner_event_without_malloc)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    auto func = std::bind(&DecomposeAnalyzer::EventHandle, &DecomposeAnalyzer::GetInstance(),
        std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::MEMORY_OWNER};
    EventDispatcher::GetInstance().Subscribe(
        SubscriberId::DECOMPOSE_ANALYZER, eventList, EventDispatcher::Priority::High, func);
 
    EventHandler(eventMap["PtaAccessEvent"]);
    EventHandler(eventMap["DescribeOwnerEvent"]);
    EventHandler(eventMap["TorchStepOwnerEvent"]);
    EventHandler(eventMap["PtaCachingFreeEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"13,ACCESS,UNKNOWN,aten.add,13,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,type:PTA,"
"dtype:torch.float16,shape:torch.Size([1,5])}\",,\n"
"54,FREE,PTA,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, init_memory_owner)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    auto func = std::bind(&DecomposeAnalyzer::EventHandle, &DecomposeAnalyzer::GetInstance(),
        std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::MEMORY_OWNER};
    EventDispatcher::GetInstance().Subscribe(
        SubscriberId::DECOMPOSE_ANALYZER, eventList, EventDispatcher::Priority::High, func);
 
    EventHandler(eventMap["PtaCachingMallocEvent"]);
    EventHandler(eventMap["PtaCachingFreeEvent"]);
    EventHandler(eventMap["PtaWorkspaceMallocEvent"]);
    EventHandler(eventMap["PtaWorkspaceFreeEvent"]);
    EventHandler(eventMap["AtbMallocEvent"]);
    EventHandler(eventMap["AtbFreeEvent"]);
    EventHandler(eventMap["MindsporeMallocEvent"]);
    EventHandler(eventMap["MindsporeFreeEvent"]);
    EventHandler(eventMap["HalDeviceMallocEvent"]);
    EventHandler(eventMap["HalDeviceFreeEvent"]);

    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10,owner:PTA}\",,\n"
"54,FREE,PTA,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n"
"3,MALLOC,PTA_WORKSPACE,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:2,addr:0x0000000000003039,size:10,total:10,used:10,"
"owner:PTA_WORKSPACE}\",,\n"
"54,FREE,PTA_WORKSPACE,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:2,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n"
"3,MALLOC,ATB,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:3,addr:0x0000000000003039,size:10,total:10,used:10,owner:ATB}\",,\n"
"54,FREE,ATB,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:3,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n"
"3,MALLOC,MINDSPORE,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:4,addr:0x0000000000003039,size:10,total:10,used:10,owner:MINDSPORE}\",,\n"
"54,FREE,MINDSPORE,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:4,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n"
"3,MALLOC,HAL,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:5,addr:0x0000000000003039,size:10,owner:CANN@IDEDD,page_type:normal,alloc_type:create}\",,\n"
"54,FREE,HAL,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:5,addr:0x0000000000003039,size:10}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, updata_owner_by_access_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    auto func = std::bind(&DecomposeAnalyzer::EventHandle, &DecomposeAnalyzer::GetInstance(),
        std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::MEMORY_OWNER};
    EventDispatcher::GetInstance().Subscribe(
        SubscriberId::DECOMPOSE_ANALYZER, eventList, EventDispatcher::Priority::High, func);
 
    EventHandler(eventMap["PtaCachingMallocEvent"]);
    EventHandler(eventMap["PtaAccessEvent"]);
    EventHandler(eventMap["PtaCachingFreeEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10,owner:PTA@ops@aten}\",,\n"
"13,ACCESS,UNKNOWN,aten.add,13,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,type:PTA,dtype:torch.float16,"
"shape:torch.Size([1,5])}\",,\n"
"54,FREE,PTA,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}
 
TEST_F(TestProcess, updata_owner_failed_by_atb_access_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    auto func = std::bind(&DecomposeAnalyzer::EventHandle, &DecomposeAnalyzer::GetInstance(),
        std::placeholders::_1, std::placeholders::_2);
    std::vector<EventBaseType> eventList{EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::MEMORY_OWNER};
    EventDispatcher::GetInstance().Subscribe(
        SubscriberId::DECOMPOSE_ANALYZER, eventList, EventDispatcher::Priority::High, func);
 
    EventHandler(eventMap["AtbMallocEvent"]);
    EventHandler(eventMap["AtbAccessEvent"]);
    EventHandler(eventMap["AtbFreeEvent"]);
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,ATB,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10,owner:ATB}\",,\n"
"14,ACCESS,UNKNOWN,addOperation,14,123,1234,0,0x000000000000303e,\"{allocation_id:1,addr:0x000000000000303e,size:5,type:ATB,"
"dtype:FLOAT,format:NZD,shape:[1,5,]}\",,\n"
"54,FREE,ATB,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}

TEST_F(TestProcess, get_different_allocation_id_with_trace_start_and_stop_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["PtaCachingMallocEvent"]);
    EventHandler(eventMap["TraceStartEvent"]);
    EventHandler(eventMap["PtaAccessEvent"]);
    EventHandler(eventMap["TraceStopEvent"]);
    EventHandler(eventMap["PtaCachingFreeEvent"]);

    CleanUpDumpSharedEvents();
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10}\",,\n"
"13,ACCESS,UNKNOWN,aten.add,13,123,1234,0,0x0000000000003039,\"{allocation_id:2,addr:0x0000000000003039,size:10,type:PTA,"
"dtype:torch.float16,shape:torch.Size([1,5])}\",,\n"
"54,FREE,PTA,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:3,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n"
"1,SYSTEM,START_TRACE,N/A,12,123,1234,N/A,N/A,,,\n"
"2,SYSTEM,STOP_TRACE,N/A,13,123,1234,N/A,N/A,,,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}

TEST_F(TestProcess, get_the_same_allocation_id_with_trace_start_and_stop_event)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["TraceStartEvent"]);
    EventHandler(eventMap["PtaCachingMallocEvent"]);
    EventHandler(eventMap["PtaAccessEvent"]);
    EventHandler(eventMap["PtaCachingFreeEvent"]);
    EventHandler(eventMap["TraceStopEvent"]);

    CleanUpDumpSharedEvents();
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10}\",,\n"
"13,ACCESS,UNKNOWN,aten.add,13,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,type:PTA,"
"dtype:torch.float16,shape:torch.Size([1,5])}\",,\n"
"54,FREE,PTA,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n"
"1,SYSTEM,START_TRACE,N/A,12,123,1234,N/A,N/A,,,\n"
"2,SYSTEM,STOP_TRACE,N/A,13,123,1234,N/A,N/A,,,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}

TEST_F(TestProcess, add_memory_event_into_state)
{
    config.enableCStack = false;
    config.enablePyStack = false;
    config.dataFormat = 0;
    std::string path = "./testmsmemscope";
    strncpy_s(config.outputDir, sizeof(config.outputDir), path.c_str(), sizeof(config.outputDir) - 1);
    Dump::GetInstance(config).handlerMap_[testdDevId] = MakeDataHandler(config, DataType::MEMORY_EVENT, testdDevId);    // 重置文件指针
    Dump::GetInstance(config).handlerMap_[testdDevId]->Init();
    MemoryState::ResetCount();
 
    EventHandler(eventMap["TraceStartEvent"]);
    EventHandler(eventMap["PtaCachingMallocEvent"]);
    EventHandler(eventMap["PtaAccessEvent"]);
    EventHandler(eventMap["PtaCachingFreeEvent"]);
    EventHandler(eventMap["TraceStopEvent"]);

    CleanUpDumpSharedEvents();
 
    std::string result = "ID,Event,Event Type,Name,Timestamp(ns),Process Id,Thread Id,Device Id,Ptr,Attr"
",Call Stack(Python),Call Stack(C)\n"
"3,MALLOC,PTA,N/A,3,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:10,used:10}\",,\n"
"13,ACCESS,UNKNOWN,aten.add,13,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,type:PTA,"
"dtype:torch.float16,shape:torch.Size([1,5])}\",,\n"
"54,FREE,PTA,N/A,54,123,1234,0,0x0000000000003039,\"{allocation_id:1,addr:0x0000000000003039,size:10,total:0,used:0}\",,\n"
"1,SYSTEM,START_TRACE,N/A,12,123,1234,N/A,N/A,,,\n"
"2,SYSTEM,STOP_TRACE,N/A,13,123,1234,N/A,N/A,,,\n";
    std::string fileContent;
    Dump::GetInstance(config).handlerMap_[testdDevId].reset();
    bool hasReadFile = ReadFile(path, fileContent);
    EXPECT_EQ(result, fileContent);
    EXPECT_TRUE(hasReadFile);
}