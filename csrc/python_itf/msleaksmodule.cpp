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
#include <vector>
#include <string>
#include <unordered_map>
#include <cstring>
#include "watcherobject.h"
#include "tracerobject.h"
#include "describerobject.h"
#include "report_tensor.h"
#include "recordfuncobject.h"
#include "event_report.h"
#include "oom_handler.h"
#include "trace_manager/event_trace_manager.h"
#include "call_stack.h"

namespace MemScope {

PyDoc_STRVAR(MsmemscopeCModuleDoc,
"The part of the module msmemscope that is implemented in CXX.\n\
 \n\
...");

PyDoc_STRVAR(StartDoc,
"start()\n--\n\nstart trace data.");
static PyObject* MsmemscopeStart(PyObject* self, PyObject* args)
{
    ConfigManager::Instance().InitStartConfig();
    Py_RETURN_NONE;
}

PyDoc_STRVAR(StopDoc,
"stop()\n--\n\nstop trace data.");
static PyObject* MsmemscopeStop(PyObject* self, PyObject* args)
{
    EventTraceManager::Instance().SetTraceStatus(EventTraceStatus::NOT_IN_TRACING);
    EventTraceManager::Instance().CleanUpEventTraceManager();
    Py_RETURN_NONE;
}

PyDoc_STRVAR(StepDoc,
"step()\n--\n\nmark step info.");
static PyObject* MsmemscopeStep(PyObject* self, PyObject* args)
{
    if (!EventTraceManager::Instance().IsTracingEnabled()) {
        Py_RETURN_NONE;
    }
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportPyStepRecord()) {
        PyErr_SetString(PyExc_TypeError, "Report Step Record Failed");
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(ConfigDoc,
"config(**kwargs)\n--\n\n"
"Configure msmemscope module parameters.\n\n"
"Args:\n"
"    **kwargs: Configuration parameters as keyword arguments\n\n"
"Examples:\n"
"    msmemscope.config(call_stack=\"c:10,python:5\", level='0,1')");
static PyObject* MsmemscopeConfig(PyObject* self, PyObject* args, PyObject* kwargs)
{
    if (PyTuple_Size(args) > 0) {
        PyErr_SetString(PyExc_TypeError, "config() takes no positional arguments");
        return nullptr;
    }
    
    if (!kwargs || PyDict_Size(kwargs) == 0) {
        PyErr_SetString(PyExc_ValueError, "At least one keyword argument is required");
        return nullptr;
    }
    
    std::unordered_map<std::string, std::string> cpp_config;

    PyObject *key;
    PyObject *value;
    Py_ssize_t pos = 0;

    while (PyDict_Next(kwargs, &pos, &key, &value)) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "Keyword argument names must be strings");
            return nullptr;
        }

        const char* key_str = PyUnicode_AsUTF8(key);
        if (!key_str) {
            return nullptr;
        }

        // 检查值是否为字符串类型（必须加引号）
        if (!PyUnicode_Check(value)) {
            PyErr_Format(PyExc_TypeError, "Value for argument '%s' must be a string (use quotes)", key_str);
            return nullptr;
        }

        const char* value_str = PyUnicode_AsUTF8(value);
        if (!value_str) {
            return nullptr;
        }

        cpp_config.emplace(key_str, value_str);
    }

    bool ret = ConfigManager::Instance().SetConfig(cpp_config);
    if (!ret) {
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
    if (!PyArg_ParseTuple(args, "O", &memory_info)) {
        PyErr_SetString(PyExc_TypeError, "Invalid argument: expected a dictionary");
        return nullptr;
    }
    
    if (!PyDict_Check(memory_info)) {
        PyErr_SetString(PyExc_TypeError, "Invalid argument: expected a dictionary");
        return nullptr;
    }
    
    // 构建MemorySnapshotRecord结构体
    MemorySnapshotInfo info;
    
    // 从memory_info字典中提取信息
    info.device = PyLong_AsLong(PyDict_GetItemString(memory_info, "device"));
    info.memory_reserved = PyLong_AsUnsignedLongLong(PyDict_GetItemString(memory_info, "memory_reserved"));
    info.max_memory_reserved = PyLong_AsUnsignedLongLong(PyDict_GetItemString(memory_info, "max_memory_reserved"));
    info.memory_allocated = PyLong_AsUnsignedLongLong(PyDict_GetItemString(memory_info, "memory_allocated"));
    info.max_memory_allocated = PyLong_AsUnsignedLongLong(PyDict_GetItemString(memory_info, "max_memory_allocated"));
    info.total_memory = PyLong_AsUnsignedLongLong(PyDict_GetItemString(memory_info, "total_memory"));
    info.free_memory = PyLong_AsUnsignedLongLong(PyDict_GetItemString(memory_info, "free_memory"));
    info.name = std::string(PyUnicode_AsUTF8(PyDict_GetItemString(memory_info, "name")));
    
    // 检查是否是OOM快照，如果是则获取调用栈信息
    CallStackString stack;
    if (info.name.find("OOM") != std::string::npos || info.name.find("oom") != std::string::npos) {
        // OOM快照，从OOMHandler实例获取调用栈
        stack = OOMHandler::Instance().GetOOMStack();
    }

    // 传递参数给ReportMemorySnapshot，包含调用栈信息
    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportMemorySnapshot(info, std::move(stack))) {
        PyErr_SetString(PyExc_TypeError, "Report Memory Snapshot Failed");
    }
    
    Py_RETURN_NONE;
}

static PyMethodDef g_MsmemscopeMethods[] = {
    {"start", reinterpret_cast<PyCFunction>(MsmemscopeStart), METH_NOARGS, StartDoc},
    {"stop", reinterpret_cast<PyCFunction>(MsmemscopeStop), METH_NOARGS, StopDoc},
    {"step", reinterpret_cast<PyCFunction>(MsmemscopeStep), METH_NOARGS, StepDoc},
    {"config", reinterpret_cast<PyCFunction>(MsmemscopeConfig), METH_VARARGS | METH_KEYWORDS, ConfigDoc},
    {"_take_snapshot", reinterpret_cast<PyCFunction>(MsmemscopeTakeSnapshot), METH_VARARGS, TakeSnapshotDoc},
    {nullptr, nullptr, 0, nullptr}
};

static struct PyModuleDef g_MsmemscopeCModule = {
    PyModuleDef_HEAD_INIT,
    "_msmemscope",                   /* m_name */
    MsmemscopeCModuleDoc,            /* m_doc */
    -1,                           /* m_size */
    g_MsmemscopeMethods,             /* m_methods */
};

}

PyMODINIT_FUNC PyInit__msmemscope(void)
{
    PyObject* m = PyModule_Create(&MemScope::g_MsmemscopeCModule);
    if (m == nullptr) {
        return nullptr;
    }

    std::vector<PyObject*> functions{
        MemScope::PyMemScope_GetWatcher(),
        MemScope::PyMemScope_GetTracer(),
        MemScope::PyMemScope_GetDescriber(),
        MemScope::PyMemScope_GetReportTensor(),
        MemScope::PyMemScope_GetRecordFunction(),
    };
    std::vector<std::string> functionNames{
        "_watcher",
        "_tracer",
        "_describer",
        "_report_tensor",
        "_record_function",
    };

    for (size_t i = 0; i < functions.size(); i++) {
        if (functions[i] == nullptr) {
            Py_DECREF(m);
            return nullptr;
        }
        if (PyModule_AddObject(m, functionNames[i].c_str(), functions[i]) < 0) {
            std::string errorInfo = "Failed to bind " + functionNames[i];
            PyErr_SetString(PyExc_ImportError, errorInfo.c_str());
            Py_DECREF(functions[i]);
            Py_DECREF(m);
            return nullptr;
        }
    }

    return m;
}