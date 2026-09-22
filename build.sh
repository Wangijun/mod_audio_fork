#!/bin/bash
set -e

# ============================================================
# build.sh - 构建并安装 mod_audio_fork for FreeSWITCH
# ============================================================

# 解析脚本目录（在任何 cd 之前执行一次）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 自动推导 FreeSWITCH 默认安装路径
if [ -d "/data/docker_fs_server/freeswitch-install/include/freeswitch" ]; then
    DEFAULT_FS_INC="/data/docker_fs_server/freeswitch-install/include/freeswitch"
    DEFAULT_FS_LIB="/data/docker_fs_server/freeswitch-install/lib/libfreeswitch.so"
    DEFAULT_FS_MOD="/data/docker_fs_server/freeswitch-install/lib/freeswitch/mod"
else
    DEFAULT_FS_INC="/usr/local/freeswitch/include/freeswitch"
    DEFAULT_FS_LIB="/usr/local/freeswitch/lib/libfreeswitch.so"
    DEFAULT_FS_MOD="/usr/local/freeswitch/mod"
fi

# 配置（可通过环境变量覆盖）
FREESWITCH_INCLUDE_DIR="${FREESWITCH_INCLUDE_DIR:-$DEFAULT_FS_INC}"
FREESWITCH_LIBRARY="${FREESWITCH_LIBRARY:-$DEFAULT_FS_LIB}"
FREESWITCH_MOD_DIR="${FREESWITCH_MOD_DIR:-$DEFAULT_FS_MOD}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
INSTALL="${INSTALL:-true}"

# 输出颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # 无颜色

log_info()  { echo -e "${GREEN}[信息]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[警告]${NC} $*"; }
log_error() { echo -e "${RED}[错误]${NC} $*"; }

# ---- 安装依赖 ----
install_dependencies() {
    log_info "正在安装构建依赖..."
    apt-get update -qq
    apt-get install -y -qq cmake libwebsockets-dev git build-essential 2>&1 | tail -5
    log_info "依赖安装完成。"
}

# ---- 构建 ----
build() {
    log_info "正在构建 mod_audio_fork (${BUILD_TYPE})..."
    log_info "  FreeSWITCH 头文件目录: ${FREESWITCH_INCLUDE_DIR}"
    log_info "  FreeSWITCH 库文件: ${FREESWITCH_LIBRARY}"

    # 验证 FreeSWITCH 路径是否存在
    if [ ! -d "${FREESWITCH_INCLUDE_DIR}" ]; then
        log_error "未找到 FreeSWITCH 头文件目录: ${FREESWITCH_INCLUDE_DIR}"
        exit 1
    fi
    if [ ! -f "${FREESWITCH_LIBRARY}" ]; then
        log_error "未找到 FreeSWITCH 库文件: ${FREESWITCH_LIBRARY}"
        exit 1
    fi

    # 创建构建目录
    mkdir -p "${SCRIPT_DIR}/build"
    cd "${SCRIPT_DIR}/build"

    # 配置
    cmake .. \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DFREESWITCH_INCLUDE_DIR="${FREESWITCH_INCLUDE_DIR}" \
        -DFREESWITCH_LIBRARY="${FREESWITCH_LIBRARY}"

    # 构建
    make -j"$(nproc)"

    log_info "构建完成: ${SCRIPT_DIR}/build/mod_audio_fork.so"
}

# ---- 安装 ----
install_module() {
    local so_file="${SCRIPT_DIR}/build/mod_audio_fork.so"

    if [ ! -f "${so_file}" ]; then
        if [ -f "./build/mod_audio_fork.so" ]; then
            so_file="./build/mod_audio_fork.so"
        elif [ -f "./mod_audio_fork.so" ]; then
            so_file="./mod_audio_fork.so"
        else
            local found
            found="$(find "${SCRIPT_DIR}" -name 'mod_audio_fork.so' -type f 2>/dev/null | head -1)"
            if [ -n "${found}" ]; then
                so_file="${found}"
            else
                log_error "未找到 mod_audio_fork.so，请先运行构建。"
                exit 1
            fi
        fi
    fi

    log_info "正在安装 ${so_file} 到 ${FREESWITCH_MOD_DIR}..."
    mkdir -p "${FREESWITCH_MOD_DIR}"
    cp "${so_file}" "${FREESWITCH_MOD_DIR}/"
    log_info "模块安装成功。"
}

# ---- 主逻辑 ----
usage() {
    echo "用法: $0 [deps|build|install|all]"
    echo ""
    echo "命令:"
    echo "  deps      安装构建依赖（需要 root 权限）"
    echo "  build     配置并构建 mod_audio_fork"
    echo "  install   将 mod_audio_fork.so 复制到 FreeSWITCH 模块目录"
    echo "  all       执行 deps + build + install（默认）"
    echo ""
    echo "环境变量:"
    echo "  FREESWITCH_INCLUDE_DIR  (默认: $DEFAULT_FS_INC)"
    echo "  FREESWITCH_LIBRARY      (默认: $DEFAULT_FS_LIB)"
    echo "  FREESWITCH_MOD_DIR      (默认: $DEFAULT_FS_MOD)"
    echo "  BUILD_TYPE              (默认: Release)"
}

CMD="${1:-build}"

case "${CMD}" in
    deps)
        install_dependencies
        ;;
    build)
        build
        ;;
    install)
        install_module
        ;;
    all)
        install_dependencies
        build
        install_module
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        log_error "未知命令: ${CMD}"
        usage
        exit 1
        ;;
esac
