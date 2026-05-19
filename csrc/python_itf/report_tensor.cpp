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

#include "report_tensor.h"

#include <vector>

#include "event_report.h"
#include "cpython.h"
#include "log.h"
#include "utils.h"
#include "record_info.h"
#include "securec.h"

namespace MemScope {

static PyObject* PyMemScopeNewReportTensor(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    if (type == nullptr || type->tp_alloc == nullptr) {
        return nullptr;
    }

    static PyObject *self = nullptr;
    if (self == nullptr) {
        self = type->tp_alloc(type, 0);
    }

    Py_XINCREF(self);
    return self;
}

PyDoc_STRVAR(ReportTensorDoc,
"report_tensor($self, input_list)\n--\n\nEnable debug.");
static PyObject* PyMemScopeReportTensor(PyObject *self,  PyObject *arg)
{
    static int tupleSize = 2;
    PyObject* input_list;
    int i;
    int listSize;

    if (!PyList_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list");
        return nullptr;
    }

    input_list = arg;
    listSize = PyList_Size(input_list);

    for (i = 0; i < listSize; i++) {
        PyObject* item = PyList_GetItem(input_list, i);
        uint64_t addr;
        const char *owner;
        if (!PyTuple_Check(item) || PyTuple_Size(item) != tupleSize) {
            PyErr_SetString(PyExc_TypeError, "Each item must be a tuple of (int, str)");
            return nullptr;
        }
        if (!PyArg_ParseTuple(item, "Ls", &addr, &owner)) {
            PyErr_SetString(PyExc_TypeError, "Tuple elements must be (int, str)");
            return nullptr;
        }

        if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAddrInfo(EventSubType::TORCH_OPTIMIZER_STEP_OWNER, addr, owner)) {
            LOG_ERROR("Report optimizer step hook info failed.\n");
        }
    }

    Py_RETURN_NONE;
}

static PyMethodDef PyMemScopeReportTensorMethods[] = {
    {"report_tensor", reinterpret_cast<PyCFunction>(PyMemScopeReportTensor), METH_O, ReportTensorDoc},
    {nullptr, nullptr, 0, nullptr}
};


static PyTypeObject PyMemScopeReportTensorType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "_msmemscope._report_tensor",                        /* tp_name */
    0,                                                /* tp_basicsize */
    0,                                                /* tp_itemsize */
    /* methods */
    nullptr,                                          /* tp_dealloc */
    0,                                                /* tp_vectorcall_offset */
    nullptr,                                          /* tp_getattr */
    nullptr,                                          /* tp_setattr */
    nullptr,                                          /* tp_as_async */
    nullptr,                                          /* tp_repr */
    nullptr,                                          /* tp_as_number */
    nullptr,                                          /* tp_as_sequence */
    nullptr,                                          /* tp_as_mapping */
    nullptr,                                          /* tp_hash */
    nullptr,                                          /* tp_call */
    nullptr,                                          /* tp_str */
    nullptr,                                          /* tp_getattro */
    nullptr,                                          /* tp_setattro */
    nullptr,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                               /* tp_flags */
    nullptr,                                          /* tp_doc */
    nullptr,                                          /* tp_traverse */
    nullptr,                                          /* tp_clear */
    nullptr,                                          /* tp_richcompare */
    0,                                                /* tp_weaklistoffset */
    nullptr,                                          /* tp_iter */
    nullptr,                                          /* tp_iternext */
    PyMemScopeReportTensorMethods,                       /* tp_methods */
    nullptr,                                          /* tp_members */
    nullptr,                                          /* tp_getset */
    &PyBaseObject_Type,                               /* tp_base */
    nullptr,                                          /* tp_dict */
    nullptr,                                          /* tp_descr_get */
    nullptr,                                          /* tp_descr_set */
    0,                                                /* tp_dictoffset */
    nullptr,                                          /* tp_init */
    nullptr,                                          /* tp_alloc */
    PyMemScopeNewReportTensor,                           /* tp_new */
    PyObject_Del,                                     /* tp_free */
};

PyObject* PyMemScope_GetReportTensor()
{
    if (PyType_Ready(&PyMemScopeReportTensorType) < 0) {
        return nullptr;
    }

    return PyObject_New(PyObject, &PyMemScopeReportTensorType);
}
}