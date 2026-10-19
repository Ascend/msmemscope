// -------------------------------------------------------------------------
// This file is part of the MindStudio project.
// Copyright (c) 2026 Huawei Technologies Co.,Ltd.
//
// MindStudio is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
//
//          http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PSL v2 for more details.
// -------------------------------------------------------------------------

#include "stars_common.h"

namespace MemScope
{

bool StarsCommon::isExpand = false;

void StarsCommon::SetStreamExpandStatus(uint8_t expandStatus) { isExpand = (expandStatus == 1); }

uint16_t StarsCommon::GetStreamId(uint16_t streamId, uint16_t taskId, uint16_t sqeType)
{
    if (isExpand)
    {
        if (sqeType == PLACE_HOLDER_SQE || sqeType == EVENT_RECORD_SQE)
        {
            return streamId & EXPANDING_LOW_OPERATOR;
        }
        if ((streamId & STREAM_JUDGE_BIT15_OPERATOR) != 0)
        {
            return taskId & EXPANDING_LOW_OPERATOR;
        }
        else
        {
            return streamId & EXPANDING_LOW_OPERATOR;
        }
    }
    else
    {
        if ((streamId & STREAM_JUDGE_BIT12_OPERATOR) != 0)
        {
            return streamId % STREAM_LOW_OPERATOR;
        }
        if ((streamId & STREAM_JUDGE_BIT13_OPERATOR) != 0)
        {
            streamId = taskId & COMMON_LOW_OPERATOR;
        }
        return streamId % STREAM_LOW_OPERATOR;
    }
}

uint16_t StarsCommon::GetTaskId(uint16_t streamId, uint16_t taskId, uint16_t sqeType)
{
    if (isExpand)
    {
        if (sqeType == PLACE_HOLDER_SQE || sqeType == EVENT_RECORD_SQE)
        {
            return taskId;
        }
        if ((streamId & STREAM_JUDGE_BIT15_OPERATOR) != 0)
        {
            return (taskId & STREAM_JUDGE_BIT15_OPERATOR) | (streamId & EXPANDING_LOW_OPERATOR);
        }
        else
        {
            return taskId;
        }
    }
    else
    {
        if ((streamId & STREAM_JUDGE_BIT12_OPERATOR) != 0)
        {
            taskId = taskId & TASK_LOW_OPERATOR;
            taskId |= (streamId & STREAM_HIGH_OPERATOR);
        }
        else if ((streamId & STREAM_JUDGE_BIT13_OPERATOR) != 0)
        {
            taskId = (streamId & COMMON_LOW_OPERATOR) | (taskId & COMMON_HIGH_OPERATOR);
        }
        return taskId;
    }
}
}  // namespace MemScope
