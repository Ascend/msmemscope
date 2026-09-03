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

#include <Python.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "call_stack.h"
#include "cpython.h"
#include "describerobject.h"
#include "event_report.h"
#include "oom_handler.h"
#include "op_handler.h"
#include "recordfuncobject.h"
#include "string_validator.h"
#include "trace_manager/event_trace_manager.h"
#include "tracerobject.h"
#include "watcherobject.h"

namespace MemScope
{

namespace
{
// host内存泄漏检测:python config()入口的preload状态校验。
// /proc/self/maps为ground truth,MSMEMSCOPE_API_ENV标记(wrapper source时导出)作旁证,
// 两者均未检测到才报错
constexpr const char* HOST_HOOK_SO_NAME = "libmsmemscope_host_mem_hook.so";
const char* const NPU_HOOK_SO_NAMES[] = {"libleaks_ascend_hal_hook.so", "libascend_mstx_hook.so",
                                         "libascend_kernel_hook.so", "libatb_abi_0_hook.so", "libatb_abi_1_hook.so"};

bool IsSoLoaded(const char* soName)
{
    std::ifstream mapsFile("/proc/self/maps");
    if (!mapsFile.is_open())
    {
        return false;
    }
    std::string line;
    while (std::getline(mapsFile, line))
    {
        if (line.find(soName) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

bool IsNpuHookLoaded()
{
    for (const char* soName : NPU_HOOK_SO_NAMES)
    {
        if (IsSoLoaded(soName))
        {
            return true;
        }
    }
    return false;
}

bool IsApiEnvMode(const char* mode)
{
    const char* env = std::getenv("MSMEMSCOPE_API_ENV");
    return env != nullptr && std::string(env) == mode;
}

// 拆解analysis值中的token(中英文逗号兼容,与ParseAnalysis一致),识别host-leaks与
// npu分析项(leaks/decompose/inefficient/oom前缀)两类标记
void SplitAnalysisTokens(const std::string& analysisValue, bool& hasHostLeaks, bool& hasNpuAnalysis)
{
    hasHostLeaks = false;
    hasNpuAnalysis = false;
    std::vector<std::string> tokens = Utility::SplitString(analysisValue, "，,");
    for (const std::string& token : tokens)
    {
        if (token == "host-leaks")
        {
            hasHostLeaks = true;
        }
        else if (token.rfind("oom", 0) == 0 || token == "leaks" || token == "decompose" || token == "inefficient")
        {
            hasNpuAnalysis = true;
        }
    }
}
}  // namespace

PyDoc_STRVAR(MsmemscopeCModuleDoc,
             "The part of the module msmemscope that is implemented in CXX.\n\
 \n\
...");

PyDoc_STRVAR(StartDoc, "start()\n--\n\nstart trace data.");
static PyObject* MsmemscopeStart(PyObject* self, PyObject* args)
{
    ConfigManager::Instance().InitStartConfig();
    Py_RETURN_NONE;
}

PyDoc_STRVAR(StopDoc, "stop()\n--\n\nstop trace data.");
static PyObject* MsmemscopeStop(PyObject* self, PyObject* args)
{
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    EventTraceManager::Instance().CleanUpEventTraceManager();
    Py_RETURN_NONE;
}

PyDoc_STRVAR(StepDoc, "step()\n--\n\nmark step info.");
static PyObject* MsmemscopeStep(PyObject* self, PyObject* args)
{
    if (!EventTraceManager::Instance().IsTracingEnabled())
    {
        Py_RETURN_NONE;
    }
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportPyStepRecord())
    {
        PyErr_SetString(PyExc_TypeError, "Report Step Record Failed");
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(ConfigDoc,
             "config(**kwargs)\n--\n\n"
             "Configure msmemscope module parameters.\n\n"
             "Args:\n"
             "    **kwargs: Configuration parameters as keyword arguments\n"
             "    - device: Device(s) to collect, npu | npu:<SLOT> | cpu\n"
             "    - level: Data trace level, op | kernel\n"
             "    - events: Trace event types, alloc | free | launch | access | traceback | none\n"
             "    - call_stack: C/Python call stack, e.g. c:10,python:5\n"
             "    - analysis: Analysis methods, leaks | decompose | inefficient | oom[:K] | "
             "host-leaks | none (comma-separated)\n"
             "      host-leaks is mutually exclusive with leaks/decompose/inefficient/oom and "
             "requires\n"
             "      'source msmemscope --load-api-env=host' beforehand\n"
             "    - host_leak_mode: Host leak report mode, event (default) | summary "
             "(with host-leaks)\n"
             "    - block_size_threshold: Only record host allocations with size >= N bytes "
             "(with host-leaks, default 0 = collect all)\n"
             "    - sample_rate: Explicit sampling, record 1/N of host allocations "
             "(with host-leaks, default 1 = no sampling)\n"
             "    - watch: Watch mode, start[:outid],end[,full-content]\n"
             "    - format: Output file format, csv | db\n"
             "    - output_path: Output directory [default: ./memscopeDumpResults]\n\n"
             "Examples:\n"
             "    msmemscope.config(call_stack=\"c:10,python:5\", level=\"op\", format=\"db\", "
             "output_path=\"./output\")\n"
             "    msmemscope.config(analysis=\"host-leaks\", host_leak_mode=\"summary\", "
             "block_size_threshold=\"1024\")");
static PyObject* MsmemscopeConfig(PyObject* self, PyObject* args, PyObject* kwargs)
{
    if (PyTuple_Size(args) > 0)
    {
        PyErr_SetString(PyExc_TypeError, "config() takes no positional arguments");
        return nullptr;
    }

    if (!kwargs || PyDict_Size(kwargs) == 0)
    {
        PyErr_SetString(PyExc_ValueError, "At least one keyword argument is required");
        return nullptr;
    }

    std::unordered_map<std::string, std::string> cpp_config;

    PyObject* key;
    PyObject* value;
    Py_ssize_t pos = 0;

    while (PyDict_Next(kwargs, &pos, &key, &value))
    {
        if (!PyUnicode_Check(key))
        {
            PyErr_SetString(PyExc_TypeError, "Keyword argument names must be strings");
            return nullptr;
        }

        const char* key_str = PyUnicode_AsUTF8(key);
        if (!key_str)
        {
            return nullptr;
        }

        // analysis等参数支持list形式,逐元素逗号拼接后
        // 走与字符串形式完全相同的解析路径
        if (PyList_Check(value))
        {
            std::string joined;
            const Py_ssize_t itemCount = PyList_Size(value);
            for (Py_ssize_t itemPos = 0; itemPos < itemCount; itemPos++)
            {
                PyObject* item = PyList_GetItem(value, itemPos);  // borrowed reference
                if (!PyUnicode_Check(item))
                {
                    PyErr_Format(PyExc_TypeError, "List items for argument '%s' must be strings", key_str);
                    return nullptr;
                }
                const char* item_str = PyUnicode_AsUTF8(item);
                if (!item_str)
                {
                    return nullptr;
                }
                if (!joined.empty())
                {
                    joined += ",";
                }
                joined += item_str;
            }
            cpp_config.emplace(key_str, joined);
            continue;
        }

        // 检查值是否为字符串类型（必须加引号）
        if (!PyUnicode_Check(value))
        {
            PyErr_Format(PyExc_TypeError, "Value for argument '%s' must be a string (use quotes)", key_str);
            return nullptr;
        }

        const char* value_str = PyUnicode_AsUTF8(value);
        if (!value_str)
        {
            return nullptr;
        }

        cpp_config.emplace(key_str, value_str);
    }

    // preload状态校验:analysis请求与当前进程preload状态
    // 必须一致,不一致报ValueError并提示需先source对应模式。校验放在SetConfig之前:
    // SetConfig经UpdateAnalysisType→set_enabled开窗,前置可保证校验自身的内存分配
    // 发生在钩子使能前,不被host钩子记账
    auto analysisItr = cpp_config.find("analysis");
    if (analysisItr != cpp_config.end())
    {
        bool hasHostLeaks = false;
        bool hasNpuAnalysis = false;
        SplitAnalysisTokens(analysisItr->second, hasHostLeaks, hasNpuAnalysis);
        if (hasHostLeaks && !IsSoLoaded(HOST_HOOK_SO_NAME) && !IsApiEnvMode("host"))
        {
            PyErr_SetString(PyExc_ValueError,
                            "analysis contains 'host-leaks' but the host memory hook is not preloaded; "
                            "run 'source msmemscope --load-api-env=host' in this shell before starting python");
            return nullptr;
        }
        if (hasNpuAnalysis && !IsNpuHookLoaded() && !IsApiEnvMode("npu"))
        {
            PyErr_SetString(PyExc_ValueError,
                            "analysis contains npu analysis methods (leaks/decompose/inefficient/oom) but "
                            "no npu hook is preloaded; run 'source msmemscope --load-api-env' in this shell "
                            "before starting python");
            return nullptr;
        }
    }

    bool ret = ConfigManager::Instance().SetConfig(cpp_config);
    if (!ret)
    {
        PyErr_SetString(PyExc_ValueError, "Set msmemscope trace config failed!");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(TakeSnapshotDoc,
             "take_snapshot(memory_info)\n--\n\n"
             "Take a memory snapshot and report it.\n\n"
             "Args:\n"
             "    memory_info: Memory information dictionary\n\n"
             "Examples:\n"
             "    msmemscope.take_snapshot({\"device\": 0, \"output\": \"/path/to/snapshot\", ...})");
static PyObject* MsmemscopeTakeSnapshot(PyObject* self, PyObject* args)
{
    PyObject* memory_info = nullptr;
    if (!PyArg_ParseTuple(args, "O", &memory_info))
    {
        PyErr_SetString(PyExc_TypeError, "Invalid argument: expected a dictionary");
        return nullptr;
    }

    if (!PyDict_Check(memory_info))
    {
        PyErr_SetString(PyExc_TypeError, "Invalid argument: expected a dictionary");
        return nullptr;
    }

    // 构建MemorySnapshotRecord结构体，使用PythonDictObject安全提取字典字段
    MemorySnapshotInfo info;
    Utility::PythonDictObject pyInfo(memory_info);

    info.device = pyInfo.GetItem("device", Utility::PythonObject(0)).Cast<int32_t>();
    info.memory_reserved = pyInfo.GetItem("memory_reserved", Utility::PythonObject(0)).Cast<uint64_t>();
    info.max_memory_reserved = pyInfo.GetItem("max_memory_reserved", Utility::PythonObject(0)).Cast<uint64_t>();
    info.memory_allocated = pyInfo.GetItem("memory_allocated", Utility::PythonObject(0)).Cast<uint64_t>();
    info.max_memory_allocated = pyInfo.GetItem("max_memory_allocated", Utility::PythonObject(0)).Cast<uint64_t>();
    info.total_memory = pyInfo.GetItem("total_memory", Utility::PythonObject(0)).Cast<uint64_t>();
    info.free_memory = pyInfo.GetItem("free_memory", Utility::PythonObject(0)).Cast<uint64_t>();
    info.name = pyInfo.GetItem("name", Utility::PythonObject("")).Cast<std::string>();

    // 检查是否是OOM快照，如果是则获取调用栈信息
    CallStackString stack;
    if (info.name.find("OOM") != std::string::npos || info.name.find("oom") != std::string::npos)
    {
        // OOM快照，从OOMHandler实例获取调用栈
        stack = OOMHandler::Instance().GetOOMStack();
    }

    // 传递参数给ReportMemorySnapshot，包含调用栈信息
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportMemorySnapshot(info, std::move(stack)))
    {
        PyErr_SetString(PyExc_TypeError, "Report Memory Snapshot Failed");
    }

    Py_RETURN_NONE;
}

PyDoc_STRVAR(EnableNpuSanitizerDoc,
             "_enable_npu_sanitizer()\n--\n\nEnable the NPU Sanitizer op handler flag.\n"
             "This is called internally by msmemscope.enable_npu_sanitizer() to notify the C++ layer\n"
             "that the sanitizer is active and sanitizer-op MSTX marks should be processed.");
static PyObject* MsmemscopeEnableNpuSanitizer(PyObject* self, PyObject* args)
{
    SanitizerOpHandler::SetEnabled(true);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(ReportCpuTensorDoc,
             "_report_cpu_tensor(ptr, size, is_alloc, py_stack)\n--\n\n"
             "Report a CPU tensor memory block alloc/free event.\n"
             "Returns True if the event is accepted, False if deduplicated.");
static PyObject* MsmemscopeReportCpuTensor(PyObject* self, PyObject* args)
{
    unsigned long long ptr = 0;
    long long size = 0;
    int isAlloc = 0;
    const char* pyStack = "";
    if (!PyArg_ParseTuple(args, "KLi|s", &ptr, &size, &isAlloc, &pyStack))
    {
        return nullptr;
    }
    bool accepted =
        EventReport::Instance(MemScopeCommType::SHARED_MEMORY)
            .ReportCpuTensor(ptr, static_cast<uint64_t>(size), isAlloc != 0, std::string(pyStack ? pyStack : ""));
    if (accepted)
    {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyMethodDef g_MsmemscopeMethods[] = {
    {"start", reinterpret_cast<PyCFunction>(MsmemscopeStart), METH_NOARGS, StartDoc},
    {"stop", reinterpret_cast<PyCFunction>(MsmemscopeStop), METH_NOARGS, StopDoc},
    {"step", reinterpret_cast<PyCFunction>(MsmemscopeStep), METH_NOARGS, StepDoc},
    {"config", reinterpret_cast<PyCFunction>(MsmemscopeConfig), METH_VARARGS | METH_KEYWORDS, ConfigDoc},
    {"_take_snapshot", reinterpret_cast<PyCFunction>(MsmemscopeTakeSnapshot), METH_VARARGS, TakeSnapshotDoc},
    {"_enable_npu_sanitizer", reinterpret_cast<PyCFunction>(MsmemscopeEnableNpuSanitizer), METH_NOARGS,
     EnableNpuSanitizerDoc},
    {"_report_cpu_tensor", reinterpret_cast<PyCFunction>(MsmemscopeReportCpuTensor), METH_VARARGS, ReportCpuTensorDoc},
    {nullptr, nullptr, 0, nullptr}};

static struct PyModuleDef g_MsmemscopeCModule = {
    PyModuleDef_HEAD_INIT,
    "_msmemscope",        /* m_name */
    MsmemscopeCModuleDoc, /* m_doc */
    -1,                   /* m_size */
    g_MsmemscopeMethods,  /* m_methods */
};

}  // namespace MemScope

PyMODINIT_FUNC PyInit__msmemscope(void)
{
    PyObject* m = PyModule_Create(&MemScope::g_MsmemscopeCModule);
    if (m == nullptr)
    {
        return nullptr;
    }

    std::vector<PyObject*> functions{
        MemScope::PyMemScope_GetWatcher(),
        MemScope::PyMemScope_GetTracer(),
        MemScope::PyMemScope_GetDescriber(),
        MemScope::PyMemScope_GetRecordFunction(),
    };
    std::vector<std::string> functionNames{
        "_watcher",
        "_tracer",
        "_describer",
        "_record_function",
    };

    for (size_t i = 0; i < functions.size(); i++)
    {
        if (functions[i] == nullptr)
        {
            Py_DECREF(m);
            return nullptr;
        }
        if (PyModule_AddObject(m, functionNames[i].c_str(), functions[i]) < 0)
        {
            std::string errorInfo = "Failed to bind " + functionNames[i];
            PyErr_SetString(PyExc_ImportError, errorInfo.c_str());
            Py_DECREF(functions[i]);
            Py_DECREF(m);
            return nullptr;
        }
    }

    return m;
}
