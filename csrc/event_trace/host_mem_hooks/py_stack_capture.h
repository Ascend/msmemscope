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

#ifndef MSMEMSCOPE_HOST_MEM_PY_STACK_CAPTURE_H
#define MSMEMSCOPE_HOST_MEM_PY_STACK_CAPTURE_H

#include <stdint.h>

#include <string>

#include "host_mem_common.h"

/*
 * host内存泄漏py调用栈采集: 以分配时刻存活量(liveBytes/块数refs-1)为O(1)门控判据,
 * 泄漏候选栈达阈值(默认>10M且>50块,或>50M且>10块)才补采py栈,每栈至多一次;
 * 无解释器不采集,NA由闭窗按精确unfreed派生。
 *
 * 性能契约(热路径): ①配置门(1次原子读,未启用即返)→②约束判定(2次原子读+比较)
 * →③状态/解释器门(纯检查)。采集路径(慢): ⑤.5身份门(PyGILState_GetThisThreadState,
 * 未绑定tstate的纯native线程跳过)→PyInterpGuard取GIL(Ensure/Release,已持GIL幂等)+走链
 * +缓存+scratch格式化+RealMalloc拷贝;走链中Python API可能分配→钩子重入,
 * thread_local重入标记使递归层直接返回(防栈溢出)。
 *
 * 线程模型: 采集在持GIL线程上执行(GIL互斥采集者);pyBuf经发布桥(栈分片锁内
 * 终判+写入)发布——与清表在途条目复位互斥、双采集锁内收敛;读者acquire读后
 * 释放无竞态;帧串缓存写者与关窗Shutdown互斥锁串行(Shutdown收集键后释放锁再
 * 持GIL DecRef,锁序与采集GIL→mtx一致,不反向);全部容器/串RealMalloc底座。
 */

namespace hostmem
{

// 泄漏候选约束门控参数(环境变量可覆盖,开窗时读取,非热路径):
// 判据=(liveBytes>liveBytesMin && 块数>liveBlocksMin) || (liveBytes>liveBytesBig && 块数>liveBlocksBig)
// 默认(10M&&50)||(50M&&10)
struct PyStackGateParams
{
    int64_t liveBytesMin = 10485760;  // 一级: 存活字节下限(默认10M)
    int64_t liveBlocksMin = 50;       // 一级: 存活块数下限(默认50)
    int64_t liveBytesBig = 52428800;  // 二级: 大块场景存活字节下限(默认50M)
    int64_t liveBlocksBig = 10;       // 二级: 大块场景存活块数下限(默认10)
};

class PyStackCapture
{
   public:
    // 开窗参数通道: pyStackDepth==0=不启用(默认零开销);阈值经环境变量读取(调试接口,
    // 不对外呈现),开窗时由SvcSetEnabled经get_params快照调用
    static void Configure(uint32_t pyStackDepth);

    // 热路径钩点(每次分配的约束判定+达标补采,RecordMalloc在块表插入后调用);
    // 采集完成后pyState==PYTHON,该栈不再采
    static void CaptureIfEnabled(StackRecord& rec);

    // 闭窗收尾(窗口聚合完成后调用): 清空PyCodeFrameCache(解释器已finalize仅清容器不DecRef键),
    // 配置复位
    static void Shutdown();

    // 闭窗混合栈组装: frameDesc尾部追加marker+pyBuf(已采集)或按闭窗精确unfreed判据派生NA
    // (泄漏点无py的诚实标注);返回是否追加了py文本
    static bool AppendMixedStack(std::string& frameDesc, const StackRecord& rec, uint64_t unfreedBytes,
                                 uint64_t unfreedCount);

    // 行内数据版(中间快照符号化用): 文本/状态在快照冻结内深拷贝到行内,
    // 不引用StackRecord——恢复门控后新采集(pyBuf发布桥)不触碰行内数据
    static bool AppendMixedStack(std::string& frameDesc, uint32_t pyState, const char* pyText, size_t pyLen,
                                 uint64_t unfreedBytes, uint64_t unfreedCount);

   private:
    // 泄漏候选判据,闭窗派生NA与热路径门控共用
    static bool GateSatisfied(int64_t liveBytes, uint64_t liveBlocks);
};

}  // namespace hostmem

#endif /* MSMEMSCOPE_HOST_MEM_PY_STACK_CAPTURE_H */
