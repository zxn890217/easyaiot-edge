#!/usr/bin/env bash
# ============================================
# RK3588 边缘盒子 —— 构建 / 更新 一键脚本
# ============================================
# 面向已刷好 Ubuntu/Debian/Buildroot 的 RK3588 盒子，把「NPU 体检 → 编译带 RKNN 后端的
# RUNTIME → 构建 arm64 镜像 → 接线设备透传 → 重启 → 端到端校验」串成一条命令。
# x86 主机上跑不动 NPU 那部分，可用 doctor/verify 做演练（RK3588_ALLOW_NON_ARM64=1）。
#
# 用法（首次拉下来若无执行位：chmod +x install_rk3588.sh，或直接 bash install_rk3588.sh）:
#   ./install_rk3588.sh doctor               # 只体检，不改任何东西（先看驱动/SDK/设备齐不齐）
#   ./install_rk3588.sh install              # 首次安装：中间件 + VIDEO + WEB + RUNTIME
#   ./install_rk3588.sh build                # 只构建：arm64 镜像 + RUNTIME(RKNN) + NPU 挂载接线
#   ./install_rk3588.sh update               # git pull → build → 重启服务 → verify
#   ./install_rk3588.sh verify               # NPU 端到端校验（容器设备/librknnrt/后端回落/健康）
#   ./install_rk3588.sh npu                  # NPU 设备节点、librknnrt 版本、三核负载一览
#   ./install_rk3588.sh runtime-bundle X.tgz # 用离线包更新 /opt/easyaiot/RUNTIME（OTA 场景）
#   ./install_rk3588.sh model-export         # 打印 .rknn 转换的正确姿势（只能在 x86 控制面做）
#   ./install_rk3588.sh status|logs|restart|start|stop
#
# 环境变量:
#   RKNN_SDK_ROOT          rknpu2 SDK 目录（其下有 include/rknn_api.h）。留空自动探测，
#                          约定落点 RUNTIME/.rknn-sdk —— docker 编译时它随 /src 一起进容器
#   RUNTIME_WITH_RKNN      on(默认) / auto / off
#                          on：缺 SDK 直接失败，避免静默编出无 NPU 后端的二进制（盒子只能跑 CPU）
#   RUNTIME_INFER_BACKEND  auto(默认) / rknn / onnx —— 透传给 VIDEO 控制面与 RUNTIME
#   RUNTIME_NPU_CORE_MASK  auto(默认) / all / per_thread / core0 / core1 / core2 / core0_1
#   EASYAIOT_RUNTIME_BUILD_MODE  docker(默认，与 VIDEO 同源 glibc) / host(需 conda)
#   WEB_PORT               WEB HTTPS 端口（默认 8888）
#   RK3588_SKIP_VIDEO=1 / RK3588_SKIP_WEB=1 / EASYAIOT_RUNTIME_SKIP=1   分段跳过
#   RK3588_ALLOW_NON_ARM64=1  非 aarch64 机器上演练本脚本
#   EASYAIOT_MEDIA_ROOT    媒体根（默认 /mnt/easyaiot-media）
# ============================================
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
export EASYAIOT_ROOT="$SCRIPT_DIR"
export EASYAIOT_DEPLOY_PROFILE=edge
export EASYAIOT_EDGE_MORPHOLOGY="${EASYAIOT_EDGE_MORPHOLOGY:-standalone}"
export EASYAIOT_MEDIA_ROOT="${EASYAIOT_MEDIA_ROOT:-/mnt/easyaiot-media}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()    { echo -e "${BLUE}[RK3588]${NC} $1"; }
success() { echo -e "${GREEN}[RK3588]${NC} $1"; }
warn()    { echo -e "${YELLOW}[RK3588]${NC} $1"; }
error()   { echo -e "${RED}[RK3588]${NC} $1"; }
step()    { echo -e "\n${BLUE}==== $1 ====${NC}"; }
# .scripts/docker/module_update_helpers.sh 优先复用 print_* 输出
print_info()    { info "$1"; }
print_success() { success "$1"; }
print_warning() { warn "$1"; }
print_error()   { error "$1"; }

# 与各模块安装器共用同一份探测逻辑（RKNN SDK / 驱动库 / 设备节点）
# shellcheck disable=SC1091
source "$SCRIPT_DIR/RUNTIME/scripts/rknn_sdk.sh"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/.scripts/docker/module_update_helpers.sh"

WEB_PORT="${WEB_PORT:-8888}"
RUNTIME_WITH_RKNN="${RUNTIME_WITH_RKNN:-on}"
export WEB_PORT RUNTIME_WITH_RKNN

RUNTIME_BIN_PATH="$SCRIPT_DIR/RUNTIME/build/RUNTIME"
VIDEO_CONTAINER="video-service"

is_arm64() {
    case "$(uname -m)" in
        aarch64|arm64) return 0 ;;
        *) return 1 ;;
    esac
}

# aarch64 走 VIDEO 的 ARM 安装器（Dockerfile.arm + linux/arm64 基础镜像 + arm wheels 缓存）
video_installer() {
    if is_arm64 && [ -f "VIDEO/install_linux_arm.sh" ]; then
        echo "VIDEO/install_linux_arm.sh"
    else
        echo "VIDEO/install_linux.sh"
    fi
}

docker_ready() {
    command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1
}

# 磁盘可用量（整数 GB）。只用 POSIX df：busybox/Buildroot 的 df 没有 GNU 的 -BG --output。
# 取不到就输出空串，调用方按「未知」处理。
avail_gb() {
    local path="${1:-.}" kb
    kb="$(df -Pk "$path" 2>/dev/null | awk 'END { if ($4 ~ /^[0-9]+$/) print $4 }')"
    [ -n "$kb" ] || return 0
    awk -v k="$kb" 'BEGIN { printf "%d", k / 1048576 }'
}

# ============================================================
# 体检
# ============================================================
rknn_build_mode_hint() {
    local mode="${EASYAIOT_RUNTIME_BUILD_MODE:-docker}"
    if [ "$mode" = "docker" ]; then
        info "  docker 编译模式下 SDK 必须进容器：放 RUNTIME/.rknn-sdk 最省事（仓库已随 /src 挂载）"
    fi
}

check_arch() {
    local next="${1:-doctor}"
    step "架构与系统"
    printf '  主机名    : %s\n' "$(hostname 2>/dev/null || echo '-')"
    printf '  内核      : %s\n' "$(uname -sr)"
    printf '  架构      : %s\n' "$(uname -m)"
    if [ -r /etc/os-release ]; then
        printf '  发行版    : %s\n' "$( . /etc/os-release && echo "${PRETTY_NAME:-$NAME}")"
    fi
    if is_arm64; then
        success "  aarch64：符合 RK3588 盒子预期"
        return 0
    fi
    if [ "${RK3588_ALLOW_NON_ARM64:-0}" = "1" ]; then
        warn "  当前架构 $(uname -m) 非 aarch64：已按 RK3588_ALLOW_NON_ARM64=1 放行，仅用于演练"
        return 0
    fi
    error "  当前架构 $(uname -m) 不是 aarch64：RK3588 脚本请在盒子上运行"
    error "  只想在 x86 上产出 arm64 离线包：bash RUNTIME/scripts/export_runtime_os_container.sh ubuntu24"
    error "  确需放行：RK3588_ALLOW_NON_ARM64=1 $0 $next"
    return 1
}

check_npu_driver() {
    step "NPU 驱动与设备节点"
    local lib rc=0
    while IFS= read -r node; do
        [ -n "$node" ] && printf '  设备节点  : %s\n' "$node"
    done <<< "$(npu_device_nodes)"
    if [ -z "$(npu_device_nodes)" ]; then
        error "  未找到 /dev/rga、/dev/rknpu、/dev/mpp_service、/dev/dri/renderD* 任一节点"
        error "  请确认内核已加载 rknpu / rga / rkmpp 驱动（厂商固件或 rkdeveloptool 刷的镜像）"
        rc=1
    fi
    if lib="$(rknn_runtime_lib)"; then
        printf '  运行时库  : %s\n' "$lib"
        local ver
        ver="$(strings -a "$lib" 2>/dev/null | grep -m1 -i 'librknnrt version' || true)"
        [ -n "$ver" ] && printf '  库版本    : %s\n' "$ver"
    else
        error "  未找到 librknnrt.so：RUNTIME 会回落 ONNX Runtime CPU，NPU 用不上"
        error "  装法：把 rknpu2 的 runtime/RK3588/Linux/librknn_api/aarch64/librknnrt.so"
        error "        拷到 /usr/lib/ 并 ldconfig，或设置 RKNN_SDK_ROOT 指向 SDK"
        rc=1
    fi
    return "$rc"
}

check_rknn_sdk() {
    step "编译期 RKNN SDK（rknn_api.h）"
    local sdk
    if sdk="$(rknn_sdk_probe)"; then
        success "  SDK 根目录: $sdk"
        printf '  头文件    : %s\n' "$(ls "$sdk"/include/rknn_api.h "$sdk"/rknpu2/include/rknn_api.h \
            "$sdk"/librknn_api/include/rknn_api.h 2>/dev/null | head -n1)"
        rknn_build_mode_hint
        return 0
    fi
    if rknn_header_found; then
        success "  系统 include 里已有 rknn_api.h，可直接编 RKNN 后端"
        return 0
    fi
    if [ "$(printf '%s' "$RUNTIME_WITH_RKNN" | tr '[:upper:]' '[:lower:]')" = "off" ]; then
        warn "  RUNTIME_WITH_RKNN=off：不编 NPU 后端，跳过 SDK 检查"
        return 0
    fi
    error "  未找到 rknpu2 SDK（rknn_api.h），RUNTIME 编不出 RKNN 后端"
    error "  取 SDK：Rockchip 官方 rknpu2 包的 runtime/RK3588/Linux/librknn_api"
    error "          （内含 include/rknn_api.h 与 aarch64/librknnrt.so）"
    error "  摆放（二选一）："
    error "    1) mkdir -p RUNTIME/.rknn-sdk && cp -a <SDK>/include <SDK>/aarch64 \"$PWD/RUNTIME/.rknn-sdk/\""
    error "       （docker 编译模式唯一可行做法，目录已在 .gitignore 内）"
    error "    2) export RKNN_SDK_ROOT=/path/to/librknn_api"
    if [ "$(printf '%s' "$RUNTIME_WITH_RKNN" | tr '[:upper:]' '[:lower:]')" = "auto" ]; then
        warn "  RUNTIME_WITH_RKNN=auto：将继续编纯 ONNX Runtime 版本"
        return 0
    fi
    return 1
}

check_resources() {
    step "资源与磁盘"
    local cores mem_gb free_gb
    cores="$(nproc 2>/dev/null || echo '?')"
    mem_gb="$(awk '/MemTotal/ {printf "%.1f", $2/1048576}' /proc/meminfo 2>/dev/null || echo '?')"
    free_gb="$(avail_gb "$SCRIPT_DIR")"
    printf '  CPU 核数  : %s\n' "$cores"
    printf '  内存      : %s GB\n' "$mem_gb"
    printf '  仓库盘可用: %s GB\n' "${free_gb:-?}"
    [ "$cores" != "?" ] && [ "$cores" -lt 4 ] && warn "  核数偏少，RUNTIME 编译会很久；可 EASYAIOT_RUNTIME_SKIP=1 分段执行"
    [ -n "${free_gb:-}" ] && [ "$free_gb" -lt 20 ] && warn "  剩余空间不足 20 GB，镜像与 .build-cache 可能写满"
    mkdir -p "$EASYAIOT_MEDIA_ROOT"/{alert_images,playbacks,local-storage} 2>/dev/null \
        || info "  媒体根 ${EASYAIOT_MEDIA_ROOT} 需要 sudo 才能创建（安装阶段会再试一次）"
    return 0
}

# ldd 直接看二进制有没有链到 librknnrt（CMakeLists 只在开启 RKNN 时才链接）
binary_links_rknn() {
    command -v ldd >/dev/null 2>&1 \
        && ldd "$1" 2>/dev/null | grep -q librknnrt
}

# 已产出的二进制是否真带 RKNN 后端：0=带 1=不带 2=还没编
runtime_has_rknn() {
    [ -x "$RUNTIME_BIN_PATH" ] || return 2
    local cache="$SCRIPT_DIR/RUNTIME/build/CMakeCache.txt"
    if [ -f "$cache" ] && grep -q '^RUNTIME_WITH_RKNN:BOOL=ON' "$cache" 2>/dev/null; then
        return 0
    fi
    binary_links_rknn "$RUNTIME_BIN_PATH" && return 0
    return 1
}

report_runtime_artifact() {
    step "RUNTIME 产物"
    local rc=0
    runtime_has_rknn || rc=$?
    case "$rc" in
        0) success "  $RUNTIME_BIN_PATH 已编入 RKNN NPU 后端" ;;
        1) error   "  $RUNTIME_BIN_PATH 未编入 RKNN 后端（当前只会用 ONNX Runtime CPU）"
           error   "  重编：$0 build" ;;
        2) warn    "  尚未编译：$RUNTIME_BIN_PATH 不存在（先 $0 build 或 install）" ;;
    esac
    if [ -f "$SCRIPT_DIR/RUNTIME/build/VERSION" ]; then
        printf '  版本      : %s\n' "$(grep -m1 '^version=' "$SCRIPT_DIR/RUNTIME/build/VERSION" | cut -d= -f2-)"
    fi
    [ -f "$SCRIPT_DIR/RUNTIME/deploy.env" ] && printf '  deploy.env: %s\n' "$SCRIPT_DIR/RUNTIME/deploy.env"
    return 0
}

doctor_all() {
    local what="${1:-doctor}" rc=0
    check_arch "$what" || rc=1
    docker_ready || { error "Docker 不可用（或未运行）：请先装 docker + compose 插件"; rc=1; }
    [ -f "$(video_installer)" ] || { error "缺少 $(video_installer)"; rc=1; }
    if is_arm64 || [ "${RK3588_ALLOW_NON_ARM64:-0}" = "1" ]; then
        check_npu_driver || rc=1
        check_rknn_sdk || rc=1
    fi
    check_resources
    report_runtime_artifact
    echo ""
    if [ "$rc" -eq 0 ]; then
        success "体检通过，可以执行：$0 build"
    else
        error "体检有阻塞项，先按上面的提示补齐再 build/update"
    fi
    return "$rc"
}

npu_snapshot() {
    step "NPU 状态"
    local nodes
    nodes="$(npu_device_nodes)"
    if [ -n "$nodes" ]; then
        ls -l $nodes 2>/dev/null | sed 's/^/  /' || true
    else
        warn "  无 NPU/RGA/MPP 设备节点"
    fi
    local lib
    if lib="$(rknn_runtime_lib)"; then
        printf '  librknnrt : %s\n' "$lib"
        ls -l "$lib" | sed 's/^/  /' || true
    fi
    local f
    for f in /sys/kernel/debug/rknpu/load /sys/class/devfreq/fdab0000.npu/load; do
        if [ -r "$f" ]; then
            printf '  NPU 负载  : %s -> %s\n' "$f" "$(tr -s ' \t' ' ' < "$f")"
            return 0
        fi
    done
    if [ -d /sys/class/devfreq/fdab0000.npu ]; then
        warn "  NPU 负载需 root 或挂载 debugfs 才能读：sudo cat /sys/kernel/debug/rknpu/load"
    fi
    for f in /sys/class/devfreq/fdab0000.npu/governor /sys/class/devfreq/fdb00000.gpu/governor; do
        [ -r "$f" ] && printf '  调频策略  : %s = %s\n' "$f" "$(cat "$f")"
    done
    if [ -r /sys/class/devfreq/fdab0000.npu/governor ] \
       && [ "$(cat /sys/class/devfreq/fdab0000.npu/governor)" != "performance" ]; then
        warn "  NPU 未锁定 performance，推理时延会有抖动："
        warn "    echo performance | sudo tee /sys/class/devfreq/fdab0000.npu/governor"
    fi
    return 0
}

# ============================================================
# 构建
# ============================================================
export_rknn_env() {
    local sdk=""
    sdk="$(rknn_sdk_probe || true)"
    if [ -n "$sdk" ]; then
        export RKNN_SDK_ROOT="$sdk"
        info "使用 RKNN SDK: $sdk"
    fi
    if [ -n "${RUNTIME_INFER_BACKEND:-}" ]; then
        export RUNTIME_INFER_BACKEND
        info "推理后端强制: $RUNTIME_INFER_BACKEND"
    fi
    if [ -n "${RUNTIME_NPU_CORE_MASK:-}" ]; then
        local mask
        if mask="$(npu_core_mask_normalize "$RUNTIME_NPU_CORE_MASK")"; then
            export RUNTIME_NPU_CORE_MASK="$mask"
            info "NPU 核绑定: $mask"
        else
            error "RUNTIME_NPU_CORE_MASK 取值非法: $RUNTIME_NPU_CORE_MASK"
            error "  可用：auto|all|per_thread|core0|core1|core2|core0_1|core0_2|core1_2|数字掩码"
            return 1
        fi
    fi
    # 避免 RUNTIME 安装器在 TTY 下反问编译方式（盒子多为 ssh 无人值守）
    export EASYAIOT_RUNTIME_BUILD_MODE="${EASYAIOT_RUNTIME_BUILD_MODE:-docker}"
    return 0
}

build_runtime_arm() {
    if [ "${EASYAIOT_RUNTIME_SKIP:-0}" = "1" ]; then
        warn "EASYAIOT_RUNTIME_SKIP=1，跳过 RUNTIME 编译"
        return 0
    fi
    step "编译 RUNTIME（RKNN NPU 后端，RUNTIME_WITH_RKNN=$RUNTIME_WITH_RKNN）"
    if ! bash RUNTIME/install_linux.sh build; then
        error "RUNTIME 编译失败"
        return 1
    fi
    local rc=0
    runtime_has_rknn || rc=$?
    if [ "$rc" != "0" ]; then
        error "编译完成但二进制里没有 RKNN 后端，NPU 不会生效"
        [ "$rc" = "2" ] && error "  产物缺失：$RUNTIME_BIN_PATH"
        [ "$rc" = "1" ] && error "  多半是 SDK 没被容器看到：见 RUNTIME/scripts/rknn_sdk.sh 的探测路径"
        return 1
    fi
    success "RUNTIME 已带 RKNN 后端"
    return 0
}

build_images() {
    local vi; vi="$(video_installer)"
    if [ "${RK3588_SKIP_VIDEO:-0}" = "1" ]; then
        warn "RK3588_SKIP_VIDEO=1，跳过 VIDEO 镜像构建"
    else
        step "构建 VIDEO 镜像（$vi，linux/arm64）"
        # 先出镜像：RUNTIME 默认在 video-service 同源容器里编译，glibc 才和运行环境一致
        bash "$vi" build
    fi

    build_runtime_arm

    if [ "${RK3588_SKIP_WEB:-0}" = "1" ]; then
        warn "RK3588_SKIP_WEB=1，跳过 WEB 镜像构建"
    else
        step "构建 WEB 镜像"
        bash WEB/install_linux.sh build
    fi
}

wire_npu_mount() {
    step "接线 NPU 设备/库到 VIDEO 容器"
    # 该脚本会探测 librknnrt.so 与设备节点，生成 .docker-compose.runtime.override.yaml
    bash VIDEO/scripts/ensure_runtime_cpp.sh wire
}

# ============================================================
# 更新 / 安装
# ============================================================
git_sync() {
    step "同步代码"
    if [ ! -d "$SCRIPT_DIR/.git" ]; then
        warn "  非 git 仓库（离线安装包），跳过 git pull"
        return 0
    fi
    if ! easyaiot_git_worktree_clean; then
        warn "  工作区有未提交改动，跳过 git pull（避免覆盖现场修改）"
        # head 提前退出会让 git 收到 SIGPIPE，pipefail 下会误杀本脚本，故显式吞掉
        git status --porcelain 2>/dev/null | head -n 20 | sed 's/^/    /' || true
        return 0
    fi
    easyaiot_git_pull_ff_only strict
}

restart_services() {
    step "重启服务"
    bash "$(video_installer)" restart || true
    bash WEB/install_linux.sh restart || true
}

install_all() {
    step "首次安装（standalone 边缘形态）"
    docker_ready || { error "请先安装并启动 Docker"; return 1; }
    mkdir -p "$EASYAIOT_MEDIA_ROOT"/{alert_images,playbacks,local-storage} 2>/dev/null \
        || sudo mkdir -p "$EASYAIOT_MEDIA_ROOT"/{alert_images,playbacks,local-storage}
    # 镜像先行，RUNTIME 才能在 video-service 同源容器里编译
    if [ "${RK3588_SKIP_VIDEO:-0}" != "1" ]; then
        bash "$(video_installer)" build || warn "VIDEO 镜像构建失败，稍后由 install 重试"
    fi
    build_runtime_arm || return 1
    bash install.sh install
    wire_npu_mount
    bash "$(video_installer)" restart || true
}

update_all() {
    doctor_all update || return 1
    git_sync || return 1
    build_images || return 1
    wire_npu_mount
    restart_services
    verify_all
}

# ============================================================
# 校验
# ============================================================
verify_host_side() {
    step "宿主侧"
    local rc=0
    runtime_has_rknn || rc=$?
    [ "$rc" = "0" ] || { error "  RUNTIME 未带 RKNN 后端，NPU 校验无法通过"; rc=1; }
    if docker_ready; then
        success "  Docker 可用"
    else
        error "  Docker 不可用"; rc=1
    fi
    if [ -s "$SCRIPT_DIR/VIDEO/.docker-compose.runtime.override.yaml" ]; then
        if grep -q 'rknn-lib' "$SCRIPT_DIR/VIDEO/.docker-compose.runtime.override.yaml"; then
            success "  compose override 已挂载 rknn-lib"
        else
            error "  override 里没有 rknn-lib：跑一次 $0 build 或在盒子上补 RKNN SDK/librknnrt.so"
            rc=1
        fi
        grep -E '^\s+- /dev/' "$SCRIPT_DIR/VIDEO/.docker-compose.runtime.override.yaml" \
            | sed 's/^/    设备透传:/' || warn "  override 未包含任何 /dev 节点"
    else
        error "  缺少 VIDEO/.docker-compose.runtime.override.yaml（跑 $0 build 生成）"
        rc=1
    fi
    return "$rc"
}

verify_container_side() {
    step "容器内 NPU 通路"
    if ! docker_ready; then
        error "  Docker 不可用，跳过容器内校验"
        return 1
    fi
    local state
    state="$(docker inspect -f '{{.State.Status}}' "$VIDEO_CONTAINER" 2>/dev/null || echo missing)"
    if [ "$state" != "running" ]; then
        error "  ${VIDEO_CONTAINER} 未运行（${state}）：先 $0 restart"
        return 1
    fi
    local rc=0 out
    out="$(docker exec "$VIDEO_CONTAINER" sh -c '
        echo "-- 设备节点 --"
        ls /dev/rga /dev/rknpu /dev/rknpu_ll /dev/mpp_service /dev/dri/renderD* 2>/dev/null || echo "（无）"
        echo "-- librknnrt --"
        ls -1 /opt/easyaiot/rknn-lib/librknnrt.so /usr/lib/librknnrt.so 2>/dev/null || echo "（容器内未找到）"
        echo "-- RUNTIME --"
        test -x /opt/easyaiot/RUNTIME/build/RUNTIME && /opt/easyaiot/RUNTIME/build/RUNTIME --version 2>&1 | head -n2
        echo "-- 动态库解析 --"
        ldd /opt/easyaiot/RUNTIME/build/RUNTIME 2>/dev/null | grep -E "rknnrt|not found" || true
    ' 2>&1)" || { error "  docker exec 失败"; return 1; }
    echo "$out" | sed 's/^/  /'
    echo "$out" | grep -q 'librknnrt.so' \
        || { error "  容器内看不到 librknnrt.so：dlopen 失败会回落 ONNX Runtime"; rc=1; }
    echo "$out" | grep -q 'not found' \
        && { error "  容器内有未解析的依赖库（见上方 not found）"; rc=1; }
    echo "$out" | grep -Eq '/dev/(rga|rknpu|mpp_service|dri/renderD)' \
        || { error "  容器内没有 NPU/MPP 设备节点：重建容器时确认 override 生效"; rc=1; }
    return "$rc"
}

verify_services() {
    step "服务健康"
    local rc=0
    if curl -sf http://127.0.0.1:6000/actuator/health >/dev/null 2>&1; then
        success "  VIDEO /actuator/health OK"
    else
        error "  VIDEO :6000 健康检查不可达"; rc=1
    fi
    if curl -skf "https://127.0.0.1:${WEB_PORT}/health" >/dev/null 2>&1 \
       || curl -skf "https://127.0.0.1:${WEB_PORT}/" >/dev/null 2>&1; then
        success "  WEB OK（https://<本机IP>:${WEB_PORT}）"
    else
        error "  WEB :${WEB_PORT} 不可达"; rc=1
    fi
    local inis
    inis="$(ls "$SCRIPT_DIR"/RUNTIME/config/task_*.ini 2>/dev/null || true)"
    if [ -n "$inis" ]; then
        info "  当前任务的推理后端配置（由 VIDEO 控制面写入）："
        grep -H -E '^\s*(infer_backend|npu_core_mask|model_path)' $inis 2>/dev/null | sed 's/^/    /' || true
    else
        info "  暂无 task_*.ini：下发一个 RKNN 模型的算法任务后再跑 verify"
    fi
    npu_load_while_running
    return "$rc"
}

# 有算法任务在跑时，NPU 负载非 0 才是「真的走了 NPU」的硬证据
npu_load_while_running() {
    local load=""
    local f
    for f in /sys/kernel/debug/rknpu/load /sys/class/devfreq/fdab0000.npu/load; do
        [ -r "$f" ] && load="$(tr -s ' \t' ' ' < "$f")" && break
    done
    if [ -z "$load" ]; then
        info "  读不到 NPU 负载（需 root）：sudo cat /sys/kernel/debug/rknpu/load"
        return 0
    fi
    info "  NPU 负载: $load"
    # 只要有一个核不是 0%，就是「推理真跑在 NPU 上」的硬证据。
    # 不依赖 "Core0:  0%" 的空格排版（不同内核/rknpu 版本排版不一致）。
    if printf '%s\n' "$load" | grep -Eq '[1-9][0-9]*[[:space:]]*%'; then
        success "  NPU 有负载，推理确实在 NPU 上跑"
    else
        warn "  三核均为 0%：可能没有任务在推理，或已回落 ONNX Runtime CPU"
        warn "  确认方法：$0 verify 看后端日志，或在 WEB 上跑一路视频算法任务后重试"
    fi
}

verify_all() {
    local rc=0
    verify_host_side      || rc=1
    verify_container_side || rc=1
    verify_services       || rc=1
    echo ""
    if [ "$rc" -eq 0 ]; then
        success "RK3588 NPU 链路校验通过"
        info "登录页 https://<盒子IP>:${WEB_PORT}（默认 admin/admin123），在模型导出里选 RKNN 即可下发到本机"
    else
        error "校验未通过，按上面的 [ERROR] 逐项处理"
    fi
    return "$rc"
}

# ============================================================
# 离线包 / 模型
# ============================================================
runtime_bundle() {
    local pkg="${1:-}"
    local dest="${EASYAIOT_RUNTIME_INSTALL_DIR:-/opt/easyaiot/RUNTIME}"
    if [ -z "$pkg" ] || [ ! -f "$pkg" ]; then
        error "用法：$0 runtime-bundle <easyaiot-runtime-<os>-arm64.tar.gz>"
        error "  离线包在 x86 控制面产出：RUNTIME_OS_FAMILY=ubuntu24 bash RUNTIME/scripts/export_runtime_os_container.sh ubuntu24"
        return 1
    fi
    step "更新 RUNTIME 离线包 → $dest"
    # 安装器自带 smoke 校验（OS/ABI 不匹配会拒绝），成功打印 RUNTIME_OK，失败打印 INSTALL_FAIL
    bash RUNTIME/install_runtime_cpp.sh "$dest" "$pkg"
    local bin="$dest/bin/RUNTIME" ver=""
    if [ -f "$dest/VERSION" ]; then
        ver="$(grep -m1 '^version=' "$dest/VERSION" 2>/dev/null | cut -d= -f2-)"
        printf '  版本      : %s\n' "${ver:-?}"
    fi
    if binary_links_rknn "$bin"; then
        success "  $bin 已带 RKNN 后端"
    elif [ -x "$bin" ]; then
        warn "  $bin 未链接 librknnrt：该包是在缺 SDK 的环境下编的，盒子只能跑 CPU"
        warn "  重出包（x86 上把 aarch64 SDK 放 RUNTIME/.rknn-sdk）：RUNTIME_WITH_RKNN=on bash RUNTIME/scripts/export_runtime_os_container.sh ubuntu24"
    fi
    # 注意：standalone 形态下 VIDEO 挂的是 RUNTIME/deploy.env 里的 RUNTIME_HOST_DIR/build/RUNTIME，
    # 与离线包的 $dest/bin/RUNTIME 不是同一份；两者只会用其中一份。
    local host_dir=""
    if [ -f "$SCRIPT_DIR/RUNTIME/deploy.env" ]; then
        host_dir="$(grep -m1 '^RUNTIME_HOST_DIR=' "$SCRIPT_DIR/RUNTIME/deploy.env" | cut -d= -f2-)"
    fi
    if [ -n "$host_dir" ] && [ "$host_dir" != "$dest" ]; then
        warn "  VIDEO 容器当前挂载的是 $host_dir/build/RUNTIME（来自 RUNTIME/deploy.env）"
        warn "  要让刚装的离线包生效：$0 build 重编仓库产物，或按 atomic 形态部署 VIDEO"
    else
        wire_npu_mount || true
    fi
    success "离线包已更新：$dest"
}

model_export_hint() {
    step ".rknn 模型转换只能在控制面做"
    cat <<'EOF'
  rknn-toolkit2 只发布 x86_64 Linux 版本，盒子上做不了 .pt/.onnx → .rknn 转换。
  正确做法（在 x86 控制面/中心机上，二选一）：

    1) 走平台界面：WEB 模型导出页选「RKNN (RK3588 NPU)」
       后端 POST /model/export/<model_id>/export/rknn，任务异步执行，产物入 exports 桶
       并回填 model 表的 rknn_model_path，算法任务下发时自动选到 .rknn。

    2) 手动转换（调试单个模型）：
       pip install rknn-toolkit2          # 必须 x86_64 Linux + Python 3.10/3.11
       python3 RUNTIME/scripts/ensure_rknn_model.py \
         --input yolov8n.onnx --output /models/<id>/rknn/model.rknn \
         --target-platform rk3588 --imgsz 640
       # INT8 量化需额外 --dataset calib/dataset.txt（每行一张校准图）

  盒子侧只负责加载：产物三件套 model.rknn / model.names / model.rknn.json 放到任务模型目录，
  RUNTIME 探到 librknnrt.so + NPU 设备节点就会用 RKNN 引擎，否则自动回落 ONNX Runtime。
EOF
}

usage() {
    # 头部注释块为 2-32 行（32 行为收尾的 # ===），改注释时同步这里
    sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^#\{1,2\} \{0,1\}//'
}

run_container_cmd() {
    local cmd="$1"
    docker_ready || { error "Docker 不可用"; return 1; }
    # 安装器自己会 cd 到 VIDEO 并从 ../ 推断 EASYAIOT_ROOT，无需在子目录里绕路调用
    bash "$(video_installer)" "$cmd"
}

main() {
    local cmd="${1:-doctor}"
    case "$cmd" in
        doctor)   doctor_all ;;
        npu)      npu_snapshot ;;
        build)    export_rknn_env && build_images && wire_npu_mount && verify_host_side ;;
        install)  export_rknn_env && install_all && verify_all ;;
        update)   export_rknn_env && update_all ;;
        verify)   verify_all ;;
        runtime-bundle) export_rknn_env && runtime_bundle "${2:-}" ;;
        model-export)   model_export_hint ;;
        start|stop|restart|status|logs)
            run_container_cmd "$cmd"
            [ "$cmd" = "logs" ] || bash WEB/install_linux.sh "$cmd" || true
            ;;
        help|-h|--help) usage ;;
        *)
            error "未知命令: $cmd"
            usage
            exit 1
            ;;
    esac
}

main "$@"
