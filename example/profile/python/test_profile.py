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

import torch
import torch_npu
import torch.nn as nn
import torch.optim as optim
import msmemscope


def test():
    msmemscope.config(format='csv', output_path='./output') # 通过msmemscope.config()来设置各种采集配置
    device = torch.device('npu:0') # 可以更改为想要的卡号
    torch.npu.set_device(device)


    class SimpleModel(nn.Module):
        def __init__(self):
            super(SimpleModel, self).__init__()
            self.linear = nn.Linear(10, 10)

        def forward(self, x):
            out = self.linear(x)
            return out

    model = SimpleModel().to(device)
    criterion = nn.MSELoss()
    optimizer = optim.SGD(model.parameters(), lr=0.01)
    inputs = torch.randn(32, 10).to(device)
    targets = torch.randn(32, 10).to(device)
    
    msmemscope.start() # 通过msmemscope.start()标识采集开始
    for epoch in range(6):
        outputs = model(inputs)
        loss = criterion(outputs, targets)
        loss.requires_grad_(True)
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()
        if epoch % 2 == 0:
            memory_allocated = torch.npu.memory_allocated(device) / (1024 ** 2)
            max_memory_allocated = torch.npu.max_memory_allocated(device) / (1024 ** 2)
            print(f"Epoch {epoch} : Current Memory Allocated = {memory_allocated:.2f} MB,", 
            f"Max Memory Allocated = {max_memory_allocated:.2f} MB")
        
        torch.npu.empty_cache()
    msmemscope.stop() # 通过msmemscope.stop()标识采集结束


def main():
    test()
    print("Test finished.")

if __name__ == "__main__":
    main()