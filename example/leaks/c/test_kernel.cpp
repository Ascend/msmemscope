#include "kernel_operator.h"
#include "acl/acl.h"
using namespace AscendC;

constexpr int32_t BYTESIZE = 256;
constexpr int32_t NUM_DATA = BYTESIZE / sizeof(half);

extern "C" __global__ __aicore__ void test_kernel(__gm__ uint8_t *gm)
{
    TPipe pipe;
    TBuf<QuePosition::VECCALC> xlm;
    GlobalTensor<half> xGm;
    pipe.InitBuffer(xlm, BYTESIZE);
    LocalTensor<half> xLm = xlm.Get<half>();
    xGm.SetGlobalBuffer((__gm__ half *)gm, NUM_DATA);
    DataCopy(xLm, xGm, NUM_DATA);
    DataCopy(xGm, xLm, NUM_DATA);
}

extern "C" void test_kernel_do(uint32_t blockDim, void *l2ctrl, void *stream, uint8_t *gm)
{
    test_kernel<<<blockDim, l2ctrl, stream>>>(gm);
}
