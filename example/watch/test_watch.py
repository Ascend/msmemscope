# -------------------------------------------------------------------------
# This file is part of the MindStudio project.
# Copyright (c) 2025 Huawei Technologies Co.,Ltd.
#
# MindStudio is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#
#          http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
# -------------------------------------------------------------------------

# pylint: disable=duplicate-code  # 示例文件刻意保持相似结构，便于对照不同功能

import torch
from torch import nn, optim
import msmemscope


class SimpleModel(nn.Module):
    def __init__(self):
        super().__init__()
        self.linear = nn.Linear(10, 10)

    def forward(self, x):
        out = self.linear(x)
        return out


def test():
    msmemscope.config(format='csv', watch='start,end,full-content')  # 可以在这里设置监测的算子
    device = torch.device('npu:0')
    torch.npu.set_device(device)

    model = SimpleModel().to(device)

    criterion = nn.MSELoss()
    optimizer = optim.SGD(model.parameters(), lr=0.01)

    inputs = torch.randn(32, 10).to(device)
    targets = torch.randn(32, 10).to(device)
    test_tensor = torch.randn(100, 100, dtype=torch.float32).to(device)

    msmemscope.start()
    for epoch in range(5):
        outputs = model(inputs)
        msmemscope.watcher.watch(test_tensor, name="leaksStWatch")  # 添加监测的tensor
        loss = criterion(outputs, targets)
        msmemscope.watcher.remove(test_tensor)  # 去除监测的tensor
        loss.requires_grad_(True)
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

    msmemscope.stop()


def main():
    test()


if __name__ == "__main__":
    main()
