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

#include <Python.h>
#include <frameobject.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "log.h"
#include "utils.h"

extern "C"
{
    PyGILState_STATE PyGILState_Ensure(void) __attribute__((weak));
    PyFrameObject* PyEval_GetFrame(void) __attribute__((weak));
    PyCodeObject* PyFrame_GetCode(PyFrameObject*) __attribute__((weak));
    PyObject* PyObject_Str(PyObject*) __attribute__((weak));
    const char* PyUnicode_AsUTF8(PyObject*) __attribute__((weak));
    int PyFrame_GetLineNumber(PyFrameObject*) __attribute__((weak));
    PyFrameObject* PyFrame_GetBack(PyFrameObject*) __attribute__((weak));
    void PyGILState_Release(PyGILState_STATE) __attribute__((weak));
    PyObject* PyImport_ImportModule(const char* name) __attribute__((weak));
    PyObject* PyImport_GetModule(PyObject* name) __attribute__((weak));
    PyObject* PyObject_GetAttrString(PyObject* v, const char* name) __attribute__((weak));
    PyObject* PyObject_GetAttr(PyObject* v, PyObject* name) __attribute__((weak));
    PyObject* PyUnicode_InternFromString(const char* u) __attribute__((weak));
    int PyCallable_Check(PyObject*) __attribute__((weak));
    PyObject* PyObject_Call(PyObject* callable, PyObject* args, PyObject* kwargs) __attribute__((weak));
    PyObject* PyEval_GetGlobals(void) __attribute__((weak));
    PyObject* PyLong_FromUnsignedLong(unsigned long ival) __attribute__((weak));
    PyObject* PyFloat_FromDouble(double fval) __attribute__((weak));
    double PyFloat_AsDouble(PyObject* op) __attribute__((weak));
    PyObject* PyBool_FromLong(long ok) __attribute__((weak));
    int PyObject_IsTrue(PyObject* v) __attribute__((weak));
    long PyLong_AsLong(PyObject* obj) __attribute__((weak));
    unsigned long PyLong_AsUnsignedLong(PyObject* obj) __attribute__((weak));
    unsigned long long PyLong_AsUnsignedLongLong(PyObject* obj) __attribute__((weak));
    PyObject* PyDict_GetItemString(PyObject* v, const char* key) __attribute__((weak));
    PyObject* PyList_AsTuple(PyObject* v) __attribute__((weak));
    // 源码里搜不到该名字: IsInstance所用PyObject_TypeCheck宏(新版为static inline)会展开成
    // PyType_IsSubtype调用。此处弱声明把该引用降为弱引用,否则钩子so会带强未定义符号导致链接失败。勿删
    int PyType_IsSubtype(PyTypeObject* a, PyTypeObject* b) __attribute__((weak));
    const char* Py_GetVersion() __attribute__((weak));
    PyThreadState* PyInterpreterState_ThreadHead(PyInterpreterState* interp) __attribute__((weak));
    PyThreadState* PyThreadState_Next(PyThreadState* tstate) __attribute__((weak));
    PyThreadState* PyThreadState_Swap(PyThreadState* newts) __attribute__((weak));
    void PyEval_SetProfile(Py_tracefunc func, PyObject* arg) __attribute__((weak));
    PyInterpreterState* PyInterpreterState_Get(void) __attribute__((weak));
    PyThreadState* PyThreadState_Get(void) __attribute__((weak));
    void PyErr_Fetch(PyObject**, PyObject**, PyObject**) __attribute__((weak));
    void PyErr_Restore(PyObject*, PyObject*, PyObject*) __attribute__((weak));
}

namespace Utility
{

const Version VER39("3.9.0");
constexpr uint32_t PRE_ALLOC_SIZE = 2048;
constexpr uint32_t THREAD_NAME_LEN = 16;
TraceCbFunc callFunc = nullptr;
PyInterpreterState* interpreter = nullptr;

Version GetPyVersion()
{
    const char* ver = Py_GetVersion();
    if (ver == nullptr)
    {
        return Version("-1");
    }
    std::string version(ver);
    size_t pos = version.find(' ');
    return Version(version.substr(0, pos));
}

bool IsPyInterpRepeInited()
{
    if (Py_IsInitialized != nullptr && Py_IsInitialized() != 0)
    {
        return true;
    }
    return false;
}

bool IsPyVersionAtLeast39()
{
    static Version ver = GetPyVersion();  // 非const: Version::operator<非const成员
    return !(ver < VER39);
}

PyInterpGuard::PyInterpGuard() : gstate(PyGILState_Ensure()) {}

PyInterpGuard::~PyInterpGuard() { PyGILState_Release(gstate); }

bool IsPythonThread()
{
    char name[THREAD_NAME_LEN] = {0};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    std::string threadName = std::string(name);
    if (threadName.find("python") != std::string::npos || threadName.find("VLLM") != std::string::npos ||
        threadName.find("mindie") != std::string::npos)
    {
        return true;
    }
    return false;
}

namespace
{
// WalkFrames本地格式化(hal/mstx路径,无缓存): 核心无失败分支,每帧格式化不可失败,
// 修复旧实现"错误return只带前导引号的残缺串"缺陷
struct BuildStackCtx
{
    std::string* out;
};

void AppendFrameStr(void* ctx, PyCodeObject* /*code*/, const char* filename, const char* funcname, int lineno,
                    uint32_t /*depth*/)
{
    BuildStackCtx* c = static_cast<BuildStackCtx*>(ctx);
    // 逐段append,避免构造完整临时串(pyStack已reserve)
    c->out->append(filename);
    c->out->append("(");
    c->out->append(std::to_string(lineno));
    c->out->append("): ");
    c->out->append(funcname);
    c->out->push_back('\n');
}

void BuildPyStackString(uint32_t pyDepth, std::string& pyStack)
{
    pyStack.reserve(PRE_ALLOC_SIZE);
    pyStack += "\"";
    BuildStackCtx ctx{&pyStack};
    uint32_t walked = PyStackCore::WalkFrames(pyDepth, AppendFrameStr, &ctx);
    if (walked == 0)
    {
        // 无帧→NA完整串(修复:旧实现frame==NULL直接return空串,调用方无从区分)
        pyStack = "\"NA\"";
        return;
    }
    pyStack += "\"";
}
}  // namespace

void PythonCallstack(uint32_t pyDepth, std::string& pyStack)
{
    thread_local static std::once_flag flag;
    thread_local static bool isPythonThread;

    std::call_once(flag, []() { isPythonThread = IsPythonThread(); });
    if (!isPythonThread || !IsPyInterpRepeInited())
    {
        pyStack = "\"NA\"";
        return;
    }
    if (!IsPyVersionAtLeast39())
    {
        pyStack = "\"NA\"";
        return;
    }
    if (PyGILState_Check != nullptr && PyGILState_Check() == 1)
    {
        // Check快路径: 已持GIL直接走链,免Ensure阻塞
        BuildPyStackString(pyDepth, pyStack);
        return;
    }
    // Ensure兜底: 未持GIL行为与现状一致(阻塞取GIL后采集),hal/mstx零回归
    PyInterpGuard stat{};
    BuildPyStackString(pyDepth, pyStack);
}

PythonObject::PythonObject() {}

PythonObject::~PythonObject()
{
    if (ptr != nullptr && Py_DecRef != nullptr)
    {
        Py_DecRef(ptr);
    }
}

PythonObject::PythonObject(PyObject* o) { SetPtr(o); }

PythonObject::PythonObject(const PyObject* o) { SetPtr(const_cast<PyObject*>(o)); }

PythonObject::PythonObject(const PythonObject& obj) { SetPtr(obj.ptr); }

PythonObject& PythonObject::operator=(const PythonObject& obj)
{
    if (this == &obj)
    {
        return *this;
    }
    SetPtr(obj.ptr);
    return *this;
}

PythonObject::PythonObject(const int32_t& input) : PythonObject(static_cast<PyObject*>(PythonNumberObject(input))) {};
PythonObject::PythonObject(const uint32_t& input) : PythonObject(static_cast<PyObject*>(PythonNumberObject(input))) {};
PythonObject::PythonObject(const uint64_t& input) : PythonObject(static_cast<PyObject*>(PythonNumberObject(input))) {};
PythonObject::PythonObject(const double& input) : PythonObject(static_cast<PyObject*>(PythonNumberObject(input))) {};
PythonObject::PythonObject(const std::string& input)
    : PythonObject(static_cast<PyObject*>(PythonStringObject(input))) {};
PythonObject::PythonObject(const char* input) : PythonObject(static_cast<PyObject*>(PythonStringObject(input))) {};
PythonObject::PythonObject(const bool& input) : PythonObject(static_cast<PyObject*>(PythonBoolObject(input))) {};

PythonObject& PythonObject::NewRef()
{
    if (!IsBad())
    {
        Py_IncRef(ptr);
    }
    return *this;
}

void PythonObject::SetPtr(PyObject* o)
{
    if (ptr != nullptr && Py_DecRef != nullptr)
    {
        Py_DecRef(ptr);
    }
    if (o != nullptr && Py_IncRef != nullptr)
    {
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
    if (IsBad() || !PyLong_Check(ptr))
    {
        throw std::runtime_error("Unsupported type conversion");
    }
    return static_cast<int32_t>(PyLong_AsLong(ptr));
}

template <>
uint32_t PythonObject::Cast<uint32_t>()
{
    if (IsBad() || !PyLong_Check(ptr))
    {
        throw std::runtime_error("Unsupported type conversion");
    }
    return static_cast<uint32_t>(PyLong_AsUnsignedLong(ptr));
}

template <>
uint64_t PythonObject::Cast<uint64_t>()
{
    if (IsBad() || !PyLong_Check(ptr))
    {
        throw std::runtime_error("Unsupported type conversion");
    }
    return static_cast<uint64_t>(PyLong_AsUnsignedLongLong(ptr));
}

template <>
double PythonObject::Cast<double>()
{
    if (IsBad() || !IsInstance("float"))
    {
        throw std::runtime_error("Unsupported type conversion");
    }
    return static_cast<double>(PyFloat_AsDouble(ptr));
}

template <>
bool PythonObject::Cast<bool>()
{
    if (IsBad() || !PyObject_IsTrue(ptr))
    {
        return false;
    }
    return true;
}

template <>
std::string PythonObject::Cast<std::string>()
{
    if (IsBad())
    {
        return std::string();
    }
    PyObject* strObj = PyObject_Str(ptr);
    if (strObj == nullptr)
    {
        return std::string();
    }
    const char* s = PyUnicode_AsUTF8(strObj);
    if (s == nullptr)
    {
        Py_DecRef(strObj);
        return std::string();
    }

    std::string ret = std::string(s);
    Py_DecRef(strObj);
    return ret;
}

bool PythonObject::IsInstance(const PythonObject& type) const
{
    if (IsBad() || type.IsBad())
    {
        return false;
    }

    if (!PyType_Check(static_cast<PyObject*>(type)))
    {
        return false;
    }

    return PyObject_TypeCheck(ptr, reinterpret_cast<PyTypeObject*>(type.ptr));
}

bool PythonObject::IsInstance(const std::string& type) const
{
    static PythonObject builtin;
    if (builtin.IsBad())
    {
        builtin = PythonObject::Import("builtins");
    }
    PythonObject pytype = builtin.Get(type);
    return IsInstance(pytype);
}

bool PythonObject::IsCallable() const { return PyCallable_Check(ptr); }

std::string PythonObject::Type() const
{
    if (IsBad())
    {
        return std::string();
    }

    return std::string(ptr->ob_type->tp_name);
}

void GetPyFuncInfo(PyFrameObject* frame, std::string& info, std::string& hash)
{
    if (frame == nullptr)
    {
        return;
    }
    PyCodeObject* code = PyFrame_GetCode(frame);
    if (code == nullptr)
    {
        return;
    }
    PythonObject codeObj(reinterpret_cast<PyObject*>(code));
    std::string funcName = PythonObject(codeObj.Get("co_name")).Cast<std::string>();
    std::string fileName = PythonObject(codeObj.Get("co_filename")).Cast<std::string>();
    std::string lineNum = std::to_string(PyFrame_GetLineNumber(frame));
    hash = fileName + ":" + funcName;
    info = fileName + "(" + lineNum + "): " + funcName;
    PythonObject torch = PythonObject::Import("torch", true);
    if (torch.IsBad())
    {
        Py_DecRef(reinterpret_cast<PyObject*>(code));
        return;
    }
    static PythonObject moduleCode = torch.Get("nn").Get("Module").Get("__call__").Get("__code__");
    if (reinterpret_cast<PyObject*>(code) == static_cast<PyObject*>(moduleCode))
    {
        PythonObject frameObj(reinterpret_cast<PyObject*>(frame));
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
    if (funcName != ignoreCFunc)
    {
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
    if (callFunc == nullptr)
    {
        return 0;
    }
    std::string hash;
    std::string info;
    switch (what)
    {
        case PyTrace_CALL:
        {
            GetPyFuncInfo(frame, info, hash);
            callFunc(hash, info, MemScope::PyTraceType::PYCALL, 0);
            break;
        }
        case PyTrace_RETURN:
        {
            GetPyFuncInfo(frame, info, hash);
            callFunc(hash, info, MemScope::PyTraceType::PYRETURN, 0);
            break;
        }
        case PyTrace_C_CALL:
        {
            std::string pyHash;
            std::string pyInfo;
            info = PythonObject(arg).Cast<std::string>();
            GetPyFuncInfo(frame, pyInfo, pyHash);
            if (IsIgnoreCFunc(pyHash))
            {
                break;
            }
            callFunc(info, pyInfo, MemScope::PyTraceType::CCALL, 0);
            callFunc(info, info, MemScope::PyTraceType::CCALL, 0);
            break;
        }
        case PyTrace_C_RETURN:
        {
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
    if (interpreter != nullptr)
    {
        auto* thread_state = PyInterpreterState_ThreadHead(interpreter);
        while (thread_state != nullptr)
        {
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
    if (frame != nullptr)
    {
        Py_IncRef(reinterpret_cast<PyObject*>(frame));
    }
    size_t depth = 0;
    while (frame != nullptr && depth <= stackMaxDepth)
    {
        std::string info;
        std::string hash;
        GetPyFuncInfo(frame, info, hash);
        if (callFunc != nullptr)
        {
            callFunc(hash, type + info, MemScope::PyTraceType::PYCALL, time);
        }
        PyFrameObject* prevFrame = PyFrame_GetBack(frame);
        Py_DecRef(reinterpret_cast<PyObject*>(frame));
        frame = prevFrame;
        ++depth;
    }
    if (frame != nullptr)
    {
        Py_DecRef(reinterpret_cast<PyObject*>(frame));
    }
}

void RegisterTraceCb(TraceCbFunc call)
{
    callFunc = call;
    PythonObject torch_npu = PythonObject::Import("torch_npu", true);
    if (!torch_npu.IsBad())
    {
        torch_npu.Get("npu").Get("_lazy_init").Call();
    }
    PyThreadState* init = PyThreadState_Get();
    interpreter = PyInterpreterState_Get();
    if (!init)
    {
        std::cout << "[msmemscope] Error: Failed to get main thread state, PythonTracer will not start." << std::endl;
        return;
    }
    std::vector<PyThreadState*> thread_states = getInterpreterThreads(interpreter);
    uint64_t time = GetTimeNanoseconds();
    for (const auto thread_state : thread_states)
    {
        PyThreadState_Swap(thread_state);
        GetTraceCallStack("start: ", time);
        PyEval_SetProfile(pyProfileFn, nullptr);
    }
    PyThreadState_Swap(init);
}

void UnRegisterTraceCb()
{
    uint64_t time = GetTimeNanoseconds();
    for (const auto thread_state : getInterpreterThreads(interpreter))
    {
        PyThreadState_Swap(thread_state);
        GetTraceCallStack("stop: ", time);
        PyEval_SetProfile(nullptr, nullptr);
    }
    callFunc = nullptr;
}

PythonObject PythonObject::Import(const std::string& name, bool fromcache, bool ignore)
{
    if (!IsPyInterpRepeInited())
    {
        return PythonObject();
    }

    PyObject* m = nullptr;
    if (fromcache)
    {
        PythonObject pyname(name);
        m = PyImport_GetModule(pyname);
    }
    else
    {
        m = PyImport_ImportModule(name.c_str());
    }
    if (m == nullptr)
    {
        if (ignore)
        {
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
    PyObject* globals = PyEval_GetGlobals();
    if (globals == nullptr)
    {
        if (ignore)
        {
            PyErr_Clear();
        }
        return PythonObject();
    }

    return PythonObject(PyDict_GetItemString(globals, name.c_str()));
}

PythonObject PythonObject::Get(const std::string& name, bool ignore) const
{
    if (ptr == nullptr)
    {
        return PythonObject();
    }
    PyObject* o = PyObject_GetAttrString(ptr, name.c_str());
    if (o == nullptr)
    {
        if (ignore)
        {
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
    if (ptr == nullptr || index.ptr == nullptr)
    {
        return PythonObject();
    }

    do
    {
        PyObject* getitem = PyObject_GetAttrString(ptr, "__getitem__");
        if (getitem == nullptr)
        {
            break;
        }

        PyObject* args = PyTuple_New(1);
        if (args == nullptr)
        {
            Py_DecRef(getitem);
            break;
        }

        Py_IncRef(index);
        PyTuple_SetItem(args, 0, index);

        PyObject* ret = PyObject_CallObject(getitem, args);
        if (ret == nullptr)
        {
            Py_DecRef(getitem);
            Py_DecRef(args);
            break;
        }

        return PythonObject(ret);
    } while (0);

    /* 走到这个分支说明异常了 */
    if (ignore)
    {
        PyErr_Clear();
    }
    return PythonObject();
}

PythonObject PythonObject::Call(bool ignore)
{
    if (ptr == nullptr)
    {
        return PythonObject();
    }
    if (!PyCallable_Check(ptr))
    {
        return PythonObject();
    }

    PyObject* o = PyObject_CallObject(ptr, nullptr);
    if (o == nullptr)
    {
        if (ignore)
        {
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
    if (ptr == nullptr)
    {
        return PythonObject();
    }
    if (!PyCallable_Check(ptr))
    {
        return PythonObject();
    }

    if (!PyTuple_Check(args))
    {
        return PythonObject();
    }

    PyObject* o = PyObject_CallObject(ptr, args);
    if (o == nullptr && ignore)
    {
        PyErr_Clear();
    }
    PythonObject ret(o);
    Py_DecRef(o);
    return ret;
}

PythonObject PythonObject::Call(PythonObject& args, PythonObject& kwargs, bool ignore)
{
    if (ptr == nullptr)
    {
        return PythonObject();
    }
    if (!PyCallable_Check(ptr))
    {
        return PythonObject();
    }

    if (!PyTuple_Check(args) || PyDict_Check(kwargs))
    {
        return PythonObject();
    }

    PyObject* o = PyObject_Call(ptr, args, kwargs);
    if (o == nullptr && ignore)
    {
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
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonNumberObject::PythonNumberObject(PyObject* o)
{
    /* PyFloat_Check需要访问PyFloat_Type结构体，此处不要直接使用，需要用isinstance函数 */
    if (o == nullptr || (!PyLong_Check(o) && !PythonObject(o).IsInstance("float")))
    {
        return;
    }
    SetPtr(o);
}

PythonNumberObject::PythonNumberObject(const int32_t& input)
{
    PyObject* o = PyLong_FromLong(input);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonNumberObject::PythonNumberObject(const uint32_t& input)
{
    PyObject* o = PyLong_FromUnsignedLong(input);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonNumberObject::PythonNumberObject(const uint64_t& input)
{
    PyObject* o = PyLong_FromUnsignedLongLong(input);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonNumberObject::PythonNumberObject(const double& input)
{
    PyObject* o = PyFloat_FromDouble(input);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonStringObject::PythonStringObject()
{
    PyObject* o = PyUnicode_FromString("");
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonStringObject::PythonStringObject(PyObject* o)
{
    if (o == nullptr || !PyUnicode_Check(o))
    {
        return;
    }
    SetPtr(o);
}

PythonStringObject::PythonStringObject(const std::string& input)
{
    PyObject* o = PyUnicode_FromString(input.c_str());
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonStringObject::PythonStringObject(const char* input)
{
    PyObject* o = PyUnicode_FromString(input);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonBoolObject::PythonBoolObject()
{
    PyObject* o = PyBool_FromLong(0);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonBoolObject::PythonBoolObject(PyObject* o)
{
    PyObject* ret = PyObject_IsTrue(o) ? PyBool_FromLong(1) : PyBool_FromLong(0);
    SetPtr(ret);
    if (ret != nullptr)
    {
        Py_DecRef(ret);
    }
}

PythonBoolObject::PythonBoolObject(const bool& input)
{
    PyObject* o = PyBool_FromLong(input);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonListObject::PythonListObject()
{
    PyObject* o = PyList_New(0);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonListObject::PythonListObject(PyObject* o)
{
    if (o == nullptr || !PyList_Check(o))
    {
        return;
    }
    SetPtr(o);
}

PythonListObject::PythonListObject(size_t size)
{
    PyObject* o = PyList_New(size);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

size_t PythonListObject::Size() const
{
    if (IsBad())
    {
        return 0;
    }
    return PyList_Size(ptr);
}

PythonTupleObject PythonListObject::ToTuple(bool ignore)
{
    if (IsBad())
    {
        return PythonTupleObject(static_cast<PyObject*>(nullptr));
    }

    PyObject* o = PyList_AsTuple(ptr);
    if (o == nullptr && ignore)
    {
        PyErr_Clear();
    }

    PythonTupleObject ret(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
    return ret;
}

PythonTupleObject::PythonTupleObject()
{
    PyObject* o = PyTuple_New(0);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonTupleObject::PythonTupleObject(PyObject* o)
{
    if (o == nullptr || !PyTuple_Check(o))
    {
        return;
    }
    SetPtr(o);
}

size_t PythonTupleObject::Size() const
{
    if (IsBad())
    {
        return 0;
    }
    return PyTuple_Size(ptr);
}

PythonDictObject::PythonDictObject()
{
    PyObject* o = PyDict_New();
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonDictObject::PythonDictObject(PyObject* o)
{
    if (o == nullptr || !PyDict_Check(o))
    {
        return;
    }
    SetPtr(o);
}

PythonSetObject::PythonSetObject()
{
    PyObject* o = PySet_New(nullptr);
    SetPtr(o);
    if (o != nullptr)
    {
        Py_DecRef(o);
    }
}

PythonSetObject::PythonSetObject(PyObject* o)
{
    if (o == nullptr || !PythonObject(o).IsInstance("set"))
    {
        return;
    }
    SetPtr(o);
}

size_t PythonSetObject::Size() const
{
    if (IsBad())
    {
        return 0;
    }
    if (PySet_Size == nullptr)
    {
        return 0;
    }
    return PySet_Size(ptr);
}

void MemScopePythonCall(const std::string& module, const std::string& function)
{
    if (!Utility::IsPyInterpRepeInited())
    {
        LOG_ERROR("Python Interpreter initialization FAILED");
        return;
    }

    Utility::PyInterpGuard stat;
    Utility::PythonObject sys = Utility::PythonObject::Import("sys");
    Utility::PythonObject modules = sys.Get("modules");
    Utility::PythonObject torch = modules.GetItem(Utility::PythonObject("torch"));
    if (!torch.IsBad())
    {
        Utility::PythonObject pythonModule = Utility::PythonObject::Import(module, false);
        if (pythonModule.IsBad())
        {
            LOG_ERROR("import %s FAILED", module.c_str());
            return;
        }

        Utility::PythonObject pythonFunction = pythonModule.Get(function);
        if (pythonFunction.IsBad())
        {
            LOG_ERROR("cannot get function %s", function.c_str());
            return;
        }
        pythonFunction.Call();
    }
    return;
}

}  // namespace Utility

namespace
{
// 字段直读布局自检结果: -1未判定/0不匹配/1匹配;不匹配即永久走属性路径
std::atomic<int> g_fieldLayoutState{-1};
// 编译/运行版本一致性判定结果: -1未判定/0不同族/1同族。用原子状态而非函数内静态对象:
// 本链路处于分配钩子调用链上,日志自身的分配可触发同线程嵌套采集,命中C++函数内静态
// 初始化保护(同线程重入)会自锁;原子状态重入时最多重复计算一次,无锁无等待
std::atomic<int> g_versionState{-1};

// 本文件(含被调helper)用到的libpython弱符号一次校验: 弱符号解析是整体性的(同一libpython),
// 除随3.9引入的帧API外其余符号自2.x起即导出,故一处校验等价于逐处判空;任一缺失即整体退避
bool PySymbolsReady()
{
    return PyEval_GetFrame != nullptr && PyFrame_GetCode != nullptr && PyFrame_GetBack != nullptr &&
           PyFrame_GetLineNumber != nullptr && Py_IncRef != nullptr && Py_DecRef != nullptr &&
           PyObject_GetAttr != nullptr && PyUnicode_AsUTF8 != nullptr && PyUnicode_InternFromString != nullptr &&
           PyErr_Fetch != nullptr && PyErr_Restore != nullptr && PyErr_Clear != nullptr;
}

// 编译期头文件版本与运行时解释器主次版本是否一致: PyCodeObject布局按编译期头定死,
// 3.11/3.12/3.13/3.14各改过一次(3.12加co_version、3.13改localsplus、3.14热字段前置),
// 版本不同族时按字段直读会取到非str对象并触发PyErr_BadArgument
bool VersionMatchesBuild()
{
    const int cached = g_versionState.load(std::memory_order_acquire);
    if (cached >= 0)
    {
        return cached == 1;
    }
    bool matched = true;
    const char* runtime = Py_GetVersion != nullptr ? Py_GetVersion() : nullptr;
    int major = 0;
    int minor = 0;
    // 版本不可知/格式异常时按同族处理,由随后的字段自检兜底
    if (runtime != nullptr && std::sscanf(runtime, "%d.%d", &major, &minor) == 2 &&
        (major != PY_MAJOR_VERSION || minor != PY_MINOR_VERSION))
    {
        matched = false;
    }
    // 原子交换发布判定: 仅抢到未判定状态的一方告警,并发首次采集也只打印一次
    const int previous = g_versionState.exchange(matched ? 1 : 0, std::memory_order_acq_rel);
    if (previous == -1 && !matched)
    {
        LOG_WARN(
            "msmemscope built with Python %d.%d but running on %d.%d: code object layout may mismatch, "
            "fall back to attribute access for python stack capture",
            PY_MAJOR_VERSION, PY_MINOR_VERSION, major, minor);
    }
    return matched;
}

void ReleaseCodeAttrNames(PyObject* filenameName, PyObject* funcnameName)
{
    if (filenameName != nullptr)
    {
        Py_DecRef(filenameName);
    }
    if (funcnameName != nullptr)
    {
        Py_DecRef(funcnameName);
    }
}

// code对象属性名按需创建、不跨调用缓存: interned名对象随解释器销毁,缓存会在宿主
// Py_Finalize重建解释器后悬空(解释器结构可能复用同一地址,指针比对挡不住);
// 快路径定型后本就不需要名对象,故稳态无额外开销,仅属性路径/首次自检承担创建成本
bool CreateCodeAttrNames(PyObject** filenameName, PyObject** funcnameName)
{
    *filenameName = PyUnicode_InternFromString("co_filename");
    *funcnameName = PyUnicode_InternFromString("co_name");
    if (*filenameName != nullptr && *funcnameName != nullptr)
    {
        return true;
    }
    ReleaseCodeAttrNames(*filenameName, *funcnameName);  // 部分成功同样回收
    *filenameName = nullptr;
    *funcnameName = nullptr;
    PyErr_Clear();
    return false;
}

// 取UTF-8视图: 视图随对象存活(调用方须持对象到回调返回后);失败清错并返回空串
const char* Utf8OrEmpty(PyObject* o)
{
    const char* text = o != nullptr ? PyUnicode_AsUTF8(o) : nullptr;
    if (text == nullptr)
    {
        PyErr_Clear();
        return "";
    }
    return text;
}

// 取code对象属性字符串: *owner持有属性对象(调用方DecRef,保证utf8视图在回调期内有效)
const char* CodeAttrUtf8(PyCodeObject* code, PyObject* nameObj, PyObject** owner)
{
    *owner = nameObj != nullptr ? PyObject_GetAttr(reinterpret_cast<PyObject*>(code), nameObj) : nullptr;
    return Utf8OrEmpty(*owner);
}

// 属性值与编译期偏移读出的字段是否同一对象(只比较指针,不解引用)
bool AttrIsField(PyCodeObject* code, PyObject* nameObj, PyObject* fieldValue)
{
    PyObject* value = PyObject_GetAttr(reinterpret_cast<PyObject*>(code), nameObj);
    if (value == nullptr)
    {
        PyErr_Clear();
        return false;
    }
    const bool same = (value == fieldValue);
    Py_DecRef(value);
    return same;
}

// 字段直读布局自检: 编译期偏移读出的co_filename/co_name与属性值指向同一对象,才认为
// 编译/运行版本同族。快路径会同时直读这两个字段,故两个偏移都必须校验——只验一个时,
// 另一个偏移若错位,快路径会把该偏移上的位模式当作PyObject*解引用。
// 版本号相同也不保证布局一致(如3.13t/3.14t自由线程构型的对象头不同),故不能只看版本;
// 调用点保证两个名对象非空(见UseFieldFastPath),此处无需判空
bool FieldLayoutMatches(PyCodeObject* code, PyObject* filenameName, PyObject* funcnameName)
{
    return AttrIsField(code, filenameName, code->co_filename) && AttrIsField(code, funcnameName, code->co_name);
}

// 是否启用字段直读快路径: 首次用真实code对象自检,结果缓存,不匹配永久退化到属性访问。
// 仅在状态未判定时使用两个名对象,定型后传nullptr亦可
bool UseFieldFastPath(PyCodeObject* code, PyObject* filenameName, PyObject* funcnameName)
{
    const int state = g_fieldLayoutState.load(std::memory_order_acquire);
    if (state >= 0)
    {
        return state == 1;
    }
    const bool matched = FieldLayoutMatches(code, filenameName, funcnameName);
    g_fieldLayoutState.store(matched ? 1 : 0, std::memory_order_release);
    return matched;
}

// 宿主pending error暂存: PyErr_Fetch/Restore只做指针转移(不归一化、不执行用户代码),
// 采集期间错误指示器为空;析构Restore覆盖安装时自带旧值释放(3.11及更早换curexc_*后
// XDECREF旧值,3.12+经_PyErr_SetRaisedException释放),采集自产错误随之清除,无需前置PyErr_Clear
struct HostErrorState
{
    PyObject* type{nullptr};
    PyObject* value{nullptr};
    PyObject* traceback{nullptr};

    HostErrorState() { PyErr_Fetch(&type, &value, &traceback); }

    ~HostErrorState() { PyErr_Restore(type, value, traceback); }

    HostErrorState(const HostErrorState&) = delete;
    HostErrorState& operator=(const HostErrorState&) = delete;
};
}  // namespace

#ifdef MSMEMSCOPE_UNIT_TEST
// 仅供单元测试注入,语义见cpython.h声明处
void PyStackCore::SetFieldLayoutStateForTest(int state) { g_fieldLayoutState.store(state, std::memory_order_release); }
#endif

// PyStackCore定义在全局作用域(声明见cpython.h,namespace Utility之外):
// WalkFrames为hal/mstx侧与钩子侧py采集共享的帧走链核心,两方均无前缀调用
uint32_t PyStackCore::WalkFrames(uint32_t maxDepth, PyFrameVisitor visitor, void* ctx)
{
    if (maxDepth == 0 || visitor == nullptr || !PySymbolsReady())
    {
        return 0;
    }
    PyFrameObject* frame = PyEval_GetFrame();
    if (frame == nullptr)
    {
        return 0;
    }
    // 宿主pending error(异常传播/raise/except回收/finalize清理中很常见)暂存到本作用域,
    // 采集期间错误指示器为空,任意出口由析构原样放回;不必因宿主正在异常展开而整体丢弃Python栈
    HostErrorState hostError;
    // 帧串获取: 默认走code对象的co_filename/co_name属性(3.x语义稳定);结构体字段偏移在编译期
    // 定死,运行期解释器版本不同族时会读到非str对象并触发PyErr_BadArgument。版本同族且一次性
    // 自检通过才启用字段直读快路径。PyUnicode_AsUTF8返回借用utf8视图(仅回调期内有效)
    const bool versionMatched = VersionMatchesBuild();
    // 名对象仅在首次自检或属性路径需要
    const bool needAttrNames = !versionMatched || g_fieldLayoutState.load(std::memory_order_acquire) != 1;
    PyObject* filenameName = nullptr;
    PyObject* funcnameName = nullptr;
    if (needAttrNames && !CreateCodeAttrNames(&filenameName, &funcnameName))
    {
        return 0;
    }
    Py_IncRef(reinterpret_cast<PyObject*>(frame));
    uint32_t depth = 0;
    while (frame != nullptr && depth < maxDepth)
    {
        PyCodeObject* code = PyFrame_GetCode(frame);
        if (code == nullptr)
        {
            break;  // 无code不回调: frame强引用随循环出口统一释放
        }
        PyObject* filenameObj = nullptr;
        PyObject* funcnameObj = nullptr;
        const char* filename = nullptr;
        const char* funcname = nullptr;
        if (versionMatched && UseFieldFastPath(code, filenameName, funcnameName))
        {
            filename = Utf8OrEmpty(code->co_filename);
            funcname = Utf8OrEmpty(code->co_name);
        }
        else
        {
            filename = CodeAttrUtf8(code, filenameName, &filenameObj);
            funcname = CodeAttrUtf8(code, funcnameName, &funcnameObj);
        }
        visitor(ctx, code, filename, funcname, PyFrame_GetLineNumber(frame), depth);
        // 属性对象持有到回调返回后: PyUnicode_AsUTF8视图随对象存活
        if (filenameObj != nullptr)
        {
            Py_DecRef(filenameObj);
        }
        if (funcnameObj != nullptr)
        {
            Py_DecRef(funcnameObj);
        }
        // 引用守恒: PyFrame_GetBack返回新引用(3.9+文档定义),直接作下轮frame
        // 并在本轮DecRef原frame,两两平衡,tstate持有的引用不动
        PyFrameObject* prevFrame = PyFrame_GetBack(frame);
        Py_DecRef(reinterpret_cast<PyObject*>(frame));
        frame = prevFrame;
        Py_DecRef(reinterpret_cast<PyObject*>(code));
        depth++;
    }
    if (frame != nullptr)
    {
        Py_DecRef(reinterpret_cast<PyObject*>(frame));  // 截断/无code退出: 释放未消费的强引用
    }
    ReleaseCodeAttrNames(filenameName, funcnameName);
    // 出口兜底由hostError析构完成,采集自产错误随Restore的旧值一并释放;若泄漏pending error,
    // 宿主会在_Py_CheckFunctionResult处报"returned a result with an exception set"
    return depth;
}
