#include "acl/acl.h"
#include "kernel_operator.h"
using namespace AscendC;

// 本案例用于验证 msmemscope 工具的内存泄漏采集能力,内核无需执行真实计算,
// 因此 test_kernel 保持空实现即可。泄漏检测依赖 host 侧的 aclrtMalloc/aclrtFree
// 事件与 mstx step 打点(见 main.cpp),与内核内部逻辑无关,空内核不影响采集结果。
extern "C" __global__ __aicore__ void test_kernel(__gm__ uint8_t *gm) {}

extern "C" void test_kernel_do(uint32_t blockDim, void *l2ctrl, void *stream, uint8_t *gm)
{
    test_kernel<<<blockDim, l2ctrl, stream>>>(gm);
}
