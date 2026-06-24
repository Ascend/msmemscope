#!/bin/bash

# 设置脚本遇到错误立即退出，避免错误累积
set -e

# 定义颜色输出，让用户更容易识别不同级别的信息
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # 无颜色

# 工具基本信息配置
TOOL_NAME="memscope"
ARCH=$(uname -m)  # 获取系统架构
VERSION="26.0.0"  # 版本号
RUN_FILE="mindstudio-memscope_${VERSION}_${ARCH}.run"  # 根据架构生成文件名
BUILD_DIR="$(cd "$(dirname "$0")" && pwd)"  # 获取脚本所在绝对路径
TEMP_DIR="/tmp/${TOOL_NAME}_build_$$"      # 使用进程ID确保临时目录唯一性

# 源目录定义 - 这些是需要打包的交付件目录
PYTHON_DIR="../python"
BIN_DIR="../output/bin"
LIB64_DIR="../output/lib64"

# 日志输出函数 - 所有用户可见的输出都是英文
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    echo -e "${BLUE}[DEBUG]${NC} $1"
}

# 检查源目录是否存在
check_source_dirs() {
    local missing_dirs=()

    # 检查每个必需的目录是否存在
    [ ! -d "$PYTHON_DIR" ] && missing_dirs+=("$PYTHON_DIR")
    [ ! -d "$BIN_DIR" ] && missing_dirs+=("$BIN_DIR")
    [ ! -d "$LIB64_DIR" ] && missing_dirs+=("$LIB64_DIR")

    # 如果有目录缺失，报告错误
    if [ ${#missing_dirs[@]} -gt 0 ]; then
        log_error "The following source directories are missing:"
        for dir in "${missing_dirs[@]}"; do
            echo "  $dir"
        done
        return 1
    fi

    return 0
}

# 创建临时构建目录
create_temp_dir() {
    mkdir -p "$TEMP_DIR"
    mkdir -p "$TEMP_DIR/payload"  # payload目录用于存放要打包的文件
}

# 生成文件校验和
generate_file_hashes() {
    local payload_dir="$TEMP_DIR/payload/msmemscope"
    local hash_file="$payload_dir/file_checksums.sha256"

    log_info "Generating file checksums..."

    # 检查sha256sum命令是否可用
    if ! command -v sha256sum >/dev/null 2>&1; then
        log_warn "sha256sum not available, skipping checksum generation"
        return 0
    fi

    cd "$payload_dir"

    # 生成详细的文件哈希列表（排除校验文件自身）
    find . -type f ! -name "file_checksums.sha256" ! -name "version.txt" -exec sha256sum {} \; > "$hash_file"

    # 计算总体哈希（基于哈希文件本身）
    local overall_hash=$(sha256sum "$hash_file" | cut -d' ' -f1)
    echo "build_hash: $overall_hash" >> "version.txt"
    echo "integrity_check: enabled" >> "version.txt"

    cd - > /dev/null
    log_info "File checksums generated with overall hash: ${overall_hash:0:16}..."
}

# 复制交付件到临时目录
copy_artifacts() {
    log_info "Copying artifacts to temporary directory..."

    # 在payload下创建msmemscope目录
    mkdir -p "$TEMP_DIR/payload/msmemscope"

    # 使用rsync保持文件权限和属性，如果rsync不可用则用cp
    if command -v rsync >/dev/null 2>&1; then
        rsync -av "$PYTHON_DIR/" "$TEMP_DIR/payload/msmemscope/python/" --exclude="*.pyc" --exclude="__pycache__"
        rsync -av "$BIN_DIR/" "$TEMP_DIR/payload/msmemscope/bin/"
        rsync -av "$LIB64_DIR/" "$TEMP_DIR/payload/msmemscope/lib64/"
    else
        mkdir -p "$TEMP_DIR/payload/msmemscope"
        cp -r "$PYTHON_DIR" "$TEMP_DIR/payload/msmemscope/"
        cp -r "$BIN_DIR" "$TEMP_DIR/payload/msmemscope/"
        cp -r "$LIB64_DIR" "$TEMP_DIR/payload/msmemscope/"
    fi

    # 复制 _msmemscope.so 到 python/msmemscope 目录
    log_info "Copying _msmemscope.so to python directory..."
    if [ -f "$TEMP_DIR/payload/msmemscope/lib64/_msmemscope.so" ]; then
        cp "$TEMP_DIR/payload/msmemscope/lib64/_msmemscope.so" "$TEMP_DIR/payload/msmemscope/python/msmemscope/"
        log_info "Copied _msmemscope.so to python/msmemscope/"
    else
        log_warn "_msmemscope.so not found in lib64 directory"
    fi

    # 创建版本信息文件，放在msmemscope目录下
    echo "version: $VERSION" > "$TEMP_DIR/payload/msmemscope/version.txt"
    echo "build_date: $(date '+%Y-%m-%d %H:%M:%S')" >> "$TEMP_DIR/payload/msmemscope/version.txt"

    # 生成文件校验和
    generate_file_hashes
}

# 创建安装脚本 - 这个脚本会被嵌入到run文件中
create_install_script() {
    cat > "$TEMP_DIR/install.sh" << 'EOF'
#!/bin/bash

# 设置严格模式，遇到错误立即退出
set -e

# 颜色定义用于用户输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 安装配置
TOOL_NAME="msmemscope"
DEFAULT_INSTALL_PATH="."
VERSION="VERSION_PLACEHOLDER"

# 日志函数 - 所有用户输出都是英文
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    echo -e "${BLUE}[DEBUG]${NC} $1"
}

# 验证文件完整性
verify_installation_integrity() {
    local install_path="$1"
    local install_dir="$install_path/msmemscope"
    local checksum_file="$install_dir/file_checksums.sha256"

    log_info "Verifying installation integrity..."

    # 检查校验文件是否存在
    if [ ! -f "$checksum_file" ]; then
        log_warn "Checksum file not found, skipping integrity verification"
        return 0
    fi

    # 检查sha256sum命令
    if ! command -v sha256sum >/dev/null 2>&1; then
        log_warn "sha256sum not available, skipping integrity verification"
        return 0
    fi

    cd "$install_dir"

    # 执行校验
    if sha256sum -c file_checksums.sha256 --quiet > /dev/null 2>&1; then
        log_info "✓ All files passed integrity verification"
        cd - > /dev/null
        return 0
    else
        log_error "✗ Integrity verification failed! Some files are corrupted or modified."
        log_error "Run 'sha256sum -c file_checksums.sha256' for details."
        cd - > /dev/null
        return 1
    fi
}

# 检查磁盘空间是否足够
check_disk_space() {
    local install_path="$1"
    local required_space=100  # 最小需要100MB空间

    # 获取安装路径所在分区的可用空间（MB）
    local available_space=$(df -m "$install_path" | awk 'NR==2 {print $4}')

    if [ "$available_space" -lt "$required_space" ]; then
        log_error "Insufficient disk space! Required: ${required_space}MB, Available: ${available_space}MB"
        return 1
    fi

    log_info "Disk space check passed: ${available_space}MB available"
    return 0
}

# 验证安装路径的合法性
validate_install_path() {
    local install_path="$1"

    # 检查路径是否为空
    if [ -z "$install_path" ]; then
        log_error "Install path cannot be empty"
        return 1
    fi

    # 检查是否为绝对路径
    if [[ "$install_path" != /* ]]; then
        log_error "Install path must be an absolute path: $install_path"
        return 1
    fi

    # 检查安装目录本身是否存在
    if [ -e "$install_path" ]; then
        if [ ! -d "$install_path" ]; then
            log_error "Install path exists but is not a directory: $install_path"
            return 1
        fi
    else
        log_error "Install directory does not exist: $install_path"
        return 1
    fi

    # 检查安装目录是否有写入权限
    if [ ! -w "$install_path" ]; then
        log_error "No write permission for directory: $install_path"
        return 1
    fi

    # 检查路径是否包含特殊字符
    if [[ "$install_path" =~ [\|\<\>\"\'] ]]; then
        log_error "Install path contains invalid characters"
        return 1
    fi

    return 0
}

# 设置文件权限
set_file_permissions() {
    local install_dir="$1/msmemscope"

    # 设置最外层msmemscope文件夹权限为drwxr-x--- (750) - 所有者需要写权限以便删除
    chmod 750 "$install_dir"

    # 设置所有子目录权限为drwxr-x--- (750) - 所有者需要写权限以便删除子目录
    find "$install_dir" -mindepth 1 -type d -exec chmod 750 {} \;

    # 设置bin目录下所有文件权限为-rwxr-x--- (750) - 二进制和shell脚本需要执行权限
    if [ -d "$install_dir/bin" ]; then
        find "$install_dir/bin" -type f -exec chmod 750 {} \;
    fi

    # 设置lib64目录下so文件权限为-rw-r----- (640) - 只需要读权限，所有者需要写权限以便删除
    if [ -d "$install_dir/lib64" ]; then
        find "$install_dir/lib64" -name "*.so" -type f -exec chmod 640 {} \;
    fi

    # 设置python/msmemscope目录下python文件权限为-rw-r----- (640) - python文件只需要读权限
    if [ -d "$install_dir/python/msmemscope" ]; then
        find "$install_dir/python/msmemscope" -name "*.py" -type f -exec chmod 640 {} \;
    fi

    local set_env_script="$install_dir/set_env.sh"
    local uninstall_script="$install_dir/uninstall.sh"
    local version_file="$install_dir/version.txt"

    # set_env.sh和uninstall.sh权限为-rwxr-x--- (750) - 需要执行权限
    [ -f "$set_env_script" ] && chmod 750 "$set_env_script"
    [ -f "$uninstall_script" ] && chmod 750 "$uninstall_script"

    # version.txt权限为640 - 只需要读权限
    [ -f "$version_file" ] && chmod 640 "$version_file"
}

# 创建环境变量设置脚本
create_set_env_script() {
    local install_path="$1"
    local set_env_script="$install_path/msmemscope/set_env.sh"

    log_info "Creating environment setup script..."

    cat > "$set_env_script" << 'SETENV_EOF'
#!/bin/bash

# msmemscope environment setup script
# This script sets up PYTHONPATH and PATH for msmemscope

# Get the directory where this script is located
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

echo "Setting up msmemscope environment..."

# FORCE msmemscope paths to be at the front

# For PYTHONPATH: remove any existing instances and add to front
if [ -d "$SCRIPT_DIR/python" ]; then
    # Remove all occurrences of msmemscope python paths
    local new_pythonpath=""
    if [ -n "$PYTHONPATH" ]; then
        # Split by colon and filter out msmemscope paths
        IFS=':' read -ra paths <<< "$PYTHONPATH"
        for path in "${paths[@]}"; do
            if [[ "$path" != *"msmemscope"* ]]; then
                if [ -z "$new_pythonpath" ]; then
                    new_pythonpath="$path"
                else
                    new_pythonpath="$new_pythonpath:$path"
                fi
            fi
        done
    fi

    # Add msmemscope python path at the very beginning
    if [ -z "$new_pythonpath" ]; then
        export PYTHONPATH="$SCRIPT_DIR/python"
    else
        export PYTHONPATH="$SCRIPT_DIR/python:$new_pythonpath"
    fi
    echo "✓ Added to PYTHONPATH (forced to front): $SCRIPT_DIR/python"
fi

# For PATH: remove any existing instances and add to front
if [ -d "$SCRIPT_DIR/bin" ]; then
    # Remove all occurrences of msmemscope bin paths
    local new_path=""
    if [ -n "$PATH" ]; then
        # Split by colon and filter out msmemscope paths
        IFS=':' read -ra paths <<< "$PATH"
        for path in "${paths[@]}"; do
            if [[ "$path" != *"msmemscope"* ]]; then
                if [ -z "$new_path" ]; then
                    new_path="$path"
                else
                    new_path="$new_path:$path"
                fi
            fi
        done
    fi

    # Add msmemscope bin path at the very beginning
    if [ -z "$new_path" ]; then
        export PATH="$SCRIPT_DIR/bin"
    else
        export PATH="$SCRIPT_DIR/bin:$new_path"
    fi
    echo "✓ Added to PATH (forced to front): $SCRIPT_DIR/bin"
fi

echo "msmemscope environment setup completed"

# Verification
echo ""
echo "Environment verification:"
echo "PYTHONPATH starts with: $(echo $PYTHONPATH | cut -d: -f1)"
echo "PATH starts with: $(echo $PATH | cut -d: -f1)"
SETENV_EOF

    log_info "Environment setup script created: $set_env_script"
}

# 执行实际的安装操作
perform_installation() {
    local install_path="$1"
    local is_upgrade="$2"

    local action="${is_upgrade:-Installing}"
    log_info "${action} to: $install_path/msmemscope"

    mkdir -p "$install_path"

    # 从run文件中提取payload部分并解压
    log_info "Extracting files..."
    # 找到payload起始行号
    PAYLOAD_START=$(awk '/^__PAYLOAD_BELOW__/ {print NR + 1; exit 0; }' "$0")

    # 检查并临时修改安装目录权限
    local write_flag=false
    if [ ! -w "$install_path" ]; then
        log_info "Installation directory not writable, temporarily adding write permission"
        chmod u+w "$install_path"
        write_flag=true
    fi

    # 从payload起始行开始提取并解压到安装目录（使用-C参数指定目录，避免cd操作）
    tail -n +$PAYLOAD_START "$0" | tar -xz -C "$install_path"

    # 恢复安装目录权限
    if [ "$write_flag" = true ]; then
        log_info "Restoring installation directory permissions"
        chmod u-w "$install_path"
    fi

    # 验证文件完整性
    if ! verify_installation_integrity "$install_path"; then
        log_error "Installation integrity check failed"
        exit 1
    fi

    # 设置文件权限
    set_file_permissions "$install_path"

    # 创建环境变量设置脚本
    create_set_env_script "$install_path"

    # 处理cann_uninstall.sh（如果存在）
    local cann_uninstall_path="$(dirname "$install_path")/cann_uninstall.sh"

    # 检查当前路径或父目录是否存在cann_uninstall.sh
    if [ -f "$cann_uninstall_path" ]; then
        log_info "Found cann_uninstall.sh at $cann_uninstall_path, configuring integration..."

        # 1. 注册卸载逻辑到cann_uninstall.sh - 先获取文件权限，添加可写权限，修改后恢复原权限
        local script_right=$(stat -c '%a' "$cann_uninstall_path")
        chmod u+w "$cann_uninstall_path"
        # 先删除已有的 msmemscope 卸载逻辑，避免重复注册
        sed -i "/uninstall_package \"share\/info\/msmemscope\"/d" "$cann_uninstall_path"
        # 在 exit 之前添加新的卸载逻辑
        sed -i "/^exit /i uninstall_package \"share\/info\/msmemscope\"" "$cann_uninstall_path"
        chmod $script_right "$cann_uninstall_path"
        log_info "Registered uninstallation logic to cann_uninstall.sh"

        # 2. 创建share/info/msmemscope目录 - 根据cann_uninstall.sh的位置确定目录
        local base_dir=$(dirname "$cann_uninstall_path")
        local share_info_dir="$base_dir/share/info/msmemscope"
        mkdir -p "$share_info_dir"
        log_info "Created directory: $share_info_dir"

        # 3. 创建version.info文件
        cat > "$share_info_dir/version.info" << VERSION_EOF
[PACKAGE]
Name=mindstudio-memscope
Version=$VERSION
VERSION_EOF
        log_info "Created version.info in $share_info_dir"

        # 4. 创建新的uninstall.sh到share/info/msmemscope目录
        cat > "$share_info_dir/uninstall.sh" << 'CANN_UNINSTALL_EOF'
#!/bin/bash

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# 日志函数
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 设置安装目录
CANN_INSTALL_DIR=../../..
MEMSCOPE_INSTALL_DIR="$CANN_INSTALL_DIR/tools/msmemscope"

log_info "Uninstalling msmemscope from $MEMSCOPE_INSTALL_DIR"

# 删除msmemscope安装目录下的所有内容
if [ -d "$MEMSCOPE_INSTALL_DIR" ]; then
    MEMSCOPE_PARENT_DIR=$(dirname "$MEMSCOPE_INSTALL_DIR")
    MEMSCOPE_PARENT_PERM=$(stat -c '%a' "$MEMSCOPE_PARENT_DIR" 2>/dev/null || echo "")
    if [ -n "$MEMSCOPE_PARENT_PERM" ] && [ ! -w "$MEMSCOPE_PARENT_DIR" ]; then
        chmod u+w "$MEMSCOPE_PARENT_DIR"
    fi
    rm -rf "$MEMSCOPE_INSTALL_DIR"
    if [ -n "$MEMSCOPE_PARENT_PERM" ] && [ ! -w "$MEMSCOPE_PARENT_DIR" ] 2>/dev/null; then
        chmod "$MEMSCOPE_PARENT_PERM" "$MEMSCOPE_PARENT_DIR"
    fi
    log_info "Removed msmemscope installation directory"
fi

# 删除注册的卸载逻辑
INSTALL_PARENT_DIR="$CANN_INSTALL_DIR"
if [ -f "$INSTALL_PARENT_DIR/cann_uninstall.sh" ]; then
    CANN_UNINSTALL_PERM=$(stat -c '%a' "$INSTALL_PARENT_DIR/cann_uninstall.sh" 2>/dev/null || echo "")
    if [ -n "$CANN_UNINSTALL_PERM" ] && [ ! -w "$INSTALL_PARENT_DIR/cann_uninstall.sh" ]; then
        chmod u+w "$INSTALL_PARENT_DIR/cann_uninstall.sh"
    fi
    sed -i "/uninstall_package \"share\/info\/msmemscope\"/d" "$INSTALL_PARENT_DIR/cann_uninstall.sh"
    if [ -n "$CANN_UNINSTALL_PERM" ]; then
        chmod "$CANN_UNINSTALL_PERM" "$INSTALL_PARENT_DIR/cann_uninstall.sh"
    fi
    log_info "Removed uninstallation registration from cann_uninstall.sh"
fi

# 删除share/info/msmemscope目录内容
share_info_dir="$INSTALL_PARENT_DIR/share/info/msmemscope"
if [ -d "$share_info_dir" ]; then
    SHARE_INFO_PARENT_DIR=$(dirname "$share_info_dir")
    SHARE_INFO_PARENT_PERM=$(stat -c '%a' "$SHARE_INFO_PARENT_DIR" 2>/dev/null || echo "")
    if [ -n "$SHARE_INFO_PARENT_PERM" ] && [ ! -w "$SHARE_INFO_PARENT_DIR" ]; then
        chmod u+w "$SHARE_INFO_PARENT_DIR"
    fi
    rm -rf "$share_info_dir"
    if [ -n "$SHARE_INFO_PARENT_PERM" ] && [ ! -w "$SHARE_INFO_PARENT_DIR" ] 2>/dev/null; then
        chmod "$SHARE_INFO_PARENT_PERM" "$SHARE_INFO_PARENT_DIR"
    fi
    log_info "Removed share/info/msmemscope directory"
fi


log_info "msmemscope uninstallation completed successfully"
CANN_UNINSTALL_EOF

        # 设置执行权限
        chmod +x "$share_info_dir/uninstall.sh"
        log_info "Created uninstall.sh in $share_info_dir"
    fi

    log_info "File ${is_upgrade:-installation} completed"
}

# 创建卸载脚本
create_uninstall_script() {
    local install_path="$1"

    # 生成卸载脚本文件，放在msmemscope目录下
    cat > "$install_path/msmemscope/uninstall.sh" << 'UNINSTALL_EOF'
#!/bin/bash

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# 日志函数
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 获取安装目录
if [ -n "${BASH_SOURCE[0]}" ]; then
    # 使用 BASH_SOURCE 更可靠
    SCRIPT_PATH="${BASH_SOURCE[0]}"
else
    # 回退到 $0
    SCRIPT_PATH="$0"
fi
INSTALL_DIR="$(cd "$(dirname "$SCRIPT_PATH")" && pwd)"

# 检查安装完整性，确保是有效的安装
check_installation_integrity() {
    # 检查版本文件是否存在
    if [ ! -f "$INSTALL_DIR/version.txt" ]; then
        log_error "Installation directory is incomplete or corrupted"
        return 1
    fi

    # 检查必要的目录是否存在
    local required_dirs=("bin" "python" "lib64")
    for dir in "${required_dirs[@]}"; do
        if [ ! -d "$INSTALL_DIR/$dir" ]; then
            log_error "Missing required directory: $dir"
            return 1
        fi
    done

    return 0
}

# 确认卸载操作，避免误操作
confirm_uninstall() {
    echo "=============================================="
    echo "           Uninstall msmemscope"
    echo "=============================================="
    echo "Installation directory: $INSTALL_DIR"
    echo "Version: $(grep 'version:' $INSTALL_DIR/version.txt 2>/dev/null | cut -d' ' -f2 || echo 'Unknown')"
    echo ""

    # 显示警告信息
    echo -e "${YELLOW}Warning: This operation will permanently delete ALL files and subdirectories${NC}"
    echo -e "${YELLOW}within the installation directory, including any user-created content!${NC}"
    echo ""
    echo -e "${RED}The following directory and ALL its contents will be deleted:${NC}"
    echo -e "${RED}  $INSTALL_DIR${NC}"
    echo ""

    read -p "Are you sure you want to uninstall? (y/N): " confirm
    case "$confirm" in
        [yY]|[yY][eE][sS])
            return 0
            ;;
        *)
            echo "Uninstall cancelled"
            exit 0
            ;;
    esac
}

# 检查是否有进程正在使用安装目录
check_running_processes() {
    log_info "Checking for processes using the installation directory..."

    # 使用lsof检查是否有进程在使用安装目录
    if command -v lsof >/dev/null 2>&1; then
        if lsof +D "$INSTALL_DIR" 2>/dev/null | grep -q "."; then
            log_warn "Found processes using the installation directory:"
            lsof +D "$INSTALL_DIR" 2>/dev/null | head -10
            return 1
        fi
    fi

    # 使用fuser作为备选检查方法
    if command -v fuser >/dev/null 2>&1; then
        if fuser "$INSTALL_DIR" 2>/dev/null; then
            log_warn "Found processes using the installation directory"
            return 1
        fi
    fi

    return 0
}

cleanup_environment_variables() {
    local install_dir="$INSTALL_DIR"

    # 清理 PYTHONPATH
    if [ -n "$PYTHONPATH" ]; then
        local new_pythonpath=""
        IFS=':' read -ra paths <<< "$PYTHONPATH"
        for path in "${paths[@]}"; do
            # 排除所有包含安装目录的路径
            if [[ "$path" != *"$install_dir"* ]]; then
                if [ -z "$new_pythonpath" ]; then
                    new_pythonpath="$path"
                else
                    new_pythonpath="$new_pythonpath:$path"
                fi
            fi
        done

        if [ -z "$new_pythonpath" ]; then
            unset PYTHONPATH
            log_info "PYTHONPATH unset"
        else
            export PYTHONPATH="$new_pythonpath"
            log_info "PYTHONPATH cleaned"
        fi
    fi

    # 清理 PATH
    if [ -n "$PATH" ]; then
        local new_path=""
        IFS=':' read -ra paths <<< "$PATH"
        for path in "${paths[@]}"; do
            # 排除所有包含安装目录的路径
            if [[ "$path" != *"$install_dir"* ]]; then
                if [ -z "$new_path" ]; then
                    new_path="$path"
                else
                    new_path="$new_path:$path"
                fi
            fi
        done

        export PATH="$new_path"
        log_info "PATH cleaned"
    fi

    log_info "Environment variables cleanup completed"
}

# 执行卸载操作
perform_uninstall() {
    log_info "Starting uninstallation..."

    # 检查是否有进程在使用
    if ! check_running_processes; then
        log_warn "Processes are using the installation directory"
        read -p "Force uninstall anyway? (y/N): " force_uninstall
        case "$force_uninstall" in
            [yY]|[yY][eE][sS])
                log_warn "Forcing uninstall..."
                ;;
            *)
                echo "Uninstall cancelled"
                exit 1
                ;;
        esac
    fi

    # 清理环境变量
    log_info "Cleaning up environment variables..."
    cleanup_environment_variables

    # 删除安装目录
    log_info "Removing installation directory: $INSTALL_DIR"
    rm -rf "$INSTALL_DIR"

    log_info "Uninstallation completed successfully"
}

# 主函数
main() {
    echo "=============================================="
    echo "          msmemscope Uninstaller"
    echo "=============================================="

    # 检查安装完整性
    if ! check_installation_integrity; then
        log_error "Installation directory is incomplete, manual cleanup may be required"
        exit 1
    fi

    # 确认卸载
    confirm_uninstall

    # 执行卸载
    perform_uninstall
}

# 脚本入口点
main "$@"
UNINSTALL_EOF

    log_info "Uninstall script created: $install_path/msmemscope/uninstall.sh"
}

# 显示安装完成信息
show_installation_info() {
    local install_path="$1"
    local is_upgrade="$2"

    # 规范化路径，移除多余的 ./ 和 ../
    local normalized_path=$(cd "$install_path" && pwd)

    local action="${is_upgrade:-Installation}"
    echo ""
    echo "=============================================="
    echo "          $TOOL_NAME ${action} Complete"
    echo "=============================================="
    echo "Installation path: $normalized_path/msmemscope"
    echo "Version: $(grep 'version:' "$install_path/msmemscope/version.txt" | cut -d' ' -f2)"
    echo "Integrity: $(grep 'integrity_check:' "$install_path/msmemscope/version.txt" | cut -d' ' -f2 || echo 'unknown')"
    echo "Install time: $(date '+%Y-%m-%d %H:%M:%S')"
    echo ""
    echo "Environment setup:"
    echo "  To set up environment variables, run: source $normalized_path/msmemscope/set_env.sh"
    echo ""

    # 如果是升级操作，显示升级成功信息
    if [ -n "$is_upgrade" ]; then
        log_info "Upgrade completed successfully"
    else
        log_info "Installation completed successfully"
    fi
}

# 安装模式的主函数
install_main() {
    local install_path=""
    local is_upgrade=false

    # 解析命令行参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            --install-path=*)
                install_path="${1#*=}"
                shift
                ;;
            --upgrade)
                is_upgrade=true
                shift
                ;;
            *)
                log_warn "Unknown parameter: $1"
                shift
                ;;
        esac
    done

    log_info "Starting installation to: $install_path"

    # 设置默认安装路径：检查Ascend环境变量，提示用户选择安装位置
    if [ -z "$install_path" ]; then
        # 检查ASCEND相关环境变量
        local ascend_home=""
        if [ -n "$ASCEND_TOOLKIT_HOME" ]; then
            ascend_home="$ASCEND_TOOLKIT_HOME"
        elif [ -n "$ASCEND_HOME_PATH" ]; then
            ascend_home="$ASCEND_HOME_PATH"
        fi

        if [ -n "$ascend_home" ]; then
            local suggested_path="$ascend_home/tools"
            echo ""
            echo "Detected Ascend environment: $ascend_home"
            read -p "Install to $suggested_path? (Y/n): " confirm
            case "$confirm" in
                [yY]|[yY][eE][sS]|"")
                    install_path="$suggested_path"
                    log_info "Will install to: $install_path"
                    # 若目标目录下已有msmemscope子目录，自动启用升级模式
                    if [ -d "$install_path/msmemscope" ] && [ -f "$install_path/msmemscope/version.txt" ]; then
                        log_info "Existing installation detected at $install_path/msmemscope, auto-enabling upgrade mode"
                        is_upgrade=true
                    fi
                    ;;
                *)
                    log_info "User chose not to install to $suggested_path"
                    ;;
            esac
        fi

        # 若用户未选择Ascend路径，或环境变量不存在，则使用当前目录
        if [ -z "$install_path" ]; then
            install_path="$DEFAULT_INSTALL_PATH"
            # 将相对路径转换为绝对路径，并去除末尾的斜杠
            if [[ "$install_path" != /* ]]; then
                install_path="$(pwd)/$install_path"
            fi
            # 规范化路径，移除多余的 ./ 和 ../
            install_path=$(cd "$install_path" && pwd)
            log_info "Using installation path: $install_path"
        fi
    fi

    # 验证安装路径
    validate_install_path "$install_path" || exit 1

    # 检查是否存在cann_uninstall.sh文件，如果存在则在安装路径后添加/tools
    if [ -f "$install_path/cann_uninstall.sh" ]; then
        log_info "Found cann_uninstall.sh at $install_path, appending '/tools' to installation path"
        install_path="$install_path/tools"
        log_info "Updated installation path: $install_path"
    fi

    # 检查磁盘空间
    check_disk_space "$install_path" || exit 1

    # 检查安装状态
    if [ -d "$install_path/msmemscope" ] && [ -f "$install_path/msmemscope/version.txt" ]; then
        if [ "$is_upgrade" = true ]; then
            # 升级模式
            log_info "Starting upgrade process..."
        else
            # 普通安装模式但已存在，自动转为升级模式
            log_warn "Existing installation detected at: $install_path/msmemscope";
            log_info "Current version: $(cat "$install_path/msmemscope/version.txt" 2>/dev/null || echo "unknown")";
            log_info "Automatically upgrading to version: $VERSION";
            log_info "Starting upgrade process..."
            is_upgrade=true
        fi
    elif [ "$is_upgrade" = true ]; then
        # 升级模式但目录无效
        log_error "Target directory is not a valid installation for upgrade.";
        log_error "A valid installation directory must contain: $install_path/msmemscope/version.txt";
        log_error "Please check the installation path and ensure you're pointing to an existing MSMemScope installation."
        exit 1
    else
        # 全新安装
        log_info "Starting fresh installation..."
    fi

    # 执行安装操作
    perform_installation "$install_path" "$([ "$is_upgrade" = true ] && echo "Upgrading")"
    create_uninstall_script "$install_path"
    show_installation_info "$install_path" "$([ "$is_upgrade" = true ] && echo "Upgrade")"
}

# 升级模式的主函数
upgrade_main() {
    local install_path=""

    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            --install-path=*)
                install_path="${1#*=}"
                shift
                ;;
            *)
                log_warn "Unknown parameter: $1"
                shift
                ;;
        esac
    done

    # 检查必须参数
    if [ -z "$install_path" ]; then
        log_error "Upgrade requires --install-path parameter"
        echo "Usage: $0 --upgrade --install-path=<path>"
        exit 1
    fi

    # 检查目标目录是否存在
    if [ ! -d "$install_path" ]; then
        log_error "Target installation directory does not exist: $install_path"
        exit 1
    fi

    # 验证目标目录是否是有效的安装
    if [ ! -f "$install_path/msmemscope/version.txt" ]; then
        log_error "Target directory is not a valid $TOOL_NAME installation."
        log_error "A valid installation directory must contain: $install_path/msmemscope/version.txt"
        log_error "Please check the installation path and ensure you're pointing to an existing MSMemScope installation."
        exit 1
    fi

    log_info "Starting upgrade for: $install_path"
    # 调用安装主函数，并设置升级标志
    install_main --install-path="$install_path" --upgrade
}

# 显示版本信息
show_version() {
    echo "=============================================="
    echo "           $TOOL_NAME Version Info"
    echo "=============================================="

    # 检查是否在run文件内部（安装前）
    if [ -f "version.txt" ]; then
        # 在构建目录中
        cat "version.txt"
    else
        # 尝试从run文件的payload中提取版本信息
        local payload_start=$(awk '/^__PAYLOAD_BELOW__/ {print NR + 1; exit 0; }' "$0")
        if [ -n "$payload_start" ]; then
            # 提取version.txt文件内容
            tail -n +$payload_start "$0" | tar -xz -O "msmemscope/version.txt" 2>/dev/null || \
            echo "Version: $VERSION (build $(date '+%Y%m%d'))"
        else
            echo "Version: $VERSION (build $(date '+%Y%m%d'))"
        fi
    fi
    echo ""
}

# 显示帮助信息
show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --install               Install the tool"
    echo "  --install-path=PATH     Specify installation path (must be absolute)"
    echo "  --upgrade               Upgrade an existing installation"
    echo "  --uninstall             Uninstall the tool"
    echo "  --version               Show version information"
    echo "  --help                  Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 --install                                    # Install to default path"
    echo "  $0 --install --install-path=/usr/local/msmemscope"
    echo "  $0 --upgrade --install-path=/opt/msmemscope"
    echo "  $0 --uninstall --install-path=/opt/msmemscope"
    echo "  $0 --version                                    # Show version"
    echo "  $0 --help"
    echo ""
}

# 卸载模式的主函数
uninstall_main() {
    local install_path=""

    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            --install-path=*)
                install_path="${1#*=}"
                shift
                ;;
            *)
                log_warn "Unknown parameter: $1"
                shift
                ;;
        esac
    done

    # 检查必须参数
    if [ -z "$install_path" ]; then
        log_error "Uninstall requires --install-path parameter"
        echo "Usage: $0 --uninstall --install-path=<path>"
        exit 1
    fi

    # 如果install-path目录下存在cann_uninstall.sh，说明传入的是CANN根目录，需要追加tools子目录
    if [ -f "$install_path/cann_uninstall.sh" ]; then
        log_info "Detected CANN root directory, appending 'tools' to install path"
        install_path="$install_path/tools"
    fi

    # 检查目标目录是否存在
    if [ ! -d "$install_path" ]; then
        log_error "Target installation directory does not exist: $install_path"
        exit 1
    fi

    # 验证目标目录是否是有效的安装
    local uninstall_script="$install_path/msmemscope/uninstall.sh"
    if [ ! -f "$uninstall_script" ]; then
        log_error "Target directory is not a valid $TOOL_NAME installation."
        log_error "A valid installation directory must contain: $uninstall_script"
        log_error "Please check the installation path and ensure you're pointing to an existing MSMemScope installation."
        exit 1
    fi

    log_info "Starting uninstallation for: $install_path"

    # 直接调用已安装软件包中的uninstall.sh，并自动输入多个y确认（支持多次确认提示）
    echo -e "y\ny" | bash "$uninstall_script"

    # 删除cann_uninstall.sh中的注册信息
    local cann_uninstall_path="$install_path/cann_uninstall.sh"
    if [ -f "$cann_uninstall_path" ]; then
        log_info "Removing uninstall registration from cann_uninstall.sh"
        sed -i "/uninstall_package \"share\/info\/msmemscope\"/d" "$cann_uninstall_path"
        log_info "Uninstall registration removed from cann_uninstall.sh"
    fi

    # 删除share/info/msmemscope目录
    local share_info_dir="$install_path/share/info/msmemscope"
    if [ -d "$share_info_dir" ]; then
        log_info "Removing share/info/msmemscope directory"
        rm -rf "$share_info_dir"
        log_info "share/info/msmemscope directory removed"
    fi

    log_info "Uninstallation completed successfully"

    # 检查卸载是否成功
    if [ $? -eq 0 ]; then
        log_info "Uninstallation completed successfully"
    else
        log_error "Uninstallation failed"
        exit 1
    fi
}

# 主执行逻辑 - 根据参数调用不同的功能模块
main() {
    # 检查是否有参数，没有则显示帮助
    if [ $# -eq 0 ]; then
        show_help
        exit 0
    fi

    case "$1" in
        --install)
            shift
            install_main "$@"
            ;;
        --upgrade)
            shift
            upgrade_main "$@"
            ;;
        --uninstall)
            shift
            uninstall_main "$@"
            ;;
        --version|-v)
            show_version
            ;;
        --help|-h)
            show_help
            ;;
        *)
            log_error "Unknown command: $1"
            show_help
            exit 1
            ;;
    esac
}

# 脚本入口点
main "$@"
exit 0

# 标记payload开始的位置，安装脚本会从这里开始读取压缩包
__PAYLOAD_BELOW__
EOF
}

# 创建最终的run文件
create_run_file() {
    log_info "Creating run file: $RUN_FILE"

    # 复制安装脚本到run文件
    cp "$TEMP_DIR/install.sh" "$BUILD_DIR/$RUN_FILE"

    # 替换版本号占位符为实际版本号
    sed -i "s/VERSION_PLACEHOLDER/$VERSION/g" "$BUILD_DIR/$RUN_FILE"

    # 打包payload并追加到run文件
    log_info "Packaging payload..."
    cd "$TEMP_DIR/payload"
    tar -cz . >> "$BUILD_DIR/$RUN_FILE"  # 使用.而不是*避免隐藏文件被忽略
    cd - > /dev/null

    chmod 740 "$BUILD_DIR/$RUN_FILE"

    # 验证run文件是否创建成功
    if [ -f "$BUILD_DIR/$RUN_FILE" ] && [ -x "$BUILD_DIR/$RUN_FILE" ]; then
        log_info "Run file created successfully: $BUILD_DIR/$RUN_FILE"
    else
        log_error "Failed to create run file"
        return 1
    fi
}

# 清理临时文件
cleanup() {
    if [ -d "$TEMP_DIR" ]; then
        log_info "Cleaning up temporary files..."
        rm -rf "$TEMP_DIR"
    fi
}

# 显示构建完成信息
show_build_info() {
    local run_file_size=$(du -h "$RUN_FILE" | cut -f1)
    local run_file_path="$BUILD_DIR/$RUN_FILE"

    # 从version.txt获取版本信息
    local version_info=""
    local integrity_info=""
    if [ -f "$TEMP_DIR/payload/msmemscope/version.txt" ]; then
        version_info=$(grep "version:" "$TEMP_DIR/payload/msmemscope/version.txt" | cut -d' ' -f2)
        integrity_info=$(grep "integrity_check:" "$TEMP_DIR/payload/msmemscope/version.txt" | cut -d' ' -f2)
    fi

    echo ""
    echo "=============================================="
    echo "           $TOOL_NAME Run Package Built"
    echo "=============================================="
    echo "File: $RUN_FILE"
    echo "Size: $run_file_size"
    echo "Package: $(basename "$RUN_FILE")"
    echo "Version: ${version_info:-1.0.0}"
    echo "Integrity Check: ${integrity_info:-enabled}"
    echo ""
    echo "Usage instructions:"
    echo "  Install: bash $RUN_FILE --install [--install-path=/path]"
    echo "  Upgrade: bash $RUN_FILE --upgrade --install-path=/path"
    echo "  Version: bash $RUN_FILE --version"
    echo "  Help:    bash $RUN_FILE --help"
    echo ""
    echo "After installation:"
    echo "  To set up environment: source <install-path>/msmemscope/set_env.sh"
    echo ""
    log_info "Build process completed successfully"
}

# 主构建函数
main() {
    # 环境变量优先级最高
    if [ -n "$BUILD_VERSION" ]; then
        VERSION="$BUILD_VERSION"
    fi

    # 解析命令行参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            --build-version=*)
                if [ -z "$BUILD_VERSION" ]; then
                    VERSION="${1#*=}"
                fi
                shift
                ;;
            --build-version)
                if [ -z "$BUILD_VERSION" ]; then
                    VERSION="$2"
                fi
                shift 2
                ;;
            *)
                log_warn "Unknown parameter: $1"
                shift
                ;;
        esac
    done

    # 更新 RUN_FILE
    RUN_FILE="mindstudio-memscope_${VERSION}_${ARCH}.run"

    log_info "Starting build process for $RUN_FILE ..."

    # 检查源目录
    if ! check_source_dirs; then
        log_error "Source directory check failed, build aborted"
        exit 1
    fi

    # 创建临时目录
    create_temp_dir

    # 设置退出时清理临时目录
    trap cleanup EXIT

    # 复制交付件
    copy_artifacts

    # 创建安装脚本
    create_install_script

    # 创建run文件
    if ! create_run_file; then
        log_error "Failed to create run file"
        exit 1
    fi

    # 显示构建完成信息
    show_build_info
}

# 脚本入口点
main "$@"
