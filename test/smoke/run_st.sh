#!/bin/bash
set -euo pipefail

PROJECT_NAME="msmemscope"

# ==============================
# Setup paths
# ==============================
# 获取脚本所在目录作为项目根目录（假设此脚本放在 ./test/smoke/ 或类似位置）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR/../.."  # 因为当前在 test/smoke 下，所以回退两级

# ==============================
# Step 1: Install Python dependencies
# ==============================
REQUIREMENTS_FILE="$SCRIPT_DIR/requirements.txt"
if [ -f "$REQUIREMENTS_FILE" ]; then
    echo "[INFO] Installing Python dependencies from $REQUIREMENTS_FILE..."
    if ! python -m pip install -r "$REQUIREMENTS_FILE"; then
        echo "[WARN] Dependency install failed, continuing with existing environment..." >&2
    fi
else
    echo "[WARN] requirements.txt not found: $REQUIREMENTS_FILE"
fi

# ==============================
# Step 2: Build project
# ==============================

echo "[INFO] Building $PROJECT_NAME..."
cd "$PROJECT_ROOT/build"
if ! python build.py local; then
    echo "[ERROR] Build failed!" >&2
    exit 1
fi

# ==============================
# Step 3: Copy Python extension
# ==============================
SO_SRC="$PROJECT_ROOT/output/lib64/_${PROJECT_NAME}.so"
SO_DST="$PROJECT_ROOT/python/${PROJECT_NAME}/_${PROJECT_NAME}.so"

if [ ! -f "$SO_SRC" ]; then
    echo "[ERROR] Shared object not found: $SO_SRC" >&2
    exit 1
fi

mkdir -p "$(dirname "$SO_DST")"
cp "$SO_SRC" "$SO_DST"
echo "[INFO] Copied $SO_DST"

# ==============================
# Step 4: Setup output/python symlink
# ==============================

PYTHON_SYMLINK="$PROJECT_ROOT/output/python"
rm -f "$PYTHON_SYMLINK"  # 删除已存在的文件或链接
ln -s ../python "$PYTHON_SYMLINK"
echo "[INFO] Created symlink: $PYTHON_SYMLINK -> $PROJECT_ROOT/python"

# ==============================
# Step 5: Link output into isolated smoke workspace
# ==============================
SMOKE_DIR="$PROJECT_ROOT/test/smoke/$PROJECT_NAME"
OUTPUT_LINK="$SMOKE_DIR/output"
mkdir -p "$SMOKE_DIR"
rm -f "$OUTPUT_LINK"
ln -s "$PROJECT_ROOT/output" "$OUTPUT_LINK"
echo "[INFO] Created symlink: $OUTPUT_LINK -> $PROJECT_ROOT/output"

# ==============================
# Step 6: Run smoke test in isolated workspace
# ==============================
cd "$SCRIPT_DIR"
if [ ! -f "run_st.py" ]; then
    echo "[ERROR] Smoke test script not found: $SCRIPT_DIR/run_st.py" >&2
    exit 1
fi

echo "[INFO] Running smoke test..."
if ! python run_st.py; then
    echo "[ERROR] Smoke test failed!" >&2
    exit 1
fi
