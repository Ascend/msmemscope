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

#pragma once

#include <Python.h>
#include <frameobject.h>

#include <functional>
#include <map>

#include "record_info.h"
#include "ustring.h"

namespace Utility
{

using TraceCbFunc = std::function<void(std::string, std::string, MemScope::PyTraceType, uint64_t)>;
void RegisterTraceCb(TraceCbFunc call);
void UnRegisterTraceCb();
bool IsPyInterpRepeInited();
void PythonCallstack(uint32_t pyDepth, std::string& pyStack);
Version GetPyVersion();
void GetPyFuncInfo(PyFrameObject* frame, std::string& info, std::string& hash);
bool IsIgnoreCFunc(std::string hash);
int pyProfileFn(PyObject* obj, PyFrameObject* frame, int what, PyObject* arg);
void GetTraceCallStack(std::string type, uint64_t time);

class PythonObject
{
   public:
    PythonObject();
    ~PythonObject();
    PythonObject(const PythonObject& obj);
    PythonObject& operator=(const PythonObject& obj);
    explicit PythonObject(PyObject* o);
    explicit PythonObject(const PyObject* o);
    explicit PythonObject(const int32_t& input);
    explicit PythonObject(const uint32_t& input);
    explicit PythonObject(const double& input);
    explicit PythonObject(const std::string& input);
    explicit PythonObject(const char* input);
    explicit PythonObject(const bool& input);
    template <typename T>
    explicit PythonObject(const std::vector<T>& input);
    template <typename T1, typename T2>
    explicit PythonObject(const std::map<T1, T2>& input);
    PythonObject& NewRef();

    template <typename T>
    T Cast();
    operator PyObject*() const { return ptr; }

    /* 获取模块对象 */
    static PythonObject Import(const std::string& name, bool fromcache = false, bool ignore = true);
    static PythonObject GetGlobal(const std::string& name, bool ignore = true);
    /* 用于获取对象属性，相当于python代码中的obj.xx */
    PythonObject Get(const std::string& name, bool ignore = true) const;
    /* 用于获取容器内对象，相当于python代码中的obj[xx] */
    PythonObject GetItem(const PythonObject& index, bool ignore = true) const;
    PythonObject operator[](const PythonObject& index) const { return GetItem(index, true); }

    /* 用于调用可调用对象，相当于python代码中的obj()，为了简单只实现了args+kwargs参数形式 */
    /* args需要是PythonTupleObject类型，kwargs需要是PythonDictObject */
    PythonObject Call(bool ignore = true);
    PythonObject operator()(bool ignore = true) { return Call(ignore); }
    PythonObject Call(PythonObject& args, bool ignore = true);
    PythonObject operator()(PythonObject& args, bool ignore = true) { return Call(args, ignore); }
    PythonObject Call(PythonObject& args, PythonObject& kwargs, bool ignore = true);
    PythonObject operator()(PythonObject& args, PythonObject& kwargs, bool ignore = true)
    {
        return Call(args, kwargs, ignore);
    }

    bool IsBad() const { return ptr == nullptr; }
    bool IsInstance(const std::string& type) const;
    bool IsInstance(const PythonObject& type) const;
    bool IsCallable() const;
    std::string Type() const;

   protected:
    void SetPtr(PyObject* o);
    PyObject* ptr{nullptr};
};

class PythonNumberObject : public PythonObject
{
   public:
    PythonNumberObject();
    explicit PythonNumberObject(PyObject* o);
    explicit PythonNumberObject(const int32_t& input);
    explicit PythonNumberObject(const uint32_t& input);
    explicit PythonNumberObject(const double& input);
};

class PythonStringObject : public PythonObject
{
   public:
    PythonStringObject();
    explicit PythonStringObject(PyObject* o);
    explicit PythonStringObject(const std::string& input);
    explicit PythonStringObject(const char* input);
};

class PythonBoolObject : public PythonObject
{
   public:
    PythonBoolObject();
    explicit PythonBoolObject(PyObject* o);
    explicit PythonBoolObject(const bool& input);
};

class PythonTupleObject : public PythonObject
{
   public:
    PythonTupleObject();
    explicit PythonTupleObject(PyObject* o);
    template <typename T>
    explicit PythonTupleObject(const std::vector<T>& input);

    size_t Size() const;
    template <typename T>
    T GetItem(size_t pos, bool ignore = true);
};

class PythonListObject : public PythonObject
{
   public:
    PythonListObject();
    explicit PythonListObject(PyObject* o);
    explicit PythonListObject(size_t size);
    template <typename T>
    explicit PythonListObject(const std::vector<T>& input);

    size_t Size() const;
    template <typename T>
    PythonListObject& Append(const T& value, bool ignore = true);
    template <typename T>
    T GetItem(size_t pos, bool ignore = true);
    template <typename T>
    const T operator[](size_t pos)
    {
        return GetItem<T>(pos, true);
    };
    template <typename T>
    PythonListObject& SetItem(size_t pos, const T& item, bool ignore = true);
    template <typename T>
    PythonListObject& Insert(size_t pos, const T& item, bool ignore = true);
    PythonTupleObject ToTuple(bool ignore = true);
};

class PythonDictObject : public PythonObject
{
   public:
    PythonDictObject();
    explicit PythonDictObject(PyObject* o);
    template <typename T1, typename T2>
    explicit PythonDictObject(const std::map<T1, T2>& input);

    template <typename T1, typename T2>
    PythonDictObject& Add(T1 key, T2 value, bool ignore = true);
    template <typename T>
    PythonDictObject& Delete(T key, bool ignore = true);
    template <typename T>
    PythonObject GetItem(T key, bool ignore = true);
    template <typename T>
    PythonObject operator[](T key)
    {
        return GetItem(key, true);
    }
};

class PyInterpGuard
{
   public:
    PyInterpGuard();
    ~PyInterpGuard();

   private:
    PyGILState_STATE gstate;
};

void MemScopePythonCall(const std::string& module, const std::string& function);

}  // namespace Utility

/**************************** 以下为模板函数的实现，调用者无需关注 ***********************************/
extern "C"
{
    void Py_IncRef(PyObject*) __attribute__((weak));
    void Py_DecRef(PyObject*) __attribute__((weak));
    void PyErr_Clear(void) __attribute__((weak));
    PyObject* PyList_New(Py_ssize_t size) __attribute__((weak));
    int PyList_SetItem(PyObject* op, Py_ssize_t i, PyObject* newitem) __attribute__((weak));
    int PyList_Append(PyObject* op, PyObject* newitem) __attribute__((weak));
    PyObject* PyList_GetItem(PyObject* op, Py_ssize_t i) __attribute__((weak));
    Py_ssize_t PyList_Size(PyObject* op) __attribute__((weak));
    PyObject* PyTuple_New(Py_ssize_t size) __attribute__((weak));
    int PyTuple_SetItem(PyObject* op, Py_ssize_t i, PyObject* newitem) __attribute__((weak));
    PyObject* PyTuple_GetItem(PyObject* op, Py_ssize_t i) __attribute__((weak));
    Py_ssize_t PyTuple_Size(PyObject* op) __attribute__((weak));
    PyObject* PyDict_New(void) __attribute__((weak));
    int PyDict_SetItem(PyObject* op, PyObject* key, PyObject* value) __attribute__((weak));
    int PyDict_DelItem(PyObject* op, PyObject* key) __attribute__((weak));
    PyObject* PyDict_GetItem(PyObject* op, PyObject* key) __attribute__((weak));
    PyObject* PySet_New(PyObject* iterable) __attribute__((weak));
    int PySet_Add(PyObject* set, PyObject* key) __attribute__((weak));
    PyObject* PyObject_CallObject(PyObject* callable, PyObject* args) __attribute__((weak));
    PyObject* PyLong_FromUnsignedLongLong(unsigned long long v) __attribute__((weak));
    PyObject* PyLong_FromLong(long v) __attribute__((weak));
    PyObject* PyUnicode_FromString(const char* u) __attribute__((weak));
}

namespace Utility
{
template <typename T>
PythonObject::PythonObject(const std::vector<T>& input) : PythonObject(PythonListObject(input)){};

template <typename T1, typename T2>
PythonObject::PythonObject(const std::map<T1, T2>& input) : PythonObject(PythonDictObject(input)){};

template <typename T>
PythonListObject::PythonListObject(const std::vector<T>& input)
{
    PyObject* o = PyList_New(input.size());
    if (o == nullptr)
    {
        return;
    }

    Py_ssize_t i = 0;
    for (const T& ele : input)
    {
        if (PyList_SetItem(o, i, PythonObject(ele).NewRef()) != 0)
        {
            Py_DecRef(o);
            return;
        }
        i++;
    }

    SetPtr(o);
}

template <typename T>
PythonListObject& PythonListObject::Append(const T& value, bool ignore)
{
    if (IsBad())
    {
        return *this;
    }
    PythonObject o(value);
    if (PyList_Append(ptr, o) != 0 && ignore)
    {
        PyErr_Clear();
    }
    return *this;
}

template <typename T>
T PythonListObject::GetItem(size_t pos, bool ignore)
{
    if (IsBad())
    {
        throw std::runtime_error("List is invalid");
    }

    if (static_cast<size_t>(PyList_Size(ptr)) <= pos)
    {
        throw std::runtime_error("List index outof range");
    }

    PyObject* o = PyList_GetItem(ptr, pos);
    if (o == nullptr && ignore)
    {
        PyErr_Clear();
    }

    return PythonObject(o).Cast<T>();
}

template <typename T>
PythonListObject& PythonListObject::SetItem(size_t pos, const T& item, bool ignore)
{
    if (IsBad())
    {
        throw std::runtime_error("List is invalid");
    }

    if (static_cast<size_t>(PyList_Size(ptr)) <= pos)
    {
        throw std::runtime_error("List index outof range");
    }

    PyObject* old = PyList_GetItem(ptr, pos);
    if (old != nullptr)
    {
        Py_DecRef(old);
    }

    if (PyList_SetItem(ptr, pos, PythonObject(item).NewRef()) != 0 && ignore)
    {
        PyErr_Clear();
    }

    return *this;
}

template <typename T>
PythonListObject& PythonListObject::Insert(size_t pos, const T& item, bool ignore)
{
    if (IsBad())
    {
        throw std::runtime_error("List is invalid");
    }

    PythonObject o(item);
    if (PyList_Insert(ptr, pos, o) != 0)
    {
        if (ignore)
        {
            PyErr_Clear();
        }
    }

    return *this;
}

template <typename T>
PythonTupleObject::PythonTupleObject(const std::vector<T>& input)
{
    PyObject* o = PyTuple_New(input.size());
    if (o == nullptr)
    {
        return;
    }

    Py_ssize_t i = 0;
    for (const T& ele : input)
    {
        if (PyTuple_SetItem(o, i, PythonObject(ele).NewRef()) != 0)
        {
            Py_DecRef(o);
            return;
        }
        i++;
    }

    SetPtr(o);
}

template <typename T>
T PythonTupleObject::GetItem(size_t pos, bool ignore)
{
    if (IsBad())
    {
        throw std::runtime_error("Tuple is invalid");
    }

    if (static_cast<size_t>(PyTuple_Size(ptr)) <= pos)
    {
        throw std::runtime_error("Tuple index outof range");
    }

    PyObject* o = PyTuple_GetItem(ptr, pos);
    if (o == nullptr && ignore)
    {
        PyErr_Clear();
    }

    return PythonObject(o).Cast<T>();
}

template <typename T1, typename T2>
PythonDictObject::PythonDictObject(const std::map<T1, T2>& input)
{
    PyObject* d = PyDict_New();
    if (d == nullptr)
    {
        return;
    }

    for (const std::pair<T1, T2>& pair : input)
    {
        PythonObject key(pair.first);
        if (key.IsBad())
        {
            Py_DecRef(d);
            return;
        }
        PythonObject value(pair.second);
        if (value.IsBad())
        {
            Py_DecRef(d);
            return;
        }
        if (PyDict_SetItem(d, key.NewRef(), value.NewRef()) != 0)
        {
            Py_DecRef(d);
            return;
        }
    }

    SetPtr(d);
}

template <typename T1, typename T2>
PythonDictObject& PythonDictObject::Add(T1 key, T2 value, bool ignore)
{
    if (IsBad())
    {
        throw std::runtime_error("Dict is invalid");
    }

    if (PyDict_SetItem(ptr, PythonObject(key).NewRef(), PythonObject(value).NewRef()) != 0 && ignore)
    {
        PyErr_Clear();
    }
    return *this;
}

template <typename T>
PythonDictObject& PythonDictObject::Delete(T key, bool ignore)
{
    if (IsBad())
    {
        throw std::runtime_error("Dict is invalid");
    }

    PythonObject o(key);
    if (PyDict_DelItem(ptr, o) != 0 && ignore)
    {
        PyErr_Clear();
    }
    return *this;
}

template <typename T>
PythonObject PythonDictObject::GetItem(T key, bool ignore)
{
    if (IsBad())
    {
        throw std::runtime_error("Dict is invalid");
    }

    PythonObject o(key);
    PyObject* item = PyDict_GetItem(ptr, o);
    if (item == nullptr && ignore)
    {
        PyErr_Clear();
    }
    return PythonObject(item);
}
}  // namespace Utility
