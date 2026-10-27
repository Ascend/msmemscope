# Triton ACCESS - internal NPU tests

Developer tests for triton-ascend under operator execution + memory access
collection. **Not** a user-facing sample.

| File | Purpose |
|------|---------|
| `test_triton_access.py` / `.sh` | Minimal NPU smoke (assert + dump check) |
| `test_triton_e2e_gate.py` | C++ enable-gate six-state E2E |
| `bench_triton_collect_overhead.py` | Optional Python-side microbench (#305) |

Full multi-op ACCESS suite is **not** in this PR (line-budget); results are in the
PR description. C++ gate UT: `test/event_trace/test_triton_collect_gate.cpp`.

```bash
# CPU UT (no NPU), from repo root
python3 test/unit_python/test_triton_collection.py -v

# NPU smoke
export TOOL_PATH=/path/to/msmemscope/install
bash test/triton_access/test_triton_access.sh

# NPU E2E six-state
python3 test/triton_access/test_triton_e2e_gate.py --out logs/e2e_gate.log
# E2E needs same LD_PRELOAD as smoke (export via TOOL_PATH/lib64 hooks)
```
