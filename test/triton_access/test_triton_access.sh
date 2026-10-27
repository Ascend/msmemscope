#!/bin/bash
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

set -euo pipefail
cd "$(dirname "$0")" || exit 1

TOOL_PATH="${TOOL_PATH:-}"
if [[ -z "${TOOL_PATH}" || ! -d "${TOOL_PATH}/lib64" ]]; then
  echo "FAIL: set TOOL_PATH to msMemScope install dir (with lib64/)." >&2
  exit 1
fi
for so in libleaks_ascend_hal_hook.so libascend_mstx_hook.so libascend_kernel_hook.so; do
  if [[ ! -f "${TOOL_PATH}/lib64/${so}" ]]; then
    echo "FAIL: missing ${TOOL_PATH}/lib64/${so}" >&2
    exit 1
  fi
done

REPO_ROOT="$(cd ../.. && pwd)"
export PYTHONPATH="${REPO_ROOT}/python:${PYTHONPATH:-}"
export LD_PRELOAD="${TOOL_PATH}/lib64/libleaks_ascend_hal_hook.so:${TOOL_PATH}/lib64/libascend_mstx_hook.so:${TOOL_PATH}/lib64/libascend_kernel_hook.so"
export LD_LIBRARY_PATH="${TOOL_PATH}/lib64/:${LD_LIBRARY_PATH:-}"

# Optional: source /usr/local/Ascend/ascend-toolkit/set_env.sh
exec python3 test_triton_access.py
