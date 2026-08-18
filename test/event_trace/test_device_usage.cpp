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
#include <memory>

#define private public
#include "event_trace/trace_manager/event_trace_manager.h"
#include "event_trace/event_report.h"
#include "memory_state_manager.h"
#undef private
#include "event_trace/vallina_symbol.h"
#include "bit_field.h"
#include "config_info.h"
#include "event_dispatcher.h"
#include "process.h"

using namespace MemScope;

namespace
{
// flag bit10~13 内存类型（与 event_report.cpp 常量一致）
constexpr uint64_t MEM_VIRT_BIT = 10;
constexpr uint64_t DEVICE_FLAG_BASE = (0x1ULL << MEM_VIRT_BIT);  // DEVICE 空间
constexpr uint64_t HOST_FLAG_BASE = (0x2ULL << MEM_VIRT_BIT);    // HOST 空间

// 事件捕获订阅者：替换 DUMP 槽位（本测试不依赖 dump 落盘），同步捕获报告层发出的事件
class EventCaptor
{
   public:
    std::shared_ptr<MemoryEvent> last;

    void Handle(std::shared_ptr<EventBase>& event, MemoryState* state)
    {
        if (auto memEvent = std::dynamic_pointer_cast<MemoryEvent>(event))
        {
            last = memEvent;
        }
    }
};
}  // namespace

class DeviceUsageTest : public ::testing::Test
{
   protected:
    EventCaptor captor_;

    void SetUp() override
    {
        // 采集配置：所有卡 + alloc/free 事件（IsNeedSkip 判定依赖）
        Config config{};
        config.collectAllNpu = true;
        BitField<decltype(config.eventType)> eventBit;
        eventBit.setBit(static_cast<size_t>(EventType::ALLOC_EVENT));
        eventBit.setBit(static_cast<size_t>(EventType::FREE_EVENT));
        config.eventType = eventBit.getValue();
        ConfigManager::Instance().SetConfig(config);
        EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::IN_TRACING);  // 正常采集态（非影子）

        EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
        report.Init();
        report.deviceUsedQueryWarned_ = false;   // 重置查询失败告警标志（Init 不重置，用例独立运行时"首次告警"断言才有效）
        report.processUsedQueryWarned_ = false;

        // 捕获事件：替换 DUMP 订阅（框架层已确认 SendEvent → Route 同步分发）
        EventDispatcher::GetInstance().UnSubscribe(SubscriberId::DUMP);
        auto func = std::bind(&EventCaptor::Handle, &captor_, std::placeholders::_1, std::placeholders::_2);
        EventDispatcher::GetInstance().Subscribe(SubscriberId::DUMP, {EventBaseType::MALLOC, EventBaseType::FREE},
                                                 EventDispatcher::Priority::Lowest, func);

        // 清理单例跨用例残留状态（统计累计 + 整卡/本进程用量缓存 + 存量块）
        MemoryStateManager::GetInstance().halUsed_.clear();
        MemoryStateManager::GetInstance().hostUsed_ = 0;
        MemoryStateManager::GetInstance().deviceUsedCache_.fill(-1);
        MemoryStateManager::GetInstance().processUsedCache_.fill(-1);
        MemoryStateManager::GetInstance().poolsMap_.clear();
    }

    void TearDown() override
    {
        // 取消订阅：EventCaptor 随 fixture 析构，若不取消则槽位持有悬垂绑定，
        // 后续测试用例的事件分发会调用已释放对象（复用内存被覆写 → SIGBUS）
        EventDispatcher::GetInstance().UnSubscribe(SubscriberId::DUMP);
        captor_.last.reset();
        MemoryStateManager::GetInstance().halUsed_.clear();
        MemoryStateManager::GetInstance().hostUsed_ = 0;
        MemoryStateManager::GetInstance().deviceUsedCache_.fill(-1);
        MemoryStateManager::GetInstance().processUsedCache_.fill(-1);
        MemoryStateManager::GetInstance().poolsMap_.clear();
        ConfigManager::Instance().SetConfig(Config{});
        EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    }
};

// UT-2: 查询失败 → 返回 -1、缓存不变、限频告警（查询失败）
TEST_F(DeviceUsageTest, query_failed_returns_negative_one)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    // 未注册 devId（HOST 设备 DEVICE_ID_CPU 不在 dcmi 设备映射中）必然查询失败：
    // 无卡环境 dcmi 不可用、有卡环境查表未命中，两种环境行为一致——返回 -1，且不写缓存
    EXPECT_EQ(report.QueryDeviceUsed(DEVICE_ID_CPU), -1);
    EXPECT_EQ(MemoryStateManager::GetInstance().deviceUsedCache_[0], -1);
    EXPECT_TRUE(report.deviceUsedQueryWarned_);  // 首次失败告警
    // 连续失败：仍返回 -1（告警已限频，不重复输出）
    EXPECT_EQ(report.QueryDeviceUsed(DEVICE_ID_CPU), -1);
    // 本进程用量查询同样失败（DEVICE_ID_CPU 不在 dcmi 设备映射）：-1、不写缓存、限频告警
    EXPECT_EQ(report.QueryProcessUsed(DEVICE_ID_CPU), -1);
    EXPECT_EQ(MemoryStateManager::GetInstance().processUsedCache_[0], -1);
    EXPECT_TRUE(report.processUsedQueryWarned_);
    EXPECT_EQ(report.QueryProcessUsed(DEVICE_ID_CPU), -1);
}

// UT-2: 查询失败不覆盖最近一次成功缓存值（缓存保持原值）
TEST_F(DeviceUsageTest, query_failed_keeps_previous_success_cache)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    MemoryStateManager::GetInstance().deviceUsedCache_[3] = 999;  // 模拟最近一次成功查询的缓存值
    EXPECT_EQ(report.QueryDeviceUsed(DEVICE_ID_CPU), -1);  // 未注册 devId 查询必然失败
    EXPECT_EQ(MemoryStateManager::GetInstance().deviceUsedCache_[3], 999);  // 缓存不被覆盖为 -1
    MemoryStateManager::GetInstance().processUsedCache_[3] = 888;
    EXPECT_EQ(report.QueryProcessUsed(DEVICE_ID_CPU), -1);
    EXPECT_EQ(MemoryStateManager::GetInstance().processUsedCache_[3], 888);  // 缓存不被覆盖为 -1
}

// UT-2: HAL MALLOC 事件 deviceUsed = 本次查询值（HAL MALLOC 查询填值）
TEST_F(DeviceUsageTest, hal_malloc_device_used_equals_query_value)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t flag = DEVICE_FLAG_BASE | 0;  // devId=0, DEVICE 空间
    // 带调用栈版本（正常采集路径）：3 参数版本为影子事件，不查询、不分发
    CallStackString stack;
    EXPECT_TRUE(report.ReportHalMalloc(0x1000, 100, flag, std::move(stack)));

    ASSERT_NE(captor_.last, nullptr);
    EXPECT_EQ(captor_.last->eventType, EventBaseType::MALLOC);
    EXPECT_EQ(captor_.last->device, 0);
    // 事件值 == 直接查询值（无卡环境均为 -1；有卡环境为真实整卡/本进程用量）
    EXPECT_EQ(captor_.last->deviceUsed, report.QueryDeviceUsed(0));
    EXPECT_EQ(captor_.last->processUsed, report.QueryProcessUsed(0));
    // 缓存与事件填值一致（无卡环境查询失败：事件 -1、缓存保持初始 -1；有卡环境均为真实用量）
    EXPECT_EQ(MemoryStateManager::GetInstance().deviceUsedCache_[0], captor_.last->deviceUsed);
    EXPECT_EQ(MemoryStateManager::GetInstance().processUsedCache_[0], captor_.last->processUsed);
}

// UT-2: HAL FREE 事件按 halPtrs 查表回填设备后查询填值（HAL FREE 查询填值）
TEST_F(DeviceUsageTest, hal_free_device_used_equals_query_value)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    uint64_t flag = DEVICE_FLAG_BASE | 2;  // devId=2
    // 带调用栈版本（正常采集路径）：3 参数版本为影子事件，不查询、不分发
    CallStackString stack;
    EXPECT_TRUE(report.ReportHalMalloc(0x2000, 100, flag, std::move(stack)));
    captor_.last.reset();
    EXPECT_TRUE(report.ReportHalRelease(0x2000, CallStackString{}));

    ASSERT_NE(captor_.last, nullptr);
    EXPECT_EQ(captor_.last->eventType, EventBaseType::FREE);
    EXPECT_EQ(captor_.last->device, 2);  // 分配时语义：与 MALLOC flag 解析同源
    EXPECT_EQ(captor_.last->deviceUsed, report.QueryDeviceUsed(2));
    EXPECT_EQ(captor_.last->processUsed, report.QueryProcessUsed(2));
}

// UT-2: halPtrs 查表未命中 → 事件被过滤，无查询（halPtrs 查表未命中）
TEST_F(DeviceUsageTest, hal_release_unregistered_addr_filtered)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    EXPECT_TRUE(report.ReportHalRelease(0x9999, CallStackString{}));  // 未注册地址
    EXPECT_EQ(captor_.last, nullptr);  // 无事件产生（无查询调用）
}

// UT-2: HOST_PINNED 不查询（HOST_PINNED 不查询）
TEST_F(DeviceUsageTest, host_pinned_does_not_query)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    // HOST 空间事件 device 归一为 DEVICE_ID_CPU，IsNeedSkip(DEVICE_ID_CPU) 依赖 collectCpu
    Config config = MemScope::GetConfig();
    config.collectCpu = true;
    ConfigManager::Instance().SetConfig(config);
    MemoryStateManager::GetInstance().deviceUsedCache_[0] = 1234;  // 即使缓存有值也不读
    MemoryStateManager::GetInstance().processUsedCache_[0] = 2345;
    uint64_t flag = HOST_FLAG_BASE;  // HOST 空间
    // 带调用栈版本（正常采集路径）：3 参数版本为影子事件，不查询、不分发
    CallStackString stack;
    EXPECT_TRUE(report.ReportHalMalloc(0x3000, 100, flag, std::move(stack)));

    ASSERT_NE(captor_.last, nullptr);
    EXPECT_EQ(captor_.last->eventSubType, EventSubType::HOST_PINNED);
    EXPECT_EQ(captor_.last->device, DEVICE_ID_CPU);
    EXPECT_EQ(captor_.last->deviceUsed, -1);  // 跳过查询与缓存读取，保持 -1
    // processUsed 不是报告层查询值/缓存值：报告层未查询未读缓存（缓存预置 2345 未被读走），
    // 统计层按 HOST 语义回填进程 VmRSS（UpdateUsage HOST 分支），故为 >0 的真实 RSS 而非 -1
    EXPECT_NE(captor_.last->processUsed, 2345);  // 未读缓存
    EXPECT_GT(captor_.last->processUsed, 0);     // VmRSS（进程必有 RSS）
}

// UT-2: 池事件读缓存（无查询调用）
TEST_F(DeviceUsageTest, pool_event_reads_cache)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    MemoryStateManager::GetInstance().deviceUsedCache_[0] = 500;   // 模拟最近一次 HAL 查询结果
    MemoryStateManager::GetInstance().processUsedCache_[0] = 300;  // 本进程用量缓存

    MemoryUsage usage{};
    usage.deviceIndex = 0;
    usage.dataType = 0;  // MALLOC
    usage.ptr = 0x4000;
    usage.allocSize = 100;
    usage.totalAllocated = 50;
    usage.totalReserved = 200;
    CallStackString stack;
    EXPECT_TRUE(report.ReportMemPoolRecord(EventSubType::PTA_CACHING, usage, std::move(stack)));

    ASSERT_NE(captor_.last, nullptr);
    EXPECT_EQ(captor_.last->device, 0);
    EXPECT_EQ(captor_.last->deviceUsed, 500);  // 缓存值，非查询值
    EXPECT_EQ(captor_.last->processUsed, 300);  // 缓存值，非查询值
}

// UT-2: 无任何 HAL 事件时池事件 deviceUsed=-1（池事件无缓存时）
TEST_F(DeviceUsageTest, pool_event_no_cache_negative_one)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    MemoryUsage usage{};
    usage.deviceIndex = 0;
    usage.dataType = 0;
    usage.ptr = 0x5000;
    usage.allocSize = 100;
    usage.totalAllocated = 50;
    usage.totalReserved = 200;
    CallStackString stack;
    EXPECT_TRUE(report.ReportMemPoolRecord(EventSubType::PTA_CACHING, usage, std::move(stack)));

    ASSERT_NE(captor_.last, nullptr);
    EXPECT_EQ(captor_.last->deviceUsed, -1);
    EXPECT_EQ(captor_.last->processUsed, -1);
}

// UT-2: 按设备隔离
TEST_F(DeviceUsageTest, pool_event_cache_per_device_isolation)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    MemoryStateManager::GetInstance().deviceUsedCache_[0] = 500;
    MemoryStateManager::GetInstance().deviceUsedCache_[2] = 800;
    MemoryStateManager::GetInstance().processUsedCache_[0] = 300;
    MemoryStateManager::GetInstance().processUsedCache_[2] = 700;

    MemoryUsage usage0{};
    usage0.deviceIndex = 0;
    usage0.dataType = 0;
    usage0.ptr = 0x6000;
    usage0.allocSize = 100;
    usage0.totalAllocated = 50;
    usage0.totalReserved = 200;
    CallStackString stack0;
    EXPECT_TRUE(report.ReportMemPoolRecord(EventSubType::PTA_CACHING, usage0, std::move(stack0)));
    EXPECT_EQ(captor_.last->deviceUsed, 500);
    EXPECT_EQ(captor_.last->processUsed, 300);

    MemoryUsage usage2{};
    usage2.deviceIndex = 2;
    usage2.dataType = 0;
    usage2.ptr = 0x7000;
    usage2.allocSize = 100;
    usage2.totalAllocated = 50;
    usage2.totalReserved = 200;
    CallStackString stack2;
    EXPECT_TRUE(report.ReportMemPoolRecord(EventSubType::PTA_CACHING, usage2, std::move(stack2)));
    EXPECT_EQ(captor_.last->deviceUsed, 800);
    EXPECT_EQ(captor_.last->processUsed, 700);
}

// UT-2: 影子事件不查询、deviceUsed=-1（影子路径不查询/影子池事件不读缓存）
TEST_F(DeviceUsageTest, shadow_event_device_used_stays_negative_one)
{
    EventReport& report = EventReport::Instance(MemScopeCommType::MEMORY_DEBUG);
    MemoryStateManager::GetInstance().deviceUsedCache_[0] = 500;  // 缓存有值，影子事件也不读

    // 影子 HAL MALLOC（报告层 shadow 重载无查询调用；此处走完整管线断言事件字段）
    auto shadow = std::make_shared<MemoryEvent>();
    shadow->eventType = EventBaseType::MALLOC;
    shadow->eventSubType = EventSubType::HAL;
    shadow->poolType = PoolType::HAL;
    shadow->addr = 0x8000;
    shadow->device = 0;
    shadow->pid = 1;
    shadow->size = 100;
    shadow->isShadowEvent = true;
    EventHandler(shadow);
    EXPECT_EQ(shadow->deviceUsed, -1);  // 无查询、不读缓存，保持报告层默认值
    EXPECT_EQ(shadow->processUsed, -1);

    // 影子池事件（ReportMemPoolRecord 影子模式提前返回，不读缓存——代码路径见 event_report.cpp:440-446）
    MemoryUsage usage{};
    usage.deviceIndex = 0;
    usage.dataType = 0;
    usage.ptr = 0x9000;
    usage.allocSize = 100;
    usage.totalAllocated = 50;
    usage.totalReserved = 200;
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);  // 影子采集态
    CallStackString stack;
    EXPECT_TRUE(report.ReportMemPoolRecord(EventSubType::PTA_CACHING, usage, std::move(stack)));
    EXPECT_EQ(captor_.last, nullptr);  // 影子事件不进入分析分发（captor 收不到）
}
