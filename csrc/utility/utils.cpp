/* -------------------------------------------------------------------------
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

#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace Utility
{

uint64_t GetProcessVmRss()
{
    // 1s TTL缓存:该值仅用于事件统计字段回填(FREE事件的used曲线,见
    // MemoryStateManager::AddEvent/UpdateUsage),秒级精度足够;无缓存时事件
    // 洪峰下每事件一次fopen+fscanf+fclose(/proc/self/statm),每秒可达数万次
    // 文件IO。并发竞态良性:多线程同时过期仅多读几次/proc,写入值等价
    static std::atomic<uint64_t> cachedRss{0};
    static std::atomic<uint64_t> cachedAtNs{0};
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    const uint64_t at = cachedAtNs.load(std::memory_order_relaxed);
    if (at != 0 && now - at < 1000000000ull)
    {
        return cachedRss.load(std::memory_order_relaxed);
    }

    FILE *fp = fopen("/proc/self/statm", "r");
    if (fp == nullptr)
    {
        return 0;
    }

    uint32_t vms_pages;
    uint32_t rss_pages;
    if (fscanf(fp, "%u %u", &vms_pages, &rss_pages) != 2)
    {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    static const uint32_t page_size = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
    const uint64_t rss = static_cast<uint64_t>(rss_pages) * page_size;
    cachedRss.store(rss, std::memory_order_relaxed);
    cachedAtNs.store(now, std::memory_order_relaxed);
    return rss;
}
}  // namespace Utility
