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
#   ./install_rk3588.sh sdk-setup            # 扫描本机现成 rknpu2 SDK 并落到 RUNTIME/.rknn-sdk
#   ./install_rk3588.sh mpp-setup            # 暂存 rk_mpi.h + 修补 librockchip_mpp 到 RUNTIME/.mpp-sdk
#   ./install_rk3588.sh runtime-bundle X.tgz # 用离线包更新 /opt/easyaiot/RUNTIME（OTA 场景）
#   ./install_rk3588.sh model-export         # 打印 .rknn 转换的正确姿势（只能在 x86 控制面做）
#   ./install_rk3588.sh status|logs|restart|start|stop
#
# verify 只回答「整条链路通不通」。要按判据逐条取证（shell / Python 控制面 / C++ 三处 NPU
# 探测是否同源、card 节点是否真进了 devices 白名单、librockchip_mpp 与硬编硬解链路）用：
#   bash RUNTIME/scripts/verify_rk_media.sh                            # 宿主全量（含实跑探针）
#   bash RUNTIME/scripts/verify_rk_media.sh --no-probe                 # 只做静态判据校验
#   bash RUNTIME/scripts/verify_rk_media.sh --container=video-service  # 进容器复跑并比对
#
# 环境变量:
#   RKNN_SDK_ROOT          rknpu2 SDK 目录（其下有 include/rknn_api.h）。留空自动探测，
#                          约定落点 RUNTIME/.rknn-sdk —— docker 编译时它随 /src 一起进容器
#   RUNTIME_WITH_RKNN      auto(默认) / on / off
#                          auto：有 SDK 就编 NPU 后端，没有则编纯 ONNX Runtime 并大字告警
#                          on  ：缺 SDK 直接失败（CI/出包机用，避免产出无 NPU 后端的二进制）
#                          多数盒子镜像只装了 librknnrt.so（运行期）而没有 rknn_api.h（编译期），
#                          先跑 ./install_rk3588.sh sdk-setup 补 SDK，再 build 即可上 NPU
#   MPP_SDK_ROOT           Rockchip MPP SDK 目录（include/rockchip/rk_mpi.h + lib/）。留空自动
#                          探测，约定落点 RUNTIME/.mpp-sdk —— 必须是 mpp-setup 产出的那份
#   RUNTIME_WITH_MPP       auto(默认) / on / off —— rkvdec 硬解 + VEPU 硬编后端
#                          auto：有 SDK 就编，没有则编纯软解软编（1080p25 约吃 1.5 个 A76）
#                          盒子镜像普遍只有 librockchip_mpp.so.0 而没有 rk_mpi.h，且那份库要求
#                          GLIBC_2.29（video-service 容器是 glibc 2.28）—— 先跑 mpp-setup 把
#                          头文件收进仓库、把版本节点重定向到 GLIBC_2.17，再 build
#   MPP_HEADER_DIR         手工指定 rk_mpi.h 所在目录（跳过全盘扫描）
#   MPP_PATCH_TOOL         预编译好的 RUNTIME/tools/mpp_glibc_patch（盒子无 gcc 时用）
#   MPP_FORCE=1            忽略已就绪的 RUNTIME/.mpp-sdk，重新暂存并重打补丁
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
source "$SCRIPT_DIR/RUNTIME/scripts/mpp_sdk.sh"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/.scripts/docker/module_update_helpers.sh"

WEB_PORT="${WEB_PORT:-8888}"
RUNTIME_WITH_RKNN="${RUNTIME_WITH_RKNN:-auto}"
RUNTIME_WITH_MPP="${RUNTIME_WITH_MPP:-auto}"
export WEB_PORT RUNTIME_WITH_RKNN RUNTIME_WITH_MPP

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
        error "  未找到 /dev/rga、/dev/rknpu、/dev/mpp_service、/dev/dri/renderD*、/dev/dri/card* 任一节点"
        error "  请确认内核已加载 rknpu / rga / rkmpp 驱动（厂商固件或 rkdeveloptool 刷的镜像）"
        rc=1
    fi
    if lib="$(rknn_runtime_lib)"; then
        printf '  运行时库  : %s\n' "$lib"
        local ver
        ver="$(librknnrt_version "$lib")"
        printf '  库版本    : %s\n' "${ver:-未知（librknnrt.so 里没读到版本串）}"
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
    error "  盒子镜像通常只带运行期 librknnrt.so，编译期头文件要自己补："
    error "    1) ./install_rk3588.sh sdk-setup    # 先扫本机有没有现成 SDK"
    error "    2) 没有就下一份（盒子能上网时）："
    error "       git clone --depth 1 https://github.com/rockchip-linux/rknpu2 /tmp/rknpu2"
    error "       RKNN_SDK_DIR=/tmp/rknpu2/runtime/RK3588/Linux/librknn_api \\"
    error "         ./install_rk3588.sh sdk-setup"
    error "    3) 已有 SDK 只想临时指路：export RKNN_SDK_ROOT=/path/to/librknn_api"
    error "  离线盒子：在 x86 上 clone 后 scp runtime/RK3588/Linux/librknn_api 过来再做 2)"
    if [ "$(printf '%s' "$RUNTIME_WITH_RKNN" | tr '[:upper:]' '[:lower:]')" = "auto" ]; then
        warn "  RUNTIME_WITH_RKNN=auto：本次继续编纯 ONNX Runtime（CPU），$0 verify 会明确指出没上 NPU"
        warn "  想让缺 SDK 变成硬失败（CI/出包机）：RUNTIME_WITH_RKNN=on $0 build"
        return 0
    fi
    return 1
}

# 在有 rknn_api.h 的路径里挑出 SDK 根：三种官方布局都要还原成 <root>
#   <root>/include/rknn_api.h  <root>/rknpu2/include/rknn_api.h  <root>/librknn_api/include/rknn_api.h
sdk_root_of_header() {
    sed -e 's#/librknn_api/include/rknn_api\.h$##' \
        -e 's#/rknpu2/include/rknn_api\.h$##' \
        -e 's#/include/rknn_api\.h$##'
}

# 扫本机现成的 rknn_api.h：厂商 SDK、rknpu2 解压包、conda 环境里都可能出现。
# 限定深度，避免在大目录上跑飞。
sdk_scan_headers() {
    local root
    for root in /opt /usr/local /usr/include /srv /home /root /userdata /data \
                /oem /vendor /workspace /work; do
        [ -d "$root" ] || continue
        # 深度放到 8：真实布局常在 ~/rknn-toolkit2/rknpu2/runtime/RK3588/Linux/librknn_api/include
        find "$root" -maxdepth 8 \
            \( -name .git -o -name node_modules -o -name site-packages -o -name .build-cache \) -prune -o \
            \( -type f -o -type l \) -name rknn_api.h -print 2>/dev/null
    done
}

# 把 SDK 整理成 RUNTIME/.rknn-sdk/{include,lib}：docker 编译模式唯一稳的做法
sdk_setup() {
    step "准备编译期 RKNN SDK → RUNTIME/.rknn-sdk"
    local target="$SCRIPT_DIR/RUNTIME/.rknn-sdk" src="" hits
    if [ -n "${RKNN_SDK_DIR:-}" ]; then
        [ -d "$RKNN_SDK_DIR" ] || { error "  RKNN_SDK_DIR 不存在: $RKNN_SDK_DIR"; return 1; }
        src="$RKNN_SDK_DIR"
        info "  使用显式指定的 SDK: $src"
    else
        hits="$(sdk_scan_headers | sdk_root_of_header | sort -u || true)"
        if [ -z "$hits" ]; then
            error "  本机没搜到 rknn_api.h（厂商镜像通常不带编译期头文件）"
            error "  下一份官方 SDK 再跑本命令："
            error "    git clone --depth 1 https://github.com/rockchip-linux/rknpu2 /tmp/rknpu2"
            error "    RKNN_SDK_DIR=/tmp/rknpu2/runtime/RK3588/Linux/librknn_api $0 sdk-setup"
            error "  离线盒子：x86 上 clone 后 scp 该 librknn_api 目录过来（约 8 MB）"
            return 1
        fi
        printf '  搜到候选 SDK 根目录：\n'
        printf '%s\n' "$hits" | sed 's/^/    /'
        src="$(printf '%s\n' "$hits" | head -n1)"
        info "  取第一个：$src（换别的用 RKNN_SDK_DIR=/path $0 sdk-setup）"
    fi
    # 头文件：三种布局都试
    local inc="" cand
    if [ "$src" = "$target" ] || [ "$src" = "$target/include" ]; then
        success "  RUNTIME/.rknn-sdk 已就绪，无需重复拷贝"
        export RKNN_SDK_ROOT="$target"
        return 0
    fi
    for cand in "$src/include" "$src/rknpu2/include" "$src/librknn_api/include"; do
        [ -f "$cand/rknn_api.h" ] && { inc="$cand"; break; }
    done
    if [ -z "$inc" ]; then
        # 允许直接指向头文件所在目录
        [ -f "$src/rknn_api.h" ] && inc="$src"
    fi
    [ -n "$inc" ] || { error "  $src 下找不到 rknn_api.h（应指向含 include/ 的 SDK 根）"; return 1; }
    mkdir -p "$target/include" "$target/lib" || { error "  无法创建 $target"; return 1; }
    cp -aL "$inc"/. "$target/include"/ 2>/dev/null || { error "  拷贝头文件失败"; return 1; }
    success "  头文件 → $target/include（$(find "$target/include" -name '*.h' | wc -l) 个 .h）"
    # 链接期用的 .so：优先 SDK 自带的 aarch64 版本，其次盒子驱动那份
    local lib="" l
    for l in "$src/librknn_api/aarch64/librknnrt.so" "$src/aarch64/librknnrt.so" \
             "$src/lib/librknnrt.so" "$src/rknpu2/lib/librknnrt.so"; do
        [ -f "$l" ] && { lib="$l"; break; }
    done
    if [ -z "$lib" ]; then
        lib="$(rknn_system_runtime_lib 2>/dev/null || true)"
        [ -n "$lib" ] && info "  SDK 内无 librknnrt.so，改用盒子驱动那份做链接桩: $lib"
    fi
    if [ -n "$lib" ]; then
        cp -aL "$lib" "$target/lib/librknnrt.so" 2>/dev/null \
            && success "  链接库 → $target/lib/librknnrt.so" \
            || warn "  拷贝 librknnrt.so 失败，cmake 需自行在系统路径找到它"
    else
        error "  既没找到 SDK 的 librknnrt.so，盒子上也还没有运行期驱动库"
        error "  编译期会过不了 find_library(rknnrt)；请确认 rknpu2 驱动已装（$0 npu 查看）"
        return 1
    fi
    # 版本不一致只影响运行期：cmake 链接用 SDK 的，ld.so 实际加载哪份看 LD_LIBRARY_PATH
    local sdk_ver="" drv_ver="" sys_lib
    sdk_ver="$(librknnrt_version "$target/lib/librknnrt.so")"
    if sys_lib="$(rknn_system_runtime_lib 2>/dev/null)"; then
        drv_ver="$(librknnrt_version "$sys_lib")"
    fi
    [ -n "$sdk_ver" ] && printf '  SDK 库版本: %s\n' "$sdk_ver"
    if [ -n "$sys_lib" ]; then
        printf '  盒子库版本: %s (%s)\n' "${drv_ver:-unknown}" "$sys_lib"
    else
        warn "  盒子系统路径（/usr/lib 等）没有 librknnrt.so：编译能过，运行期需把 LD_LIBRARY_PATH 指向上面这份"
    fi
    if [ -n "$sdk_ver" ] && [ -n "$drv_ver" ] && [ "$sdk_ver" != "$drv_ver" ]; then
        warn "  两者版本不同：运行期以盒子驱动（内核 rknpu）那份为准"
        warn "  若跑起来报 RKNN_ERR_DEVICE_UNMATCH，换成与驱动同版本的 rknpu2 SDK 重编即可"
    fi
    export RKNN_SDK_ROOT="$target"
    info "  完成。接着跑：$0 build"
}

# 扫本机现成的 rk_mpi.h（厂商 rootfs 的开发包、SDK 解压包都可能是来源）
scan_mpp_headers() {
    local root
    for root in /usr/include /usr/local/include /opt /srv /home /root \
                /userdata /data /oem /vendor /workspace; do
        [ -d "$root" ] || continue
        find "$root" -maxdepth 6 \
            \( -name .git -o -name node_modules -o -name site-packages -o -name .build-cache \
               -o -name .mpp-sdk -o -name .rknn-sdk \) -prune -o \
            \( -type f -o -type l \) -name rk_mpi.h -print 2>/dev/null
    done
}

# 头文件路径 → 存放 rk_mpi.h 的目录。两种布局都只去掉尾巴即可：
#   <root>/include/rockchip/rk_mpi.h -> <root>/include/rockchip
#   <sdk>/  rockchip/rk_mpi.h        -> <sdk>/rockchip
# 结果直接喂给 cp -aL "$dir/."，与 RUNTIME/CMakeLists.txt 的两段 find_path 对得上。
mpp_header_dir() {
    sed -e 's#/rk_mpi\.h$##'
}

# 容器里能不能解析到某个 soname（video-service 没起来时返回 2 = 未知）。
# 库名走位置参数传给 sh，省掉「外层双引号里再套内层引号」那套转义——之前那样写会把
# glob 也一起引号化，ls 反而永远失败。
container_has_lib() {
    local name="$1" container="${2:-$VIDEO_CONTAINER}"
    docker_ready || return 2
    docker exec "$container" sh -c '
        name="$1"
        for d in /usr/lib64 /usr/lib /lib64 /lib /usr/lib/aarch64-linux-gnu \
                 /usr/local/lib64 /usr/local/lib; do
            for f in "$d/$name" "$d/${name}."*; do
                [ -e "$f" ] && exit 0
            done
        done
        exit 1' sh "$name" >/dev/null 2>&1 && return 0
    return 1
}

# 把宿主 MPP 开发包整理成 RUNTIME/.mpp-sdk/{include/rockchip,lib}，
# 并把 librockchip_mpp 的 GLIBC_2.29 版本依赖重定向到容器（glibc 2.28）有的 GLIBC_2.17。
# 这是 docker 同源编译模式下唯一能让 rkmpp 后端编出来、又能跑起来的做法。
mpp_setup() {
    step "准备编译期 MPP SDK → RUNTIME/.mpp-sdk（含 GLIBC 版本节点重定向）"
    local target="$SCRIPT_DIR/RUNTIME/.mpp-sdk"
    local from="${MPP_GLIBIC_FROM:-GLIBC_2.29}" to="${MPP_GLIBIC_TO:-GLIBC_2.17}"

    # 幂等：已就绪就只做体检，重复跑不会把补丁后的库再套一层
    if [ "${MPP_FORCE:-0}" != "1" ] && [ -f "$target/include/rockchip/rk_mpi.h" ] \
       && mpp_staged_lib >/dev/null 2>&1; then
        success "  RUNTIME/.mpp-sdk 已就绪（重做用 MPP_FORCE=1 $0 mpp-setup）"
        _mpp_report "$target"
        export MPP_SDK_ROOT="$target"
        return 0
    fi

    # --- 1) 头文件 ----------------------------------------------------------
    local hdr="" cand
    if [ -n "${MPP_HEADER_DIR:-}" ]; then
        [ -d "$MPP_HEADER_DIR" ] || { error "  MPP_HEADER_DIR 不存在: $MPP_HEADER_DIR"; return 1; }
        hdr="$MPP_HEADER_DIR"
        info "  使用显式指定的头文件目录: $hdr"
    else
        for cand in /usr/include/rockchip /usr/local/include/rockchip; do
            [ -f "$cand/rk_mpi.h" ] && { hdr="$cand"; break; }
        done
    fi
    if [ -z "$hdr" ]; then
        local hits
        hits="$(scan_mpp_headers | mpp_header_dir | sort -u || true)"
        if [ -z "$hits" ]; then
            error "  本机没搜到 rk_mpi.h（厂商精简镜像常只带运行库，不带开发包）"
            error "  装开发包：sudo apt-get install librkmpp-dev   # 或 librockchip-mpp-dev"
            error "  或从 MPP 源码树取：rockchip-linux/mpp 的 inc/ 目录"
            error "  已有别处副本时：MPP_HEADER_DIR=/path/to/include/rockchip $0 mpp-setup"
            return 1
        fi
        printf '  搜到候选头文件目录：\n'
        printf '%s\n' "$hits" | sed 's/^/    /'
        hdr="$(printf '%s\n' "$hits" | head -n1)"
        info "  取第一个：$hdr（换别的用 MPP_HEADER_DIR=/path $0 mpp-setup）"
    fi
    [ -f "$hdr/rk_mpi.h" ] || { error "  $hdr 下没有 rk_mpi.h"; return 1; }

    mkdir -p "$target/include/rockchip" "$target/lib" \
        || { error "  无法创建 $target（docker 编译要靠它进容器，必须落在仓库内）"; return 1; }
    if [ "${hdr%/}" = "${target}/include/rockchip" ]; then
        # MPP_FORCE=1 重跑时，来源可能就是上一次暂存的结果（例如仓库被放在 /root 下
        # 且手工指定过 MPP_HEADER_DIR）。源和目标是同一个目录，cp 只会自撞。
        info "  头文件已在暂存目录里，跳过拷贝"
    else
        cp -aL "$hdr"/. "$target/include/rockchip"/ 2>/dev/null \
            || { error "  拷贝头文件失败"; return 1; }
    fi
    success "  头文件 → $target/include/rockchip（$(find "$target/include/rockchip" -name '*.h' | wc -l) 个 .h）"

    # --- 2) 运行库（+ glibc 版本节点重定向）--------------------------------
    local sys=""
    if ! sys="$(mpp_system_runtime_lib 2>/dev/null)"; then
        error "  宿主系统路径里没有 librockchip_mpp.so*：MPP 用户态驱动未安装"
        error "  装厂商包（librockchip-mpp / librkmpp）后重跑本命令"
        rm -rf "$target"
        return 1
    fi
    info "  宿主运行库: $sys"
    local soname tmp_in="$target/.mpp-in.tmp" patched="$target/.mpp-out.tmp"
    soname="$(mpp_soname "$sys" || true)"
    [ -n "$soname" ] || soname="$(basename "$sys")"
    cp -aL "$sys" "$tmp_in" 2>/dev/null \
        || { error "  拷贝 $sys 失败"; rm -f "$tmp_in"; return 1; }

    local rc=0
    if ! mpp_patch_tool >/dev/null 2>&1; then
        warn "  编译不出 mpp_glibc_patch（宿主无 gcc/cc，且未预置 MPP_PATCH_TOOL）"
        warn "  无法判断/修复 ${from} 依赖：原样暂存。容器里链接若报"
        warn "    version \`${from}' not found —— 装个 gcc 再 MPP_FORCE=1 $0 mpp-setup"
    elif mpp_needs_glibc_patch "$tmp_in" "$from"; then
        info "  检测到 ${from} 依赖（容器 glibc 2.28 没有），重定向到 ${to}"
        if ! mpp_patch_glibc "$tmp_in" "$patched" "$from" "$to"; then
            error "  版本节点重定向失败：见 RUNTIME/tools/mpp_glibc_patch.c 的说明"
            rm -f "$tmp_in" "$patched"
            return 1
        fi
        mv -f "$patched" "$tmp_in" || { error "  落盘失败"; rm -f "$tmp_in"; return 1; }
        success "  已重定向：$from -> $to"
        # 补丁后必须仍然只要求容器有的版本；再查一遍，挡掉 2.30/2.31 这类漏网的
        if mpp_needs_glibc_patch "$tmp_in" "$from"; then
            error "  重定向后仍残留 ${from}（elf_hash 自检未过或存在共享偏移）—— 不敢用这份库"
            rm -f "$tmp_in"
            return 1
        fi
    else
        info "  未发现 ${from} 依赖，原样使用"
    fi
    # 目录里绝不留下第二个 librockchip_mpp：CMake 的 find_file 按 .so.0 → .so.1 顺序取，
    # 挑到未打补丁的那份就等于白干。
    rm -f "$target"/lib/librockchip_mpp.so* 2>/dev/null || true
    mv -f "$tmp_in" "$target/lib/$soname" \
        || { error "  写入 $target/lib/$soname 失败"; rm -f "$tmp_in"; return 1; }
    ln -sf "$soname" "$target/lib/librockchip_mpp.so" 2>/dev/null || true
    success "  运行库 → $target/lib/$soname（DT_NEEDED 用的就是这个名字）"

    # --- 3) 传递依赖：容器缺一个，RUNTIME 就整个起不来 ----------------------
    _mpp_stage_deps "$target/lib/$soname" "$target/lib" || rc=$?

    _mpp_report "$target"
    export MPP_SDK_ROOT="$target"
    info "  完成。接着跑：$0 build"
    return "$rc"
}

# glibc / libstdc++ 这类基础库绝不能从宿主搬进容器（会顶掉容器自己的，直接 SIGSEGV，
# 与 ensure_runtime_cpp.sh 里对 CUDA 的处理同理）。它们缺版本节点该走补丁，不该走挂载。
MPP_BASE_LIBS=" libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libutil.so.1
               libresolv.so.1 libnss_files.so.2 libstdc++.so.6 libgcc_s.so.1
               ld-linux-aarch64.so.1 ld-linux-x86-64.so.2 "

# 名字是否命中基线库。这里不能写成 `case "$MPP_BASE_LIBS" in *" $need "*` ——
# 那个串是多行的，libutil.so.1 后面跟的是换行而不是空格，永远匹配不上，
# 结果就是宿主 glibc 被 cp 进 .mpp-sdk/lib 顶掉容器自己那份（直接 SIGSEGV）。
_mpp_is_base_lib() {
    local name="$1" base
    for base in $MPP_BASE_LIBS; do
        [ "$base" = "$name" ] && return 0
    done
    return 1
}

# 把 soname 的 DT_NEEDED 里「容器没有、宿主有」的那些补进 $2 目录，同样过一遍 glibc 补丁。
# 返回非 0 表示有依赖既不在容器里也搬不过来 —— 调用方告警即可，暂存目录仍然可用。
_mpp_stage_deps() {
    local lib="$1" libdir="$2" need missing=0 staged=0
    local from="${MPP_GLIBIC_FROM:-GLIBC_2.29}" to="${MPP_GLIBIC_TO:-GLIBC_2.17}"
    local needed
    needed="$(mpp_lib_needed "$lib" 2>/dev/null || true)"
    [ -n "$needed" ] || return 0   # 没有 readelf/objdump：跳过，交给运行期报错
    for need in $needed; do
        if _mpp_is_base_lib "$need"; then continue; fi
        local have=0
        container_has_lib "$need" || have=$?
        if [ "$have" = "0" ]; then
            continue
        elif [ "$have" = "2" ]; then
            info "  容器未在运行，跳过 $need 的存在性检查"
            continue
        fi
        local src
        src="$(mpp_find_soname "$need")" || {
            error "  $lib 需要 $need：容器里没有，宿主上也找不到 —— 容器内加载 RUNTIME 会失败"
            error "  应急：$0 build 前 export RUNTIME_WITH_MPP=off，先把编解码留在 CPU"
            missing=$((missing + 1))
            continue
        }
        cp -aL "$src" "$libdir/$need" 2>/dev/null || {
            warn "  拷贝 $src 失败，跳过 $need"
            missing=$((missing + 1))
            continue
        }
        if mpp_needs_glibc_patch "$libdir/$need" "$from"; then
            if mpp_patch_glibc "$libdir/$need" "$libdir/.$need.tmp" "$from" "$to" >/dev/null 2>&1; then
                mv -f "$libdir/.$need.tmp" "$libdir/$need"
                info "  依赖 $need 也做了 $from -> $to 重定向"
            else
                rm -f "$libdir/.$need.tmp"
                warn "  依赖 $need 带 ${from} 且补丁失败：容器内可能仍加载不了"
            fi
        fi
        success "  依赖 $need → $libdir/$need（来自 $src）"
        staged=$((staged + 1))
    done
    [ "$staged" -gt 0 ] && info "  共补入 $staged 个传递依赖，已随 RUNTIME/.mpp-sdk/lib 一起挂载"
    return "$missing"
}

# 在宿主常见前缀里找一个 soname 实体（厂商库多为「只有版本号后缀、无 dev 软链」）
mpp_find_soname() {
    local name="$1" dir cand
    for dir in /usr/lib /usr/lib64 /usr/local/lib /usr/lib/aarch64-linux-gnu \
               /usr/local/lib/aarch64-linux-gnu /oem/usr/lib /vendor/usr/lib /usr/lib/rockchip; do
        for cand in "$dir/$name" "$dir/$name.0" "$dir/$name.1"; do
            [ -e "$cand" ] && { printf '%s\n' "$cand"; return 0; }
        done
    done
    return 1
}

# 暂存结果一览（体检与 mpp-setup 结尾共用）
_mpp_report() {
    local target="$1" lib ver="" soname=""
    lib="$(mpp_staged_lib 2>/dev/null || true)"
    if [ -z "$lib" ]; then
        warn "  暂存目录里没有 librockchip_mpp：$target/lib"
        return 0
    fi
    soname="$(basename "$lib")"
    ver="$(mpp_version "$lib")"
    printf '  暂存 SDK   : %s\n' "$target"
    printf '  编译期库   : lib/%s%s\n' "$soname" "${ver:+ (version $ver)}"
    if command -v readelf >/dev/null 2>&1 || command -v objdump >/dev/null 2>&1; then
        printf '  版本依赖   : %s\n' "$(mpp_lib_versions "$lib" 2>/dev/null \
            | awk -F'version=' '/version=/ {split($2, a, " "); printf "%s ", a[1]}' || true)"
    fi
    return 0
}

# MPP 体检：编译期头文件 + 运行库形态。与 RKNN 的区别在于这里多一道坎 ——
# 板上那份 librockchip_mpp 要求 GLIBC_2.29，video-service 容器（glibc 2.28）吃不下，
# 必须是 mpp-setup 重定向过版本节点、落在 RUNTIME/.mpp-sdk 里的那份。
check_mpp_sdk() {
    step "编译期 Rockchip MPP（rk_mpi.h + librockchip_mpp）"
    local sdk want
    want="$(printf '%s' "$RUNTIME_WITH_MPP" | tr '[:upper:]' '[:lower:]')"
    if sdk="$(mpp_sdk_probe)"; then
        success "  SDK 根目录: $sdk"
        _mpp_report "$sdk"
    elif mpp_header_found; then
        success "  系统 include 里已有 rk_mpi.h（EASYAIOT_RUNTIME_BUILD_MODE=host 可直接编）"
        warn "  docker 编译看不到宿主 /usr，上硬解仍需：$0 mpp-setup"
    elif [ "$want" = "off" ]; then
        warn "  RUNTIME_WITH_MPP=off：不编 rkmpp 后端，跳过检查"
        return 0
    else
        error "  未找到 MPP 开发包（rockchip/rk_mpi.h），RUNTIME 编不出 rkvdec/VEPU 后端"
        error "  盒子镜像通常只带运行期 librockchip_mpp.so.0，编译期头文件要自己补："
        error "    1) ./install_rk3588.sh mpp-setup   # 扫本机 /usr/include/rockchip 等"
        error "    2) 扫不到就装包：sudo apt-get install -y librkmpp-dev   # 或 librockchip-mpp-dev"
        error "    3) 已有别处副本：MPP_HEADER_DIR=/path/to/rockchip $0 mpp-setup"
        if [ "$want" = "auto" ]; then
            warn "  RUNTIME_WITH_MPP=auto：本次继续编纯软解/软编（1080p25 约吃 1.5 个 A76）"
            return 0
        fi
        return 1
    fi
    # 库的形态决定容器里链不链得动：只有暂存那份保证改过版本节点
    local lib
    if lib="$(mpp_staged_lib 2>/dev/null)"; then
        if mpp_patch_tool >/dev/null 2>&1 && mpp_needs_glibc_patch "$lib"; then
            error "  $lib 仍要求 ${MPP_GLIBIC_FROM:-GLIBC_2.29}：容器里链接/加载都会失败"
            error "  重打补丁：MPP_FORCE=1 $0 mpp-setup"
            return 1
        fi
    elif [ "$want" != "off" ]; then
        warn "  暂存目录里没有 librockchip_mpp：当前只有宿主系统那份（要求 GLIBC_2.29）"
        warn "  docker 编译会失败：$0 mpp-setup 后重跑 $0 build"
        [ "$want" = "on" ] && return 1
    fi
    return 0
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

# ldd 直接看二进制有没有链到 librockchip_mpp（CMakeLists 只在开启 MPP 时才链接）
binary_links_mpp() {
    command -v ldd >/dev/null 2>&1 \
        && ldd "$1" 2>/dev/null | grep -q librockchip_mpp
}

# 已产出的二进制是否真带 rkmpp 编解码后端：0=带 1=不带 2=还没编
# 只看 CMakeCache / ldd，不看运行期 —— 运行期能不能真用上硬解由 verify 里的 /health 说了算
runtime_has_mpp() {
    [ -x "$RUNTIME_BIN_PATH" ] || return 2
    local cache="$SCRIPT_DIR/RUNTIME/build/CMakeCache.txt"
    if [ -f "$cache" ] && grep -q '^RUNTIME_WITH_MPP:BOOL=ON' "$cache" 2>/dev/null; then
        return 0
    fi
    binary_links_mpp "$RUNTIME_BIN_PATH" && return 0
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
    local mrc=0
    runtime_has_mpp || mrc=$?
    case "$mrc" in
        0) success "  已编入 rkmpp 编解码后端（rkvdec 硬解 + VEPU 硬编）" ;;
        1) warn    "  未编入 rkmpp 后端：解帧/推流会吃 CPU（约 1.5 个 A76 @1080p25）"
           warn    "  补法：$0 mpp-setup && $0 build" ;;
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
        check_mpp_sdk || rc=1
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
    # 编解码运行库单独列一行：mpp-setup 打过补丁的那份就在仓库里，路径与宿主原件不同
    if lib="$(mpp_runtime_lib 2>/dev/null)"; then
        printf '  librockchip_mpp : %s\n' "$lib"
        command -v readelf >/dev/null 2>&1 \
            && printf '  SONAME          : %s\n' "$(mpp_soname "$lib")"
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
    # MPP（rkvdec/VEPU）：仓库内那份是唯一能被容器里的 ld 接受的形态
    local mpp=""
    mpp="$(mpp_sdk_probe || true)"
    if [ -n "$mpp" ]; then
        export MPP_SDK_ROOT="$mpp"
        info "使用 MPP SDK: $mpp"
    elif [ "$(printf '%s' "$RUNTIME_WITH_MPP" | tr '[:upper:]' '[:lower:]')" != "off" ]; then
        warn "未找到 MPP SDK（RUNTIME/.mpp-sdk）：本次编出的 RUNTIME 无 rkmpp 后端，编解码留在 CPU"
        warn "  上硬解/硬编：$0 mpp-setup 后重跑 $0 build"
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
    step "编译 RUNTIME（RUNTIME_WITH_RKNN=$RUNTIME_WITH_RKNN RUNTIME_WITH_MPP=$RUNTIME_WITH_MPP）"
    if ! bash RUNTIME/install_linux.sh build; then
        error "RUNTIME 编译失败"
        return 1
    fi
    local rc=0 want
    want="$(printf '%s' "$RUNTIME_WITH_RKNN" | tr '[:upper:]' '[:lower:]')"
    runtime_has_rknn || rc=$?
    if [ "$rc" = "2" ]; then
        error "  产物缺失：$RUNTIME_BIN_PATH"
        return 1
    fi
    if [ "$rc" = "1" ]; then
        if [ "$want" = "on" ]; then
            error "编译完成但二进制里没有 RKNN 后端，NPU 不会生效"
            error "  多半是 SDK 没被容器看到：见 RUNTIME/scripts/rknn_sdk.sh 的探测路径"
            return 1
        fi
        warn "编译完成，但二进制未链接 librknnrt —— 本次是纯 ONNX Runtime（CPU）版本"
        [ "$want" != "off" ] && warn "  上 NPU：$0 sdk-setup 补齐 rknpu2 SDK（rknn_api.h）后重跑 $0 build"
        warn "  现在也能用：视频算法任务照跑，只是不占 NPU 算力；随时用 $0 verify 复核"
    else
        success "RUNTIME 已带 RKNN 后端"
    fi
    # 编解码后端单独判一次：RKNN 只解决推理，硬解/硬编是另一条链路
    local mrc=0 mwant
    mwant="$(printf '%s' "$RUNTIME_WITH_MPP" | tr '[:upper:]' '[:lower:]')"
    runtime_has_mpp || mrc=$?
    if [ "$mrc" = "2" ]; then
        error "  产物缺失：$RUNTIME_BIN_PATH"
        return 1
    elif [ "$mrc" = "1" ]; then
        if [ "$mwant" = "on" ]; then
            error "编译完成但二进制里没有 rkmpp 后端，编解码仍会吃 CPU"
            error "  多半是 RUNTIME/.mpp-sdk 没被容器看到：$0 mpp-setup 后重跑 $0 build"
            return 1
        fi
        warn "编译完成，但二进制未链接 librockchip_mpp —— 解帧/推流走 CPU"
        warn "  上硬解/硬编：$0 mpp-setup && $0 build"
    else
        success "RUNTIME 已带 rkmpp 编解码后端"
    fi
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
    step "接线 NPU/MPP 设备与运行库到 VIDEO 容器"
    # 该脚本会探测 librknnrt.so、RUNTIME/.mpp-sdk/lib 与设备节点（含 /dev/dma_heap/*），
    # 生成 .docker-compose.runtime.override.yaml
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
# 真正的端到端探针：dlopen librknnrt.so 并调一次 rknn_init。
# 「设备节点在 / 库文件在 / ldd 全解析」都不足以证明 NPU 可用 —— 本次线上故障就是容器里
# renderD128/129 齐全、ldd 干净，但 librknnrt 打不开 RKNPU 的 card 主节点，
# 体检却报「NPU 链路校验通过」。
# 探针脚本宿主与容器共用同一份，保证判据一致。
# 结论取值：PASS / DEV_FAIL（设备打不开） / MODEL_FAIL（设备通了、模型加载失败）
#          / NO_LIB / NO_MODEL / NO_SYM / NO_PY，后面带 rknn_init 返回值
_rknn_probe_script() {
    cat <<'PY'
import ctypes, sys
lib, model = sys.argv[1], sys.argv[2]
try:
    rt = ctypes.CDLL(lib)
except OSError as e:
    print("NO_LIB %s" % e)
    sys.exit(0)
try:
    buf = open(model, "rb").read()
except OSError as e:
    print("NO_MODEL %s" % e)
    sys.exit(0)
ctx = ctypes.c_void_p()
try:
    r = rt.rknn_init(ctypes.byref(ctx), ctypes.c_char_p(buf), len(buf), 0, 0)
except Exception as e:
    print("NO_SYM %s" % e)
    sys.exit(0)
if r == 0 and ctx.value:
    try:
        rt.rknn_destroy(ctx)
    except Exception:
        pass
print("RET %d" % r)
PY
}

# 把 python 的原始输出归类
_rknn_probe_verdict() {
    local out="$1" ret
    ret="$(printf '%s\n' "$out" | grep -oE 'RET +-?[0-9]+' | head -n1 | awk '{print $2}')"
    if printf '%s' "$out" | grep -q 'NO_LIB'; then printf 'NO_LIB -\n'
    elif printf '%s' "$out" | grep -q 'NO_MODEL'; then printf 'NO_MODEL -\n'
    elif printf '%s' "$out" | grep -q 'NO_SYM'; then printf 'NO_SYM -\n'
    elif printf '%s' "$out" | grep -q 'failed to open rknn device'; then printf 'DEV_FAIL %s\n' "${ret:--}"
    elif [ "$ret" = "0" ]; then printf 'PASS 0\n'
    elif [ -z "$ret" ]; then printf 'NO_PY -\n'
    else printf 'MODEL_FAIL %s\n' "$ret"
    fi
    return 0
}

# 宿主侧：rknn_init_probe <librknnrt.so> <模型>
rknn_init_probe() {
    local lib="$1" model="$2" py="${3:-python3}" tmp out
    if ! command -v "$py" >/dev/null 2>&1; then printf 'NO_PY -\n'; return 0; fi
    tmp="$(mktemp /tmp/rknn_probe_XXXXXX.py)" || { printf 'NO_PY -\n'; return 0; }
    _rknn_probe_script > "$tmp"
    out="$("$py" "$tmp" "$lib" "$model" 2>&1)" || true
    rm -f "$tmp"
    _rknn_probe_verdict "$out"
}

# 容器侧：rknn_init_probe_container <容器内 librknnrt.so> <容器内模型>
rknn_init_probe_container() {
    local lib="$1" model="$2" container="${3:-$VIDEO_CONTAINER}" tmp out
    tmp="$(mktemp /tmp/rknn_probe_XXXXXX.py)" || { printf 'NO_PY -\n'; return 0; }
    _rknn_probe_script > "$tmp"
    if ! docker cp "$tmp" "$container:/tmp/rknn_probe.py" >/dev/null 2>&1; then
        rm -f "$tmp"
        printf 'NO_PY -\n'
        return 0
    fi
    rm -f "$tmp"
    out="$(docker exec "$container" python3 /tmp/rknn_probe.py "$lib" "$model" 2>&1)" || true
    _rknn_probe_verdict "$out"
}

# 仓库里随便找一颗 .rknn 当探针模型（只读，不会改模型文件）
rknn_probe_model() {
    local m
    for m in "$SCRIPT_DIR"/VIDEO/data/models/*/model.rknn "$SCRIPT_DIR"/RUNTIME/data/models/*/model.rknn; do
        [ -s "$m" ] && { printf '%s\n' "$m"; return 0; }
    done
    return 0
}

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
        # rkmpp 的运行库不新增挂载点（.mpp-sdk 在已挂载的 RUNTIME 目录里），
        # 判据是 LD_LIBRARY_PATH 里那条 .mpp-sdk/lib
        local mrc=0
        runtime_has_mpp || mrc=$?
        if [ "$mrc" = "0" ]; then
            if grep -q 'mpp-sdk/lib' "$SCRIPT_DIR/VIDEO/.docker-compose.runtime.override.yaml"; then
                success "  compose override 已为容器开启 rkmpp 运行库通路"
            else
                error "  RUNTIME 链了 librockchip_mpp，但 override 的 LD_LIBRARY_PATH 里没有 .mpp-sdk/lib"
                error "  重新生成：bash VIDEO/scripts/ensure_runtime_cpp.sh wire，再 $0 restart"
                rc=1
            fi
        elif [ "$mrc" = "1" ]; then
            warn "  本次 RUNTIME 无 rkmpp 后端，跳过编解码运行库检查（$0 mpp-setup && $0 build 可补）"
        fi
        grep -E '^\s+- /dev/' "$SCRIPT_DIR/VIDEO/.docker-compose.runtime.override.yaml" \
            | sed 's/^/    设备透传:/' || warn "  override 未包含任何 /dev 节点"
        # RKNPU 的 card 主节点必须出现，否则容器内一定打不开设备
        if [ "$(uname -m)" = "aarch64" ]; then
            local need nd
            need="$(npu_device_nodes | grep '/dev/dri/card' || true)"
            for nd in $need; do
                if grep -qF -- "$nd:$nd" "$SCRIPT_DIR/VIDEO/.docker-compose.runtime.override.yaml"; then
                    success "  override 已透传 $nd（librknnrt 实际打开的节点）"
                else
                    error "  override 缺 $nd：容器内 rknn_init 会报 failed to open rknn device"
                    error "  重新生成：bash VIDEO/scripts/ensure_runtime_cpp.sh wire，再 $0 restart"
                    rc=1
                fi
            done
            if [ -z "$need" ]; then
                warn "  sysfs 里没识别出 RKNPU 的 card 节点（驱动名不匹配？），跳过该项检查"
            fi
        fi
    else
        error "  缺少 VIDEO/.docker-compose.runtime.override.yaml（跑 $0 build 生成）"
        rc=1
    fi
    # 宿主侧真调一次 rknn_init：设备不通就别指望容器通
    local hlib hmodel verdict
    hlib="$(rknn_system_runtime_lib 2>/dev/null || true)"
    [ -n "$hlib" ] || hlib="$(rknn_runtime_lib 2>/dev/null || true)"
    hmodel="$(rknn_probe_model)"
    if [ -n "$hlib" ] && [ -n "$hmodel" ]; then
        verdict="$(rknn_init_probe "$hlib" "$hmodel")"
        case "${verdict%% *}" in
            PASS)   success "  宿主 rknn_init 通过（${verdict}）" ;;
            DEV_FAIL) error "  宿主 rknn_init 打不开 NPU 设备（${verdict}）：内核 rknpu 驱动未加载"
                      error "  容器侧不可能通，先修宿主：dmesg | grep -i rknpu"
                      rc=1 ;;
            MODEL_FAIL) warn "  宿主能开设备，但加载模型失败（${verdict}）："
                        warn "    ${hmodel}"
                        warn "    多半是 librknnrt 版本与模型导出用的 rknn-toolkit2 不匹配，"
                        warn "    或模型的 target_platform 不是 rk3588。换 SDK/重导出模型。"
                        rc=1 ;;
            *)      warn "  宿主 rknn_init 探针未执行（${verdict}）" ;;
        esac
    else
        info "  缺 librknnrt.so 或仓库内无 .rknn 模型，跳过宿主 rknn_init 探针"
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
    local rc=0 out verdict
    # 探针脚本里只用双引号：整段是 sh -c '…' 传给 docker exec 的，套单引号会截断。
    out="$(docker exec "$VIDEO_CONTAINER" sh -c '
        # ls 带多个参数时，只要有一个不存在就整体返回非零。以前写成
        #   ls A B … || echo "（容器内未找到）"
        # 结果是把存在的 A 列出来之后，还额外印一句「未找到」，把成功的探测报成失败。
        # 判空要看输出有没有内容，不能看 ls 的退出码。
        emit() {
            _label="$1"; shift
            _got="$("$@" 2>/dev/null)"
            if [ -n "$_got" ]; then printf "%s\n" "$_got"; else echo "$_label"; fi
        }
        echo "-- 设备节点 --"
        emit "（无）" ls /dev/rga /dev/rknpu /dev/rknpu_ll /dev/mpp_service /dev/vpu_service /dev/dri/renderD* /dev/dri/card*
        emit "（无 /dev/dma_heap）" ls -d /dev/dma_heap/*
        echo "-- cgroup 设备白名单 --"
        cat /sys/fs/cgroup/devices/devices.list 2>/dev/null | grep -E "^c (226|10|241)" || true
        echo "-- librknnrt --"
        emit "（容器内未找到）" ls -1 /opt/easyaiot/rknn-lib/librknnrt.so /usr/lib/librknnrt.so
        echo "-- librockchip_mpp --"
        emit "（容器内未找到）" ls -1 /opt/easyaiot/RUNTIME/.mpp-sdk/lib/librockchip_mpp.so*
        echo "-- MPP SoC 探测 --"
        # MPP 靠 /proc/device-tree/compatible 认 rk3588；docker 默认用 tmpfs 盖住容器里的
        # /sys（只单独透出 /sys/fs/cgroup），所以那个符号链接在容器内是悬空的。
        if [ -r /proc/device-tree/compatible ]; then
            tr "\0" " " < /proc/device-tree/compatible; echo ""
        else
            echo "DTREE_MISSING（容器内读不到 /proc/device-tree/compatible）"
        fi
        echo "-- RUNTIME --"
        if [ -x /opt/easyaiot/RUNTIME/build/RUNTIME ]; then
            # mpp[x]: 开头的是 MPP 初始化日志，跟版本号混在一起会干扰下面的 grep，过滤掉
            /opt/easyaiot/RUNTIME/build/RUNTIME --version 2>&1 | grep -v "^mpp\[" | head -n2
        else
            echo "（未编译：先跑 install_rk3588.sh build）"
        fi
        echo "-- 动态库解析 --"
        LC_ALL=C ldd /opt/easyaiot/RUNTIME/build/RUNTIME 2>/dev/null \
            | grep -E "rknnrt|rockchip_mpp|libdrm|=>[[:space:]]*not found" || true
    ' 2>&1)" || { error "  docker exec 失败"; return 1; }
    echo "$out" | sed 's/^/  /'
    echo "$out" | grep -q 'librknnrt\.so' \
        || { error "  容器内看不到 librknnrt.so：dlopen 失败会回落 ONNX Runtime"; rc=1; }
    # 只认 ldd 的 "libfoo.so => not found" 和加载器的 "version `GLIBC_x' not found"。
    # 裸 grep not found 会把 MPP 自己的日志 "can not found match soc name" 当成缺库（假失败）。
    echo "$out" | grep -Eq '=>[[:space:]]*not found|version .*not found' \
        && { error "  容器内有未解析的依赖库（见上方 not found）"; rc=1; }
    echo "$out" | grep -Eq '/dev/(rga|rknpu|mpp_service|dri/renderD)' \
        || { error "  容器内没有 NPU/MPP 设备节点：重建容器时确认 override 生效"; rc=1; }
    if echo "$out" | grep -q 'DTREE_MISSING'; then
        error "  容器内读不到设备树 —— MPP 认不出 SoC，rkvdec/VEPU 的平台匹配会失败"
        error "  wire 时会自动把 /sys/firmware/devicetree/base 只读挂进容器；还报就说明"
        error "  override 没生效：docker inspect $VIDEO_CONTAINER | grep -A8 Binds"
        rc=1
    fi
    # rkmpp 后端是「直接链接」，不像 RKNN 那样 dlopen 失败可回落：
    # 容器里解析不到 librockchip_mpp 就是整个 RUNTIME 起不来，必须硬失败。
    if echo "$out" | grep -Eq 'librockchip_mpp\.so.*=>[[:space:]]*not found'; then
        error "  容器内解析不到 librockchip_mpp：RUNTIME 会直接起不来（不是回落软解）"
        error "  先确认 $0 mpp-setup 产出过 RUNTIME/.mpp-sdk/lib，再 wire + restart"
        rc=1
    fi
    # 硬证据：容器里真跑一次 rknn_init。上面的 ls/grep 只能证明「文件在」，
    # 证明不了设备可访问（缺 card 主节点时 ls 一样是干净的）。
    local cmodel
    cmodel="$(docker exec "$VIDEO_CONTAINER" sh -c \
        'for f in /app/data/models/*/model.rknn; do [ -s "$f" ] && { echo "$f"; exit 0; }; done' 2>/dev/null || true)"
    if [ -n "$cmodel" ]; then
        verdict="$(rknn_init_probe_container /opt/easyaiot/rknn-lib/librknnrt.so "$cmodel")"
        case "${verdict%% *}" in
            PASS) success "  容器内 rknn_init 通过（${verdict}，模型 ${cmodel}）" ;;
            DEV_FAIL)
                error "  容器内 rknn_init 打不开 NPU 设备（${verdict}，模型 ${cmodel}）"
                error "  九成是缺 RKNPU 的 /dev/dri/card* 主节点：docker 的 devices: 精确到 major:minor，"
                error "  只透传 renderD* 时容器里 mknod 也打不开（cgroup v1 的 c *:* m 只允许 mknod）。"
                error "  修法：bash VIDEO/scripts/ensure_runtime_cpp.sh wire && $0 restart"
                error "  验证：docker exec $VIDEO_CONTAINER cat /sys/fs/cgroup/devices/devices.list | grep 226"
                rc=1 ;;
            MODEL_FAIL)
                warn "  容器内能打开 NPU 设备，但模型加载失败（${verdict}，模型 ${cmodel}）"
                warn "  与宿主同款问题的话，是 librknnrt 版本 / 模型 target_platform 不匹配，不是容器配置"
                ;;
            *)  warn "  容器内 rknn_init 探针未执行（${verdict}）" ;;
        esac
    else
        info "  容器内没有 .rknn 模型，跳过 rknn_init 端到端探针（下发一个 RKNN 算法任务后再 verify）"
    fi
    return "$rc"
}

verify_services() {
    step "服务健康"
    local rc=0
    if curl -sf http://127.0.0.1:6000/actuator/health >/dev/null 2>&1; then
        success "  VIDEO /actuator/health OK"
    else
        error "  VIDEO :6000 健康检查不可达"; rc=1
        # 容器 running 但端口没起来，属应用侧启动失败（不是 NPU/编解码配置问题）。
        # RUNTIME 是直链接二进制，缺任一依赖都会在加载期挂掉并带走整个服务。
        info "  看启动日志定位：docker logs --tail=200 $VIDEO_CONTAINER"
        info "  端口占用情况：ss -lntp | grep 6000"
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
        info "  当前任务的推理/编解码后端配置（由 VIDEO 控制面写入）："
        grep -H -E '^\s*(infer_backend|npu_core_mask|model_path|hwaccel|hwaccel_decode|hwaccel_encode)' \
            $inis 2>/dev/null | sed 's/^/    /' || true
        # RKNN 只解决推理；hwaccel 没写成 rkmpp 说明视频链路可能还在吃 CPU
        if runtime_has_mpp; then
            if grep -l -E '^[[:space:]]*hwaccel[[:space:]]*=[[:space:]]*rkmpp' $inis >/dev/null 2>&1; then
                success "  已有任务配置 hwaccel=rkmpp（rkvdec 硬解 + VEPU 硬编）"
            elif grep -l -E '^[[:space:]]*hwaccel[[:space:]]*=[[:space:]]*none' $inis >/dev/null 2>&1; then
                warn "  有任务配置 hwaccel=none：编解码全走 CPU，libx264 会吃掉 30-40 % 算力"
                warn "  清掉 VIDEO 里的 RUNTIME_HWACCEL / RUNTIME_FORCE_SOFT_AV 后重建任务"
            elif grep -l -E '^[[:space:]]*hwaccel[[:space:]]*=' $inis >/dev/null 2>&1; then
                info "  任务配置为 hwaccel=auto（RUNTIME 自行探测 rkmpp）；功能正常，但建议显式 rkmpp"
            else
                warn "  RUNTIME 带 rkmpp 后端，但任务 ini 里没有 hwaccel —— VIDEO 控制面版本过旧"
                warn "  重启 video-service 让新版 runtime_config_service 生效后重建任务"
            fi
        fi
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
    info "逐条取证（三处 NPU 判据是否同源 / 白名单是否真透传 / rkmpp 编解码链路）："
    info "  bash RUNTIME/scripts/verify_rk_media.sh --container=video-service"
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
    # 离线包是「直接链接」rkmpp 的话，包里必须自带那份改过版本节点的库，
    # 否则盒子上的 RUNTIME 会连启动都启动不了（不是回落软解）。
    if binary_links_mpp "$bin"; then
        if compgen -G "$dest/mpp-lib/librockchip_mpp.so*" >/dev/null 2>&1 \
           || compgen -G "$dest/.mpp-sdk/lib/librockchip_mpp.so*" >/dev/null 2>&1; then
            success "  $bin 已带 rkmpp 后端，且包内含 librockchip_mpp"
        else
            error "  $bin 链接了 librockchip_mpp，但离线包里没有这份库 —— 装到节点上 RUNTIME 会起不来"
            error "  补法：把盒子上 RUNTIME/.mpp-sdk/lib 的内容打进包的 RUNTIME/mpp-lib/（$0 mpp-setup 的产物）"
        fi
    elif [ -x "$bin" ]; then
        warn "  $bin 未链接 librockchip_mpp：编解码走 CPU（1080p25 约吃 1.5 个 A76）"
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
    # 头部注释块为 2-47 行（47 行为收尾的 # ===），改注释时同步这里
    sed -n '2,47p' "${BASH_SOURCE[0]}" | sed 's/^#\{1,2\} \{0,1\}//'
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
        sdk-setup) sdk_setup ;;
        mpp-setup) mpp_setup ;;
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
