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

#ifndef RECORD_INFO_H
#define RECORD_INFO_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <sys/types.h>
#include <memory>

#include "securec.h"
#include "utils.h"

namespace MemScope {

constexpr int32_t GD_INVALID_NUM = 9999;
const size_t KERNELNAME_MAX_SIZE = 128;
const size_t ATB_NAME_MAX_SIZE = 64;
const size_t ATEN_NAME_MAX_SIZE = 128;
const size_t OP_NAME_MAX_SIZE = 128;
const size_t ATB_PARAMS_MAX_SIZE = 128;
const size_t MEM_ATTR_MAX_SIZE = 128;
const size_t ADDR_OWNER_SIZE = 64;
constexpr unsigned long long FLAG_INVALID = UINT64_MAX;

enum class RecordSubType {
    MALLOC = 0,
    FREE,
    USER_DEFINED,
    PTA_OPTIMIZER_STEP,
    INIT,
    FINALIZE,
    NORMAL,
    HANDLEV2,
    FLAGV2,
    KERNEL_START,
    KERNEL_END,
    ATEN_START,
    ATEN_END,
    ATB_START,
    ATB_END,
};

enum class MemoryDataType {
    MEMORY_MALLOC = 0,
    MEMORY_FREE,
    MEMORY_BLOCK_FREE,
    MEMORY_INVALID
};

enum class MemoryAllocatorType {
    ALLOCATOR_INNER = 0,
    ALLOCATOR_EXTERNAL,
    ALLOCATOR_INVALID,
};

struct MemoryUsage {
    int8_t deviceType;          // 20-npu
    int8_t deviceIndex;
    uint8_t dataType;           // 0-malloc, 1-free, 2-block_free
    uint8_t allocatorType;      // 0-Inner(for PTA), 1-external(for GE)
    uint64_t ptr;
    int64_t allocSize;
    int64_t totalAllocated;
    int64_t totalReserved;
    int64_t totalActive;
    int64_t streamPtr;
};

struct MemorySnapshotInfo {
    int device;               // 设备ID
    uint64_t memory_reserved; // 当前保留的内存
    uint64_t max_memory_reserved; // 最大保留的内存
    uint64_t memory_allocated; // 当前分配的内存
    uint64_t max_memory_allocated; // 最大分配的内存
    uint64_t total_memory;    // 总内存
    uint64_t free_memory;     // 空闲内存
    std::string name;           // 快照名称
};

enum class MemOpSpace : uint8_t {
    SVM = 0U,
    DEVICE,
    HOST,
    DVPP,
    INVALID,
};

enum class PyTraceType : uint8_t {
    PYCALL = 0,
    PYEXCEPTION,
    PYLINE,
    PYRETURN,
    CCALL,
    CEXCEPTION,
    CRETURN,
    PYOPCODE,
};

enum class MemPageType : uint32_t {
    MEM_NORMAL_PAGE_TYPE = 0, // 4K
    MEM_HUGE_PAGE_TYPE, // 2M
    MEM_GIANT_PAGE_TYPE, // 1G
    MEM_MAX_PAGE_TYPE
};

enum class MarkType : int32_t {
    MARK_A = 0,
    RANGE_START_A,
    RANGE_END,
};

enum class AccessType : uint8_t {
    READ = 0,
    WRITE,
    UNKNOWN,
};

struct CallStackInfo {
    uint64_t pyLen;
    uint64_t cLen;
    char *pyStack;
    char *cStack;
};
struct CallStackString {
    std::string cStack;
    std::string pyStack;
    CallStackString() {}
    CallStackString(std::string& c, std::string& python) : cStack(c), pyStack(python) {}
};

}
#endif