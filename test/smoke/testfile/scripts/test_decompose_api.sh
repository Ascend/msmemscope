# -------------------------------------------------------------------------
# This file is part of the MindStudio project.
# Copyright (c) 2026 Huawei Technologies Co.,Ltd.
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

MSLEAKS_DIR="../../msmemscope/output"
export LD_LIBRARY_PATH=${MSLEAKS_DIR}/lib64:${LD_LIBRARY_PATH}
export LD_PRELOAD=${MSLEAKS_DIR}/lib64/libleaks_ascend_hal_hook.so:${MSLEAKS_DIR}/lib64/libascend_mstx_hook.so:${MSLEAKS_DIR}/lib64/libascend_kernel_hook.so
export PYTHONPATH=${MSLEAKS_DIR}/python:$PYTHONPATH

torchrun --nproc_per_node=2 ../../testfile/scripts/test_decompose_api.py --standalone