MSLEAKS_DIR="../../msmemscope/output"
export PYTHONPATH=${MSLEAKS_DIR}/python:$PYTHONPATH
python ../../testfile/scripts/test_cpu_tensor_smoke.py
