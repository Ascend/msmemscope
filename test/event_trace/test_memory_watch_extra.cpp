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
#define private public
#include "event_trace/memory_watch/memory_watch.h"
#undef private
#include "event_trace/memory_watch/tensor_dumper.h"
#include "event_trace/memory_watch/tensor_monitor.h"
#include "utility/utils.h"

using namespace MemScope;

namespace {
constexpr uint64_t UT_TENSOR_ADDR = 0x5A5A5A0011ULL;

// 清空可能由其他用例遗留的监控张量，保证本用例初始不在监控态
void ResetTensorMonitor()
{
    TensorMonitor::GetInstance().ClearCmdWatchTensor();
    auto pythonMap = TensorMonitor::GetInstance().GetPythonWatchedTensorsMap();
    for (auto& item : pythonMap) {
        MonitoredTensor tensor = item.second;
        TensorMonitor::GetInstance().DeleteWatchTensor(tensor);
    }
}
}  // namespace

// C接口OpExcuteBegin/OpExcuteEnd：计数累加与tensor数组转换
TEST(MemoryWatchExtraTest, op_excute_c_wrappers)
{
    auto& instance = MemoryWatch::GetInstance();
    ResetTensorMonitor();
    instance.firstWatchTarget_ = "ut_first";
    instance.lastWatchTarget_ = "ut_last";
    instance.outputId_ = UINT32_MAX;
    instance.ClearWatchedTargetName();
    instance.targetNameCnt_.clear();

    char opName[] = "ut/opA";
    OpExcuteBegin(nullptr, opName);
    OpExcuteBegin(nullptr, opName);
    ASSERT_EQ(instance.targetNameCnt_.count("opA"), 1U);
    EXPECT_EQ(instance.targetNameCnt_["opA"], 2U);

    MonitoredTensor tensor = {};
    tensor.data = nullptr;
    tensor.dataSize = 0;
    OpExcuteEnd(nullptr, opName, &tensor, 1);
    // 非start/end算子且不在监控态：不改变监控目标，计数保持
    EXPECT_EQ(instance.GetWatchedTargetName(), "");
    EXPECT_EQ(instance.targetNameCnt_["opA"], 2U);
}

// atb外层kernel置位repeat标记后，内层begin/end应提前返回，不触发dump；
// 外层end清除标记后再dump一次
TEST(MemoryWatchExtraTest, kernel_repeat_watch_suppressed)
{
    auto& instance = MemoryWatch::GetInstance();
    ResetTensorMonitor();
    instance.firstWatchTarget_ = "ut_first";
    instance.lastWatchTarget_ = "ut_last";
    instance.outputId_ = UINT32_MAX;
    instance.ClearWatchedTargetName();

    MonitoredTensor tensor = {};
    tensor.data = reinterpret_cast<void*>(static_cast<uintptr_t>(UT_TENSOR_ADDR));
    tensor.dataSize = 0;
    TensorMonitor::GetInstance().AddWatchTensor(tensor);
    TensorDumper::GetInstance().SetDumpNums(UT_TENSOR_ADDR, 3);

    uint64_t tid = Utility::GetTid();
    instance.KernelExcuteBegin(nullptr, "ut_kernel", true);
    EXPECT_TRUE(instance.isRepeatWatch_[tid]);
    // 外层begin已触发一次dump
    EXPECT_EQ(TensorDumper::GetInstance().GetDumpNums(UT_TENSOR_ADDR), 2);

    // 内层begin命中repeat标记，直接返回，不产生dump
    instance.KernelExcuteBegin(nullptr, "ut_kernel", false);
    EXPECT_EQ(TensorDumper::GetInstance().GetDumpNums(UT_TENSOR_ADDR), 2);

    // 内层end同样被抑制
    instance.KernelExcuteEnd(nullptr, "ut_kernel", false);
    EXPECT_EQ(TensorDumper::GetInstance().GetDumpNums(UT_TENSOR_ADDR), 2);

    // 外层end清除标记并执行一次dump
    instance.KernelExcuteEnd(nullptr, "ut_kernel", true);
    EXPECT_FALSE(instance.isRepeatWatch_[tid]);
    EXPECT_EQ(TensorDumper::GetInstance().GetDumpNums(UT_TENSOR_ADDR), 1);

    TensorMonitor::GetInstance().DeleteWatchTensor(tensor);
    TensorDumper::GetInstance().DeleteDumpNums(UT_TENSOR_ADDR);
}

// C接口ATBKernelExcute：after/before后缀分派；无后缀时打印错误且不改变状态
TEST(MemoryWatchExtraTest, atb_kernel_c_wrapper_and_invalid_info)
{
    auto& instance = MemoryWatch::GetInstance();
    ResetTensorMonitor();
    instance.firstWatchTarget_ = "ut_first";
    instance.lastWatchTarget_ = "ut_last";
    instance.outputId_ = UINT32_MAX;
    instance.ClearWatchedTargetName();

    Mki::SVector<Mki::Tensor> tensors = {};
    tensors.push_back(Mki::Tensor{});

    std::string before = "/ut/atbKernel/before";
    ATBKernelExcute(nullptr, const_cast<char*>(before.c_str()), tensors);
    uint64_t tid = Utility::GetTid();
    EXPECT_TRUE(instance.isRepeatWatch_[tid]);

    std::string after = "/ut/atbKernel/after";
    ATBKernelExcute(nullptr, const_cast<char*>(after.c_str()), tensors);
    EXPECT_FALSE(instance.isRepeatWatch_[tid]);
    EXPECT_EQ(instance.GetWatchedTargetName(), "");

    // 既非before也非after：走无效kernel信息分支，状态保持不变
    std::string invalid = "/ut/atbKernel/unknown";
    ATBKernelExcute(nullptr, const_cast<char*>(invalid.c_str()), tensors);
    EXPECT_FALSE(instance.isRepeatWatch_[tid]);
    EXPECT_EQ(instance.GetWatchedTargetName(), "");
}
