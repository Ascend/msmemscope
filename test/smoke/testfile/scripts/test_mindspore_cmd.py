# Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

import numpy as np
import mindspore as ms
from mindspore import nn
import mindspore.dataset as ds
import mstx

class Net(nn.Cell):
    def __init__(self):
        super(Net, self).__init__()
        self.fc = nn.Dense(2,2)
    def construct(self, x):
        y = self.fc(x)
        return y

def generator():
    for _ in range(2):
        yield (np.ones([2, 2]).astype(np.float32), np.ones([2]).astype(np.int32))

def train(net):
    optimizer = nn.Momentum(net.trainable_params(), 1, 0.9)
    loss = nn.SoftmaxCrossEntropyWithLogits(sparse=True)
    data = ds.GeneratorDataset(generator, ["data", "label"])
    model = ms.train.Model(net, loss, optimizer)
    
    range_id = mstx.range_start("step start", None)
    model.train(1, data)
    mstx.range_end(range_id)

if __name__ == '__main__':
    # Note: mstx only supports Ascend device and cannot be used in mindspore.nn.Cell.construct
    # when in mindspore.GRAPH_MODE
    ms.set_context(mode=ms.PYNATIVE_MODE)
    ms.set_device(device_target="Ascend", device_id=0)
    net = Net()
    for i in range(5):
        train(net)