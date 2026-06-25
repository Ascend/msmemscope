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

#ifndef RUNETIME_HOOKS_H
#define RUNETIME_HOOKS_H

#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "acl_hooks.h"
#include "vallina_symbol.h"

namespace MemScope
{
constexpr uint64_t MAX_BINARY_SIZE = 32ULL * 1024 * 1024 * 1024;  // 32GB

struct RuntimeLibLoader
{
    static void *Load(void) { return LibLoad("libruntime.so"); }
};

struct ACLImplLibLoader
{
    static void *Load(void)
    {
        // 新老CANN包兼容方案
        void *handle = LibLoad("libascendcl_impl.so");
        if (handle != nullptr)
        {
            return handle;
        }
        return LibLoad("libacl_rt_impl.so");
    }
};

const void *GetHandleByStubFunc(const void *stubFunc);
}  // namespace MemScope

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef RTS_API
#define RTS_API
#endif  // RTS_API

    using aclrtBinary = void *;
    using aclrtBinHandle = void *;
    using aclrtFuncHandle = void *;
    using aclrtArgsHandle = void *;
    using aclrtParamHandle = void *;

    typedef enum
    {
        ACL_RT_ENGINE_TYPE_AIC = 0,
        ACL_RT_ENGINE_TYPE_AIV,
    } aclrtEngineType;

    typedef enum aclrtLaunchKernelAttrId
    {
        ACL_RT_LAUNCH_KERNEL_ATTR_SCHEM_MODE = 1,
        ACL_RT_LAUNCH_KERNEL_ATTR_ENGINE_TYPE = 3,
        ACL_RT_LAUNCH_KERNEL_ATTR_BLOCKDIM_OFFSET,
        ACL_RT_LAUNCH_KERNEL_ATTR_BLOCK_TASK_PREFETCH,
        ACL_RT_LAUNCH_KERNEL_ATTR_DATA_DUMP,
        ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT,
    } aclrtLaunchKernelAttrId;

    typedef union aclrtLaunchKernelAttrValue
    {
        uint8_t schemMode;
        uint32_t localMemorySize;
        aclrtEngineType engineType;
        uint32_t blockDimOffset;
        uint8_t isBlockTaskPrefetch;
        uint8_t isDataDump;
        uint16_t timeout;
        uint32_t rsv[4];
    } aclrtLaunchKernelAttrValue;

    typedef struct aclrtLaunchKernelAttr
    {
        aclrtLaunchKernelAttrId id;
        aclrtLaunchKernelAttrValue value;
    } aclrtLaunchKernelAttr;

    typedef struct aclrtLaunchKernelCfg
    {
        aclrtLaunchKernelAttr *attrs;
        size_t numAttrs;
    } aclrtLaunchKernelCfg;

    typedef struct aclrtPlaceHolderInfo
    {
        uint32_t addrOffset;
        uint32_t dataOffset;
    } aclrtPlaceHolderInfo;

    typedef enum tagRtError
    {
        RT_ERROR_NONE = 0x0,                      // success
        RT_ERROR_INVALID_VALUE = 0x1,             // invalid value
        RT_ERROR_MEMORY_ALLOCATION = 0x2,         // memory allocation fail
        RT_ERROR_INVALID_RESOURCE_HANDLE = 0x3,   // invalid handle
        RT_ERROR_INVALID_DEVICE_POINTER = 0x4,    // invalid device point
        RT_ERROR_INVALID_MEMCPY_DIRECTION = 0x5,  // invalid memory copy direction
        RT_ERROR_INVALID_DEVICE = 0x6,            // invalid device
        RT_ERROR_NO_DEVICE = 0x7,                 // no valid device
        RT_ERROR_CMD_OCCUPY_FAILURE = 0x8,        // command occpuy failure
        RT_ERROR_SET_SIGNAL_FAILURE = 0x9,        // set signal failure
        RT_ERROR_UNSET_SIGNAL_FAILURE = 0xA,      // unset signal failure
        RT_ERROR_OPEN_FILE_FAILURE = 0xB,         // unset signal failure
        RT_ERROR_WRITE_FILE_FAILURE = 0xC,
        RT_ERROR_MEMORY_ADDRESS_UNALIGNED = 0xD,
        RT_ERROR_DRV_ERR = 0xE,
        RT_ERROR_LOST_HEARTBEAT = 0xF,
        RT_ERROR_REPORT_TIMEOUT = 0x10,
        RT_ERROR_NOT_READY = 0x11,
        RT_ERROR_DATA_OPERATION_FAIL = 0x12,
        RT_ERROR_INVALID_L2_INSTR_SIZE = 0x13,
        RT_ERROR_DEVICE_PROC_HANG_OUT = 0x14,
        RT_ERROR_DEVICE_POWER_UP_FAIL = 0x15,
        RT_ERROR_DEVICE_POWER_DOWN_FAIL = 0x16,
        RT_ERROR_FEATURE_NOT_SUPPROT = 0x17,
        RT_ERROR_KERNEL_DUPLICATE = 0x18,             // register same kernel repeatly
        RT_ERROR_MODEL_STREAM_EXE_FAILED = 0x91,      // the model stream failed
        RT_ERROR_MODEL_LOAD_FAILED = 0x94,            // the model stream failed
        RT_ERROR_END_OF_SEQUENCE = 0x95,              // end of sequence
        RT_ERROR_NO_STREAM_CB_REG = 0x96,             // no callback register info for stream
        RT_ERROR_DATA_DUMP_LOAD_FAILED = 0x97,        // data dump load info fail
        RT_ERROR_CALLBACK_THREAD_UNSUBSTRIBE = 0x98,  // callback thread unsubstribe
        RT_ERROR_RESERVED
    } rtError_t;

    typedef void *rtStream_t;

    typedef uint32_t rtMemType_t;

    typedef struct tagRtSmData
    {
        uint64_t L2_mirror_addr;          // preload or swap source addr
        uint32_t L2_data_section_size;    // every data size
        uint8_t L2_preload;               // 1 - preload from mirrorAddr, 0 - no preload
        uint8_t modified;                 // 1 - data will be modified by kernel, 0 - no modified
        uint8_t priority;                 // data priority
        int8_t prev_L2_page_offset_base;  // remap source section offset
        uint8_t L2_page_offset_base;      // remap destination section offset
        uint8_t L2_load_to_ddr;           // 1 - need load out, 0 - no need
        uint8_t reserved[2];              // reserved
    } rtSmData_t;

    typedef struct tagRtSmCtrl
    {
        rtSmData_t data[8];  // data description
        uint64_t size;       // max page Num
        uint8_t remap[64];   /* just using for static remap mode, default:0xFF
                                array index: virtual l2 page id, array value: physic l2 page id */
        uint8_t l2_in_main;  // 0-DDR, 1-L2, default:0xFF
        uint8_t reserved[3];
    } rtSmDesc_t;

    typedef struct rtHostInputInfo
    {
        uint16_t addrOffset;
        uint16_t dataOffset;
    } rtHostInputInfo_t;

    typedef struct tagRtArgsEx
    {
        void *args;                           // args host mem addr
        rtHostInputInfo_t *hostInputInfoPtr;  // nullptr means no host mem input
        uint32_t argsSize;                    // input + output + tiling addr size + tiling data size + host mem
        uint16_t tilingAddrOffset;            // tiling addr offset
        uint16_t tilingDataOffset;            // tiling data offset
        uint16_t hostInputInfoNum;            // hostInputInfo num
        uint8_t hasTiling;                    // if has tiling: 0 means no tiling
        uint8_t isNoNeedH2DCopy;              // is no need host to device copy: 0 means need H2D copy,
                                              // others means doesn't need H2D copy.
        uint8_t reserved[4];
    } rtArgsEx_t;

    using rtFuncHandle = void *;
    using rtLaunchArgsHandle = void *;
    using RtStreamT = void *;
    using RtSmDescT = void *;

    typedef struct tagRtTaskCfgInfo
    {
        uint8_t qos;
        uint8_t partId;
        uint8_t schemMode;  // rtschemModeType_t 0:normal;1:batch;2:sync
        uint8_t res[1];     // res
    } rtTaskCfgInfo_t;

    typedef struct tagRtTaskCfgInfoByHandle
    {
        uint8_t qos;
        uint8_t partId;
        uint8_t schemMode;  // rtschemModeType_t 0:normal;1:batch;2:sync
        bool d2dCrossFlag;  // d2dCrossFlag true:D2D_CROSS flase:D2D_INNER
        uint32_t blockDimOffset;
        uint8_t dumpflag;  // dumpflag 0:fault 2:RT_KERNEL_DUMPFLAG 4:RT_FUSION_KERNEL_DUMPFLAG
    } RtTaskCfgInfoT;

    typedef struct tagRtAicpuArgsEx
    {
        void *args;                              // args host mem addr
        rtHostInputInfo_t *hostInputInfoPtr;     // nullptr means no host mem input
        rtHostInputInfo_t *kernelOffsetInfoPtr;  // KernelOffsetInfo, it is different for CCE Kernel and fwk kernel
        uint32_t argsSize;
        uint16_t hostInputInfoNum;      // hostInputInfo num
        uint16_t kernelOffsetInfoNum;   // KernelOffsetInfo num
        uint32_t soNameAddrOffset;      // just for CCE Kernel, default value is 0xffff for FWK kernel
        uint32_t kernelNameAddrOffset;  // just for CCE Kernel, default value is 0xffff for FWK kernel
        bool isNoNeedH2DCopy;           // is no need host to device copy: 0 means need H2D copy,
                                        // other means doesn't need H2D copy.
        uint8_t reserved[3];
    } RtAicpuArgsExT;

    RTS_API rtError_t rtKernelLaunch(const void *stubFunc, uint32_t blockDim, void *args, uint32_t argsSize,
                                     rtSmDesc_t *smDesc, rtStream_t stm);
    RTS_API rtError_t rtKernelLaunchWithHandleV2(void *hdl, const uint64_t tilingKey, uint32_t blockDim,
                                                 rtArgsEx_t *argsInfo, rtSmDesc_t *smDesc, rtStream_t stm,
                                                 const rtTaskCfgInfo_t *cfgInfo);
    RTS_API rtError_t rtKernelLaunchWithFlagV2(const void *stubFunc, uint32_t blockDim, rtArgsEx_t *argsInfo,
                                               rtSmDesc_t *smDesc, rtStream_t stm, uint32_t flags,
                                               const rtTaskCfgInfo_t *cfgInfo);
    RTS_API rtError_t rtAicpuKernelLaunchExWithArgs(const uint32_t kernelType, const char *const opName,
                                                    const uint32_t blockDim, const RtAicpuArgsExT *argsInfo,
                                                    RtSmDescT *const smDesc, const RtStreamT stm, const uint32_t flags);
    RTS_API rtError_t rtLaunchKernelByFuncHandle(rtFuncHandle funcHandle, uint32_t blockDim,
                                                 rtLaunchArgsHandle argsHandle, RtStreamT stm);
    RTS_API rtError_t rtLaunchKernelByFuncHandleV2(rtFuncHandle funcHandle, uint32_t blockDim,
                                                   rtLaunchArgsHandle argsHandle, RtStreamT stm,
                                                   const RtTaskCfgInfoT *cfgInfo);

    // 适配为C接口
    RTS_API aclError aclrtLaunchKernelImpl(aclrtFuncHandle funcHandle, uint32_t blockDim, const void *argsData,
                                           size_t argsSize, aclrtStream stream);
    RTS_API aclError aclrtLaunchKernelWithConfig(aclrtFuncHandle funcHandle, uint32_t blockDim, aclrtStream stream,
                                                 aclrtLaunchKernelCfg *cfg, aclrtArgsHandle argsHandle, void *reserve);
    RTS_API aclError aclrtLaunchKernelV2Impl(aclrtFuncHandle funcHandle, uint32_t blockDim, const void *argsData,
                                             size_t argsSize, aclrtLaunchKernelCfg *cfg, aclrtStream stream);
    RTS_API aclError aclrtLaunchKernelWithHostArgs(aclrtFuncHandle funcHandle, uint32_t blockDim, aclrtStream stream,
                                                   aclrtLaunchKernelCfg *cfg, void *hostArgs, size_t argsSize,
                                                   aclrtPlaceHolderInfo *placeHolderArray, size_t placeHolderNum);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif
