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

scripts_dir=../../testfile/scripts

g++ -I "${ATB_HOME_PATH}/include" -I "${ASCEND_HOME_PATH}/include" -L "${ATB_HOME_PATH}/lib" -L "${ASCEND_HOME_PATH}/lib64" ${scripts_dir}/op_atb.cpp -l atb -l ascendcl -o ${scripts_dir}/op_atb

file=${scripts_dir}/op_atb
if [ ! -f "$file" ]; then
    echo "Failed: $file not exist."
    exit 1
fi

if [ ! -x "$file" ]; then
    if chmod +x "$file"; then
        echo "$file is executable."
    else
        echo "Failed: $file cannot be executed."
        exit 1
    fi
fi

${scripts_dir}/op_atb
rm -r $file