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

#ifndef STATE_MANAGER_H
#define STATE_MANAGER_H

#include <cstdint>

namespace MemScope {

enum class PoolType : uint8_t {
    HOST = 0,
    HAL,
    PTA_CACHING,
    PTA_WORKSPACE,
    MINDSPORE,
    ATB,
    INVALID,
};

// 内存池（PTA/ATB/MindSpore）判断：池事件有 used/total 语义（报告时已填 totalAllocated/totalReserved），
// 与 HAL/HOST 区分（dump 统计键映射依赖）
inline bool IsMemoryPool(PoolType type)
{
    return type == PoolType::PTA_CACHING || type == PoolType::PTA_WORKSPACE || type == PoolType::ATB ||
           type == PoolType::MINDSPORE;
}

class StateKey {
public:
    virtual ~StateKey() {};
};

class StateManager {
public:
    virtual ~StateManager() {};
};

}

#endif