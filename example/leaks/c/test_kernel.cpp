#include "kernel_operator.h"
#include "acl/acl.h"
using namespace AscendC;

constexpr int32_t DATA_LENGTH = 1024;

extern "C" __global__ __aicore__ void add_kernel(__gm__ uint8_t *x, __gm__ uint8_t *y, __gm__ uint8_t *z)
{
    GlobalTensor<float> xGm, yGm, zGm;
    xGm.SetGlobalBuffer((__gm__ float *)x, DATA_LENGTH);
    yGm.SetGlobalBuffer((__gm__ float *)y, DATA_LENGTH);
    zGm.SetGlobalBuffer((__gm__ float *)z, DATA_LENGTH);

    TPipe pipe;
    TQue<QuePosition::VECIN, 1> inQueueX, inQueueY;
    TQue<QuePosition::VECOUT, 1> outQueueZ;
    pipe.InitBuffer(inQueueX, 1, DATA_LENGTH * sizeof(float));
    pipe.InitBuffer(inQueueY, 1, DATA_LENGTH * sizeof(float));
    pipe.InitBuffer(outQueueZ, 1, DATA_LENGTH * sizeof(float));

    LocalTensor<float> xLocal = inQueueX.AllocTensor<float>();
    LocalTensor<float> yLocal = inQueueY.AllocTensor<float>();
    DataCopy(xLocal, xGm, DATA_LENGTH);
    DataCopy(yLocal, yGm, DATA_LENGTH);
    inQueueX.EnQue(xLocal);
    inQueueY.EnQue(yLocal);

    LocalTensor<float> xDeq = inQueueX.DeQue<float>();
    LocalTensor<float> yDeq = inQueueY.DeQue<float>();
    LocalTensor<float> zLocal = outQueueZ.AllocTensor<float>();
    Add(zLocal, xDeq, yDeq, DATA_LENGTH);
    outQueueZ.EnQue(zLocal);
    inQueueX.FreeTensor(xDeq);
    inQueueY.FreeTensor(yDeq);

    LocalTensor<float> zDeq = outQueueZ.DeQue<float>();
    DataCopy(zGm, zDeq, DATA_LENGTH);
    outQueueZ.FreeTensor(zDeq);
}

extern "C" void add_kernel_do(uint32_t blockDim, void *l2ctrl, void *stream, uint8_t *x, uint8_t *y, uint8_t *z)
{
    add_kernel<<<blockDim, l2ctrl, stream>>>(x, y, z);
}
