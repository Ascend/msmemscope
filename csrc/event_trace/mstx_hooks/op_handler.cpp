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
#include "op_handler.h"

#include <cstring>
#include <map>
#include <set>

#include "cpython.h"
#include "log.h"
#include "securec.h"
#include "ustring.h"
#include "utils.h"

namespace MemScope
{

SanitizerOpHandler& SanitizerOpHandler::GetInstance()
{
    static SanitizerOpHandler instance;
    return instance;
}

bool SanitizerOpHandler::sanitizerEnabled_ = false;

void SanitizerOpHandler::SetEnabled(bool enabled) { sanitizerEnabled_ = enabled; }

bool SanitizerOpHandler::IsEnabled() { return sanitizerEnabled_; }

bool SanitizerOpHandler::ExtractField(const char* msg, const std::string& key, std::string& value)
{
    std::string msgString(msg);
    std::string searchKey = key + "=";
    size_t startPos = msgString.find(searchKey);
    if (startPos == std::string::npos)
    {
        return false;
    }
    startPos += searchKey.length();
    size_t endPos = msgString.find_first_of(";", startPos);
    if (endPos == std::string::npos)
    {
        endPos = msgString.length();
    }
    value = msgString.substr(startPos, endPos - startPos);
    return true;
}

bool SanitizerOpHandler::ParseAccessItem(const std::string& item, MemoryAccessItem& out)
{
    // 格式: alias:addr:size
    size_t firstColon = item.find(':');
    if (firstColon == std::string::npos || firstColon == 0)
    {
        LOG_WARN("[SanitizerOpHandler] Invalid access item format (missing first colon): %s", item.c_str());
        return false;
    }
    size_t secondColon = item.find(':', firstColon + 1);
    if (secondColon == std::string::npos || secondColon == item.length() - 1)
    {
        LOG_WARN("[SanitizerOpHandler] Invalid access item format (missing second colon): %s", item.c_str());
        return false;
    }

    // 提取 alias
    std::string alias = item.substr(0, firstColon);
    // 提取 addr
    std::string addrStr = item.substr(firstColon + 1, secondColon - firstColon - 1);
    // 提取 size
    std::string sizeStr = item.substr(secondColon + 1);

    // 复制 alias（最多31字符 + '\0'）
    size_t aliasLen = alias.length() > 31 ? 31 : alias.length();
    if (memcpy_s(out.alias, sizeof(out.alias), alias.c_str(), aliasLen) != EOK)
    {
        LOG_WARN("[SanitizerOpHandler] memcpy_s failed for alias");
        return false;
    }
    out.alias[aliasLen] = '\0';

    // 解析 addr（支持十进制和十六进制）
    if (!Utility::StrToUint64(out.ptr, addrStr))
    {
        LOG_WARN("[SanitizerOpHandler] Invalid addr value: %s", addrStr.c_str());
        return false;
    }

    // 解析 size
    if (!Utility::StrToUint64(out.size, sizeStr))
    {
        LOG_WARN("[SanitizerOpHandler] Invalid size value: %s", sizeStr.c_str());
        return false;
    }

    return true;
}

std::vector<MemoryAccessItem> SanitizerOpHandler::ParseAccessList(const std::string& listStr)
{
    std::vector<MemoryAccessItem> result;
    if (listStr.empty())
    {
        return result;
    }

    std::string remaining = listStr;
    size_t commaPos;
    while ((commaPos = remaining.find(',')) != std::string::npos)
    {
        std::string item = remaining.substr(0, commaPos);
        MemoryAccessItem accessItem;
        if (ParseAccessItem(item, accessItem))
        {
            result.push_back(accessItem);
        }
        remaining = remaining.substr(commaPos + 1);
    }
    // 处理最后一项（或仅有一项的情况）
    if (!remaining.empty())
    {
        MemoryAccessItem accessItem;
        if (ParseAccessItem(remaining, accessItem))
        {
            result.push_back(accessItem);
        }
    }

    return result;
}

void SanitizerOpHandler::TriggerKernelLaunch(const std::string& name, int32_t streamId,
                                             const std::vector<MemoryAccessItem>& reads,
                                             const std::vector<MemoryAccessItem>& writes)
{
    if (!Utility::IsPyInterpRepeInited())
    {
        LOG_ERROR("[SanitizerOpHandler] Python Interpreter initialization FAILED, skip kernel launch");
        return;
    }

    Utility::PyInterpGuard stat;

    // 导入 torch_npu.npu._sanitizer 模块
    Utility::PythonObject sanitizerMod = Utility::PythonObject::Import("torch_npu.npu._sanitizer", false);
    if (sanitizerMod.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to import torch_npu.npu._sanitizer");
        return;
    }

    // 获取 npu_sanitizer.event_handler
    Utility::PythonObject npuSanitizer = sanitizerMod.Get("npu_sanitizer");
    if (npuSanitizer.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to get npu_sanitizer instance");
        return;
    }

    Utility::PythonObject eventHandler = npuSanitizer.Get("event_handler");
    if (eventHandler.IsBad() || eventHandler.GetPtr() == Py_None)
    {
        LOG_WARN("[SanitizerOpHandler] event_handler not initialized, skip kernel launch for op '%s'", name.c_str());
        return;
    }

    Utility::PythonObject handleFunc = eventHandler.Get("_handle_kernel_launch");
    if (handleFunc.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to get _handle_kernel_launch method");
        return;
    }

    // 构建 read / write 的地址集合和 tensor_aliases 字典
    std::set<uint64_t> readPtrs;
    std::set<uint64_t> writePtrs;
    std::map<uint64_t, std::vector<std::string>> tensorAliases;

    for (const auto& item : reads)
    {
        readPtrs.insert(item.ptr);
        tensorAliases[item.ptr].push_back(std::string(item.alias));
    }
    for (const auto& item : writes)
    {
        writePtrs.insert(item.ptr);
        tensorAliases[item.ptr].push_back(std::string(item.alias));
    }

    // read_only = readPtrs - writePtrs (in-place 视为 write)
    std::set<uint64_t> readOnly;
    for (auto p : readPtrs)
    {
        if (writePtrs.find(p) == writePtrs.end())
        {
            readOnly.insert(p);
        }
    }

    // 构建 Python set: read_only
    PyObject* pyReadOnly = PySet_New(nullptr);
    if (pyReadOnly == nullptr)
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to create read_only set");
        return;
    }
    for (auto p : readOnly)
    {
        PyObject* pyVal = PyLong_FromUnsignedLongLong(p);
        PySet_Add(pyReadOnly, pyVal);
        Py_DecRef(pyVal);
    }

    // 构建 Python set: read_write
    PyObject* pyReadWrite = PySet_New(nullptr);
    if (pyReadWrite == nullptr)
    {
        Py_DecRef(pyReadOnly);
        LOG_ERROR("[SanitizerOpHandler] Failed to create read_write set");
        return;
    }
    for (auto p : writePtrs)
    {
        PyObject* pyVal = PyLong_FromUnsignedLongLong(p);
        PySet_Add(pyReadWrite, pyVal);
        Py_DecRef(pyVal);
    }

    // outputs 和 storage_dataptrs_accessed 传空集合
    PyObject* pyOutputs = PySet_New(nullptr);
    PyObject* pyStorageDataptrs = PySet_New(nullptr);

    // 构建 tensor_aliases: {ptr: [alias, ...]}
    PyObject* pyTensorAliases = PyDict_New();
    for (const auto& kv : tensorAliases)
    {
        PyObject* pyAliases = PyList_New(kv.second.size());
        for (size_t i = 0; i < kv.second.size(); ++i)
        {
            PyList_SetItem(pyAliases, i, PyUnicode_FromString(kv.second[i].c_str()));
        }
        PyObject* pyKey = PyLong_FromUnsignedLongLong(kv.first);
        PyDict_SetItem(pyTensorAliases, pyKey, pyAliases);
        Py_DecRef(pyKey);
        Py_DecRef(pyAliases);
    }

    // 组装 args 元组: (stream, read_only, read_write, outputs, operator, tensor_aliases,
    //                    storage_dataptrs_accessed)
    PyObject* args = PyTuple_New(7);
    if (args == nullptr)
    {
        Py_DecRef(pyReadOnly);
        Py_DecRef(pyReadWrite);
        Py_DecRef(pyOutputs);
        Py_DecRef(pyStorageDataptrs);
        Py_DecRef(pyTensorAliases);
        LOG_ERROR("[SanitizerOpHandler] Failed to create args tuple");
        return;
    }

    PyTuple_SetItem(args, 0, PyLong_FromLong(streamId));
    PyTuple_SetItem(args, 1, pyReadOnly);
    PyTuple_SetItem(args, 2, pyReadWrite);
    PyTuple_SetItem(args, 3, pyOutputs);
    PyTuple_SetItem(args, 4, PyUnicode_FromString(name.c_str()));
    PyTuple_SetItem(args, 5, pyTensorAliases);
    PyTuple_SetItem(args, 6, pyStorageDataptrs);

    // 调用 _handle_kernel_launch
    PyObject* result = PyObject_CallObject(handleFunc, args);
    if (result == nullptr)
    {
        PyErr_Clear();
        LOG_WARN("[SanitizerOpHandler] _handle_kernel_launch call failed for op '%s'", name.c_str());
    }
    else
    {
        Py_DecRef(result);
    }

    Py_DecRef(args);
}

void SanitizerOpHandler::Handle(const char* msg, int32_t streamId)
{
    if (msg == nullptr)
    {
        LOG_WARN("[SanitizerOpHandler] Received null message");
        return;
    }

    // 仅在 npu_sanitizer 使能时才处理打点消息
    if (!sanitizerEnabled_)
    {
        return;
    }

    // 提取 name 字段（必填）
    std::string name;
    if (!ExtractField(msg, "name", name) || name.empty())
    {
        LOG_WARN("[SanitizerOpHandler] Missing or empty 'name' field in message: %s", msg);
        return;
    }

    // 提取 read 字段（可选）
    std::string readStr;
    ExtractField(msg, "read", readStr);

    // 提取 write 字段（可选）
    std::string writeStr;
    ExtractField(msg, "write", writeStr);

    // 解析读写列表
    auto reads = ParseAccessList(readStr);
    auto writes = ParseAccessList(writeStr);

    LOG_INFO("[SanitizerOpHandler] Op: %s, stream: %d, reads: %zu, writes: %zu", name.c_str(), streamId, reads.size(),
             writes.size());

    // 触发 kernel launch 事件
    TriggerKernelLaunch(name, streamId, reads, writes);
}

}  // namespace MemScope
