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

#include "acl_hooks.h"
#include <cstdint>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <unordered_map>

#include "cpython.h"
#include "event_report.h"
#include "vallina_symbol.h"
#include "log.h"
#include "record_info.h"
#include "bit_field.h"
#include "trace_manager/event_trace_manager.h"

using namespace MemScope;
 
ACL_FUNC_VISIBILITY aclError aclInit(const char *configPath)
{
    using AclInit = decltype(&aclInit);
    auto vallina = VallinaSymbol<AclLibLoader>::Instance().Get<AclInit>("aclInit");
    if (vallina == nullptr) {
        LOG_ERROR("vallina func get FAILED: %s", __func__);
        return ACL_ERROR_INTERNAL_ERROR;
    }

    aclError ret = vallina(configPath);

    EventTraceManager::Instance().SetAclInitStatus(true);

    if (!EventTraceManager::Instance().IsTracingEnabled()) {
        return ret;
    }

    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAclItf(RecordSubType::INIT)) {
        LOG_ERROR("aclInit report FAILED");
    }

    return ret;
}

ACL_FUNC_VISIBILITY aclError aclFinalize()
{
    using AclFinalize = decltype(&aclFinalize);
    auto vallina = VallinaSymbol<AclLibLoader>::Instance().Get<AclFinalize>("aclFinalize");
    if (vallina == nullptr) {
        LOG_ERROR("vallina func get FAILED: %s", __func__);
        return ACL_ERROR_INTERNAL_ERROR;
    }

    aclError ret = vallina();

    if (!EventTraceManager::Instance().IsTracingEnabled()) {
        return ret;
    }

    if (!EventReport::Instance(MemScopeCommType::SHARED_MEMORY).ReportAclItf(RecordSubType::FINALIZE)) {
        LOG_ERROR("aclInit report FAILED");
    }

    return ret;
}
