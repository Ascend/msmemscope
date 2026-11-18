// -------------------------------------------------------------------------
// This file is part of the MindStudio project.
// Copyright (c) 2026 Huawei Technologies Co.,Ltd.
//
// MindStudio is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
//
//          http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PSL v2 for more details.
// -------------------------------------------------------------------------

#include <gtest/gtest.h>

#define private public
#include "dump.h"
#undef private

#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "bit_field.h"
#include "event_dispatcher.h"
#include "event_trace/trace_manager/event_trace_manager.h"
#include "kernel_hooks/acl_hooks.h"
#include "securec.h"

// 全局符号桩表与 dlsym 空指针开关定义在 test/event_trace/test_kernel_hooks.cpp
extern std::unordered_map<std::string, void *> g_funcMocks;
extern bool g_isDlsymNullptr;

namespace MemScope {
class DumpTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        // 保存全局配置，用例结束后恢复，避免影响其他测试文件
        savedConfig_ = GetConfig();
        // 打开全部用户可控事件并统一输出到测试目录，便于覆盖落盘链路
        Config config = Config{};
        BitField<decltype(config.dumpEventType)> dumpBit;
        dumpBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
        dumpBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
        dumpBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
        dumpBit.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
        config.dumpEventType = dumpBit.getValue();
        config.dataFormat = static_cast<uint8_t>(DataFormat::CSV);
        config.enableCStack = false;
        config.enablePyStack = false;
        strncpy_s(config.outputDir, sizeof(config.outputDir), "./testmsmemscope", sizeof("./testmsmemscope") - 1);
        ConfigManager::Instance().SetConfig(config);
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("./testmsmemscope");
        dumper = &Dump::GetInstance();
    }

    void TearDown() override
    {
        dumper->handlerMap_.clear();
        dumper->sharedEventLists_.clear();
        Utility::FileCreateManager::GetInstance("./testmsmemscope").SetProjectDir("");
        ConfigManager::Instance().SetConfig(savedConfig_);
        RemoveFilesInDir("./testmsmemscope/device_0/dump");
        rmdir("./testmsmemscope/device_0/dump");
        rmdir("./testmsmemscope/device_0");
        rmdir("./testmsmemscope");
    }

    // 仅删除目录下的普通文件（dump 产物不建子目录）
    static void RemoveFilesInDir(const std::string &dir)
    {
        DIR *handle = opendir(dir.c_str());
        if (handle == nullptr) {
            return;
        }
        struct dirent *entry = nullptr;
        while ((entry = readdir(handle)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            std::string path = dir + "/" + name;
            (void)unlink(path.c_str());
        }
        closedir(handle);
    }

    Dump* dumper;
    Config savedConfig_;
};

// dcmi 接口桩：覆盖 DumpSystemEvent 中 TRACE_START 查询成功分支
extern "C" int MockDcmiInitForDump() { return 0; }
extern "C" int MockDcmiGetCardListForDump(int *cardNum, int *cardList, int maxCardNum)
{
    if (cardNum != nullptr) {
        *cardNum = 1;
    }
    if (cardList != nullptr && maxCardNum > 0) {
        cardList[0] = 0;
    }
    return 0;
}
extern "C" int MockDcmiGetDeviceNumInCardForDump(int cardId, int *deviceNum)
{
    (void)cardId;
    if (deviceNum != nullptr) {
        *deviceNum = 1;
    }
    return 0;
}
extern "C" int MockDcmiGetDeviceLogicIdForDump(int *logicId, int cardId, int deviceId)
{
    (void)cardId;
    (void)deviceId;
    if (logicId != nullptr) {
        *logicId = 0;
    }
    return 0;
}
extern "C" int MockDcmiGetHbmInfoForDump(int cardId, int deviceId, struct dcmi_hbm_info *info)
{
    (void)cardId;
    (void)deviceId;
    if (info != nullptr) {
        info->memory_usage = 100;
        info->memory_size = 1024;
    }
    return 0;
}

// 桩的作用域守卫：符号桩表与 dlsym 空指针开关是全局状态（dcmi_init 又是进程级一次性初始化），
// 用例结束必须逐键还原，否则会污染同一测试二进制内后续用例
class ScopedDcmiFuncMocks
{
public:
    ScopedDcmiFuncMocks() : savedDlsymNullptr_(g_isDlsymNullptr)
    {
        g_isDlsymNullptr = false;
        Install("dcmi_init", reinterpret_cast<void *>(&MockDcmiInitForDump));
        Install("dcmi_get_card_list", reinterpret_cast<void *>(&MockDcmiGetCardListForDump));
        Install("dcmi_get_device_num_in_card", reinterpret_cast<void *>(&MockDcmiGetDeviceNumInCardForDump));
        Install("dcmi_get_device_logic_id", reinterpret_cast<void *>(&MockDcmiGetDeviceLogicIdForDump));
        Install("dcmi_get_device_hbm_info", reinterpret_cast<void *>(&MockDcmiGetHbmInfoForDump));
    }

    ~ScopedDcmiFuncMocks()
    {
        for (size_t i = 0; i < saved_.size(); ++i) {
            if (saved_[i].second) {
                g_funcMocks[saved_[i].first] = savedValues_[i];
            } else {
                g_funcMocks.erase(saved_[i].first);
            }
        }
        g_isDlsymNullptr = savedDlsymNullptr_;
    }

    ScopedDcmiFuncMocks(const ScopedDcmiFuncMocks &) = delete;
    ScopedDcmiFuncMocks &operator=(const ScopedDcmiFuncMocks &) = delete;

private:
    void Install(const std::string &symbol, void *func)
    {
        const auto iter = g_funcMocks.find(symbol);
        saved_.push_back(std::make_pair(symbol, iter != g_funcMocks.end()));
        savedValues_.push_back(iter != g_funcMocks.end() ? iter->second : nullptr);
        g_funcMocks[symbol] = func;
    }

    std::vector<std::pair<std::string, bool>> saved_;
    std::vector<void *> savedValues_;
    bool savedDlsymNullptr_;
};

TEST_F(DumpTest, GetInstance_Singleton)
{
    // 判断是否返回同一个实例（只会初始化一次）,后续调用直接返回该实例的引用
    Dump* instance1 = &Dump::GetInstance();
    Dump* instance2 = &Dump::GetInstance();
    EXPECT_EQ(instance1, instance2);
}

TEST_F(DumpTest, EventHandle_Interface)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    dumper->EventHandle(eventBase, state);
}

TEST_F(DumpTest, WritePublicEventToFile_Interface)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    // 测试接口
    dumper->EventHandle(eventBase, state);
    dumper->WritePublicEventToFile();
}

TEST_F(DumpTest, FflushEventToFile_Interface)
{
    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::HAL;

    std::shared_ptr<EventBase> eventBase = memoryEvent;
    MemoryState* state = new MemoryState();

    // 测试接口
    dumper->EventHandle(eventBase, state);
    dumper->FflushEventToFile();
}

// ==================== RFC: Precise Event Filtering Tests ====================

TEST_F(DumpTest, ShouldDumpEvent_only_launch_enabled)
{
    // 通过全局配置设置 dumpEventType = LAUNCH_EVENT (bit 2)
    Config config = Config{};
    BitField<decltype(config.dumpEventType)> dumpBit;
    dumpBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    config.dumpEventType = dumpBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    const Config& globalConfig = GetConfig();

    // MALLOC / FREE 不应落盘
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::MALLOC, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::FREE, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::ACCESS, globalConfig));

    // OP_LAUNCH / KERNEL_LAUNCH 应落盘（映射到 LAUNCH_EVENT）
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::OP_LAUNCH, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::KERNEL_LAUNCH, globalConfig));

    // 不可控事件始终落盘
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MSTX, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::SYSTEM, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::SNAPSHOT, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::CLEAN_UP, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MEMORY_OWNER, globalConfig));
}

TEST_F(DumpTest, ShouldDumpEvent_all_disabled)
{
    // 通过全局配置设置 dumpEventType = 0，不落盘任何用户可控事件
    Config config = Config{};
    config.dumpEventType = 0;
    ConfigManager::Instance().SetConfig(config);
    const Config& globalConfig = GetConfig();

    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::MALLOC, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::FREE, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::OP_LAUNCH, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::KERNEL_LAUNCH, globalConfig));
    EXPECT_FALSE(dumper->ShouldDumpEvent(EventBaseType::ACCESS, globalConfig));

    // 不可控事件仍然落盘
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MSTX, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::SYSTEM, globalConfig));
}

TEST_F(DumpTest, ShouldDumpEvent_all_enabled)
{
    // 通过全局配置设置 dumpEventType = 所有用户可控事件
    Config config = Config{};
    BitField<decltype(config.dumpEventType)> dumpBit;
    dumpBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
    dumpBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
    dumpBit.setBit(static_cast<size_t>(EventType::LAUNCH_EVENT));
    dumpBit.setBit(static_cast<size_t>(EventType::ACCESS_EVENT));
    config.dumpEventType = dumpBit.getValue();
    ConfigManager::Instance().SetConfig(config);
    const Config& globalConfig = GetConfig();

    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MALLOC, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::FREE, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::OP_LAUNCH, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::KERNEL_LAUNCH, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::ACCESS, globalConfig));
    EXPECT_TRUE(dumper->ShouldDumpEvent(EventBaseType::MSTX, globalConfig));
}

TEST_F(DumpTest, DumpMemoryEvent_leak_attr)
{
    // dumpEventType=0：WriteToFile在ShouldDumpEvent处提前返回，仅验证attr组装不产生文件
    Config config = Config{};
    config.dumpEventType = 0;
    ConfigManager::Instance().SetConfig(config);

    auto memoryEvent = std::make_shared<MemoryEvent>();
    memoryEvent->eventType = EventBaseType::MALLOC;
    memoryEvent->eventSubType = EventSubType::PTA_CACHING;
    memoryEvent->poolType = PoolType::PTA_CACHING;
    memoryEvent->addr = 0x3039;
    memoryEvent->device = 0;
    memoryEvent->id = 1;
    memoryEvent->timestamp = 1000;
    memoryEvent->size = 10;

    // 未打标块：attr不含leak键
    auto stateNormal = std::make_shared<MemoryState>();
    dumper->DumpMemoryEvent(memoryEvent, stateNormal.get());
    EXPECT_EQ(memoryEvent->attr.find("leak:true"), std::string::npos);
    EXPECT_NE(memoryEvent->attr.find("allocation_id:"), std::string::npos);

    // 疑似泄漏块（LeakAnalyzer判定①/②后置位）：MALLOC事件attr追加leak:true
    auto stateLeak = std::make_shared<MemoryState>();
    stateLeak->isLeak = true;
    dumper->DumpMemoryEvent(memoryEvent, stateLeak.get());
    EXPECT_NE(memoryEvent->attr.find("leak:true"), std::string::npos);
}

// ==================== 事件属性组装 / 快照 / OOM / 落盘链路补充 ====================

// HAL 池：输出 used/process_used/device_used；MALLOC 且 inefficientType 非空时输出分级低效标签
TEST_F(DumpTest, DumpMemoryEvent_hal_stats_and_inefficient)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::HAL;
    event->poolType = PoolType::HAL;
    event->device = GD_INVALID_NUM;
    event->size = 128;
    event->used = 10;
    event->processUsed = 20;
    event->deviceUsed = 30;
    MemoryState state;
    state.inefficientType = "fragment";

    dumper->DumpMemoryEvent(event, &state);

    EXPECT_NE(event->attr.find("used:10"), std::string::npos);
    EXPECT_NE(event->attr.find("process_used:20"), std::string::npos);
    EXPECT_NE(event->attr.find("device_used:30"), std::string::npos);
    EXPECT_NE(event->attr.find("inefficient_type:fragment"), std::string::npos);
    EXPECT_EQ(dumper->sharedEventLists_.size(), 1u);
}

// HOST 锁页内存：输出 pinned:true 与 used/process_used，不输出 device_used
TEST_F(DumpTest, DumpMemoryEvent_host_pinned_stats)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->poolType = PoolType::HOST;
    event->isPinned = true;
    event->device = GD_INVALID_NUM;
    event->used = 5;
    event->processUsed = 6;
    event->deviceUsed = 7;
    MemoryState state;

    dumper->DumpMemoryEvent(event, &state);

    EXPECT_NE(event->attr.find("pinned:true"), std::string::npos);
    EXPECT_NE(event->attr.find("used:5"), std::string::npos);
    EXPECT_NE(event->attr.find("process_used:6"), std::string::npos);
    EXPECT_EQ(event->attr.find("device_used"), std::string::npos);
}

// 内存池事件：输出 used/total 以及进程/整卡用量
TEST_F(DumpTest, DumpMemoryEvent_pool_stats)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->poolType = PoolType::PTA_CACHING;
    event->device = GD_INVALID_NUM;
    event->used = 11;
    event->total = 22;
    event->processUsed = 33;
    event->deviceUsed = 44;
    MemoryState state;

    dumper->DumpMemoryEvent(event, &state);

    EXPECT_NE(event->attr.find("used:11"), std::string::npos);
    EXPECT_NE(event->attr.find("total:22"), std::string::npos);
    EXPECT_NE(event->attr.find("process_used:33"), std::string::npos);
    EXPECT_NE(event->attr.find("device_used:44"), std::string::npos);
}

// 影子事件：保留 pinned:true 并输出 shadow:true，不输出统计键
TEST_F(DumpTest, DumpMemoryEvent_shadow_pinned_attr)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::FREE;
    event->poolType = PoolType::HOST;
    event->isPinned = true;
    event->isShadowEvent = true;
    event->device = GD_INVALID_NUM;
    MemoryState state;

    dumper->DumpMemoryEvent(event, &state);

    EXPECT_NE(event->attr.find("pinned:true"), std::string::npos);
    EXPECT_NE(event->attr.find("shadow:true"), std::string::npos);
    EXPECT_EQ(event->attr.find("used:"), std::string::npos);
}

// EventHandle：SNAPSHOT 与 OOM_DETAIL 两种派生事件的分发及属性组装
TEST_F(DumpTest, EventHandle_snapshot_and_oom_dispatch)
{
    auto snapshot = std::make_shared<SnapshotEvent>();
    snapshot->eventType = EventBaseType::SNAPSHOT;
    snapshot->device = GD_INVALID_NUM;
    // 取二进制可精确表示的比例：512/1024=50%，128/256=50%
    snapshot->memory_reserved = 256;
    snapshot->max_memory_reserved = 300;
    snapshot->memory_allocated = 128;
    snapshot->max_memory_allocated = 150;
    snapshot->total_memory = 1024;
    snapshot->free_memory = 512;
    std::shared_ptr<EventBase> snapshotBase = snapshot;
    dumper->EventHandle(snapshotBase, nullptr);

    EXPECT_NE(snapshot->attr.find("reserved:256"), std::string::npos);
    EXPECT_NE(snapshot->attr.find("max_reserved:300"), std::string::npos);
    EXPECT_NE(snapshot->attr.find("allocated:128"), std::string::npos);
    EXPECT_NE(snapshot->attr.find("max_allocated:150"), std::string::npos);
    EXPECT_NE(snapshot->attr.find("device_rate:50"), std::string::npos);
    EXPECT_NE(snapshot->attr.find("reserved_rate:50"), std::string::npos);

    auto trigger = std::make_shared<OOMTriggerEvent>();
    // EventHandle 依据 eventType 分发、依据 eventSubType 选择 OOM 触发器/记录分支
    trigger->eventType = EventBaseType::OOM_DETAIL;
    trigger->eventSubType = EventSubType::OOM_TRIGGER;
    trigger->funcName = "alloc";
    trigger->requestSize = 128;
    trigger->flag = 1;
    trigger->device = GD_INVALID_NUM;
    std::shared_ptr<EventBase> triggerBase = trigger;
    dumper->EventHandle(triggerBase, nullptr);

    EXPECT_NE(trigger->attr.find("func:alloc"), std::string::npos);
    EXPECT_NE(trigger->attr.find("req_size:128"), std::string::npos);
    EXPECT_NE(trigger->attr.find("flag:1"), std::string::npos);

    auto record = std::make_shared<OOMMemRecordEvent>();
    record->eventType = EventBaseType::OOM_DETAIL;
    record->eventSubType = EventSubType::OOM_RECENT_ALLOC;
    record->poolType = PoolType::PTA_CACHING;
    record->addr = 0x10;
    record->memSize = 64;
    record->allocTimestamp = 123;
    record->device = GD_INVALID_NUM;
    std::shared_ptr<EventBase> recordBase = record;
    dumper->EventHandle(recordBase, nullptr);

    EXPECT_NE(record->attr.find("pool:PTA"), std::string::npos);
    EXPECT_NE(record->attr.find("size:64"), std::string::npos);
    EXPECT_NE(record->attr.find("timestamp:123"), std::string::npos);
}

// WriteToFile：CSV 处理器首次创建并写入，公共事件回写与 Fflush 覆盖多处理器遍历
TEST_F(DumpTest, WriteToFile_csv_handler_public_events_and_fflush)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->poolType = PoolType::PTA_CACHING;
    event->device = 0;
    MemoryState state;
    dumper->DumpMemoryEvent(event, &state);

    ASSERT_EQ(dumper->handlerMap_.count(0), 1u);
    EXPECT_NE(dynamic_cast<CsvHandler *>(dumper->handlerMap_[0].get()), nullptr);

    // GD_INVALID_NUM 事件进入公共事件列表，WritePublicEventToFile 回写到已有处理器
    auto sharedEvent = std::make_shared<MemoryEvent>();
    sharedEvent->eventType = EventBaseType::MALLOC;
    sharedEvent->device = GD_INVALID_NUM;
    dumper->DumpMemoryEvent(sharedEvent, &state);
    ASSERT_EQ(dumper->sharedEventLists_.size(), 1u);

    dumper->WritePublicEventToFile();
    EXPECT_TRUE(dumper->sharedEventLists_.empty());

    dumper->FflushEventToFile();
}

// WriteToFile：DB 格式下取设备级锁并写入成功（可查询到 1 条记录）
TEST_F(DumpTest, WriteToFile_db_handler_with_lock)
{
    Config config = GetConfig();
    config.dataFormat = static_cast<uint8_t>(DataFormat::DB);
    ConfigManager::Instance().SetConfig(config);

    auto event = std::make_shared<MemoryEvent>();
    event->eventType = EventBaseType::MALLOC;
    event->poolType = PoolType::PTA_CACHING;
    event->device = 0;
    event->size = 64;
    MemoryState state;
    dumper->DumpMemoryEvent(event, &state);

    ASSERT_EQ(dumper->handlerMap_.count(0), 1u);
    auto *dbHandler = dynamic_cast<DbHandler *>(dumper->handlerMap_[0].get());
    ASSERT_NE(dbHandler, nullptr);
    ASSERT_NE(dbHandler->dataFileDb_, nullptr);

    sqlite3_stmt *stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(dbHandler->dataFileDb_, "SELECT COUNT(*) FROM memscope_dump;", -1, &stmt, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
}

// DumpSystemEvent：dcmi 可用时 TRACE_START 事件写入 free/total 属性
// 通过全局符号桩表注入 dcmi 接口；环境不允许 dcmi 初始化时跳过
TEST_F(DumpTest, DumpSystemEvent_trace_start_with_dcmi)
{
    ScopedDcmiFuncMocks mocks;

    auto event = std::make_shared<SystemEvent>();
    event->eventType = EventBaseType::SYSTEM;
    event->eventSubType = EventSubType::TRACE_START;
    event->device = GD_INVALID_NUM;
    dumper->DumpSystemEvent(event);

    if (event->attr.empty()) {
        GTEST_SKIP() << "dcmi init unavailable in current environment";
    }
    // 桩值 used=100MB、total=1024MB：free=(1024-100)*1024*1024，total=1024*1024*1024
    EXPECT_EQ(event->attr, "\"{free:968884224,total:1073741824}\"");
}

}
