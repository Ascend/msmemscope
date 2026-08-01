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

PyDoc_STRVAR(DescribeDoc, "describe($self, owner)\n--\n\nEnable debug.");
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
    DescribeTrace::GetInstance().AddDescribe(str);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(UnDescribeDoc, "undescribe($self, owner)\n--\n\nEnable debug.");
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
    DescribeTrace::GetInstance().EraseDescribe(str);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(DescribeAddrDoc, "describe_addr($self, addr, owner)\n--\n\nEnable debug.");
static PyObject *PyMemScopeDescribeAddr(PyObject *self, PyObject *args)
{
    PyObject *addrObj = nullptr;
    const char *str = nullptr;

    if (!PyArg_ParseTuple(args, "Os", &addrObj, &str))
    {
        return nullptr;
    }
    if (std::strlen(str) > MAX_DESCRIBE_OWNER_LENGTH)
    {
        PyErr_Format(PyExc_ValueError, "Input owner exceeds maximum allowed length %zu.", MAX_DESCRIBE_OWNER_LENGTH);
        return NULL;
    }

    uint64_t addr = static_cast<uint64_t>(PyLong_AsUnsignedLongLong(addrObj));
    if (PyErr_Occurred())
    {
        PyErr_SetString(PyExc_TypeError, "Parse tensor length failed!");
        return NULL;
    }

    DescribeTrace::GetInstance().DescribeAddr(addr, std::string(str));
    Py_RETURN_NONE;
}

static PyMethodDef PyMemScopeDescriberMethods[] = {
    {"describe_addr", reinterpret_cast<PyCFunction>(PyMemScopeDescribeAddr), METH_VARARGS, DescribeAddrDoc},
    {"describe", reinterpret_cast<PyCFunction>(PyMemScopeDescribe), METH_O, DescribeDoc},
    {"undescribe", reinterpret_cast<PyCFunction>(PyMemScopeUnDescribe), METH_O, UnDescribeDoc},
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
