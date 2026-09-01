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

#include <chrono>
#include <thread>

#include "cpython.h"
#include "trace_manager/event_trace_manager.h"

namespace
{
// 在 CLI 模式下，首个 NPU 算子之前没有任何 C++ 钩子触发（aclInit/aclrtSetDevice 均为惰性），
// 所以 `import torch, torch_npu` 之后、首个 NPU 算子之前创建的 CPU tensor 会被漏采。
// 本线程由 LD_PRELOAD 的 hook 库构造函数启动：等 Python 解释器初始化完成后立即武装 CPU tensor
// 采集（HandleWithCpuTensorCollect 会在 torch_npu 尚未导入时安装 torch_npu import POST_HOOK，
// 由它在 torch_npu 导入完成后同步注册）。这要求 `import msmemscope.cpu_tensor_collection`
// 不依赖 torch（走无 torch 门控的 MemScopePythonCallNoTorch），否则在 torch 导入前无法完成武装。
void CpuTensorTriggerThread()
{
    // 等待 Python 解释器初始化完成。Py_IsInitialized 是弱符号，libpython 未加载时为 null。
    while (!Utility::IsPyInterpRepeInited())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 在同一 GIL 持有时段内武装 CPU tensor 采集：若检测到 torch 后再释放 GIL 重新获取，
    // 主线程会在 torch→torch_npu→首个 CPU tensor 之间连续持有 GIL，导致注册晚于首个
    // CPU tensor 而漏采。这里先武装，再检查 torch 是否已进入 sys.modules 决定是否退出轮询。
    while (true)
    {
        bool torchLoaded = false;
        {
            Utility::PyInterpGuard guard;
            MemScope::EventTraceManager::Instance().HandleWithCpuTensorCollect();
            Utility::PythonObject sys = Utility::PythonObject::Import("sys");
            if (!sys.IsBad())
            {
                Utility::PythonObject modules = sys.Get("modules");
                torchLoaded = !modules.GetItem(Utility::PythonObject("torch")).IsBad();
            }
        }
        if (torchLoaded)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
}  // namespace

__attribute__((constructor)) static void StartCpuTensorTrigger() { std::thread(CpuTensorTriggerThread).detach(); }
