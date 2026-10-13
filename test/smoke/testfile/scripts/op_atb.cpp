/*
 * -------------------------------------------------------------------------
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

#include <iostream>
#include <acl/acl.h>
#include <atb/atb_infer.h>
#include <atb/utils.h>

int main(){
    int deviceId = 0;
    aclError status = aclrtSetDevice(deviceId);

    // 以elewise大类中的Add算子为例，可通过以下方式构造对应参数：
    atb::infer::ElewiseParam param;
    param.elewiseType = atb::infer::ElewiseParam::ELEWISE_ADD;

    atb::Operation *op = nullptr;
    atb::Status st = atb::CreateOperation(param, &op);

    // Tensor构造方法
    atb::Tensor a;
    a.desc.dtype = ACL_INT32;    // 配置Tensor数据类型
    a.desc.format = ACL_FORMAT_ND; // 配置Tensor格式
    a.desc.shape.dimNum = 2;       // 配置Tensor维度数
    a.desc.shape.dims[0] = 3;      // 配置Tensor第0维大小
    a.desc.shape.dims[1] = 3;      // 配置Tensor第1维大小
    a.dataSize = atb::Utils::GetTensorSize(a); // 获取Tensor内存大小
    status = aclrtMalloc(&a.deviceData, a.dataSize, ACL_MEM_MALLOC_HUGE_FIRST); // 申请device内存
    
    atb::Tensor b;
    b.desc.dtype = ACL_INT32;    // 配置Tensor数据类型
    b.desc.format = ACL_FORMAT_ND; // 配置Tensor格式
    b.desc.shape.dimNum = 2;       // 配置Tensor维度数
    b.desc.shape.dims[0] = 3;      // 配置Tensor第0维大小
    b.desc.shape.dims[1] = 3;      // 配置Tensor第1维大小
    b.dataSize = atb::Utils::GetTensorSize(b); // 获取Tensor内存大小
    status = aclrtMalloc(&b.deviceData, b.dataSize, ACL_MEM_MALLOC_HUGE_FIRST); // 申请device内存

    atb::Tensor output;
    output.desc.dtype = ACL_INT32;    // 配置Tensor数据类型
    output.desc.format = ACL_FORMAT_ND; // 配置Tensor格式
    output.desc.shape.dimNum = 2;       // 配置Tensor维度数
    output.desc.shape.dims[0] = 3;      // 配置Tensor第0维大小
    output.desc.shape.dims[1] = 3;      // 配置Tensor第1维大小
    output.dataSize = atb::Utils::GetTensorSize(output); // 获取Tensor内存大小
    status = aclrtMalloc(&output.deviceData, output.dataSize, ACL_MEM_MALLOC_HUGE_FIRST); // 申请device内存

    // 按上述方法构造所有输入和输出tensor，存入VariantPack
    atb::VariantPack variantPack;
    variantPack.inTensors = { a, b };
    variantPack.outTensors = { output };

    atb::Context *context = nullptr;
    st = atb::CreateContext(&context);
    aclrtStream stream = nullptr;
    status = aclrtCreateStream(&stream);
    context->SetExecuteStream(stream);

    uint64_t workspaceSize = 0;
    st = op->Setup(variantPack, workspaceSize, context);

    void *workspace = nullptr;
    status = aclrtMalloc(&workspace, workspaceSize, ACL_MEM_MALLOC_HUGE_FIRST);

    st = op->Execute(variantPack, (uint8_t *)workspace, workspaceSize, context);

    status = aclrtDestroyStream(stream); // 销毁stream
    status = aclrtFree(workspace);       // 销毁workspace
    st = atb::DestroyOperation(op);      // 销毁op对象
    st = atb::DestroyContext(context);   // 销毁context
    // 下面代码为释放Tensor的示例代码，实际使用时需释放VariantPack中的所有Tensor
    status = aclrtFree(a.deviceData);
    a.deviceData = nullptr;
    a.dataSize = 0;
    status = aclrtFree(b.deviceData);
    b.deviceData = nullptr;
    b.dataSize = 0;
    status = aclrtFree(output.deviceData);
    output.deviceData = nullptr;
    output.dataSize = 0;
}