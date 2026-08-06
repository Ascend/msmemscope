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

#include <cstdio>
#include <sstream>
#include <iomanip>
#include "calculate_data_check_sum.h"

// ISO标准多项式
constexpr uint64_t DATA_CHECKSUM64_POLYNOMIAL = 0x42F0E1EBA9EA3693ULL;
// 初始值和异或输出值
constexpr uint64_t DATA_CHECKSUM64_INITIAL_VALUE = 0xFFFFFFFFFFFFFFFFULL;
constexpr uint64_t DATA_CHECKSUM64_XOR_OUTPUT = 0xFFFFFFFFFFFFFFFFULL;
// 查找表大小
constexpr int DATA_CHECKSUM64_TABLE_SIZE = 256;
// 每个字节的位数
constexpr int BITS_PER_BYTE = 8;
// 预计算的查找表
static uint64_t g_checksum64Table[256];

// 初始化查找表
static void InitializeTable()
{
    static bool tableInitialized = false;

    if (tableInitialized) {
        return;
    }
    for (int i = 0; i < DATA_CHECKSUM64_TABLE_SIZE; ++i) {
        uint64_t checksum = static_cast<uint64_t>(i);
        
        for (int j = 0; j < BITS_PER_BYTE; ++j) {
            if ((checksum & 1) != 0) {
                checksum = (checksum >> 1) ^ DATA_CHECKSUM64_POLYNOMIAL;
            } else {
                checksum >>= 1;
            }
        }
        
        g_checksum64Table[i] = checksum;
    }
    
    tableInitialized = true;
}

std::string CalculateDataCheckSum64(const std::vector<uint8_t>& data)
{
    // 确保查找表已初始化
    InitializeTable();

    // 初始值
    uint64_t checksum = DATA_CHECKSUM64_INITIAL_VALUE;
    
    // 计算
    for (auto byte : data) {
        uint8_t index = static_cast<uint8_t>(checksum ^ byte);
        checksum = (checksum >> BITS_PER_BYTE) ^ g_checksum64Table[index];
    }
    
    // 异或输出
    checksum ^= DATA_CHECKSUM64_XOR_OUTPUT;
    
    // 转换为十六进制字符串（避免stringstream构造开销）
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(checksum));
    return std::string(buf);
}