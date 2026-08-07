/* -------------------------------------------------------------------------
 * This file is part of the MindStudio project.
 * Copyright (c) 2026 Huawei Technologies Co.,Ltd.
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
#include <iostream>
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

bool SanitizerOpHandler::ExtractField(const std::string& msg, const std::string& key, std::string& value)
{
    std::string searchKey = key + "=";
    size_t startPos = msg.find(searchKey);
    if (startPos == std::string::npos)
    {
        return false;
    }
    startPos += searchKey.length();
    size_t endPos = msg.find_first_of(";", startPos);
    if (endPos == std::string::npos)
    {
        endPos = msg.length();
    }
    value = msg.substr(startPos, endPos - startPos);
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

// ---------------------------------------------------------------------------
// 静态辅助函数
// ---------------------------------------------------------------------------

// 获取 _handle_kernel_launch 可调用对象及版本标识
// 返回 true 表示成功，handleFunc / hasStroge 为输出参数
static bool GetLaunchHandler(Utility::PythonObject& handleFunc, bool& hasStroge)
{
    Utility::PythonObject sanitizerMod = Utility::PythonObject::Import("torch_npu.npu._sanitizer", false);
    if (sanitizerMod.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to import torch_npu.npu._sanitizer");
        return false;
    }

    Utility::PythonObject streamCheckMod = Utility::PythonObject::Import("torch_npu.npu._stream_check", false);
    if (streamCheckMod.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to import torch_npu.npu._stream_check");
        return false;
    }

    // 有 NPURecordStreamHandler 类为 v26.1 以上新版本，_handle_kernel_launch 函数增加 storage 参数
    if (!streamCheckMod.Get("NPURecordStreamHandler").IsBad())
    {
        hasStroge = true;
    }

    Utility::PythonObject npuSanitizer = sanitizerMod.Get("npu_sanitizer");
    if (npuSanitizer.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to get npu_sanitizer instance");
        return false;
    }

    Utility::PythonObject eventHandler = npuSanitizer.Get("event_handler");
    if (eventHandler.IsBad())
    {
        LOG_WARN("[SanitizerOpHandler] event_handler not initialized, skip kernel launch");
        return false;
    }

    handleFunc = eventHandler.Get("_handle_kernel_launch");
    if (handleFunc.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to get _handle_kernel_launch method");
        return false;
    }

    return true;
}

// 根据读写内存访问信息构建 Python args 元组
static Utility::PythonTupleObject BuildLaunchArgs(const std::string& name, uint64_t stream,
                                                  const std::vector<MemoryAccessItem>& reads,
                                                  const std::vector<MemoryAccessItem>& writes, bool hasStroge)
{
    // 收集地址集合和 tensor_aliases 映射
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

    // read_only = read - write（in-place 视为 write）
    std::set<uint64_t> readOnly;
    for (auto p : readPtrs)
    {
        if (writePtrs.find(p) == writePtrs.end())
        {
            readOnly.insert(p);
        }
    }

    // 构建 Python 对象
    Utility::PythonSetObject pyReadOnly(readOnly);
    if (pyReadOnly.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to create read_only set");
        return Utility::PythonTupleObject();
    }

    Utility::PythonSetObject pyReadWrite(writePtrs);
    if (pyReadWrite.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to create read_write set");
        return Utility::PythonTupleObject();
    }

    Utility::PythonSetObject pyOutputs;
    Utility::PythonSetObject pyStorageDataptrs;

    Utility::PythonDictObject pyTensorAliases;
    for (const auto& kv : tensorAliases)
    {
        Utility::PythonListObject pyAliases(kv.second);
        pyTensorAliases.Add(kv.first, pyAliases);
    }

    // 组装 args 元组: (stream, read_only, read_write, outputs, operator,
    //                    tensor_aliases, storage_dataptrs_accessed)
    std::vector<Utility::PythonObject> argsVec;
    argsVec.push_back(Utility::PythonObject(stream));
    argsVec.push_back(pyReadOnly);
    argsVec.push_back(pyReadWrite);
    argsVec.push_back(pyOutputs);
    argsVec.push_back(Utility::PythonObject(name));
    argsVec.push_back(pyTensorAliases);
    if (hasStroge)
    {
        argsVec.push_back(pyStorageDataptrs);
    }

    Utility::PythonTupleObject args(argsVec);
    if (args.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to create args tuple");
    }
    return args;
}

// 处理 _handle_kernel_launch 返回的错误列表：打印到 stderr 并抛出 CUDASanitizerErrors
static void HandleLaunchErrors(const Utility::PythonObject& result)
{
    if (!result.IsInstance("list"))
    {
        return;
    }
    Utility::PythonListObject resultList(static_cast<PyObject*>(result));
    if (resultList.Size() == 0)
    {
        return;
    }

    // 打印每个错误到 stderr
    for (size_t i = 0; i < resultList.Size(); ++i)
    {
        std::cerr << resultList.GetItem<std::string>(i) << std::endl;
    }

    // 导入异常类
    Utility::PythonObject cudaSanitizerMod = Utility::PythonObject::Import("torch.cuda._sanitizer", false);
    if (cudaSanitizerMod.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to import torch.cuda._sanitizer for CUDASanitizerErrors");
        return;
    }
    Utility::PythonObject cudaErrorsClass = cudaSanitizerMod.Get("CUDASanitizerErrors");
    if (cudaErrorsClass.IsBad())
    {
        LOG_ERROR("[SanitizerOpHandler] Failed to get CUDASanitizerErrors class");
        return;
    }

    // raise CUDASanitizerErrors(result)
    Utility::PythonTupleObject errorArgs(std::vector<Utility::PythonObject>{result});
    Utility::PythonObject errorInstance = cudaErrorsClass.Call(errorArgs);
    if (!errorInstance.IsBad())
    {
        PyErr_SetObject(static_cast<PyObject*>(cudaErrorsClass), static_cast<PyObject*>(errorInstance));
    }
}

// ---------------------------------------------------------------------------
// TriggerKernelLaunch
// ---------------------------------------------------------------------------

void SanitizerOpHandler::TriggerKernelLaunch(const std::string& name, uint64_t stream,
                                             const std::vector<MemoryAccessItem>& reads,
                                             const std::vector<MemoryAccessItem>& writes)
{
    if (!Utility::IsPyInterpRepeInited())
    {
        LOG_ERROR("[SanitizerOpHandler] Python Interpreter initialization FAILED, skip kernel launch");
        return;
    }

    Utility::PyInterpGuard stat;

    // 1. 获取 Python 侧 handler
    Utility::PythonObject handleFunc;
    bool hasStroge = false;
    if (!GetLaunchHandler(handleFunc, hasStroge))
    {
        return;
    }

    // 2. 构建 Python args
    Utility::PythonTupleObject args = BuildLaunchArgs(name, stream, reads, writes, hasStroge);
    if (args.IsBad())
    {
        return;
    }

    // 3. 调用 _handle_kernel_launch
    Utility::PythonObject result = handleFunc.Call(args);
    if (result.IsBad())
    {
        LOG_WARN("[SanitizerOpHandler] _handle_kernel_launch call failed for op '%s'", name.c_str());
        return;
    }

    // 4. 处理返回的错误列表
    HandleLaunchErrors(result);
}

void SanitizerOpHandler::Handle(const char* msg, uint64_t stream)
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

    // 每条消息仅拷贝一次，供多个字段提取复用
    std::string msgString(msg);

    // 提取 name 字段（必填）
    std::string name;
    if (!ExtractField(msgString, "name", name) || name.empty())
    {
        LOG_WARN("[SanitizerOpHandler] Missing or empty 'name' field in message: %s", msg);
        return;
    }

    // 提取 read 字段（可选）
    std::string readStr;
    ExtractField(msgString, "read", readStr);

    // 提取 write 字段（可选）
    std::string writeStr;
    ExtractField(msgString, "write", writeStr);

    // 解析读写列表
    auto reads = ParseAccessList(readStr);
    auto writes = ParseAccessList(writeStr);

    LOG_DEBUG("[SanitizerOpHandler] Op: %s, stream: %lu, reads: %zu, writes: %zu", name.c_str(), stream, reads.size(),
              writes.size());

    // 触发 kernel launch 事件
    TriggerKernelLaunch(name, stream, reads, writes);
}

}  // namespace MemScope
