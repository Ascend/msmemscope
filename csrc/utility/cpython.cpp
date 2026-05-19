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

#include "cpython.h"
#include <mutex>
#include <Python.h>
#include <frameobject.h>
#include "log.h"
#include "utils.h"

extern "C" {
int Py_IsInitialized(void) __attribute__((weak));
PyGILState_STATE PyGILState_Ensure(void) __attribute__((weak));
PyFrameObject *PyEval_GetFrame(void) __attribute__((weak));
PyCodeObject* PyFrame_GetCode(PyFrameObject *) __attribute__((weak));
PyObject *PyObject_Str(PyObject *) __attribute__((weak));
const char *PyUnicode_AsUTF8(PyObject *) __attribute__((weak));
int PyFrame_GetLineNumber(PyFrameObject*) __attribute__((weak));
PyFrameObject* PyFrame_GetBack(PyFrameObject *) __attribute__((weak));
void PyGILState_Release(PyGILState_STATE) __attribute__((weak));
PyObject *PyImport_ImportModule(const char *name) __attribute__((weak));
PyObject *PyImport_GetModule(PyObject *name) __attribute__((weak));
PyObject *PyObject_GetAttrString(PyObject *v, const char *name) __attribute__((weak));
int PyCallable_Check(PyObject *) __attribute__((weak));
PyObject *PyObject_CallObject(PyObject *callable, PyObject *args) __attribute__((weak));
PyObject *PyObject_Call(PyObject *callable, PyObject *args, PyObject *kwargs) __attribute__((weak));
PyObject *PyUnicode_FromString(const char *u) __attribute__((weak));
PyObject *PyEval_GetGlobals(void) __attribute__((weak));
PyObject *PyLong_FromLong(long ival) __attribute__((weak));
PyObject *PyLong_FromUnsignedLong(unsigned long ival) __attribute__((weak));
PyObject *PyFloat_FromDouble(double fval) __attribute__((weak));
double PyFloat_AsDouble(PyObject *op) __attribute__((weak));
PyObject *PyBool_FromLong(long ok) __attribute__((weak));
int PyObject_IsTrue(PyObject *v) __attribute__((weak));
long PyLong_AsLong(PyObject *obj) __attribute__((weak));
unsigned long PyLong_AsUnsignedLong(PyObject *obj) __attribute__((weak));
PyObject *PyDict_GetItemString(PyObject *v, const char *key) __attribute__((weak));
PyObject *PyList_AsTuple(PyObject *v) __attribute__((weak));
int PyType_IsSubtype(PyTypeObject *a, PyTypeObject *b) __attribute__((weak));
const char *Py_GetVersion() __attribute__((weak));
PyThreadState *PyInterpreterState_ThreadHead(PyInterpreterState *interp) __attribute__((weak));
PyThreadState *PyThreadState_Next(PyThreadState *tstate) __attribute__((weak));
PyThreadState *PyThreadState_Swap(PyThreadState *newts) __attribute__((weak));
void PyEval_SetProfile(Py_tracefunc func, PyObject *arg) __attribute__((weak));
PyInterpreterState *PyInterpreterState_Get(void) __attribute__((weak));
PyThreadState *PyThreadState_Get(void) __attribute__((weak));
}

namespace Utility {

const Version VER39("3.9.0");
constexpr uint32_t PRE_ALLOC_SIZE = 2048;
constexpr uint32_t THREAD_NAME_LEN = 16;
TraceCbFunc callFunc = nullptr;
PyInterpreterState *interpreter = nullptr;

Version GetPyVersion()
{
    const char *ver = Py_GetVersion();
    if (ver == nullptr) {
        return Version("-1");
    }
    std::string version(ver);
    size_t pos = version.find(' ');
    return Version(version.substr(0, pos));
}

bool IsPyInterpRepeInited()
{
    if (Py_IsInitialized != nullptr && Py_IsInitialized() != 0) {
        return true;
    }
    return false;
}

PyInterpGuard::PyInterpGuard() : gstate(PyGILState_Ensure())
{
}

PyInterpGuard::~PyInterpGuard()
{
    PyGILState_Release(gstate);
}

bool IsPythonThread()
{
    char name[THREAD_NAME_LEN] = {0};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    std::string threadName = std::string(name);
    if (threadName.find("python") != std::string::npos ||threadName.find("VLLM") != std::string::npos ||
        threadName.find("mindie") != std::string::npos) {
        return true;
    }
    return false;
}

void PythonCallstack(uint32_t pyDepth, std::string& pyStack)
{
    thread_local static std::once_flag flag;
    thread_local static bool isPythonThread;

    std::call_once(flag, []() {
        isPythonThread = IsPythonThread();
    });
    if (!isPythonThread || !IsPyInterpRepeInited()) {
        pyStack = "\"NA\"";
        return;
    }
    PyInterpGuard stat{};
    static Version version = GetPyVersion();
    if (version < VER39) {
        pyStack = "\"NA\"";
        return;
    }
    PyFrameObject *frame = PyEval_GetFrame();
    if (frame == nullptr) {
        return;
    }
    Py_IncRef(reinterpret_cast<PyObject *>(frame));
    size_t depth = 0;
    pyStack.reserve(PRE_ALLOC_SIZE);
    pyStack += "\"";
    while (frame && depth < pyDepth) {
        PyCodeObject *code = PyFrame_GetCode(frame);
        if (code == nullptr) {
            break;
        }
        PythonObject codeObj(reinterpret_cast<PyObject*>(code));
        auto funcName = codeObj.Get("co_name");
        auto fileName = codeObj.Get("co_filename");
        if (funcName == nullptr || fileName == nullptr) {
            std::cerr << "Error: Failed to get code object attributes."<< std::endl;
            return;
        }

        auto funcNameStrObj = PyObject_Str(funcName);
        auto fileNameStrObj = PyObject_Str(fileName);
        if (funcNameStrObj == nullptr || fileNameStrObj == nullptr) {
            std::cerr << "Error: PyObject_Str failed for funcName."<< std::endl;
            return;
        }

        const char* funcNameCStr = PyUnicode_AsUTF8(funcNameStrObj);
        const char* fileNameCStr = PyUnicode_AsUTF8(fileNameStrObj);
        if (funcNameCStr == nullptr || fileNameCStr == nullptr) {
            std::cerr << "Error: Failed to convert code object attributes to UTF8 string."<< std::endl;
            return;
        }
 
        pyStack += std::string(fileNameCStr) + "(" +
                   std::to_string(PyFrame_GetLineNumber(frame)) +
                   "): " + std::string(funcNameCStr) + "\n";

        PyFrameObject *prevFrame = PyFrame_GetBack(frame);
        Py_DecRef(reinterpret_cast<PyObject *>(frame));
        frame = prevFrame;
        Py_DecRef(reinterpret_cast<PyObject *>(code));
        depth++;
    }
    if (frame != nullptr) {
        Py_DecRef(reinterpret_cast<PyObject *>(frame));
    }
    pyStack += "\"";
    return;
}

PythonObject::PythonObject() {}

PythonObject::~PythonObject()
{
    if (ptr != nullptr && Py_DecRef != nullptr) {
        Py_DecRef(ptr);
    }
}

PythonObject::PythonObject(PyObject* o)
{
    SetPtr(o);
}

PythonObject::PythonObject(const PyObject* o)
{
    SetPtr(const_cast<PyObject*>(o));
}

PythonObject::PythonObject(const PythonObject &obj)
{
    SetPtr(obj.ptr);
}

PythonObject& PythonObject::operator=(const PythonObject &obj)
{
    if (this == &obj) {
        return *this;
    }
    SetPtr(obj.ptr);
    return *this;
}

PythonObject::PythonObject(const int32_t& input)
    : PythonObject(static_cast<PyObject*>(PythonNumberObject(input))) {};
PythonObject::PythonObject(const uint32_t& input)
    : PythonObject(static_cast<PyObject*>(PythonNumberObject(input))) {};
PythonObject::PythonObject(const double& input)
    : PythonObject(static_cast<PyObject*>(PythonNumberObject(input))) {};
PythonObject::PythonObject(const std::string& input)
    : PythonObject(static_cast<PyObject*>(PythonStringObject(input))) {};
PythonObject::PythonObject(const char* input)
    : PythonObject(static_cast<PyObject*>(PythonStringObject(input))) {};
PythonObject::PythonObject(const bool& input)
    : PythonObject(static_cast<PyObject*>(PythonBoolObject(input))) {};

PythonObject& PythonObject::NewRef()
{
    if (!IsBad()) {
        Py_IncRef(ptr);
    }
    return *this;
}

void PythonObject::SetPtr(PyObject* o)
{
    if (ptr != nullptr && Py_DecRef != nullptr) {
        Py_DecRef(ptr);
    }
    if (o != nullptr && Py_IncRef != nullptr) {
        Py_IncRef(o);
    }
    ptr = o;
}

template <>
PyObject* PythonObject::Cast<PyObject*>()
{
    return ptr;
}

template <>
PythonObject PythonObject::Cast<PythonObject>()
{
    return *this;
}

template <>
int32_t PythonObject::Cast<int32_t>()
{
    if (IsBad() || !PyLong_Check(ptr)) {
        throw std::runtime_error("Unsupported type conversion");
    }
    return static_cast<int32_t>(PyLong_AsLong(ptr));
}

template <>
uint32_t PythonObject::Cast<uint32_t>()
{
    if (IsBad() || !PyLong_Check(ptr)) {
        throw std::runtime_error("Unsupported type conversion");
    }
    return static_cast<uint32_t>(PyLong_AsUnsignedLong(ptr));
}

template <>
double PythonObject::Cast<double>()
{
    if (IsBad() || !IsInstance("float")) {
        throw std::runtime_error("Unsupported type conversion");
    }
    return static_cast<double>(PyFloat_AsDouble(ptr));
}

template <>
bool PythonObject::Cast<bool>()
{
    if (IsBad() || !PyObject_IsTrue(ptr)) {
        return false;
    }
    return true;
}

template <>
std::string PythonObject::Cast<std::string>()
{
    if (IsBad()) {
        return std::string();
    }
    PyObject* strObj = PyObject_Str(ptr);
    if (strObj == nullptr) {
        return std::string();
    }
    const char* s = PyUnicode_AsUTF8(strObj);
    if (s == nullptr) {
        Py_DecRef(strObj);
        return std::string();
    }

    std::string ret = std::string(s);
    Py_DecRef(strObj);
    return ret;
}

bool PythonObject::IsInstance(const PythonObject& type) const
{
    if (IsBad() || type.IsBad()) {
        return false;
    }

    if (!PyType_Check(static_cast<PyObject*>(type))) {
        return false;
    }

    return PyObject_TypeCheck(ptr, reinterpret_cast<PyTypeObject*>(type.ptr));
}

bool PythonObject::IsInstance(const std::string& type) const
{
    static PythonObject builtin;
    if (builtin.IsBad()) {
        builtin = PythonObject::Import("builtins");
    }
    PythonObject pytype = builtin.Get(type);
    return IsInstance(pytype);
}

bool PythonObject::IsCallable() const
{
    return PyCallable_Check(ptr);
}

std::string PythonObject::Type() const
{
    if (IsBad()) {
        return std::string();
    }

    return std::string(ptr->ob_type->tp_name);
}

void GetPyFuncInfo(PyFrameObject *frame, std::string &info, std::string &hash)
{
    if (frame == nullptr) {
        return;
    }
    PyCodeObject *code = PyFrame_GetCode(frame);
    if (code == nullptr) {
        return;
    }
    PythonObject codeObj(reinterpret_cast<PyObject*>(code));
    std::string funcName = PythonObject(codeObj.Get("co_name")).Cast<std::string>();
    std::string fileName = PythonObject(codeObj.Get("co_filename")).Cast<std::string>();
    std::string lineNum = std::to_string(PyFrame_GetLineNumber(frame));
    hash = fileName + ":" + funcName;
    info = fileName + "(" + lineNum + "): " + funcName;
    PythonObject torch = PythonObject::Import("torch", true);
    if (torch.IsBad()) {
        Py_DecRef(reinterpret_cast<PyObject*>(code));
        return;
    }
    static PythonObject moduleCode = torch.Get("nn").Get("Module").Get("__call__").Get("__code__");
    if (reinterpret_cast<PyObject*>(code) == static_cast<PyObject*>(moduleCode)) {
        PythonObject frameObj(reinterpret_cast<PyObject *>(frame));
        PythonDictObject locals(frameObj.Get("f_locals"));
        auto className = locals["self"].Get("__class__").Get("__name__").Cast<std::string>();
        hash = "nn.Module: " + className;
        info = hash;
    }
    Py_DecRef(reinterpret_cast<PyObject*>(code));
}

bool IsIgnoreCFunc(std::string hash)
{
    static std::string ignoreCFunc = "__exit__";
    static std::string ignoreCFile = "contextlib.py";
    size_t pos = hash.find(":");
    std::string funcName(hash.c_str() + pos + 1);
    if (funcName != ignoreCFunc) {
        return false;
    }
    std::string fileName(hash.c_str(), pos);
    size_t fileNameSize = fileName.size();
    size_t ignoreCFileSize = ignoreCFile.size();
    return fileNameSize >= ignoreCFileSize &&
           strncmp(fileName.c_str() + fileNameSize - ignoreCFileSize, ignoreCFile.c_str(), ignoreCFileSize) == 0;
}

int pyProfileFn(PyObject* obj, PyFrameObject* frame, int what, PyObject* arg)
{
    if (callFunc == nullptr) {
        return 0;
    }
    std::string hash;
    std::string info;
    switch (what) {
        case PyTrace_CALL: {
            GetPyFuncInfo(frame, info, hash);
            callFunc(hash, info, MemScope::PyTraceType::PYCALL, 0);
            break;
        }
        case PyTrace_RETURN: {
            GetPyFuncInfo(frame, info, hash);
            callFunc(hash, info, MemScope::PyTraceType::PYRETURN, 0);
            break;
        }
        case PyTrace_C_CALL: {
            std::string pyHash;
            std::string pyInfo;
            info = PythonObject(arg).Cast<std::string>();
            GetPyFuncInfo(frame, pyInfo, pyHash);
            if (IsIgnoreCFunc(pyHash)) {
                break;
            }
            callFunc(info, pyInfo, MemScope::PyTraceType::CCALL, 0);
            callFunc(info, info, MemScope::PyTraceType::CCALL, 0);
            break;
        }
        case PyTrace_C_RETURN: {
            std::string pyHash;
            std::string pyInfo;
            GetPyFuncInfo(frame, pyInfo, pyHash);
            info = PythonObject(arg).Cast<std::string>();
            callFunc(info, info, MemScope::PyTraceType::CRETURN, 0);
            callFunc(info, pyInfo, MemScope::PyTraceType::CRETURN, 0);
            break;
        }
        default:
            break;
    }
    return 0;
}

std::vector<PyThreadState*> getInterpreterThreads(PyInterpreterState* interpreter)
{
    PyInterpGuard stat;
    std::vector<PyThreadState*> threads;
    if (interpreter != nullptr) {
        auto* thread_state = PyInterpreterState_ThreadHead(interpreter);
        while (thread_state != nullptr) {
            threads.push_back(thread_state);
            thread_state = PyThreadState_Next(thread_state);
        }
    }
    return threads;
}

void GetTraceCallStack(std::string type, uint64_t time)
{
    const size_t stackMaxDepth = 128;
    auto frame = PyEval_GetFrame();
    if (frame != nullptr) {
        Py_IncRef(reinterpret_cast<PyObject *>(frame));
    }
    size_t depth = 0;
    while (frame != nullptr && depth <= stackMaxDepth) {
        std::string info;
        std::string hash;
        GetPyFuncInfo(frame, info, hash);
        if (callFunc != nullptr) {
            callFunc(hash, type + info, MemScope::PyTraceType::PYCALL, time);
        }
        PyFrameObject *prevFrame = PyFrame_GetBack(frame);
        Py_DecRef(reinterpret_cast<PyObject *>(frame));
        frame = prevFrame;
        ++depth;
    }
    if (frame != nullptr) {
        Py_DecRef(reinterpret_cast<PyObject *>(frame));
    }
}

void RegisterTraceCb(TraceCbFunc call)
{
    callFunc = call;
    PythonObject torch_npu = PythonObject::Import("torch_npu", true);
    if (!torch_npu.IsBad()) {
        torch_npu.Get("npu").Get("_lazy_init").Call();
    }
    PyThreadState* init = PyThreadState_Get();
    interpreter = PyInterpreterState_Get();
    if (!init) {
        std::cout << "[msmemscope] Error: Failed to get main thread state, PythonTracer will not start." << std::endl;
        return;
    }
    std::vector<PyThreadState *> thread_states = getInterpreterThreads(interpreter);
    uint64_t time = GetTimeNanoseconds();
    for (const auto thread_state : thread_states) {
        PyThreadState_Swap(thread_state);
        GetTraceCallStack("start: ", time);
        PyEval_SetProfile(pyProfileFn, nullptr);
    }
    PyThreadState_Swap(init);
}

void UnRegisterTraceCb()
{
    uint64_t time = GetTimeNanoseconds();
    for (const auto thread_state : getInterpreterThreads(interpreter)) {
        PyThreadState_Swap(thread_state);
        GetTraceCallStack("stop: ", time);
        PyEval_SetProfile(nullptr, nullptr);
    }
    callFunc = nullptr;
}

PythonObject PythonObject::Import(const std::string& name, bool fromcache, bool ignore)
{
    if (!IsPyInterpRepeInited()) {
        return PythonObject();
    }

    PyObject* m = nullptr;
    if (fromcache) {
        PythonObject pyname(name);
        m = PyImport_GetModule(pyname);
    } else {
        m = PyImport_ImportModule(name.c_str());
    }
    if (m == nullptr) {
        if (ignore) {
            PyErr_Clear();
        }
        return PythonObject();
    }
    PythonObject ret(m);
    Py_DecRef(m);
    return ret;
}

PythonObject PythonObject::GetGlobal(const std::string& name, bool ignore)
{
    PyObject *globals = PyEval_GetGlobals();
    if (globals == nullptr) {
        if (ignore) {
            PyErr_Clear();
        }
        return PythonObject();
    }

    return PythonObject(PyDict_GetItemString(globals, name.c_str()));
}

PythonObject PythonObject::Get(const std::string& name, bool ignore) const
{
    if (ptr == nullptr) {
        return PythonObject();
    }
    PyObject* o = PyObject_GetAttrString(ptr, name.c_str());
    if (o == nullptr) {
        if (ignore) {
            PyErr_Clear();
        }
        return PythonObject();
    }
    PythonObject ret(o);
    Py_DecRef(o);
    return ret;
}

PythonObject PythonObject::GetItem(const PythonObject& index, bool ignore) const
{
    if (ptr == nullptr || index.ptr == nullptr) {
        return PythonObject();
    }

    do {
        PyObject* getitem = PyObject_GetAttrString(ptr, "__getitem__");
        if (getitem == nullptr) {
            break;
        }

        PyObject* args = PyTuple_New(1);
        if (args == nullptr) {
            Py_DecRef(getitem);
            break;
        }

        Py_IncRef(index);
        PyTuple_SetItem(args, 0, index);

        PyObject* ret = PyObject_CallObject(getitem, args);
        if (ret == nullptr) {
            Py_DecRef(getitem);
            Py_DecRef(args);
            break;
        }

        return PythonObject(ret);
    } while (0);

    /* 走到这个分支说明异常了 */
    if (ignore) {
        PyErr_Clear();
    }
    return PythonObject();
}

PythonObject PythonObject::Call(bool ignore)
{
    if (ptr == nullptr) {
        return PythonObject();
    }
    if (!PyCallable_Check(ptr)) {
        return PythonObject();
    }

    PyObject* o = PyObject_CallObject(ptr, nullptr);
    if (o == nullptr) {
        if (ignore) {
            PyErr_Clear();
        }
        return PythonObject();
    }
    PythonObject ret(o);
    Py_DecRef(o);
    return ret;
}

PythonObject PythonObject::Call(PythonObject& args, bool ignore)
{
    if (ptr == nullptr) {
        return PythonObject();
    }
    if (!PyCallable_Check(ptr)) {
        return PythonObject();
    }

    if (!PyTuple_Check(args)) {
        return PythonObject();
    }

    PyObject* o = PyObject_CallObject(ptr, args);
    if (o == nullptr && ignore) {
        PyErr_Clear();
    }
    PythonObject ret(o);
    Py_DecRef(o);
    return ret;
}

PythonObject PythonObject::Call(PythonObject& args, PythonObject& kwargs, bool ignore)
{
    if (ptr == nullptr) {
        return PythonObject();
    }
    if (!PyCallable_Check(ptr)) {
        return PythonObject();
    }

    if (!PyTuple_Check(args) || PyDict_Check(kwargs)) {
        return PythonObject();
    }

    PyObject* o = PyObject_Call(ptr, args, kwargs);
    if (o == nullptr && ignore) {
        PyErr_Clear();
    }
    PythonObject ret(o);
    Py_DecRef(o);
    return ret;
}

PythonNumberObject::PythonNumberObject()
{
    PyObject* o = PyLong_FromLong(0);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonNumberObject::PythonNumberObject(PyObject* o)
{
    /* PyFloat_Check需要访问PyFloat_Type结构体，此处不要直接使用，需要用isinstance函数 */
    if (o == nullptr || (!PyLong_Check(o) && !PythonObject(o).IsInstance("float"))) {
        return;
    }
    SetPtr(o);
}

PythonNumberObject::PythonNumberObject(const int32_t& input)
{
    PyObject* o = PyLong_FromLong(input);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonNumberObject::PythonNumberObject(const uint32_t& input)
{
    PyObject* o = PyLong_FromUnsignedLong(input);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonNumberObject::PythonNumberObject(const double& input)
{
    PyObject* o = PyFloat_FromDouble(input);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonStringObject::PythonStringObject()
{
    PyObject* o = PyUnicode_FromString("");
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonStringObject::PythonStringObject(PyObject* o)
{
    if (o == nullptr || !PyUnicode_Check(o)) {
        return;
    }
    SetPtr(o);
}

PythonStringObject::PythonStringObject(const std::string& input)
{
    PyObject* o = PyUnicode_FromString(input.c_str());
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonStringObject::PythonStringObject(const char* input)
{
    PyObject* o = PyUnicode_FromString(input);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonBoolObject::PythonBoolObject()
{
    PyObject* o = PyBool_FromLong(0);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonBoolObject::PythonBoolObject(PyObject* o)
{
    PyObject* ret = PyObject_IsTrue(o) ? PyBool_FromLong(1) : PyBool_FromLong(0);
    SetPtr(ret);
    if (ret != nullptr) {
        Py_DecRef(ret);
    }
}

PythonBoolObject::PythonBoolObject(const bool& input)
{
    PyObject* o = PyBool_FromLong(input);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonListObject::PythonListObject()
{
    PyObject* o = PyList_New(0);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonListObject::PythonListObject(PyObject* o)
{
    if (o == nullptr || !PyList_Check(o)) {
        return;
    }
    SetPtr(o);
}

PythonListObject::PythonListObject(size_t size)
{
    PyObject* o = PyList_New(size);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

size_t PythonListObject::Size() const
{
    if (IsBad()) {
        return 0;
    }
    return PyList_Size(ptr);
}

PythonTupleObject PythonListObject::ToTuple(bool ignore)
{
    if (IsBad()) {
        return PythonTupleObject(static_cast<PyObject*>(nullptr));
    }

    PyObject* o = PyList_AsTuple(ptr);
    if (o == nullptr && ignore) {
        PyErr_Clear();
    }

    PythonTupleObject ret(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
    return ret;
}

PythonTupleObject::PythonTupleObject()
{
    PyObject* o = PyTuple_New(0);
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonTupleObject::PythonTupleObject(PyObject* o)
{
    if (o == nullptr || !PyTuple_Check(o)) {
        return;
    }
    SetPtr(o);
}

size_t PythonTupleObject::Size() const
{
    if (IsBad()) {
        return 0;
    }
    return PyTuple_Size(ptr);
}

PythonDictObject::PythonDictObject()
{
    PyObject* o = PyDict_New();
    SetPtr(o);
    if (o != nullptr) {
        Py_DecRef(o);
    }
}

PythonDictObject::PythonDictObject(PyObject* o)
{
    if (o == nullptr || !PyDict_Check(o)) {
        return;
    }
    SetPtr(o);
}

void MemScopePythonCall(const std::string& module, const std::string& function)
{
    if (!Utility::IsPyInterpRepeInited()) {
        LOG_ERROR("Python Interpreter initialization FAILED");
        return;
    }

    Utility::PyInterpGuard stat;
    Utility::PythonObject sys = Utility::PythonObject::Import("sys");
    Utility::PythonObject modules = sys.Get("modules");
    Utility::PythonObject torch = modules.GetItem(Utility::PythonObject("torch"));
    if (!torch.IsBad()) {
        Utility::PythonObject pythonModule = Utility::PythonObject::Import(module, false);
        if (pythonModule.IsBad()) {
            LOG_ERROR("import %s FAILED", module.c_str());
            return;
        }
 
        Utility::PythonObject pythonFunction = pythonModule.Get(function);
        if (pythonFunction.IsBad()) {
            LOG_ERROR("cannot get function %s", function.c_str());
            return;
        }
        pythonFunction.Call();
    }
    return;
}

}