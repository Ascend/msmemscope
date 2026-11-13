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
#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "event.h"
#include "event_dispatcher.h"
#define private public
#include "inefficient_analyzer.h"
#undef private
#include "memory_state_manager.h"

// 假设以下类和结构体已定义
namespace MemScope {
// 测试用例
class InefficientAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        analyzer = &InefficientAnalyzer::GetInstance();
    }

    InefficientAnalyzer* analyzer;
};

TEST_F(InefficientAnalyzerTest, TestOPLaunchStartEnd)
{
    uint64_t pid = 1234;

    // ATB_START
    auto eventStart = std::make_shared<EventBase>();
    eventStart->eventType = EventBaseType::OP_LAUNCH;
    eventStart->eventSubType = EventSubType::ATB_START;
    eventStart->pid = pid;
    MemoryState state;
    analyzer->EventHandle(eventStart, &state);

    // ATB_END
    auto eventEnd = std::make_shared<EventBase>();
    eventEnd->eventType = EventBaseType::OP_LAUNCH;
    eventEnd->eventSubType = EventSubType::ATB_END;
    eventEnd->pid = pid;
    analyzer->EventHandle(eventEnd, &state);
}

TEST_F(InefficientAnalyzerTest, TestMALLOCEventHandling)
{
    uint64_t pid = 1234;

    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::MALLOC;
    event->eventSubType = EventSubType::ATB;
    event->pid = pid;
    MemoryState state;
    analyzer->EventHandle(event, &state);
}

TEST_F(InefficientAnalyzerTest, TestACCESSEventTemporaryIdleness)
{
    uint64_t pid = 1234;

    auto mallocEvent = std::make_shared<EventBase>();
    mallocEvent->eventType = EventBaseType::MALLOC;
    mallocEvent->eventSubType = EventSubType::ATB;
    mallocEvent->pid = pid;
    auto event1 = std::make_shared<EventBase>();
    event1->eventType = EventBaseType::ACCESS;
    event1->eventSubType = EventSubType::ATB;
    event1->pid = pid;
    auto event2 = std::make_shared<EventBase>();
    event2->eventType = EventBaseType::ACCESS;
    event2->eventSubType = EventSubType::ATB;
    event2->pid = pid;
    MemoryState state;
    auto memMallocEvent = std::dynamic_pointer_cast<MemoryEvent>(mallocEvent);
    auto memEvent1 = std::dynamic_pointer_cast<MemoryEvent>(event1);
    auto memEvent2 = std::dynamic_pointer_cast<MemoryEvent>(event2);
    state.events.push_back(memMallocEvent);
    state.events.push_back(memEvent1);
    state.events.push_back(memEvent2);
    state.apiId.push_back(0);
    state.apiId.push_back(1);
    state.apiId.push_back(200);

    analyzer->EventHandle(event2, &state);
}

// 影子事件应被跳过
TEST_F(InefficientAnalyzerTest, TestShadowEventIsSkipped)
{
    auto shadowEvent = std::make_shared<MemoryEvent>();
    shadowEvent->eventType = EventBaseType::MALLOC;
    shadowEvent->poolType = PoolType::PTA_CACHING;
    shadowEvent->eventSubType = EventSubType::PTA_CACHING;
    shadowEvent->pid = 12345;
    shadowEvent->addr = 0x9999;
    shadowEvent->size = 512;
    shadowEvent->isShadowEvent = true;

    std::shared_ptr<EventBase> eventBase = shadowEvent;
    MemoryState state;

    // Should return immediately without modifying state or crashing
    EXPECT_NO_THROW(analyzer->EventHandle(eventBase, &state));
    // State should remain unmodified
    EXPECT_TRUE(state.events.empty());
}

// ==================== 增强覆盖用例（真实事件流 + 白盒分支） ====================
namespace
{
// 构造指定类型/池/地址的内存事件（默认device=0）
std::shared_ptr<MemoryEvent> MakeInefficientMemEvent(EventBaseType eventType, PoolType pool, uint64_t pid,
                                                     uint64_t addr, uint64_t id)
{
    auto event = std::make_shared<MemoryEvent>();
    event->eventType = eventType;
    event->eventSubType = EventSubType::PTA_CACHING;
    event->poolType = pool;
    event->pid = pid;
    event->device = 0;
    event->addr = addr;
    event->id = id;
    event->timestamp = id * 1000;
    event->size = 256;
    event->used = 256;
    event->total = 512;
    return event;
}

// 构造OP_LAUNCH事件
std::shared_ptr<EventBase> MakeInefficientOpLaunch(EventSubType subType, uint64_t pid)
{
    auto event = std::make_shared<EventBase>();
    event->eventType = EventBaseType::OP_LAUNCH;
    event->eventSubType = subType;
    event->pid = pid;
    return event;
}

// PidState含atomic成员，不能按值返回，统一用引用初始化
void InitInefficientPidState(InefficientAnalyzer::PidState& pidState)
{
    pidState.apiId.store(0);
    pidState.mallocApiTmpId = MAX_UNIT64;
    pidState.freeApiTmpId = MAX_UNIT64;
    pidState.isOpStart = false;
    pidState.apiTmp.clear();
}

// 直接填充MemoryState的events/apiId，用于白盒驱动判定函数
void FillInefficientState(MemoryState& state, const std::vector<EventBaseType>& eventTypes,
                          const std::vector<uint64_t>& apiIds)
{
    state.events.clear();
    for (const auto& type : eventTypes)
    {
        auto event = std::make_shared<MemoryEvent>();
        event->eventType = type;
        state.events.push_back(event);
    }
    state.apiId = apiIds;
    state.inefficientType.clear();
}
}  // namespace

// 状态相关用例的公共夹具：每个用例前后复位分析器单例状态与MSM，避免跨用例残留
class InefficientAnalyzerStateTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        analyzer = &InefficientAnalyzer::GetInstance();
        ResetAnalyzerState();
        ClearMemoryStates();
    }

    void TearDown() override
    {
        ResetAnalyzerState();
        ClearMemoryStates();
    }

    static void ResetAnalyzerState()
    {
        InefficientAnalyzer::GetInstance().PTAPidStatesMap.clear();
        InefficientAnalyzer::GetInstance().ATBPidStatesMap.clear();
    }

    static void ClearMemoryStates()
    {
        auto keys = MemoryStateManager::GetInstance().GetAllStateKeys();
        for (const auto& keyPair : keys)
        {
            MemoryStateManager::GetInstance().DeteleState(keyPair.first, keyPair.second);
        }
    }

    InefficientAnalyzer* analyzer = nullptr;
};

// 独立MALLOC/ACCESS/FREE事件：apiId逐事件自增，apiTmp分类结果落入malloc/free分类字段
TEST_F(InefficientAnalyzerStateTest, malloc_access_free_standalone_events_update_pid_state)
{
    const uint64_t pid = 7001;
    auto mallocEvent = MakeInefficientMemEvent(EventBaseType::MALLOC, PoolType::PTA_CACHING, pid, 0x9100, 1);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(mallocEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> mallocBase = mallocEvent;
    analyzer->EventHandle(mallocBase, state);

    auto& pidState = analyzer->PTAPidStatesMap[pid];
    // 独立MALLOC：apiId自增到1，分类记录mallocApiTmpId
    EXPECT_EQ(pidState.apiId.load(), 1u);
    EXPECT_EQ(pidState.mallocApiTmpId, 1u);
    EXPECT_EQ(pidState.freeApiTmpId, MAX_UNIT64);
    ASSERT_EQ(state->apiId.size(), 1u);
    EXPECT_EQ(state->apiId[0], 1u);

    // 同地址ACCESS：apiId=2，ACCESS不改变free分类
    auto accessEvent = MakeInefficientMemEvent(EventBaseType::ACCESS, PoolType::PTA_CACHING, pid, 0x9100, 2);
    state = MemoryStateManager::GetInstance().AddEvent(accessEvent);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->events.size(), 2u);
    std::shared_ptr<EventBase> accessBase = accessEvent;
    analyzer->EventHandle(accessBase, state);
    EXPECT_EQ(pidState.apiId.load(), 2u);
    EXPECT_EQ(pidState.freeApiTmpId, MAX_UNIT64);
    ASSERT_EQ(state->apiId.size(), 2u);
    EXPECT_EQ(state->apiId[1], 2u);

    // 同地址FREE：apiId=3，分类记录freeApiTmpId
    auto freeEvent = MakeInefficientMemEvent(EventBaseType::FREE, PoolType::PTA_CACHING, pid, 0x9100, 3);
    state = MemoryStateManager::GetInstance().AddEvent(freeEvent);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->events.size(), 3u);
    std::shared_ptr<EventBase> freeBase = freeEvent;
    analyzer->EventHandle(freeBase, state);
    EXPECT_EQ(pidState.apiId.load(), 3u);
    EXPECT_EQ(pidState.freeApiTmpId, 3u);
    // LAST ACCESS(api2)小于malloc分类(api1)不成立，不得判定late_deallocation
    EXPECT_TRUE(state->inefficientType.empty());
}

// 独立FREE先建立free分类，后续ACCESS经真实事件流判定early_allocation
TEST_F(InefficientAnalyzerStateTest, access_after_free_classification_marks_early_allocation)
{
    const uint64_t pid = 7010;
    auto mallocEvent = MakeInefficientMemEvent(EventBaseType::MALLOC, PoolType::PTA_CACHING, pid, 0xA100, 1);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(mallocEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> mallocBase = mallocEvent;
    analyzer->EventHandle(mallocBase, state);
    EXPECT_EQ(analyzer->PTAPidStatesMap[pid].mallocApiTmpId, 1u);

    // 另一地址的独立FREE：apiId=2，建立free分类（幽灵state，首事件为FREE不进入判定）
    auto freeEvent = MakeInefficientMemEvent(EventBaseType::FREE, PoolType::PTA_CACHING, pid, 0xA200, 2);
    MemoryState* freeState = MemoryStateManager::GetInstance().AddEvent(freeEvent);
    ASSERT_NE(freeState, nullptr);
    std::shared_ptr<EventBase> freeBase = freeEvent;
    analyzer->EventHandle(freeBase, freeState);
    EXPECT_EQ(analyzer->PTAPidStatesMap[pid].freeApiTmpId, 2u);

    // 原块ACCESS（apiId=3）：MALLOC(api1) → FREE(api2) → 首次ACCESS(api3)，判定过早申请
    auto accessEvent = MakeInefficientMemEvent(EventBaseType::ACCESS, PoolType::PTA_CACHING, pid, 0xA100, 3);
    state = MemoryStateManager::GetInstance().AddEvent(accessEvent);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->events.size(), 2u);
    std::shared_ptr<EventBase> accessBase = accessEvent;
    analyzer->EventHandle(accessBase, state);
    EXPECT_EQ(state->inefficientType, "early_allocation");
    EXPECT_EQ(state->apiId.size(), state->events.size());
}

// OP_LAUNCH按子类型路由到PTA(ATEN)/ATB两张独立pid表；未知子类型仅建状态不改变apiId
TEST_F(InefficientAnalyzerStateTest, op_launch_routes_aten_and_atb_to_separate_pid_maps)
{
    const uint64_t atenPid = 7002;
    const uint64_t atbPid = 7003;
    const uint64_t kernelPid = 7004;

    auto atenStart = MakeInefficientOpLaunch(EventSubType::ATEN_START, atenPid);
    analyzer->EventHandle(atenStart, nullptr);
    EXPECT_EQ(analyzer->PTAPidStatesMap.count(atenPid), 1u);
    EXPECT_EQ(analyzer->ATBPidStatesMap.count(atenPid), 0u);
    EXPECT_EQ(analyzer->PTAPidStatesMap[atenPid].apiId.load(), 1u);
    EXPECT_TRUE(analyzer->PTAPidStatesMap[atenPid].isOpStart);

    // END复位isOpStart；空apiTmp分类直接返回
    auto atenEnd = MakeInefficientOpLaunch(EventSubType::ATEN_END, atenPid);
    analyzer->EventHandle(atenEnd, nullptr);
    EXPECT_FALSE(analyzer->PTAPidStatesMap[atenPid].isOpStart);

    auto atbStart = MakeInefficientOpLaunch(EventSubType::ATB_START, atbPid);
    analyzer->EventHandle(atbStart, nullptr);
    EXPECT_EQ(analyzer->ATBPidStatesMap.count(atbPid), 1u);
    EXPECT_EQ(analyzer->PTAPidStatesMap.count(atbPid), 0u);
    EXPECT_EQ(analyzer->ATBPidStatesMap[atbPid].apiId.load(), 1u);

    auto atbEnd = MakeInefficientOpLaunch(EventSubType::ATB_END, atbPid);
    analyzer->EventHandle(atbEnd, nullptr);
    EXPECT_FALSE(analyzer->ATBPidStatesMap[atbPid].isOpStart);

    // KERNEL_LAUNCH等非START/END子类型：落入ATB表但不自增apiId
    auto kernelLaunch = MakeInefficientOpLaunch(EventSubType::KERNEL_LAUNCH, kernelPid);
    analyzer->EventHandle(kernelLaunch, nullptr);
    ASSERT_EQ(analyzer->ATBPidStatesMap.count(kernelPid), 1u);
    EXPECT_EQ(analyzer->ATBPidStatesMap[kernelPid].apiId.load(), 0u);
    EXPECT_FALSE(analyzer->ATBPidStatesMap[kernelPid].isOpStart);
}

// op区间内内存事件只累积不增apiId，END时统一分类同一API内的malloc/free
TEST_F(InefficientAnalyzerStateTest, op_start_scope_accumulates_events_without_api_increment)
{
    const uint64_t pid = 7005;
    auto start = MakeInefficientOpLaunch(EventSubType::ATEN_START, pid);
    analyzer->EventHandle(start, nullptr);
    EXPECT_EQ(analyzer->PTAPidStatesMap[pid].apiId.load(), 1u);

    auto mallocEvent = MakeInefficientMemEvent(EventBaseType::MALLOC, PoolType::PTA_CACHING, pid, 0xB100, 1);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(mallocEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> mallocBase = mallocEvent;
    analyzer->EventHandle(mallocBase, state);
    EXPECT_EQ(analyzer->PTAPidStatesMap[pid].apiId.load(), 1u);
    // 区间内未分类，mallocApiTmpId保持MAX
    EXPECT_EQ(analyzer->PTAPidStatesMap[pid].mallocApiTmpId, MAX_UNIT64);

    auto freeEvent = MakeInefficientMemEvent(EventBaseType::FREE, PoolType::PTA_CACHING, pid, 0xB100, 2);
    state = MemoryStateManager::GetInstance().AddEvent(freeEvent);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> freeBase = freeEvent;
    analyzer->EventHandle(freeBase, state);
    EXPECT_EQ(analyzer->PTAPidStatesMap[pid].apiId.load(), 1u);
    EXPECT_EQ(analyzer->PTAPidStatesMap[pid].freeApiTmpId, MAX_UNIT64);

    // ATEN_END：同一API内既有malloc又有free，两个分类字段均指向api1
    auto end = MakeInefficientOpLaunch(EventSubType::ATEN_END, pid);
    analyzer->EventHandle(end, nullptr);
    EXPECT_EQ(analyzer->PTAPidStatesMap[pid].mallocApiTmpId, 1u);
    EXPECT_EQ(analyzer->PTAPidStatesMap[pid].freeApiTmpId, 1u);
    EXPECT_FALSE(analyzer->PTAPidStatesMap[pid].isOpStart);
}

// MINDSPORE池不在处理范围：既不建pid状态也不写入state.apiId
TEST_F(InefficientAnalyzerStateTest, mindspore_pool_memory_events_are_ignored)
{
    auto event = MakeInefficientMemEvent(EventBaseType::MALLOC, PoolType::MINDSPORE, 7008, 0xC100, 1);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(event);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> base = event;
    analyzer->EventHandle(base, state);
    EXPECT_EQ(analyzer->PTAPidStatesMap.count(7008), 0u);
    EXPECT_EQ(analyzer->ATBPidStatesMap.count(7008), 0u);
    EXPECT_TRUE(state->apiId.empty());
}

// ATB池内存事件使用ATB pid表
TEST_F(InefficientAnalyzerStateTest, atb_pool_memory_events_use_atb_pid_map)
{
    const uint64_t pid = 7009;
    auto event = MakeInefficientMemEvent(EventBaseType::MALLOC, PoolType::ATB, pid, 0xD100, 1);
    MemoryState* state = MemoryStateManager::GetInstance().AddEvent(event);
    ASSERT_NE(state, nullptr);
    std::shared_ptr<EventBase> base = event;
    analyzer->EventHandle(base, state);
    EXPECT_EQ(analyzer->ATBPidStatesMap.count(pid), 1u);
    EXPECT_EQ(analyzer->PTAPidStatesMap.count(pid), 0u);
    EXPECT_EQ(analyzer->ATBPidStatesMap[pid].apiId.load(), 1u);
    EXPECT_EQ(analyzer->ATBPidStatesMap[pid].mallocApiTmpId, 1u);
}

// EarlyAllocation白盒分支：提前break、去重、逗号追加、三类不判定出口
TEST_F(InefficientAnalyzerStateTest, early_allocation_white_box_branches)
{
    // MALLOC(api1) → ACCESS(api3) → ACCESS(api4)，free分类api2：
    // 首个与MALLOC不同的ACCESS为api3，命中“free在MALLOC与FA之间”
    MemoryState state;
    FillInefficientState(state, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::ACCESS}, {1, 3, 4});
    InefficientAnalyzer::PidState pidState;
    InitInefficientPidState(pidState);
    pidState.apiId.store(4);
    pidState.mallocApiTmpId = 1;
    pidState.freeApiTmpId = 2;
    analyzer->EarlyAllocation(state.events.back(), &state, pidState);
    EXPECT_EQ(state.inefficientType, "early_allocation");

    // 已含early_allocation：重复判定直接返回，不重复追加
    analyzer->EarlyAllocation(state.events.back(), &state, pidState);
    EXPECT_EQ(state.inefficientType, "early_allocation");

    // 已有其他类型：追加时补逗号分隔
    MemoryState commaState;
    FillInefficientState(commaState, {EventBaseType::MALLOC, EventBaseType::ACCESS}, {1, 3});
    commaState.inefficientType = "temporary_idleness";
    InefficientAnalyzer::PidState commaPidState;
    InitInefficientPidState(commaPidState);
    commaPidState.mallocApiTmpId = 1;
    commaPidState.freeApiTmpId = 2;
    analyzer->EarlyAllocation(commaState.events.back(), &commaState, commaPidState);
    EXPECT_EQ(commaState.inefficientType, "temporary_idleness,early_allocation");

    // ACCESS与MALLOC同API：未找到首个不同API的ACCESS，不判定
    MemoryState sameApiState;
    FillInefficientState(sameApiState, {EventBaseType::MALLOC, EventBaseType::ACCESS}, {1, 1});
    InefficientAnalyzer::PidState sameApiPidState;
    InitInefficientPidState(sameApiPidState);
    sameApiPidState.mallocApiTmpId = 1;
    sameApiPidState.freeApiTmpId = 2;
    analyzer->EarlyAllocation(sameApiState.events.back(), &sameApiState, sameApiPidState);
    EXPECT_TRUE(sameApiState.inefficientType.empty());

    // MALLOC与最近FREE同API：无法交换，不判定
    MemoryState sameFreeState;
    FillInefficientState(sameFreeState, {EventBaseType::MALLOC, EventBaseType::ACCESS}, {1, 3});
    InefficientAnalyzer::PidState sameFreePidState;
    InitInefficientPidState(sameFreePidState);
    sameFreePidState.mallocApiTmpId = 1;
    sameFreePidState.freeApiTmpId = 1;
    analyzer->EarlyAllocation(sameFreeState.events.back(), &sameFreeState, sameFreePidState);
    EXPECT_TRUE(sameFreeState.inefficientType.empty());

    // FA大于free分类（free在ACCESS之后）：顺序不满足，不判定
    MemoryState orderState;
    FillInefficientState(orderState, {EventBaseType::MALLOC, EventBaseType::ACCESS}, {3, 4});
    InefficientAnalyzer::PidState orderPidState;
    InitInefficientPidState(orderPidState);
    orderPidState.mallocApiTmpId = 3;
    orderPidState.freeApiTmpId = 5;
    analyzer->EarlyAllocation(orderState.events.back(), &orderState, orderPidState);
    EXPECT_TRUE(orderState.inefficientType.empty());
}

// LateDeallocation白盒分支：正常判定、逗号追加、无LA、LA在当前API、条件不满足
TEST_F(InefficientAnalyzerStateTest, late_deallocation_white_box_branches)
{
    // MALLOC(api1) → ACCESS(api2) → FREE(api5)，malloc分类api4：2 < 4 < 5
    MemoryState state;
    FillInefficientState(state, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::FREE}, {1, 2, 5});
    InefficientAnalyzer::PidState pidState;
    InitInefficientPidState(pidState);
    pidState.apiId.store(5);
    pidState.mallocApiTmpId = 4;
    analyzer->LateDeallocation(state.events.back(), &state, pidState);
    EXPECT_EQ(state.inefficientType, "late_deallocation");

    // 已有其他类型：追加时补逗号分隔
    MemoryState commaState;
    FillInefficientState(commaState, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::FREE}, {1, 2, 5});
    commaState.inefficientType = "early_allocation";
    InefficientAnalyzer::PidState commaPidState;
    InitInefficientPidState(commaPidState);
    commaPidState.apiId.store(5);
    commaPidState.mallocApiTmpId = 4;
    analyzer->LateDeallocation(commaState.events.back(), &commaState, commaPidState);
    EXPECT_EQ(commaState.inefficientType, "early_allocation,late_deallocation");

    // 倒数第二个事件不是ACCESS（无LAST ACCESS）：不判定
    MemoryState noAccessState;
    FillInefficientState(noAccessState, {EventBaseType::MALLOC, EventBaseType::FREE}, {1, 5});
    InefficientAnalyzer::PidState noAccessPidState;
    InitInefficientPidState(noAccessPidState);
    noAccessPidState.apiId.store(5);
    noAccessPidState.mallocApiTmpId = 4;
    analyzer->LateDeallocation(noAccessState.events.back(), &noAccessState, noAccessPidState);
    EXPECT_TRUE(noAccessState.inefficientType.empty());

    // 事件数不足两条：不判定
    MemoryState shortState;
    FillInefficientState(shortState, {EventBaseType::FREE}, {5});
    InefficientAnalyzer::PidState shortPidState;
    InitInefficientPidState(shortPidState);
    shortPidState.apiId.store(5);
    shortPidState.mallocApiTmpId = 4;
    analyzer->LateDeallocation(shortState.events.back(), &shortState, shortPidState);
    EXPECT_TRUE(shortState.inefficientType.empty());

    // LAST ACCESS就在当前API：不判定
    MemoryState sameApiState;
    FillInefficientState(sameApiState, {EventBaseType::MALLOC, EventBaseType::ACCESS}, {1, 4});
    InefficientAnalyzer::PidState sameApiPidState;
    InitInefficientPidState(sameApiPidState);
    sameApiPidState.apiId.store(4);
    sameApiPidState.mallocApiTmpId = 4;
    analyzer->LateDeallocation(sameApiState.events.back(), &sameApiState, sameApiPidState);
    EXPECT_TRUE(sameApiState.inefficientType.empty());

    // LA之后到FREE之间无MALLOC（LA不小于malloc分类）：不判定
    MemoryState noMallocGapState;
    FillInefficientState(noMallocGapState, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::FREE},
                         {1, 2, 5});
    InefficientAnalyzer::PidState noMallocGapPidState;
    InitInefficientPidState(noMallocGapPidState);
    noMallocGapPidState.apiId.store(5);
    noMallocGapPidState.mallocApiTmpId = 1;
    analyzer->LateDeallocation(noMallocGapState.events.back(), &noMallocGapState, noMallocGapPidState);
    EXPECT_TRUE(noMallocGapState.inefficientType.empty());
}

// TemporaryIdleness白盒分支：超过阈值、追加逗号、去重、未超阈值、事件不足、前序非ACCESS
TEST_F(InefficientAnalyzerStateTest, temporary_idleness_white_box_branches)
{
    // 两个ACCESS相隔3999个API（>3000）：判定临时闲置
    MemoryState state;
    FillInefficientState(state, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::ACCESS}, {0, 1, 4000});
    analyzer->TemporaryIdleness(state.events.back(), &state);
    EXPECT_EQ(state.inefficientType, "temporary_idleness");

    // 已含temporary_idleness：重复判定直接返回
    analyzer->TemporaryIdleness(state.events.back(), &state);
    EXPECT_EQ(state.inefficientType, "temporary_idleness");

    // 已有其他类型：追加时补逗号分隔
    MemoryState commaState;
    FillInefficientState(commaState, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::ACCESS},
                         {0, 1, 4000});
    commaState.inefficientType = "early_allocation";
    analyzer->TemporaryIdleness(commaState.events.back(), &commaState);
    EXPECT_EQ(commaState.inefficientType, "early_allocation,temporary_idleness");

    // 恰好等于阈值3000：不判定（必须严格大于）
    MemoryState equalState;
    FillInefficientState(equalState, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::ACCESS},
                         {0, 1, 3001});
    analyzer->TemporaryIdleness(equalState.events.back(), &equalState);
    EXPECT_TRUE(equalState.inefficientType.empty());

    // 后一个API值反而更小：不判定
    MemoryState decreaseState;
    FillInefficientState(decreaseState, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::ACCESS},
                         {0, 5000, 4000});
    analyzer->TemporaryIdleness(decreaseState.events.back(), &decreaseState);
    EXPECT_TRUE(decreaseState.inefficientType.empty());

    // 事件数不足两条：不判定
    MemoryState shortState;
    FillInefficientState(shortState, {EventBaseType::ACCESS}, {4000});
    analyzer->TemporaryIdleness(shortState.events.back(), &shortState);
    EXPECT_TRUE(shortState.inefficientType.empty());

    // 倒数第二个事件不是ACCESS：不判定
    MemoryState notAccessState;
    FillInefficientState(notAccessState, {EventBaseType::MALLOC, EventBaseType::ACCESS}, {0, 4000});
    analyzer->TemporaryIdleness(notAccessState.events.back(), &notAccessState);
    EXPECT_TRUE(notAccessState.inefficientType.empty());
}

// InefficientAnalysis入口门控：首事件非MALLOC、events/apiId长度不等、ACCESS/FREE分类门控
TEST_F(InefficientAnalyzerStateTest, inefficient_analysis_gates_on_event_layout)
{
    // 首事件不是MALLOC：直接返回（否则会误判定late_deallocation）
    MemoryState firstNotMalloc;
    FillInefficientState(firstNotMalloc, {EventBaseType::ACCESS, EventBaseType::ACCESS, EventBaseType::FREE},
                         {1, 2, 5});
    InefficientAnalyzer::PidState pidState;
    InitInefficientPidState(pidState);
    pidState.apiId.store(5);
    pidState.mallocApiTmpId = 4;
    analyzer->InefficientAnalysis(firstNotMalloc.events.back(), &firstNotMalloc, pidState);
    EXPECT_TRUE(firstNotMalloc.inefficientType.empty());

    // events与apiId长度不等：直接返回
    MemoryState sizeMismatch;
    FillInefficientState(sizeMismatch, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::FREE}, {1, 2});
    InitInefficientPidState(pidState);
    pidState.apiId.store(5);
    pidState.mallocApiTmpId = 4;
    analyzer->InefficientAnalysis(sizeMismatch.events.back(), &sizeMismatch, pidState);
    EXPECT_TRUE(sizeMismatch.inefficientType.empty());

    // ACCESS但尚未产生free分类：TemporaryIdleness后提前返回，不进入EarlyAllocation
    MemoryState accessNoFree;
    FillInefficientState(accessNoFree, {EventBaseType::MALLOC, EventBaseType::ACCESS}, {1, 3});
    InitInefficientPidState(pidState);
    pidState.apiId.store(3);
    pidState.mallocApiTmpId = 1;
    analyzer->InefficientAnalysis(accessNoFree.events.back(), &accessNoFree, pidState);
    EXPECT_TRUE(accessNoFree.inefficientType.empty());

    // FREE且已有malloc分类：进入LateDeallocation并判定
    MemoryState freeWithMalloc;
    FillInefficientState(freeWithMalloc, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::FREE},
                         {1, 2, 5});
    InitInefficientPidState(pidState);
    pidState.apiId.store(5);
    pidState.mallocApiTmpId = 4;
    analyzer->InefficientAnalysis(freeWithMalloc.events.back(), &freeWithMalloc, pidState);
    EXPECT_EQ(freeWithMalloc.inefficientType, "late_deallocation");

    // FREE但无malloc分类：不进入LateDeallocation
    MemoryState freeNoMalloc;
    FillInefficientState(freeNoMalloc, {EventBaseType::MALLOC, EventBaseType::ACCESS, EventBaseType::FREE},
                         {1, 2, 5});
    InitInefficientPidState(pidState);
    pidState.apiId.store(5);
    analyzer->InefficientAnalysis(freeNoMalloc.events.back(), &freeNoMalloc, pidState);
    EXPECT_TRUE(freeNoMalloc.inefficientType.empty());
}

} // namespace
