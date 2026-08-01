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