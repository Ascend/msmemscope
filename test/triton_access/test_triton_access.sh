#!/bin/bash
# Internal NPU smoke for Triton ACCESS path (not a user sample).
# Usage (from repo root): bash test/triton_access/test_triton_access.sh
# Set TOOL_PATH to the msMemScope install prefix that contains lib64/*.so

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

export LD_PRELOAD="${TOOL_PATH}/lib64/libleaks_ascend_hal_hook.so:${TOOL_PATH}/lib64/libascend_mstx_hook.so:${TOOL_PATH}/lib64/libascend_kernel_hook.so"
export LD_LIBRARY_PATH="${TOOL_PATH}/lib64/:${LD_LIBRARY_PATH:-}"

# Optional: source /usr/local/Ascend/ascend-toolkit/set_env.sh
exec python3 test_triton_access.py
