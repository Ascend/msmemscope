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

#include "watcherobject.h"

#include "event_trace/memory_watch/tensor_dumper.h"
#include "event_trace/memory_watch/tensor_monitor.h"

namespace MemScope
{

const size_t MAX_WATCH_NAME_LENGTH = 64;

/* 单例类，自定义new函数，避免重复构造 */
static PyObject* PyMemScopeNewWatcher(PyTypeObject* type, PyObject* args, PyObject* kwds)
{
    if (type == nullptr || type->tp_alloc == nullptr)
    {
        return nullptr;
    }

    /* 单例,减少重复构造 */
    static PyObject* self = nullptr;
    if (self == nullptr)
    {
        self = type->tp_alloc(type, 0);
    }

    Py_XINCREF(self);
    return self;
}

bool IsTorchTensor(PyObject* obj)
{
    PyObject* cls = PyObject_GetAttrString(obj, "__class__");
    if (!cls)
    {
        return false;
    }

    PyObject* module = PyObject_GetAttrString(cls, "__module__");
    PyObject* name = PyObject_GetAttrString(cls, "__name__");
    if (!module || !name)
    {
        Py_XDECREF(cls);
        Py_XDECREF(module);
        Py_XDECREF(name);
        return false;
    }

    const char* module_cstr = PyUnicode_AsUTF8(module);
    const char* name_cstr = PyUnicode_AsUTF8(name);
    std::string moduleStr;
    std::string nameStr;

    if (module_cstr != nullptr)
    {
        moduleStr = std::string(module_cstr);
    }
    if (name_cstr != nullptr)
    {
        nameStr = std::string(name_cstr);
    }

    bool isTensor = (!moduleStr.empty() && !nameStr.empty() && moduleStr == "torch" && nameStr == "Tensor");

    Py_DECREF(cls);
    Py_DECREF(module);
    Py_DECREF(name);
    return isTensor;
}

bool ParseTensorPtrAndSize(PyObject* tensor, void** ptr, uint64_t& length)
{
    // 调用python侧的方法获取tensor的ptr和size
    PyObject* ptrObj = PyObject_CallMethod(tensor, "data_ptr", nullptr);
    PyObject* lengthObj = PyObject_GetAttrString(tensor, "nbytes");
    if (!ptrObj || !lengthObj)
    {
        Py_XDECREF(ptrObj);
        Py_XDECREF(lengthObj);
        return false;
    }

    *ptr = reinterpret_cast<void*>((std::uintptr_t)PyLong_AsUnsignedLongLong(ptrObj));
    if (PyErr_Occurred())
    {
        PyErr_SetString(PyExc_TypeError, "Parse tensor addr failed!");
        Py_XDECREF(ptrObj);
        Py_XDECREF(lengthObj);
        return false;
    }
    length = static_cast<uint64_t>(PyLong_AsUnsignedLongLong(lengthObj));
    if (PyErr_Occurred())
    {
        PyErr_SetString(PyExc_TypeError, "Parse tensor length failed!");
        Py_XDECREF(ptrObj);
        Py_XDECREF(lengthObj);
        return false;
    }

    Py_DECREF(ptrObj);
    Py_DECREF(lengthObj);
    return true;
}

bool ParseInputArgs(PyObject* args, MonitoredTensor& tensorInfo, PyObject* lengthObj, uint64_t length)
{
    // 解析tensor或者addr+size的传入方式
    void* ptr = nullptr;
    PyObject* tensorOrAddrObject = nullptr;
    int8_t argsCount = PyTuple_Size(args);
    if (argsCount != 1)
    {
        PyErr_SetString(PyExc_TypeError, "Only one parameter without keywords can be input.");
        return false;
    }
    tensorOrAddrObject = PyTuple_GetItem(args, 0);
    if (!lengthObj)
    {  // 传入的是tensor
        if (!IsTorchTensor(tensorOrAddrObject))
        {
            PyErr_SetString(PyExc_TypeError, "Expected a tensor as args.");
            return false;
        }
        if (!ParseTensorPtrAndSize(tensorOrAddrObject, &ptr, length))
        {
            return false;
        }
    }
    else
    {  // 传入的是addr + size
        if (!PyLong_Check(tensorOrAddrObject))
        {
            PyErr_SetString(PyExc_TypeError, "Expected addr:int as args when length is set.");
            return false;
        }
        ptr = reinterpret_cast<void*>((std::uintptr_t)PyLong_AsUnsignedLongLong(tensorOrAddrObject));
        if (PyErr_Occurred())
        {
            PyErr_SetString(PyExc_TypeError, "The addr parameter is invalid.");
            return false;
        }
        length = static_cast<uint64_t>(PyLong_AsUnsignedLongLong(lengthObj));
        if (length == 0 || PyErr_Occurred())
        {
            PyErr_SetString(PyExc_TypeError, "The length parameter is invalid.");
            return false;
        }
    }
    tensorInfo.data = ptr;
    tensorInfo.dataSize = length;
    return true;
}

PyDoc_STRVAR(WatchDoc, "watch($self, tensor or addr+size)\n--\n\nAdd Monitor.");
static PyObject* PyMemScopeWatcherWatch(PyObject* self, PyObject* args, PyObject* kwds)
{
    const char* name = nullptr;
    int32_t dumpNums = -1;
    uint64_t length = 0;
    PyObject* lengthObj = nullptr;
    // 先解析关键字参数
    if (kwds != nullptr)
    {
        PyObject* nameObj = PyDict_GetItemString(kwds, "name");
        if (!nameObj)
        {
            PyErr_SetString(PyExc_TypeError, "The name parameter must be set.");
            Py_RETURN_NONE;
        }
        name = PyUnicode_AsUTF8(nameObj);
        if (name == nullptr)
        {
            PyErr_SetString(PyExc_TypeError, "Parse name failed!");
            Py_RETURN_NONE;
        }
        if (std::strlen(name) > MAX_WATCH_NAME_LENGTH)
        {
            PyErr_Format(PyExc_ValueError, "Input name exceeds maximum allowed length %zu.", MAX_WATCH_NAME_LENGTH);
            Py_RETURN_NONE;
        }
        PyObject* dumpNumsObj = PyDict_GetItemString(kwds, "dump_nums");
        if (dumpNumsObj)
        {
            dumpNums = static_cast<int32_t>(PyLong_AsLong(dumpNumsObj));
            if (PyErr_Occurred())
            {
                PyErr_SetString(PyExc_TypeError, "Parse dump_nums failed!");
                Py_RETURN_NONE;
            }
        }
        lengthObj = PyDict_GetItemString(kwds, "length");
    }
    else
    {
        PyErr_SetString(PyExc_TypeError, "At least one name keyword parameter must be entered.");
        Py_RETURN_NONE;
    }
    MonitoredTensor tensorInfo{};
    if (!ParseInputArgs(args, tensorInfo, lengthObj, length))
    {
        Py_RETURN_NONE;
    }
    uint64_t ptr = static_cast<uint64_t>((std::uintptr_t)(tensorInfo.data));
    TensorDumper::GetInstance().SetDumpNums(ptr, dumpNums);
    TensorDumper::GetInstance().SetDumpName(ptr, std::string(name));
    TensorMonitor::GetInstance().AddWatchTensor(tensorInfo);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(RemoveDoc, "remove($self, tensor or addr+size)\n--\n\nRemove Monitor.");
static PyObject* PyMemScopeWatcherRemove(PyObject* self, PyObject* args, PyObject* kwds)
{
    uint64_t length = 0;
    PyObject* lengthObj = nullptr;
    if (kwds != nullptr)
    {
        lengthObj = PyDict_GetItemString(kwds, "length");
    }
    MonitoredTensor tensorInfo{};
    if (!ParseInputArgs(args, tensorInfo, lengthObj, length))
    {
        Py_RETURN_NONE;
    }

    uint64_t ptr = static_cast<uint64_t>((std::uintptr_t)(tensorInfo.data));
    TensorDumper::GetInstance().DeleteDumpNums(ptr);
    TensorDumper::GetInstance().DeleteDumpName(ptr);
    TensorMonitor::GetInstance().DeleteWatchTensor(tensorInfo);
    Py_RETURN_NONE;
}

static PyMethodDef PyMemScopeWatcherMethods[] = {
    {"watch", reinterpret_cast<PyCFunction>(PyMemScopeWatcherWatch), METH_VARARGS | METH_KEYWORDS, WatchDoc},
    {"remove", reinterpret_cast<PyCFunction>(PyMemScopeWatcherRemove), METH_VARARGS | METH_KEYWORDS, RemoveDoc},
    {nullptr, nullptr, 0, nullptr}};

static PyTypeObject PyMemScopeWatcherType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "_msmemscope._watcher", /* tp_name */
    0,                                                             /* tp_basicsize */
    0,                                                             /* tp_itemsize */
    /* methods */
    nullptr,                  /* tp_dealloc */
    0,                        /* tp_vectorcall_offset */
    nullptr,                  /* tp_getattr */
    nullptr,                  /* tp_setattr */
    nullptr,                  /* tp_as_async */
    nullptr,                  /* tp_repr */
    nullptr,                  /* tp_as_number */
    nullptr,                  /* tp_as_sequence */
    nullptr,                  /* tp_as_mapping */
    nullptr,                  /* tp_hash */
    nullptr,                  /* tp_call */
    nullptr,                  /* tp_str */
    nullptr,                  /* tp_getattro */
    nullptr,                  /* tp_setattro */
    nullptr,                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,       /* tp_flags */
    nullptr,                  /* tp_doc */
    nullptr,                  /* tp_traverse */
    nullptr,                  /* tp_clear */
    nullptr,                  /* tp_richcompare */
    0,                        /* tp_weaklistoffset */
    nullptr,                  /* tp_iter */
    nullptr,                  /* tp_iternext */
    PyMemScopeWatcherMethods, /* tp_methods */
    nullptr,                  /* tp_members */
    nullptr,                  /* tp_getset */
    &PyBaseObject_Type,       /* tp_base */
    nullptr,                  /* tp_dict */
    nullptr,                  /* tp_descr_get */
    nullptr,                  /* tp_descr_set */
    0,                        /* tp_dictoffset */
    nullptr,                  /* tp_init */
    nullptr,                  /* tp_alloc */
    PyMemScopeNewWatcher,     /* tp_new */
    PyObject_Del,             /* tp_free */
};

PyObject* PyMemScope_GetWatcher()
{
    if (PyType_Ready(&PyMemScopeWatcherType) < 0)
    {
        return nullptr;
    }

    return PyObject_New(PyObject, &PyMemScopeWatcherType);
}
}  // namespace MemScope
