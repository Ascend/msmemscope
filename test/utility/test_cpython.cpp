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
#include <gtest/gtest.h>
#include <gtest/internal/gtest-port.h>

#include <pthread.h>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "utility/cpython.h"

using namespace Utility;

class PythonGlobalInit {
public:
    PythonGlobalInit()
    {
        Py_Initialize();
    }

    ~PythonGlobalInit()
    {
        Py_Finalize();
    }
};

// 使用静态对象，构造/析构时初始化Python，防止多个套件重复初始化和释放导致core dump
// static PythonGlobalInit GlobalPythonInit;

TEST(TestCpython, BeforeInit)
{
    Py_Finalize();
    EXPECT_FALSE(IsPyInterpRepeInited());
    std::string stack;
    PythonCallstack(20, stack);
    EXPECT_EQ(stack, "\"NA\"");
    PythonObject obj;
    EXPECT_TRUE(obj.IsBad());
    PythonObject obj2 = obj.NewRef();
    EXPECT_TRUE(obj2.IsBad());
    Py_Initialize();
}

TEST(TestCpython, PythonObjectFromTo)
{
    // 测试PythonObject的From和To转换
    int32_t inputInt = -42;
    PythonObject objInt(inputInt);
    EXPECT_FALSE(objInt.IsBad());
    EXPECT_EQ(objInt.Type(), "int");
    EXPECT_EQ(objInt.Cast<int32_t>(), inputInt);

    uint32_t inputUint = 56;
    PythonObject objUint(inputUint);
    EXPECT_FALSE(objUint.IsBad());
    EXPECT_EQ(objUint.Type(), "int");
    EXPECT_EQ(objUint.Cast<uint32_t>(), inputUint);

    double inputDouble = 3.14;
    PythonObject objDouble(inputDouble);
    EXPECT_FALSE(objDouble.IsBad());
    EXPECT_EQ(objDouble.Type(), "float");
    EXPECT_DOUBLE_EQ(objDouble.Cast<double>(), inputDouble);

    std::string inputStr = "hello";
    PythonObject objStr(inputStr);
    EXPECT_FALSE(objStr.IsBad());
    EXPECT_EQ(objStr.Type(), "str");
    EXPECT_EQ(objStr.Cast<std::string>(), inputStr);

    const char* inputChar = "world";
    PythonObject objStr1(inputChar);
    EXPECT_FALSE(objStr1.IsBad());
    EXPECT_EQ(objStr1.Type(), "str");
    EXPECT_EQ(objStr1.Cast<std::string>(), std::string(inputChar));

    bool inputBool = true;
    PythonObject objBool(inputBool);
    EXPECT_FALSE(objBool.IsBad());
    EXPECT_EQ(objBool.Type(), "bool");
    EXPECT_TRUE(objBool.Cast<bool>());

    std::vector<std::string> inputVectorStr = {"a", "bb", "ccc", "dddd"};
    PythonObject listStrObj(inputVectorStr);
    EXPECT_FALSE(listStrObj.IsBad());
    EXPECT_EQ(listStrObj.Type(), "list");

    std::vector<int32_t> inputVectorInt = {1, 2, 11, 123};
    PythonObject listIntObj(inputVectorInt);
    EXPECT_FALSE(listIntObj.IsBad());
    EXPECT_EQ(listIntObj.Type(), "list");

    std::map<int32_t, std::string> inputMapIntStr = {{1, "x"}, {2, "y"}, {3, "z"}};
    PythonObject dictIntStrObj(inputMapIntStr);
    EXPECT_FALSE(dictIntStrObj.IsBad());
    EXPECT_EQ(dictIntStrObj.Type(), "dict");

    PythonObject obj1(0);
    PythonObject obj2 = obj1;
    EXPECT_EQ(obj2.Cast<int32_t>(), 0);
}

TEST(TestCpython, PythonObjectImport)
{
    PythonObject sys = PythonObject::Import("sys");
    EXPECT_FALSE(sys.IsBad());
    EXPECT_EQ(sys.Type(), "module");
    EXPECT_EQ(static_cast<PyObject*>(sys), PyImport_ImportModule("sys"));
    PythonObject sys2 = PythonObject::Import("sys", true);
    EXPECT_EQ(static_cast<PyObject*>(sys), PyImport_ImportModule("sys"));
    PythonObject name = PythonObject::GetGlobal("__name__");
    EXPECT_TRUE(name.IsBad());
    PythonObject invalid = PythonObject::Import("invalid", false, false);
    EXPECT_TRUE(invalid.IsBad());
}

TEST(TestCpython, PythonObjectGetAttr)
{
    PythonObject sys = PythonObject::Import("sys");
    PythonObject sysPath = sys.Get("path");
    EXPECT_EQ(sysPath.Type(), "list");
    EXPECT_EQ(sysPath[PythonObject(0)].Type(), "str");

    PythonObject fexit = sys.Get("exit");
    EXPECT_EQ(fexit.Type(), "builtin_function_or_method");
    PythonObject invalid = sys.Get("invalid");
    EXPECT_TRUE(invalid.IsBad());
    EXPECT_TRUE(invalid.Get("attr").IsBad());
    EXPECT_TRUE(invalid.GetItem(PythonObject(0)).IsBad());
    EXPECT_TRUE(sysPath.GetItem(invalid).IsBad());

    std::vector<int> inputVector = {1, 2, 3, 100};
    PythonObject listObj(inputVector);
    PythonObject append = listObj.Get("append");
    EXPECT_EQ(append.Type(), "builtin_function_or_method");

    EXPECT_TRUE(invalid.Get("invalid", false).IsBad());
}

TEST(TestCpython, PythonObjectType)
{
    PythonObject none(Py_None);
    EXPECT_EQ(none.Type(), "NoneType");
    EXPECT_FALSE(none.IsCallable());

    PythonObject pytrue(Py_True);
    EXPECT_EQ(pytrue.Type(), "bool");

    PythonObject builtins(PyImport_ImportModule("builtins"));
    EXPECT_EQ(builtins.Type(), "module");
}

TEST(TestCpython, PythonObjectCall)
{
    PythonObject intClass = PythonObject::Import("builtins").Get("int");
    EXPECT_TRUE(intClass.IsCallable());
    PythonObject intObj = intClass.Call();
    EXPECT_EQ(intObj.Type(), "int");
    EXPECT_EQ(intObj.Cast<int32_t>(), 0);
    
    PythonObject intObj1(1);
    PythonTupleObject args(std::vector<PyObject*>({intObj1}));
    args.Size();
    PythonObject intObj2 = intClass.Call(args);
    EXPECT_EQ(intObj2.Type(), "int");
    EXPECT_EQ(intObj2.Cast<int32_t>(), 1);

    PythonDictObject kws(std::map<std::string, int32_t>({{"base", 10}}));
    PythonObject intObj3 = intClass.Call(args, kws);
    EXPECT_EQ(intObj2.Type(), "int");
    EXPECT_EQ(intObj2.Cast<int32_t>(), 1);

    PythonObject ret = PythonObject::Import("builtins").Call();
    EXPECT_TRUE(ret.IsBad());

    PythonObject invalid;
    EXPECT_TRUE(invalid.Call().IsBad());
    EXPECT_TRUE(invalid.Call(args).IsBad());
    EXPECT_TRUE(invalid.Call(args, kws).IsBad());
    EXPECT_TRUE(intObj1.Call().IsBad());
    EXPECT_TRUE(intObj1.Call(args).IsBad());
    EXPECT_TRUE(intObj1.Call(args, kws).IsBad());
    PythonTupleObject args2(std::vector<PyObject*>({intClass}));
    EXPECT_TRUE(intClass(args2).IsBad());
    EXPECT_TRUE(intClass(args2, kws).IsBad());
}

TEST(TestCpython, PythonNumberObject)
{
    PythonNumberObject o1;
    PythonNumberObject o2(PyLong_FromLong(123));
    PythonNumberObject o3(321);
    PythonNumberObject o4(2.33);
    PythonNumberObject o5(PythonObject("1111").Cast<PyObject*>());

    EXPECT_FALSE(o1.IsBad());
    EXPECT_EQ(o1.Cast<int32_t>(), 0);
    EXPECT_EQ(o2.Cast<int32_t>(), 123);
    EXPECT_EQ(o2.Type(), "int");
    EXPECT_EQ(o3.Type(), "int");
    EXPECT_EQ(o4.Type(), "float");
    EXPECT_TRUE(o5.IsBad());
    EXPECT_TRUE(PythonNumberObject(static_cast<PyObject*>(nullptr)).IsBad());
}

TEST(TestCpython, PythonStringObject)
{
    PythonStringObject o1;
    PythonStringObject o2(PyUnicode_FromString("hello"));
    PythonStringObject o3("OK");
    PythonStringObject o4(std::string("banana"));
    PythonStringObject o5(PythonObject(1));

    EXPECT_EQ(o1.Cast<std::string>(), "");
    EXPECT_EQ(o2.Cast<std::string>(), "hello");
    EXPECT_EQ(o3.Cast<std::string>(), "OK");
    EXPECT_EQ(o4.Cast<std::string>(), "banana");
    EXPECT_TRUE(o5.IsBad());
    EXPECT_TRUE(PythonStringObject(static_cast<PyObject*>(nullptr)).IsBad());
}

TEST(TestCpython, PythonBoolObject)
{
    PythonBoolObject o1;
    PythonBoolObject o2(Py_True);
    PythonBoolObject o3(Py_False);
    PythonBoolObject o4(true);

    EXPECT_EQ(o1.Cast<bool>(), false);
    EXPECT_EQ(o2.Cast<bool>(), true);
    EXPECT_EQ(o3.Cast<bool>(), false);
    EXPECT_EQ(o4.Cast<bool>(), true);
}

TEST(TestCpython, PythonListObject)
{
    PythonListObject empty_list(5);
    PythonListObject sys_path(static_cast<PyObject*>(PythonObject::Import("sys").Get("path")));
    PythonListObject list1(std::vector<int32_t>({1, 3, 5, 7}));
    PythonListObject list2(std::vector<std::vector<int32_t>>({{1, 3, 5, 7}, {2, 4, 6}}));
    PythonListObject list3;

    EXPECT_EQ(empty_list.Size(), 5);
    EXPECT_FALSE(sys_path.IsBad());
    EXPECT_TRUE(sys_path.Size() > 0);
    EXPECT_EQ(sys_path.GetItem<PythonObject>(0).Type(), "str");
    EXPECT_EQ(list1.Size(), 4);
    EXPECT_EQ(list1.GetItem<int32_t>(1), 3);
    EXPECT_EQ(list1.GetItem<std::string>(3), "7");
    EXPECT_EQ(list2.Size(), 2);
    EXPECT_EQ(list2.GetItem<PythonObject>(0).Type(), "list");
    EXPECT_EQ(list2.GetItem<std::string>(1), "[2, 4, 6]");
    EXPECT_EQ(list3.Size(), 0);
    list3.Append(1);
    EXPECT_EQ(list3.Size(), 1);
    list3.Append("2").Append(true);
    EXPECT_EQ(list3.Size(), 3);
    EXPECT_EQ(list3.GetItem<std::string>(1), "2");
    list3.SetItem(1, empty_list);
    EXPECT_EQ(list3.Size(), 3);
    EXPECT_EQ(list3.GetItem<PyObject*>(1), static_cast<PyObject*>(empty_list));
    list3.Insert(0, sys_path);
    EXPECT_EQ(list3.Size(), 4);
    EXPECT_EQ(list3.GetItem<PyObject*>(0), static_cast<PyObject*>(sys_path));
    PythonTupleObject tuple = list3.ToTuple();
    EXPECT_FALSE(tuple.IsBad());
}

TEST(TestCpython, TestGetPyFuncInfoInputNullFrame)
{
    std::string info = "original_info";
    std::string hash = "original_hash";
    GetPyFuncInfo(nullptr, info, hash);
    EXPECT_EQ(info, "original_info");
    EXPECT_EQ(hash, "original_hash");
}

TEST(TestCpython, NotIgnoreCFunc)
{
    EXPECT_FALSE(IsIgnoreCFunc("anyfile.py:some_function"));
    EXPECT_FALSE(IsIgnoreCFunc("contextlib.py:__init__"));
    
    EXPECT_FALSE(IsIgnoreCFunc("otherfile.py:__exit__"));
    EXPECT_FALSE(IsIgnoreCFunc("/path/to/otherfile.py:__exit__"));
}

TEST(TestCpython, TrueMatch)
{
    EXPECT_TRUE(IsIgnoreCFunc("contextlib.py:__exit__"));
    EXPECT_TRUE(IsIgnoreCFunc("dir/contextlib.py:__exit__"));
    EXPECT_TRUE(IsIgnoreCFunc("/full/path/to/contextlib.py:__exit__"));
}

TEST(TestCpython, PyProfileFnNullptrTest)
{
    PyFrameObject* frame = nullptr;
    int ret = pyProfileFn(nullptr, frame, PyTrace_CALL, nullptr);
    ASSERT_EQ(ret, 0);
}

TEST(TestCpython, GetTraceCallStackTest)
{
    std::string type = "test";
    uint64_t time = 123456;
    GetTraceCallStack(type, time);
}

namespace
{
struct WalkedFrame
{
    std::string filename;
    std::string funcname;
    int lineno;
};

// 作用域引用守卫: ASSERT提前返回时也不会漏掉DECREF
class PyOwned
{
   public:
    explicit PyOwned(PyObject* o) : ptr(o) {}
    ~PyOwned()
    {
        Py_XDECREF(ptr);
    }
    operator PyObject*() const
    {
        return ptr;
    }
    bool IsBad() const
    {
        return ptr == nullptr;
    }
    PyOwned(const PyOwned&) = delete;
    PyOwned& operator=(const PyOwned&) = delete;

   private:
    PyObject* ptr;
};

void CollectWalkedFrame(void* ctx, PyCodeObject* /*code*/, const char* filename, const char* funcname, int lineno,
                        uint32_t /*depth*/)
{
    static_cast<std::vector<WalkedFrame>*>(ctx)->push_back(WalkedFrame{filename, funcname, lineno});
}

// 回调内视图仅当帧有效: 立即拷贝为(str,str,int)元组,返回python侧供断言
PyObject* BuildWalkResult(const std::vector<WalkedFrame>& frames)
{
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(frames.size()));
    if (list == nullptr)
    {
        return nullptr;
    }
    for (size_t i = 0; i < frames.size(); ++i)
    {
        PyObject* item = Py_BuildValue("(ssi)", frames[i].filename.c_str(), frames[i].funcname.c_str(),
                                      frames[i].lineno);
        if (item == nullptr)
        {
            Py_DECREF(list);
            return nullptr;
        }
        // PyList_SetItem失败时也已窃取item引用(内部XDecref),此处不可再DECREF
        if (PyList_SetItem(list, static_cast<Py_ssize_t>(i), item) != 0)
        {
            Py_DECREF(list);
            return nullptr;
        }
    }
    return list;
}

// python侧回调入口: 在真实解释器帧链内走链。若WalkFrames泄漏pending error,按原样返回nullptr——
// 解释器会把它暴露成"returned a result with an exception set"(原缺陷的外部表现)
PyObject* PyWalkFrames(PyObject* /*self*/, PyObject* /*args*/)
{
    std::vector<WalkedFrame> frames;
    PyStackCore::WalkFrames(8, CollectWalkedFrame, &frames);
    if (PyErr_Occurred() != nullptr)
    {
        return nullptr;
    }
    return BuildWalkResult(frames);
}

// 宿主异常展开中调用: 采集期间暂存宿主异常,照常走链,返回后原样恢复(类型不变)
PyObject* PyWalkFramesWithPendingError(PyObject* /*self*/, PyObject* /*args*/)
{
    PyErr_SetString(PyExc_RuntimeError, "host exception in flight");
    std::vector<WalkedFrame> frames;
    const uint32_t walked = PyStackCore::WalkFrames(8, CollectWalkedFrame, &frames);
    PyObject* pendingExc = PyErr_Occurred();  // borrowed,指向宿主异常类型
    const int pendingKept = (pendingExc != nullptr) ? 1 : 0;
    const int sameType = (pendingExc == PyExc_RuntimeError) ? 1 : 0;
    const char* firstFunc = frames.empty() ? "" : frames[0].funcname.c_str();
    PyErr_Clear();
    return Py_BuildValue("(iiis)", static_cast<int>(walked), pendingKept, sameType, firstFunc);
}

std::string TupleField(PyObject* tuple, Py_ssize_t field)
{
    if (tuple == nullptr || !PyTuple_Check(tuple))
    {
        PyErr_Clear();
        return "";
    }
    PyObject* item = PyTuple_GetItem(tuple, field);
    if (item == nullptr || !PyUnicode_Check(item))
    {
        PyErr_Clear();
        return "";
    }
    const char* text = PyUnicode_AsUTF8(item);
    if (text == nullptr)
    {
        PyErr_Clear();
        return "";
    }
    return text;
}

std::string FrameField(PyObject* list, Py_ssize_t index, Py_ssize_t field)
{
    PyObject* tuple = PyList_GetItem(list, index);
    if (tuple == nullptr || !PyTuple_Check(tuple))
    {
        PyErr_Clear();
        return "";
    }
    return TupleField(tuple, field);
}
// ===========================================================================
// 覆盖率补充用例辅助设施(均在本匿名namespace内, 供下方新增用例复用)
// ===========================================================================

// 单独过滤运行本文件用例时自举解释器; 不finalize, 与既有用例保持一致
void EnsurePythonReady()
{
    if (!IsPyInterpRepeInited())
    {
        Py_Initialize();
    }
}

// 在给定globals中执行脚本, 失败时打印Python错误
bool ExecScript(const char* script, PyObject* globals)
{
    PyObject* result = PyRun_String(script, Py_file_input, globals, globals);
    if (result == nullptr)
    {
        PyErr_Print();
        return false;
    }
    Py_DECREF(result);
    return true;
}

// 把C函数注册进globals; PyCFunction_New成功后仅借用, 本地引用就地释放
bool RegisterFn(PyObject* globals, const char* name, PyMethodDef* def)
{
    PyObject* fn = PyCFunction_New(def, nullptr);
    if (fn == nullptr)
    {
        return false;
    }
    const int rc = PyDict_SetItemString(globals, name, fn);
    Py_DECREF(fn);
    return rc == 0;
}

// 从globals读整型; 缺失/类型不符时清错并返回-1
long GlobalLong(PyObject* globals, const char* name)
{
    PyObject* item = PyDict_GetItemString(globals, name);
    if (item == nullptr || !PyLong_Check(item))
    {
        PyErr_Clear();
        return -1;
    }
    return PyLong_AsLong(item);
}

// 从globals读字符串; 任何异常分支均清错
std::string GlobalStr(PyObject* globals, const char* name)
{
    PyObject* item = PyDict_GetItemString(globals, name);
    if (item == nullptr || !PyUnicode_Check(item))
    {
        PyErr_Clear();
        return std::string();
    }
    const char* text = PyUnicode_AsUTF8(item);
    if (text == nullptr)
    {
        PyErr_Clear();
        return std::string();
    }
    return std::string(text);
}

// 释放GIL再回补的守卫: worker线程需自行PyGILState_Ensure获取GIL,
// 主线程若持续持有GIL会与join构成死锁
class GilReleaseGuard
{
   public:
    GilReleaseGuard() : state_(PyEval_SaveThread()) {}
    ~GilReleaseGuard() { PyEval_RestoreThread(state_); }
    GilReleaseGuard(const GilReleaseGuard&) = delete;
    GilReleaseGuard& operator=(const GilReleaseGuard&) = delete;

   private:
    PyThreadState* state_;
};

// 采集回调记录: TraceCbFunc按值传参, 这里完整拷贝供断言
struct TraceEventRecord
{
    std::string hash;
    std::string info;
    MemScope::PyTraceType type;
    uint64_t time;
};

std::vector<TraceEventRecord> g_traceEvents;

void RecordTrace(std::string hash, std::string info, MemScope::PyTraceType type, uint64_t time)
{
    g_traceEvents.push_back(TraceEventRecord{hash, info, type, time});
}

// 注册/注销成对: ASSERT提前返回时也不会遗留profile与悬空回调
class TraceCbGuard
{
   public:
    TraceCbGuard() { RegisterTraceCb(RecordTrace); }
    ~TraceCbGuard() { UnRegisterTraceCb(); }
    TraceCbGuard(const TraceCbGuard&) = delete;
    TraceCbGuard& operator=(const TraceCbGuard&) = delete;
};

size_t CountEventsWithInfoPrefix(MemScope::PyTraceType type, const std::string& prefix)
{
    size_t count = 0;
    for (const TraceEventRecord& event : g_traceEvents)
    {
        if (static_cast<int>(event.type) == static_cast<int>(type) && event.info.compare(0, prefix.size(), prefix) == 0)
        {
            ++count;
        }
    }
    return count;
}

bool HasEventWithHashContaining(MemScope::PyTraceType type, const std::string& token)
{
    for (const TraceEventRecord& event : g_traceEvents)
    {
        if (static_cast<int>(event.type) == static_cast<int>(type) && event.hash.find(token) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

// Python侧回调: 帧内调用PythonCallstack, 把栈串带回C++侧断言
PyObject* PyUtCaptureStack(PyObject* /*self*/, PyObject* args)
{
    long depth = 8;
    if (args != nullptr && PyTuple_Check(args) && PyTuple_Size(args) == 1)
    {
        PyObject* first = PyTuple_GetItem(args, 0);
        if (first != nullptr && PyLong_Check(first))
        {
            depth = PyLong_AsLong(first);
        }
    }
    std::string stack;
    PythonCallstack(static_cast<uint32_t>(depth < 0 ? 0 : depth), stack);
    return PyUnicode_FromString(stack.c_str());
}

// Python侧回调: 帧内采集(文件,函数,行)与当前帧globals中的__name__
PyObject* PyUtProbeFuncInfo(PyObject* /*self*/, PyObject* /*args*/)
{
    std::string info;
    std::string hash;
    GetPyFuncInfo(PyEval_GetFrame(), info, hash);
    PythonObject globalName = PythonObject::GetGlobal("__name__");
    const std::string name = globalName.IsBad() ? std::string("<bad>") : globalName.Cast<std::string>();
    return Py_BuildValue("(sss)", info.c_str(), hash.c_str(), name.c_str());
}

// Python侧回调: 指定深度走链, 返回 (walked, [(filename, funcname, lineno), ...])
PyObject* PyUtWalkDepth(PyObject* /*self*/, PyObject* args)
{
    long depth = 8;
    if (args != nullptr && PyTuple_Check(args) && PyTuple_Size(args) == 1)
    {
        PyObject* first = PyTuple_GetItem(args, 0);
        if (first != nullptr && PyLong_Check(first))
        {
            depth = PyLong_AsLong(first);
        }
    }
    std::vector<WalkedFrame> frames;
    const uint32_t walked =
        PyStackCore::WalkFrames(static_cast<uint32_t>(depth < 0 ? 0 : depth), CollectWalkedFrame, &frames);
    PyObject* frameList = BuildWalkResult(frames);
    if (frameList == nullptr)
    {
        return nullptr;
    }
    PyObject* result = PyTuple_New(2);
    if (result == nullptr)
    {
        Py_DECREF(frameList);
        return nullptr;
    }
    PyTuple_SET_ITEM(result, 0, PyLong_FromLong(static_cast<long>(walked)));
    PyTuple_SET_ITEM(result, 1, frameList);
    return result;
}

// Python侧回调: 直接驱动pyProfileFn各分支, 返回
// (start, CALL增量, RETURN增量, C_CALL增量, C_RETURN增量, default增量)
PyObject* PyUtDriveProfile(PyObject* /*self*/, PyObject* args)
{
    std::string mode;
    if (args != nullptr && PyTuple_Check(args) && PyTuple_Size(args) == 1)
    {
        PyObject* first = PyTuple_GetItem(args, 0);
        if (first != nullptr && PyUnicode_Check(first))
        {
            const char* text = PyUnicode_AsUTF8(first);
            if (text != nullptr)
            {
                mode = text;
            }
        }
    }
    PyFrameObject* frame = PyEval_GetFrame();
    const long start = static_cast<long>(g_traceEvents.size());
    long callDelta = 0;
    long returnDelta = 0;
    long ccallDelta = 0;
    long creturnDelta = 0;
    long defaultDelta = 0;
    if (mode == "ignore_exit")
    {
        pyProfileFn(nullptr, frame, PyTrace_C_CALL, Py_None);
        ccallDelta = static_cast<long>(g_traceEvents.size()) - start;
    }
    else
    {
        pyProfileFn(nullptr, frame, PyTrace_CALL, nullptr);
        callDelta = static_cast<long>(g_traceEvents.size()) - start;
        pyProfileFn(nullptr, frame, PyTrace_RETURN, nullptr);
        returnDelta = static_cast<long>(g_traceEvents.size()) - start - callDelta;
        pyProfileFn(nullptr, frame, PyTrace_C_CALL, Py_None);
        ccallDelta = static_cast<long>(g_traceEvents.size()) - start - callDelta - returnDelta;
        pyProfileFn(nullptr, frame, PyTrace_C_RETURN, Py_None);
        creturnDelta = static_cast<long>(g_traceEvents.size()) - start - callDelta - returnDelta - ccallDelta;
        pyProfileFn(nullptr, frame, PyTrace_C_EXCEPTION, nullptr);
        defaultDelta = static_cast<long>(g_traceEvents.size()) - start - callDelta - returnDelta - ccallDelta -
                       creturnDelta;
    }
    return Py_BuildValue("(illlll)", static_cast<int>(start), callDelta, returnDelta, ccallDelta, creturnDelta,
                         defaultDelta);
}

// Python侧回调: 帧内执行GetTraceCallStack, 返回本次新增回调数
PyObject* PyUtTraceCallStack(PyObject* /*self*/, PyObject* /*args*/)
{
    const long before = static_cast<long>(g_traceEvents.size());
    GetTraceCallStack("ut: ", 424242ULL);
    return PyLong_FromLong(static_cast<long>(g_traceEvents.size()) - before);
}

// 命名python线程: 覆盖IsPythonThread=true、Ensure兜底与Check快路径
struct NamedThreadResult
{
    bool finished{false};
    std::string noGilStack;
    std::string frameStack;
    std::string zeroDepthStack;
};

void NamedPythonThreadBody(NamedThreadResult* out)
{
    pthread_setname_np(pthread_self(), "python-ut");
    // 未持GIL: 走PyInterpGuard兜底; 新线程无Python帧 → NA
    PythonCallstack(8, out->noGilStack);
    {
        PyInterpGuard guard;
        PyOwned globals(PyDict_New());
        if (!globals.IsBad())
        {
            static PyMethodDef captureDef = {"_capture_stack", PyUtCaptureStack, METH_VARARGS, nullptr};
            if (RegisterFn(globals, "_capture_stack", &captureDef) &&
                ExecScript("def outer_ut():\n"
                           "    return _capture_stack(8)\n"
                           "_frame_stack = outer_ut()\n"
                           "_zero_stack = _capture_stack(0)\n",
                           globals))
            {
                out->frameStack = GlobalStr(globals, "_frame_stack");
                out->zeroDepthStack = GlobalStr(globals, "_zero_stack");
            }
        }
    }
    out->finished = true;
}

// ---------------------------------------------------------------------------
// 版本判定: GetPyVersion/IsPyVersionAtLeast39 的首次静态初始化路径
// ---------------------------------------------------------------------------
TEST(TestCpython, GetPyVersionAndVersionGate)
{
    EnsurePythonReady();
    const char* runtime = Py_GetVersion();
    ASSERT_NE(runtime, nullptr);
    std::string full(runtime);
    const size_t spacePos = full.find(' ');
    const std::string prefix = spacePos == std::string::npos ? full : full.substr(0, spacePos);
    EXPECT_TRUE(GetPyVersion() == Version(prefix));
    // 受支持运行时均为3.9+, 第二次调用走函数内静态缓存
    EXPECT_TRUE(IsPyVersionAtLeast39());
    EXPECT_TRUE(IsPyVersionAtLeast39());
}

// ---------------------------------------------------------------------------
// PythonObject: const PyObject*构造、自赋值/非自赋值、uint64构造
// ---------------------------------------------------------------------------
TEST(TestCpython, PythonObjectPointerAssignmentAndNumbers)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    const PyObject* constTrue = Py_True;
    PythonObject objFromConstPtr(constTrue);
    EXPECT_EQ(objFromConstPtr.Type(), "bool");
    EXPECT_TRUE(objFromConstPtr.Cast<bool>());

    PythonObject obj(7);
    PythonObject& selfRef = obj;
    obj = selfRef;  // 自赋值: 保留原对象, 不释放自身
    EXPECT_FALSE(obj.IsBad());
    EXPECT_EQ(obj.Cast<int32_t>(), 7);

    PythonObject other(std::string("changed"));
    obj = other;  // 非自赋值: 释放旧引用并接管新对象
    EXPECT_EQ(obj.Cast<std::string>(), "changed");
    EXPECT_EQ(other.Cast<std::string>(), "changed");

    PythonObject bigNumber(static_cast<uint64_t>(18446744073709551615ULL));
    EXPECT_EQ(bigNumber.Cast<uint64_t>(), static_cast<uint64_t>(18446744073709551615ULL));

    PythonNumberObject numberFromU64(static_cast<uint64_t>(9876543210ULL));
    EXPECT_FALSE(numberFromU64.IsBad());
    EXPECT_EQ(numberFromU64.Cast<uint64_t>(), static_cast<uint64_t>(9876543210ULL));

    // 从有效PyObject*构造: 走PyLong_Check成功分支
    PythonNumberObject numberFromObj(static_cast<PyObject*>(bigNumber));
    EXPECT_FALSE(numberFromObj.IsBad());
    EXPECT_EQ(numberFromObj.Cast<uint64_t>(), static_cast<uint64_t>(18446744073709551615ULL));
    PythonObject text(std::string("text"));
    PythonStringObject stringFromObj(static_cast<PyObject*>(text));
    EXPECT_FALSE(stringFromObj.IsBad());
    EXPECT_EQ(stringFromObj.Cast<std::string>(), "text");
}

// ---------------------------------------------------------------------------
// Cast: 类型不符时抛出异常; IsBad/Type 的退化返回
// ---------------------------------------------------------------------------
TEST(TestCpython, PythonObjectCastErrorBranches)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PythonObject bad;
    EXPECT_THROW(bad.Cast<int32_t>(), std::runtime_error);
    EXPECT_THROW(PythonObject("abc").Cast<int32_t>(), std::runtime_error);
    EXPECT_THROW(bad.Cast<uint32_t>(), std::runtime_error);
    EXPECT_THROW(PythonObject("abc").Cast<uint32_t>(), std::runtime_error);
    EXPECT_THROW(bad.Cast<uint64_t>(), std::runtime_error);
    EXPECT_THROW(PythonObject("abc").Cast<uint64_t>(), std::runtime_error);
    EXPECT_THROW(PythonObject(1).Cast<double>(), std::runtime_error);  // int不是float
    EXPECT_THROW(bad.Cast<double>(), std::runtime_error);
    EXPECT_EQ(bad.Cast<std::string>(), "");
    EXPECT_EQ(bad.Type(), "");

    PythonObject intType = PythonObject::Import("builtins").Get("int");
    ASSERT_FALSE(intType.IsBad());
    EXPECT_FALSE(bad.IsInstance(intType));                        // 对象本身为空
    EXPECT_FALSE(PythonObject(1).IsInstance(PythonObject()));     // 类型对象为空
    EXPECT_FALSE(PythonObject(1).IsInstance(PythonObject(2)));    // 右值不是type
    EXPECT_FALSE(PythonObject(1).IsInstance("no_such_type_ut"));  // 按名字取不到类型
}

// ---------------------------------------------------------------------------
// Cast<std::string>: PyObject_Str失败 / PyUnicode_AsUTF8失败
// ---------------------------------------------------------------------------
TEST(TestCpython, PythonObjectCastStringFailureBranches)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PyOwned globals(PyDict_New());
    ASSERT_FALSE(globals.IsBad());
    ASSERT_TRUE(ExecScript("class _BadStr:\n"
                           "    def __str__(self):\n"
                           "        raise RuntimeError('bad str')\n"
                           "_bad_str = _BadStr()\n"
                           "_surrogate = \"\\ud800\"\n",
                           globals));
    PyObject* badStr = PyDict_GetItemString(globals, "_bad_str");
    ASSERT_NE(badStr, nullptr);
    {
        PythonObject obj(badStr);
        EXPECT_EQ(obj.Cast<std::string>(), "");
    }
    PyErr_Clear();  // PyObject_Str失败分支不自行清错, 测试侧清理

    PyObject* surrogate = PyDict_GetItemString(globals, "_surrogate");
    ASSERT_NE(surrogate, nullptr);
    ASSERT_TRUE(PyUnicode_Check(surrogate));
    // 孤立代理项无法编码成UTF-8: PyUnicode_AsUTF8返回空
    EXPECT_EQ(PythonObject(surrogate).Cast<std::string>(), "");
    PyErr_Clear();
}

// ---------------------------------------------------------------------------
// GetItem: 无__getitem__的对象与越界下标, ignore时清错并返回空对象
// ---------------------------------------------------------------------------
TEST(TestCpython, PythonObjectGetItemFailureBranches)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PythonObject nonContainer(123);
    EXPECT_TRUE(nonContainer.GetItem(PythonObject(0)).IsBad());
    EXPECT_EQ(PyErr_Occurred(), nullptr);

    PythonObject numbers(std::vector<int32_t>({1, 2, 3}));
    PythonObject outOfRange = numbers.GetItem(PythonObject(9));
    EXPECT_TRUE(outOfRange.IsBad());
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}

// ---------------------------------------------------------------------------
// Call: 可调用对象执行失败、参数非tuple时的退化语义
// ---------------------------------------------------------------------------
TEST(TestCpython, PythonObjectCallFailureBranches)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    // eval无参调用抛TypeError: ignore时清错
    PythonObject evalFunc = PythonObject::Import("builtins").Get("eval");
    ASSERT_TRUE(evalFunc.IsCallable());
    PythonObject failed = evalFunc.Call();
    EXPECT_TRUE(failed.IsBad());
    EXPECT_EQ(PyErr_Occurred(), nullptr);

    // Call(args): args不是tuple
    PythonObject intClass = PythonObject::Import("builtins").Get("int");
    PythonObject notTuple(1);
    EXPECT_TRUE(intClass.Call(notTuple).IsBad());
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}

// ---------------------------------------------------------------------------
// PythonListObject: 空对象操作、越界读写
// ---------------------------------------------------------------------------
TEST(TestCpython, ListObjectInvalidOperations)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PythonListObject bad(static_cast<PyObject*>(nullptr));
    EXPECT_TRUE(bad.IsBad());
    EXPECT_EQ(bad.Size(), 0u);
    bad.Append(1);  // 空对象上的Append直接返回
    EXPECT_TRUE(bad.IsBad());
    EXPECT_THROW(bad.GetItem<int32_t>(0), std::runtime_error);
    EXPECT_THROW(bad.SetItem(0, 1), std::runtime_error);
    EXPECT_THROW(bad.Insert(0, 1), std::runtime_error);
    PythonTupleObject badTuple = bad.ToTuple();
    EXPECT_TRUE(badTuple.IsBad());

    PythonListObject notList(static_cast<PyObject*>(Py_None));
    EXPECT_TRUE(notList.IsBad());

    PythonListObject valid(std::vector<int32_t>({1, 2, 3}));
    ASSERT_FALSE(valid.IsBad());
    EXPECT_EQ(valid.Size(), 3u);
    EXPECT_THROW(valid.GetItem<int32_t>(3), std::runtime_error);
    EXPECT_THROW(valid.SetItem(3, 9), std::runtime_error);

    // 从有效PyObject*构造: 走PyList_Check成功分支
    PyOwned rawList(PyList_New(0));
    ASSERT_FALSE(rawList.IsBad());
    PythonListObject fromRawList(rawList);
    EXPECT_FALSE(fromRawList.IsBad());
    EXPECT_EQ(fromRawList.Size(), 0u);
}

// ---------------------------------------------------------------------------
// PythonTupleObject: 默认/空指针/非tuple/vector构造与Size退化
// ---------------------------------------------------------------------------
TEST(TestCpython, TupleObjectConstructorsAndSize)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PythonTupleObject empty;
    EXPECT_FALSE(empty.IsBad());
    EXPECT_EQ(empty.Size(), 0u);

    PythonTupleObject fromNull(static_cast<PyObject*>(nullptr));
    EXPECT_TRUE(fromNull.IsBad());
    EXPECT_EQ(fromNull.Size(), 0u);

    PythonTupleObject notTuple(static_cast<PyObject*>(Py_None));
    EXPECT_TRUE(notTuple.IsBad());

    PythonTupleObject fromVector(std::vector<int32_t>({4, 5}));
    ASSERT_FALSE(fromVector.IsBad());
    EXPECT_EQ(fromVector.Size(), 2u);
    EXPECT_EQ(fromVector.GetItem<int32_t>(1), 5);

    PyOwned rawTuple(PyTuple_New(0));
    ASSERT_FALSE(rawTuple.IsBad());
    PythonTupleObject fromRaw(rawTuple);
    EXPECT_FALSE(fromRaw.IsBad());
    EXPECT_EQ(fromRaw.Size(), 0u);
}

// ---------------------------------------------------------------------------
// PythonDictObject: 默认构造、operator[]、默认值、不可哈希key、map构造回滚
// ---------------------------------------------------------------------------
TEST(TestCpython, DictObjectConstructorsAndMapFailures)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PythonDictObject emptyDict;
    ASSERT_FALSE(emptyDict.IsBad());
    emptyDict.Add(std::string("k"), 1);
    EXPECT_EQ(emptyDict[std::string("k")].Cast<int32_t>(), 1);
    EXPECT_EQ(emptyDict.GetItem(std::string("missing"), PythonObject(42)).Cast<int32_t>(), 42);
    EXPECT_TRUE(emptyDict.GetItem(std::string("missing")).IsBad());

    // 不可哈希key: PyDict_SetItem失败, ignore时清错
    PyOwned unhashableKey(PyList_New(0));
    ASSERT_FALSE(unhashableKey.IsBad());
    emptyDict.Add(static_cast<PyObject*>(unhashableKey), 2);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
    EXPECT_TRUE(emptyDict.GetItem(std::string("still_missing")).IsBad());

    PythonDictObject badDict(static_cast<PyObject*>(nullptr));
    EXPECT_TRUE(badDict.IsBad());
    EXPECT_THROW(badDict.Add(std::string("k"), 1), std::runtime_error);
    EXPECT_THROW(badDict.GetItem(std::string("k")), std::runtime_error);

    PythonDictObject notDict(static_cast<PyObject*>(Py_None));
    EXPECT_TRUE(notDict.IsBad());

    PyOwned rawDict(PyDict_New());
    ASSERT_FALSE(rawDict.IsBad());
    PythonDictObject fromRaw(rawDict);
    EXPECT_FALSE(fromRaw.IsBad());

    // map构造: 空key/空value/不可哈希key均回滚为不完整对象
    std::map<PyObject*, PyObject*> badKeyMap;
    badKeyMap[nullptr] = Py_None;
    PythonDictObject dictBadKey(badKeyMap);
    EXPECT_TRUE(dictBadKey.IsBad());

    std::map<PyObject*, PyObject*> badValueMap;
    badValueMap[Py_None] = nullptr;
    PythonDictObject dictBadValue(badValueMap);
    EXPECT_TRUE(dictBadValue.IsBad());

    std::map<PyObject*, PyObject*> unhashableMap;
    unhashableMap[static_cast<PyObject*>(unhashableKey)] = Py_None;
    PythonDictObject dictUnhashable(unhashableMap);
    EXPECT_TRUE(dictUnhashable.IsBad());
    PyErr_Clear();  // PyDict_SetItem失败分支未清错

    std::map<PyObject*, PyObject*> goodMap;
    goodMap[Py_None] = Py_True;
    PythonDictObject dictGood(goodMap);
    ASSERT_FALSE(dictGood.IsBad());
    EXPECT_TRUE(dictGood.GetItem(static_cast<PyObject*>(Py_None)).Cast<bool>());
}

// ---------------------------------------------------------------------------
// PythonSetObject: 默认/非法/合法构造、非法Add、不可哈希元素
// ---------------------------------------------------------------------------
TEST(TestCpython, SetObjectConstructorsAndAdd)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PythonSetObject emptySet;
    ASSERT_FALSE(emptySet.IsBad());
    EXPECT_EQ(emptySet.Size(), 0u);
    emptySet.Add(1);
    EXPECT_EQ(emptySet.Size(), 1u);

    PythonSetObject badSet(static_cast<PyObject*>(nullptr));
    EXPECT_TRUE(badSet.IsBad());
    EXPECT_EQ(badSet.Size(), 0u);
    EXPECT_THROW(badSet.Add(1), std::runtime_error);

    PythonSetObject notSet(static_cast<PyObject*>(Py_None));
    EXPECT_TRUE(notSet.IsBad());

    PyOwned rawSet(PySet_New(nullptr));
    ASSERT_FALSE(rawSet.IsBad());
    PythonSetObject fromRaw(rawSet);
    EXPECT_FALSE(fromRaw.IsBad());
    EXPECT_EQ(fromRaw.Size(), 0u);

    // 不可哈希元素: PySet_Add失败, ignore时清错且元素不落入集合
    PyOwned unhashable(PyList_New(0));
    ASSERT_FALSE(unhashable.IsBad());
    emptySet.Add(static_cast<PyObject*>(unhashable));
    EXPECT_EQ(PyErr_Occurred(), nullptr);
    EXPECT_EQ(emptySet.Size(), 1u);

    PythonSetObject builtinSet(std::set<int32_t>({1, 2, 3}));
    ASSERT_FALSE(builtinSet.IsBad());
    EXPECT_EQ(builtinSet.Size(), 3u);

    std::set<PyObject*> badElementSet;
    badElementSet.insert(nullptr);
    PythonSetObject setBadElement(badElementSet);
    EXPECT_TRUE(setBadElement.IsBad());

    std::set<PyObject*> unhashableSet;
    unhashableSet.insert(static_cast<PyObject*>(unhashable));
    PythonSetObject setUnhashable(unhashableSet);
    EXPECT_TRUE(setUnhashable.IsBad());
    PyErr_Clear();  // PySet_Add失败分支未清错
}

// ---------------------------------------------------------------------------
// WalkFrames: 参数/无帧守卫, 深度截断退出时释放未消费帧
// ---------------------------------------------------------------------------
TEST(TestCpython, WalkFramesGuardsAndDepthLimit)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    std::vector<WalkedFrame> frames;
    EXPECT_EQ(PyStackCore::WalkFrames(0, CollectWalkedFrame, &frames), 0u);
    EXPECT_EQ(PyStackCore::WalkFrames(8, nullptr, &frames), 0u);
    // C++调用栈顶无Python帧: 不产生回调
    EXPECT_EQ(PyStackCore::WalkFrames(8, CollectWalkedFrame, &frames), 0u);
    EXPECT_TRUE(frames.empty());
    EXPECT_EQ(PyErr_Occurred(), nullptr);

    PyOwned globals(PyDict_New());
    ASSERT_FALSE(globals.IsBad());
    static PyMethodDef walkDepthDef = {"_walk_depth", PyUtWalkDepth, METH_VARARGS, nullptr};
    ASSERT_TRUE(RegisterFn(globals, "_walk_depth", &walkDepthDef));
    // 两层帧链只取1帧: 循环因深度上限退出时仍有未消费帧, 必须释放
    ASSERT_TRUE(ExecScript("def inner_ut():\n"
                           "    return _walk_depth(1)\n"
                           "_walk_result = inner_ut()\n",
                           globals));
    PyObject* walkResult = PyDict_GetItemString(globals, "_walk_result");
    ASSERT_NE(walkResult, nullptr);
    ASSERT_TRUE(PyTuple_Check(walkResult));
    ASSERT_EQ(PyTuple_Size(walkResult), 2);
    EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(walkResult, 0)), 1);
    PyObject* frameList = PyTuple_GetItem(walkResult, 1);
    ASSERT_NE(frameList, nullptr);
    ASSERT_TRUE(PyList_Check(frameList));
    ASSERT_EQ(PyList_Size(frameList), 1);
    EXPECT_EQ(FrameField(frameList, 0, 0), "<string>");
    EXPECT_EQ(FrameField(frameList, 0, 1), "inner_ut");
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}

// ---------------------------------------------------------------------------
// WalkFrames: code.co_filename为孤立代理项时PyUnicode_AsUTF8失败, 退化为空文件名
// 用code.replace注入而非直接编译: 避免编译阶段对filename做编码校验
// ---------------------------------------------------------------------------
TEST(TestCpython, WalkFramesSurrogateFilenameFallsBack)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PyOwned globals(PyDict_New());
    ASSERT_FALSE(globals.IsBad());
    static PyMethodDef walkDef = {"walk_frames", PyWalkFrames, METH_NOARGS, nullptr};
    ASSERT_TRUE(RegisterFn(globals, "walk_frames", &walkDef));
    ASSERT_TRUE(ExecScript("import types\n"
                           "def sfn():\n"
                           "    return walk_frames()\n"
                           "_sfn_bad_code = sfn.__code__.replace(co_filename=\"\\ud800\")\n"
                           "_sfn = types.FunctionType(_sfn_bad_code, globals())\n"
                           "_frames_bad_name = _sfn()\n",
                           globals));

    PyObject* frames = PyDict_GetItemString(globals, "_frames_bad_name");
    ASSERT_NE(frames, nullptr);
    ASSERT_TRUE(PyList_Check(frames));
    ASSERT_EQ(PyList_Size(frames), 2);
    // 代理文件名无法转UTF-8: 该帧文件名退化为空串; 模块帧文件名正常
    EXPECT_EQ(FrameField(frames, 0, 0), "");
    EXPECT_EQ(FrameField(frames, 0, 1), "sfn");
    EXPECT_EQ(FrameField(frames, 1, 0), "<string>");
    EXPECT_EQ(FrameField(frames, 1, 1), "<module>");
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}

// ---------------------------------------------------------------------------
// GetPyFuncInfo: 真实帧上的文件名/函数名/行号, 以及帧globals中的全局变量
// ---------------------------------------------------------------------------
TEST(TestCpython, GetPyFuncInfoInRealFrame)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PyOwned globals(PyDict_New());
    ASSERT_FALSE(globals.IsBad());
    PyOwned moduleName(PyUnicode_FromString("utmod"));
    ASSERT_FALSE(moduleName.IsBad());
    ASSERT_EQ(PyDict_SetItemString(globals, "__name__", moduleName), 0);
    static PyMethodDef probeDef = {"_probe", PyUtProbeFuncInfo, METH_NOARGS, nullptr};
    ASSERT_TRUE(RegisterFn(globals, "_probe", &probeDef));
    // 先摘掉可能已导入的真实torch: GetPyFuncInfo的moduleCode静态量只在torch可取到时初始化,
    // 若被真实torch抢先初始化, 后续伪torch用例的code同一性判定将永远不成立
    ASSERT_TRUE(ExecScript("import sys\n"
                           "sys.modules.pop('torch', None)\n"
                           "def probe_fn():\n"
                           "    return _probe()\n",
                           globals));
    PyObject* probeFn = PyDict_GetItemString(globals, "probe_fn");
    ASSERT_NE(probeFn, nullptr);
    PyOwned probeResult(PyObject_CallObject(probeFn, nullptr));
    ASSERT_FALSE(probeResult.IsBad());
    ASSERT_TRUE(PyTuple_Check(probeResult));
    ASSERT_EQ(PyTuple_Size(probeResult), 3);
    const std::string info = TupleField(probeResult, 0);
    const std::string hash = TupleField(probeResult, 1);
    const std::string globalName = TupleField(probeResult, 2);
    EXPECT_EQ(hash, "<string>:probe_fn");
    EXPECT_NE(info.find("<string>("), std::string::npos);
    EXPECT_NE(info.find("): probe_fn"), std::string::npos);
    EXPECT_EQ(globalName, "utmod");
}

// ---------------------------------------------------------------------------
// GetPyFuncInfo: nn.Module.__call__帧被改写为 nn.Module:<类名>
// 被执行代码取模块级frame(f_locals即globals字典): 3.13起函数帧f_locals为代理对象,
// 生产代码按dict读取, 故这里只让"__call__.__code__"与模块code同对象来命中改写分支
// ---------------------------------------------------------------------------
TEST(TestCpython, GetPyFuncInfoTorchModuleFrame)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PyOwned globals(PyDict_New());
    ASSERT_FALSE(globals.IsBad());
    static PyMethodDef probeDef = {"_probe", PyUtProbeFuncInfo, METH_NOARGS, nullptr};
    ASSERT_TRUE(RegisterFn(globals, "_probe", &probeDef));
    ASSERT_TRUE(ExecScript("import sys, types\n"
                           "class _SelfStub:\n"
                           "    pass\n"
                           "self = _SelfStub()\n"
                           "_call_code = compile('_module_info = _probe()\\n', '<fake_torch_module>', 'exec')\n"
                           "_call_fn = types.FunctionType(_call_code, globals())\n"
                           "_fake_torch = types.ModuleType('torch')\n"
                           "_fake_torch.nn = types.SimpleNamespace(Module=types.SimpleNamespace(__call__=_call_fn))\n"
                           "sys.modules['torch'] = _fake_torch\n"
                           "exec(_call_code, globals())\n"
                           "del sys.modules['torch']\n",
                           globals));
    PyObject* result = PyDict_GetItemString(globals, "_module_info");
    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(PyTuple_Check(result));
    ASSERT_EQ(PyTuple_Size(result), 3);
    const std::string info = TupleField(result, 0);
    const std::string hash = TupleField(result, 1);
    EXPECT_EQ(hash, "nn.Module: _SelfStub");
    EXPECT_EQ(info, hash);
    // 伪模块已移除: fromcache查询应落空
    EXPECT_TRUE(PythonObject::Import("torch", true).IsBad());
}

// ---------------------------------------------------------------------------
// RegisterTraceCb/UnRegisterTraceCb: 注册期懒初始化、真实profile事件、注销后停止采集
// ---------------------------------------------------------------------------
TEST(TestCpython, TraceCbRegisterUnregisterLifecycle)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PyOwned globals(PyDict_New());
    ASSERT_FALSE(globals.IsBad());
    ASSERT_TRUE(ExecScript("import sys, types\n"
                           "_lazy_calls = []\n"
                           "_fake_torch_npu = types.ModuleType('torch_npu')\n"
                           "_fake_torch_npu.npu = types.SimpleNamespace()\n"
                           "_fake_torch_npu.npu._lazy_init = lambda: _lazy_calls.append(1)\n"
                           "sys.modules['torch_npu'] = _fake_torch_npu\n"
                           "def traced_fn(value):\n"
                           "    return len(str(value))\n",
                           globals));
    g_traceEvents.clear();
    {
        TraceCbGuard traceGuard;  // 注册期: torch_npu懒初始化+全线程安装profile
        ASSERT_TRUE(ExecScript("_traced_result = traced_fn(12345)\n"
                               "try:\n"
                               "    int('not-a-number')\n"
                               "except ValueError:\n"
                               "    pass\n",
                               globals));
    }
    // 伪torch_npu的_lazy_init在注册期被调用一次
    PyObject* lazyCalls = PyDict_GetItemString(globals, "_lazy_calls");
    ASSERT_NE(lazyCalls, nullptr);
    ASSERT_TRUE(PyList_Check(lazyCalls));
    EXPECT_EQ(PyList_Size(lazyCalls), 1);
    EXPECT_EQ(GlobalLong(globals, "_traced_result"), 5);

    // Python函数调用/返回与C函数调用/返回均被采集
    size_t pyCallCount = 0;
    bool sawPyReturn = false;
    for (const TraceEventRecord& event : g_traceEvents)
    {
        if (static_cast<int>(event.type) == static_cast<int>(MemScope::PyTraceType::PYCALL) &&
            event.hash == "<string>:traced_fn")
        {
            ++pyCallCount;
        }
        if (static_cast<int>(event.type) == static_cast<int>(MemScope::PyTraceType::PYRETURN) &&
            event.hash == "<string>:traced_fn")
        {
            sawPyReturn = true;
        }
    }
    EXPECT_GE(pyCallCount, 1u);
    EXPECT_TRUE(sawPyReturn);
    EXPECT_TRUE(HasEventWithHashContaining(MemScope::PyTraceType::CCALL, "len"));
    // C_RETURN的hash是C函数对象本身: str(len)为"<built-in function len>"
    EXPECT_TRUE(HasEventWithHashContaining(MemScope::PyTraceType::CRETURN, "len"));

    // 注销后不再采集: 后续Python执行不产生新事件
    const size_t eventsAfterUnregister = g_traceEvents.size();
    ASSERT_TRUE(ExecScript("traced_fn(7)\n", globals));
    EXPECT_EQ(g_traceEvents.size(), eventsAfterUnregister);

    ASSERT_TRUE(ExecScript("if 'torch_npu' in sys.modules:\n"
                           "    del sys.modules['torch_npu']\n",
                           globals));
}

// ---------------------------------------------------------------------------
// pyProfileFn: 直接驱动CALL/RETURN/C_CALL/C_RETURN/default及contextlib忽略分支
// ---------------------------------------------------------------------------
TEST(TestCpython, PyProfileFnDirectBranches)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PyOwned globals(PyDict_New());
    ASSERT_FALSE(globals.IsBad());
    static PyMethodDef driveDef = {"_drive_profile", PyUtDriveProfile, METH_VARARGS, nullptr};
    ASSERT_TRUE(RegisterFn(globals, "_drive_profile", &driveDef));
    g_traceEvents.clear();
    {
        TraceCbGuard traceGuard;  // 供pyProfileFn拿到非空callFunc
        ASSERT_TRUE(ExecScript("def drive_fn():\n"
                               "    return _drive_profile('normal')\n",
                               globals));
        PyObject* driveFn = PyDict_GetItemString(globals, "drive_fn");
        ASSERT_NE(driveFn, nullptr);
        PyOwned result(PyObject_CallObject(driveFn, nullptr));
        ASSERT_FALSE(result.IsBad());
        ASSERT_TRUE(PyTuple_Check(result));
        ASSERT_EQ(PyTuple_Size(result), 6);
        const long start = PyLong_AsLong(PyTuple_GetItem(result, 0));
        EXPECT_GE(start, 0);
        EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(result, 1)), 1);  // CALL: 1条
        EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(result, 2)), 1);  // RETURN: 1条
        EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(result, 3)), 2);  // C_CALL: 2条
        EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(result, 4)), 2);  // C_RETURN: 2条
        EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(result, 5)), 0);  // default: 不回调

        ASSERT_GE(g_traceEvents.size(), static_cast<size_t>(start) + 6);
        const TraceEventRecord& callEvent = g_traceEvents[static_cast<size_t>(start)];
        EXPECT_EQ(static_cast<int>(callEvent.type), static_cast<int>(MemScope::PyTraceType::PYCALL));
        EXPECT_EQ(callEvent.hash, "<string>:drive_fn");
        EXPECT_NE(callEvent.info.find("): drive_fn"), std::string::npos);
        const TraceEventRecord& returnEvent = g_traceEvents[static_cast<size_t>(start) + 1];
        EXPECT_EQ(static_cast<int>(returnEvent.type), static_cast<int>(MemScope::PyTraceType::PYRETURN));
        EXPECT_EQ(returnEvent.hash, "<string>:drive_fn");
        const TraceEventRecord& ccallInfoEvent = g_traceEvents[static_cast<size_t>(start) + 2];
        EXPECT_EQ(static_cast<int>(ccallInfoEvent.type), static_cast<int>(MemScope::PyTraceType::CCALL));
        EXPECT_EQ(ccallInfoEvent.hash, "None");  // C函数str(arg)即"None"
        EXPECT_NE(ccallInfoEvent.info.find("): drive_fn"), std::string::npos);
        const TraceEventRecord& ccallSelfEvent = g_traceEvents[static_cast<size_t>(start) + 3];
        EXPECT_EQ(static_cast<int>(ccallSelfEvent.type), static_cast<int>(MemScope::PyTraceType::CCALL));
        EXPECT_EQ(ccallSelfEvent.hash, "None");
        EXPECT_EQ(ccallSelfEvent.info, "None");
        const TraceEventRecord& creturnSelfEvent = g_traceEvents[static_cast<size_t>(start) + 4];
        EXPECT_EQ(static_cast<int>(creturnSelfEvent.type), static_cast<int>(MemScope::PyTraceType::CRETURN));
        EXPECT_EQ(creturnSelfEvent.hash, "None");
        EXPECT_EQ(creturnSelfEvent.info, "None");
        const TraceEventRecord& creturnInfoEvent = g_traceEvents[static_cast<size_t>(start) + 5];
        EXPECT_EQ(static_cast<int>(creturnInfoEvent.type), static_cast<int>(MemScope::PyTraceType::CRETURN));
        EXPECT_EQ(creturnInfoEvent.hash, "None");
        EXPECT_NE(creturnInfoEvent.info.find("): drive_fn"), std::string::npos);

        // contextlib.py:__exit__ 帧发起的C调用必须被过滤, 不产生回调
        PyOwned exitName(PyUnicode_FromString("utdir/contextlib.py"));
        ASSERT_FALSE(exitName.IsBad());
        PyOwned exitCode(Py_CompileStringObject("def __exit__(*args):\n"
                                                "    return _drive_profile('ignore_exit')\n",
                                                exitName, Py_file_input, nullptr, -1));
        ASSERT_FALSE(exitCode.IsBad());
        PyOwned exitEval(PyEval_EvalCode(exitCode, globals, globals));
        ASSERT_FALSE(exitEval.IsBad());
        PyObject* exitFn = PyDict_GetItemString(globals, "__exit__");
        ASSERT_NE(exitFn, nullptr);
        PyOwned ignoreResult(PyObject_CallObject(exitFn, nullptr));
        ASSERT_FALSE(ignoreResult.IsBad());
        ASSERT_TRUE(PyTuple_Check(ignoreResult));
        ASSERT_EQ(PyTuple_Size(ignoreResult), 6);
        for (Py_ssize_t i = 1; i < 6; ++i)
        {
            EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(ignoreResult, i)), 0);
        }
    }
}

// ---------------------------------------------------------------------------
// GetTraceCallStack: 真实帧走链; 超过128层上限时截断并释放剩余帧
// ---------------------------------------------------------------------------
TEST(TestCpython, GetTraceCallStackDeepFrameChain)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PyOwned globals(PyDict_New());
    ASSERT_FALSE(globals.IsBad());
    static PyMethodDef traceDef = {"_trace_stack", PyUtTraceCallStack, METH_NOARGS, nullptr};
    ASSERT_TRUE(RegisterFn(globals, "_trace_stack", &traceDef));
    g_traceEvents.clear();
    {
        TraceCbGuard traceGuard;
        ASSERT_TRUE(ExecScript("def _rec_ut(n):\n"
                               "    if n == 0:\n"
                               "        return _trace_stack()\n"
                               "    return _rec_ut(n - 1)\n"
                               "_walked = _rec_ut(140)\n",
                               globals));
    }
    // 140层递归远大于128上限: 循环截断后剩余帧必须释放
    EXPECT_EQ(GlobalLong(globals, "_walked"), 129);
    EXPECT_EQ(CountEventsWithInfoPrefix(MemScope::PyTraceType::PYCALL, "ut: "), 129u);
}

// ---------------------------------------------------------------------------
// MemScopePythonCall: torch缺席跳过、正常调用、模块/函数缺失只打日志
// ---------------------------------------------------------------------------
TEST(TestCpython, MemScopePythonCallBranches)
{
    EnsurePythonReady();
    PyInterpGuard guard;
    PyOwned globals(PyDict_New());
    ASSERT_FALSE(globals.IsBad());
    ASSERT_TRUE(ExecScript("import sys, types\n"
                           "_target = types.ModuleType('msms_ut_call_mod')\n"
                           "_target.calls = []\n"
                           "exec(\"def ping():\\n    calls.append(1)\\n\", _target.__dict__)\n"
                           "sys.modules['msms_ut_call_mod'] = _target\n",
                           globals));
    // torch未加载: 直接跳过, 不触碰目标模块
    MemScopePythonCall("msms_ut_call_mod", "ping");
    ASSERT_TRUE(ExecScript("_count = len(sys.modules['msms_ut_call_mod'].calls)\n", globals));
    EXPECT_EQ(GlobalLong(globals, "_count"), 0);

    ASSERT_TRUE(ExecScript("sys.modules['torch'] = types.ModuleType('torch')\n", globals));
    MemScopePythonCall("msms_ut_call_mod", "ping");
    ASSERT_TRUE(ExecScript("_count = len(sys.modules['msms_ut_call_mod'].calls)\n", globals));
    EXPECT_EQ(GlobalLong(globals, "_count"), 1);

    // 目标函数不存在 / 目标模块导入失败: 只打日志, 不再调用
    MemScopePythonCall("msms_ut_call_mod", "no_such_function");
    MemScopePythonCall("msms_ut_absent_module", "ping");
    ASSERT_TRUE(ExecScript("_count = len(sys.modules['msms_ut_call_mod'].calls)\n", globals));
    EXPECT_EQ(GlobalLong(globals, "_count"), 1);

    ASSERT_TRUE(ExecScript("del sys.modules['torch']\n"
                           "del sys.modules['msms_ut_call_mod']\n",
                           globals));
}

// ---------------------------------------------------------------------------
// PythonCallstack: 线程名含python时, Ensure兜底(无GIL)与Check快路径(持GIL)两条链
// ---------------------------------------------------------------------------
TEST(TestCpython, PythonCallstackFromNamedPythonThread)
{
    EnsurePythonReady();
    NamedThreadResult result;
    {
        // 主线程持有GIL时worker的PyGILState_Ensure会阻塞, 必须先让出GIL
        GilReleaseGuard release;
        std::thread worker(NamedPythonThreadBody, &result);
        worker.join();
    }
    EXPECT_TRUE(result.finished);
    // 未持GIL走Ensure兜底: 新线程无Python帧 → NA
    EXPECT_EQ(result.noGilStack, "\"NA\"");
    // 深度0: 走链返回0帧 → NA
    EXPECT_EQ(result.zeroDepthStack, "\"NA\"");
    // 持GIL且在Python帧内走Check快路径: 栈串包含真实帧
    EXPECT_NE(result.frameStack.find("\"<string>("), std::string::npos);
    EXPECT_NE(result.frameStack.find("): outer_ut"), std::string::npos);
    EXPECT_NE(result.frameStack.find("): <module>"), std::string::npos);
    ASSERT_FALSE(result.frameStack.empty());
    EXPECT_EQ(result.frameStack[result.frameStack.size() - 1], '"');
}

}  // namespace

// 回归: WalkFrames在真实帧链上取到的filename/funcname正确,且采集边界内产生的Python错误
// 不得外泄(旧实现字段直读跨版本取到非str对象时,type error逃逸成宿主SystemError)
TEST(TestCpython, WalkFramesInRealFrameChain)
{
    if (!IsPyInterpRepeInited())
    {
        Py_Initialize();  // 单独过滤跑本用例时自举;不finalize,避免影响同进程其他用例
    }
    PyInterpGuard guard;  // 已持GIL时Ensure幂等
    static PyMethodDef walkDef = {"walk_frames", PyWalkFrames, METH_NOARGS, nullptr};
    static PyMethodDef walkPendingDef = {"walk_frames_pending", PyWalkFramesWithPendingError, METH_NOARGS, nullptr};
    PyOwned globals(PyDict_New());
    PyOwned walkFn(PyCFunction_New(&walkDef, nullptr));
    PyOwned walkPendingFn(PyCFunction_New(&walkPendingDef, nullptr));
    ASSERT_FALSE(globals.IsBad());
    ASSERT_FALSE(walkFn.IsBad());
    ASSERT_FALSE(walkPendingFn.IsBad());
    ASSERT_EQ(PyDict_SetItemString(globals, "walk_frames", walkFn), 0);
    ASSERT_EQ(PyDict_SetItemString(globals, "walk_frames_pending", walkPendingFn), 0);

    // 帧链固定为outer→<module>两帧,filename均为"<string>";第二次采集走已定型的快路径
    // (不再创建名对象),覆盖"快路径不依赖名对象"的分支
    const char* script =
        "def outer():\n"
        "    return walk_frames()\n"
        "_frames = outer()\n"
        "_frames2 = outer()\n"
        "_pending = walk_frames_pending()\n";
    PyOwned result(PyRun_String(script, Py_file_input, globals, globals));
    if (result.IsBad())
    {
        PyErr_Print();  // 泄漏pending error时这里打印的即为原始TypeError/SystemError
        FAIL() << "WalkFrames leaked a pending Python error to the interpreter";
    }

    PyObject* frames = PyDict_GetItemString(globals, "_frames");  // borrowed
    ASSERT_NE(frames, nullptr);
    ASSERT_TRUE(PyList_Check(frames));
    ASSERT_EQ(PyList_Size(frames), 2);
    EXPECT_EQ(FrameField(frames, 0, 0), "<string>");
    EXPECT_EQ(FrameField(frames, 0, 1), "outer");
    EXPECT_EQ(FrameField(frames, 1, 0), "<string>");
    EXPECT_EQ(FrameField(frames, 1, 1), "<module>");

    PyObject* frames2 = PyDict_GetItemString(globals, "_frames2");  // borrowed
    ASSERT_NE(frames2, nullptr);
    ASSERT_TRUE(PyList_Check(frames2));
    ASSERT_EQ(PyList_Size(frames2), 2);
    EXPECT_EQ(FrameField(frames2, 0, 1), "outer");
    EXPECT_EQ(FrameField(frames2, 1, 1), "<module>");

    PyObject* pending = PyDict_GetItemString(globals, "_pending");  // borrowed
    ASSERT_NE(pending, nullptr);
    ASSERT_TRUE(PyTuple_Check(pending));
    EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(pending, 0)), 1);  // 异常展开中仍完成走链
    EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(pending, 1)), 1);  // 宿主异常仍在,未被吞掉
    EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(pending, 2)), 1);  // 异常类型未被改写
    EXPECT_EQ(TupleField(pending, 3), "<module>");  // 帧内容有效

    // 强制属性回退: 同版本运行自检必然通过,不注入则CodeAttrUtf8/CreateCodeAttrNames
    // 分支零覆盖;注入期间重复同一组帧内容断言
    PyStackCore::SetFieldLayoutStateForTest(0);
    PyOwned attrResult(PyRun_String("_frames_attr = outer()\n", Py_file_input, globals, globals));
    PyStackCore::SetFieldLayoutStateForTest(-1);  // 恢复自动判定,不影响后续用例
    if (attrResult.IsBad())
    {
        PyErr_Print();
        FAIL() << "attribute fallback path leaked a pending Python error";
    }
    PyObject* framesAttr = PyDict_GetItemString(globals, "_frames_attr");  // borrowed
    ASSERT_NE(framesAttr, nullptr);
    ASSERT_TRUE(PyList_Check(framesAttr));
    ASSERT_EQ(PyList_Size(framesAttr), 2);
    EXPECT_EQ(FrameField(framesAttr, 0, 0), "<string>");
    EXPECT_EQ(FrameField(framesAttr, 0, 1), "outer");
    EXPECT_EQ(FrameField(framesAttr, 1, 0), "<string>");
    EXPECT_EQ(FrameField(framesAttr, 1, 1), "<module>");
}
