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

#include "describerobject.h"

#include "cpython.h"
#include "describe_trace.h"
#include "utils.h"

namespace MemScope
{

const size_t MAX_DESCRIBE_OWNER_LENGTH = 128;

/* 单例类，自定义new函数，避免重复构造 */
static PyObject *PyMemScopeNewDescriber(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    if (type == nullptr || type->tp_alloc == nullptr)
    {
        return nullptr;
    }

    /* 单例,减少重复构造 */
    static PyObject *self = nullptr;
    if (self == nullptr)
    {
        self = type->tp_alloc(type, 0);
    }

    Py_XINCREF(self);
    return self;
}

// 解析 Python 分级标签列表: [(label:str, level:int), ...] → vector<pair<OwnerLevel, string>>
// 顺序约定与 Python 侧统一: 标签在前、级别在后(见 describe.py 内部接口)
static bool ParseOwnerLabels(PyObject *labelsObj, std::vector<std::pair<OwnerLevel, std::string>> &labels)
{
    if (!PyList_Check(labelsObj))
    {
        PyErr_SetString(PyExc_TypeError, "Labels must be a list of (label, level) tuples");
        return false;
    }
    Py_ssize_t listSize = PyList_Size(labelsObj);
    for (Py_ssize_t i = 0; i < listSize; ++i)
    {
        PyObject *item = PyList_GetItem(labelsObj, i);
        if (!PyTuple_Check(item) || PyTuple_Size(item) != 2)
        {
            PyErr_SetString(PyExc_TypeError, "Each label must be a (label, level) tuple");
            return false;
        }
        PyObject *labelObj = PyTuple_GetItem(item, 0);
        PyObject *levelObj = PyTuple_GetItem(item, 1);
        if (!PyUnicode_Check(labelObj) || !PyLong_Check(levelObj))
        {
            PyErr_SetString(PyExc_TypeError, "Label tuple must be (str label, int level)");
            return false;
        }
        long level = PyLong_AsLong(levelObj);
        if (level < 0 || level >= static_cast<long>(OwnerLevel::OWNER_LEVEL_NUM))
        {
            PyErr_SetString(PyExc_ValueError, "Input owner level out of range");
            return false;
        }
        std::string label = Utility::PythonObject(labelObj).Cast<std::string>();
        if (label.size() > MAX_DESCRIBE_OWNER_LENGTH)
        {
            PyErr_Format(PyExc_ValueError, "Input owner label exceeds maximum allowed length %zu.",
                         MAX_DESCRIBE_OWNER_LENGTH);
            return false;
        }
        labels.emplace_back(static_cast<OwnerLevel>(level), label);
    }
    return true;
}

// 用户接口: 用户标签(无级别, 数据区2)
PyDoc_STRVAR(DescribeDoc, "describe($self, owner)\n--\n\nEnable debug. User label (no level).");
static PyObject *PyMemScopeDescribe(PyObject *self, PyObject *arg)
{
    if (!PyUnicode_Check(arg))
    {
        PyErr_SetString(PyExc_TypeError, "Expected a string argument");
        return nullptr;
    }
    std::string str = Utility::PythonObject(arg).Cast<std::string>();
    if (str.size() > MAX_DESCRIBE_OWNER_LENGTH)
    {
        PyErr_Format(PyExc_ValueError, "Input owner exceeds maximum allowed length %zu.", MAX_DESCRIBE_OWNER_LENGTH);
        return NULL;
    }
    DescribeTrace::GetInstance().AddUserDescribe(str);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(UnDescribeDoc, "undescribe($self, owner)\n--\n\nEnable debug. User label (no level).");
static PyObject *PyMemScopeUnDescribe(PyObject *self, PyObject *arg)
{
    if (!PyUnicode_Check(arg))
    {
        PyErr_SetString(PyExc_TypeError, "Expected a string argument");
        return nullptr;
    }
    std::string str = Utility::PythonObject(arg).Cast<std::string>();
    if (str.size() > MAX_DESCRIBE_OWNER_LENGTH)
    {
        PyErr_Format(PyExc_ValueError, "Input owner exceeds maximum allowed length %zu.", MAX_DESCRIBE_OWNER_LENGTH);
        return NULL;
    }
    DescribeTrace::GetInstance().EraseUserDescribe(str);
    Py_RETURN_NONE;
}

// 内部接口: 系统标签(带级别, 数据区1), 供框架钩子使用
PyDoc_STRVAR(DescribeLabelDoc,
             "describe_label($self, label, level)\n--\n\nEnable debug. Internal system label with level.");
static PyObject *PyMemScopeDescribeLabel(PyObject *self, PyObject *args)
{
    const char *label = nullptr;
    int level = 0;
    if (!PyArg_ParseTuple(args, "si", &label, &level))
    {
        return nullptr;
    }
    if (std::strlen(label) > MAX_DESCRIBE_OWNER_LENGTH)
    {
        PyErr_Format(PyExc_ValueError, "Input owner label exceeds maximum allowed length %zu.",
                     MAX_DESCRIBE_OWNER_LENGTH);
        return NULL;
    }
    if (level < 0 || level >= static_cast<int>(OwnerLevel::OWNER_LEVEL_NUM))
    {
        PyErr_SetString(PyExc_ValueError, "Input owner level out of range");
        return NULL;
    }
    DescribeTrace::GetInstance().AddDescribe(static_cast<OwnerLevel>(level), std::string(label));
    Py_RETURN_NONE;
}

PyDoc_STRVAR(UnDescribeLabelDoc,
             "undescribe_label($self, label, level)\n--\n\nEnable debug. Internal system label with level.");
static PyObject *PyMemScopeUnDescribeLabel(PyObject *self, PyObject *args)
{
    const char *label = nullptr;
    int level = 0;
    if (!PyArg_ParseTuple(args, "si", &label, &level))
    {
        return nullptr;
    }
    if (std::strlen(label) > MAX_DESCRIBE_OWNER_LENGTH)
    {
        PyErr_Format(PyExc_ValueError, "Input owner label exceeds maximum allowed length %zu.",
                     MAX_DESCRIBE_OWNER_LENGTH);
        return NULL;
    }
    if (level < 0 || level >= static_cast<int>(OwnerLevel::OWNER_LEVEL_NUM))
    {
        PyErr_SetString(PyExc_ValueError, "Input owner level out of range");
        return NULL;
    }
    DescribeTrace::GetInstance().EraseDescribe(static_cast<OwnerLevel>(level), std::string(label));
    Py_RETURN_NONE;
}

PyDoc_STRVAR(DescribeAddrDoc,
             "describe_addr($self, addr, labels)\n--\n\nEnable debug. labels: [(label:str, level:int), ...]");
static PyObject *PyMemScopeDescribeAddr(PyObject *self, PyObject *args)
{
    PyObject *addrObj = nullptr;
    PyObject *labelsObj = nullptr;

    if (!PyArg_ParseTuple(args, "OO", &addrObj, &labelsObj))
    {
        return nullptr;
    }

    uint64_t addr = static_cast<uint64_t>(PyLong_AsUnsignedLongLong(addrObj));
    if (PyErr_Occurred())
    {
        PyErr_SetString(PyExc_TypeError, "Parse tensor address failed!");
        return NULL;
    }

    std::vector<std::pair<OwnerLevel, std::string>> labels;
    if (!ParseOwnerLabels(labelsObj, labels))
    {
        return nullptr;
    }
    DescribeTrace::GetInstance().DescribeAddr(addr, labels);
    Py_RETURN_NONE;
}

static PyMethodDef PyMemScopeDescriberMethods[] = {
    {"describe_addr", reinterpret_cast<PyCFunction>(PyMemScopeDescribeAddr), METH_VARARGS, DescribeAddrDoc},
    {"describe", reinterpret_cast<PyCFunction>(PyMemScopeDescribe), METH_O, DescribeDoc},
    {"undescribe", reinterpret_cast<PyCFunction>(PyMemScopeUnDescribe), METH_O, UnDescribeDoc},
    {"describe_label", reinterpret_cast<PyCFunction>(PyMemScopeDescribeLabel), METH_VARARGS, DescribeLabelDoc},
    {"undescribe_label", reinterpret_cast<PyCFunction>(PyMemScopeUnDescribeLabel), METH_VARARGS, UnDescribeLabelDoc},
    {nullptr, nullptr, 0, nullptr}};

static PyTypeObject PyMemScopeDescriberType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "_msmemscope._describer", /* tp_name */
    0,                                                               /* tp_basicsize */
    0,                                                               /* tp_itemsize */
    /* methods */
    nullptr,                    /* tp_dealloc */
    0,                          /* tp_vectorcall_offset */
    nullptr,                    /* tp_getattr */
    nullptr,                    /* tp_setattr */
    nullptr,                    /* tp_as_async */
    nullptr,                    /* tp_repr */
    nullptr,                    /* tp_as_number */
    nullptr,                    /* tp_as_sequence */
    nullptr,                    /* tp_as_mapping */
    nullptr,                    /* tp_hash */
    nullptr,                    /* tp_call */
    nullptr,                    /* tp_str */
    nullptr,                    /* tp_getattro */
    nullptr,                    /* tp_setattro */
    nullptr,                    /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,         /* tp_flags */
    nullptr,                    /* tp_doc */
    nullptr,                    /* tp_traverse */
    nullptr,                    /* tp_clear */
    nullptr,                    /* tp_richcompare */
    0,                          /* tp_weaklistoffset */
    nullptr,                    /* tp_iter */
    nullptr,                    /* tp_iternext */
    PyMemScopeDescriberMethods, /* tp_methods */
    nullptr,                    /* tp_members */
    nullptr,                    /* tp_getset */
    &PyBaseObject_Type,         /* tp_base */
    nullptr,                    /* tp_dict */
    nullptr,                    /* tp_descr_get */
    nullptr,                    /* tp_descr_set */
    0,                          /* tp_dictoffset */
    nullptr,                    /* tp_init */
    nullptr,                    /* tp_alloc */
    PyMemScopeNewDescriber,     /* tp_new */
    PyObject_Del,               /* tp_free */
};

PyObject *PyMemScope_GetDescriber()
{
    if (PyType_Ready(&PyMemScopeDescriberType) < 0)
    {
        return nullptr;
    }

    return PyObject_New(PyObject, &PyMemScopeDescriberType);
}
}  // namespace MemScope
