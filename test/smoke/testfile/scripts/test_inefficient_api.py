# Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

import os
import logging
import ctypes
import gc

import torch
import torch_npu
from torch_npu.npu import amp 
from torch.utils.data import DataLoader, Dataset

import mstx
import msmemscope

class CustomDateset(Dataset):
    def __init__(self, device, input_shape, out_shape):
        self.device = device
        self.input_shape = input_shape
        self.out_shape = out_shape
        self.inputs = [torch.rand(self.input_shape).to(self.device)for i in range(100)]
        self.label = [torch.rand(self.out_shape).reshape(1, -1).to(self.device) for i in range(100)]

    def __getitem__(self, idx):
        return self.inputs[idx], self.label[idx]

    def __len__(self):
        return len(self.inputs)


class SmallModel(torch.nn.Module):
    def __init__(self, in_channel=3, out_channel=12):
        super(SmallModel, self).__init__()
        self.conv1 = torch.nn.Conv2d(in_channel, in_channel, 3, padding=1)
        self.relu1 = torch.nn.ReLU()
        self.conv2 = torch.nn.Conv2d(in_channel, out_channel, 3, padding=1)

    def forward(self, input_1):
        input_1 = self.conv1(input_1)
        input_1 = self.relu1(input_1)
        input_1 = self.conv2(input_1)
        return input_1.reshape(input_1.shape[0], -1)



class TrainModel:
    def __init__(self):
        self.input_shape = (3, 24, 24)
        self.out_shape = (12, 24, 24)
        local_rank = int(os.environ["LOCAL_RANK"])  
        self.device = torch.device('npu', local_rank)  
        torch.distributed.init_process_group(backend="hccl", rank=local_rank)  
        torch_npu.npu.set_device(local_rank)
        train_data = CustomDateset(self.device, self.input_shape, self.out_shape)
        train_sampler = torch.utils.data.distributed.DistributedSampler(train_data) 
        batch_size = 1
        self.model = SmallModel(self.input_shape[0], self.out_shape[0]).to(self.device)
        self.model = torch.nn.parallel.DistributedDataParallel(self.model, device_ids=[local_rank],
                                            output_device=local_rank)  
        self.train_dataloader = DataLoader(dataset=train_data, batch_size=batch_size, sampler=train_sampler)  
        self.loss = torch.nn.MSELoss().to(self.device)
        self.optimizer = torch.optim.SGD(self.model.parameters(), lr=0.1)
        self.temporary_idleness_tensor = torch.randn(1000, 1000).to('npu:0')            # 构建临时闲置测试用例

    def train(self):

        scaler = amp.GradScaler()  

        mstx.mark("[mstx]: mstxMarkA :------training start----- ", None)
        mstx_id = None
        for imgs, labels in self.train_dataloader:

            if mstx_id is None:
                mstx_id = mstx.range_start("step start", None)
            else:
                mstx.range_end(mstx_id)
                mstx_id = mstx.range_start("step start", None)

            imgs = imgs.to(self.device)  
            labels = labels.to(self.device)  
            with amp.autocast():  
                outputs = self.model(imgs)  
                loss = self.loss(outputs, labels)  
            self.optimizer.zero_grad()

            scaler.scale(loss).backward()  
            scaler.step(self.optimizer)
            scaler.update()
            outputs = None
            loss = None

        mstx.mark("[mstx]: mstxMarkA :------training end-----", None)


if __name__ == '__main__':
    msmemscope.config(
        events='launch,access,free,alloc',
        level='kernel,op',
        device='npu',
        format='csv',
        analysis='inefficient'
    )
    msmemscope.start()
    model = TrainModel()
    model.temporary_idleness_tensor.add_(1)
    model.train()
    model.temporary_idleness_tensor.add_(1)
    logging.basicConfig(level=logging.INFO)
    logging.info("tranning finshed.")
    gc.collect()
    msmemscope.stop()