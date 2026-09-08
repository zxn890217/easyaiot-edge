#!/usr/bin/env bash
# ============================================
# Rockchip MPP（rkvdec 硬解 / VEPU 硬编）探测 —— 编译期与部署期共用
# ============================================
# 由 RUNTIME/install_linux.sh 与仓库根的 RK3588 盒子脚本 source，
# 与 rknn_sdk.sh 同一套约定：只定义函数、只往 stdout 打印路径/版本，判定交给调用方。
#
# 导出函数：
#   mpp_sdk_probe           打印可用的 MPP SDK 根目录（其下能找到 rockchip/rk_mpi.h），找不到返回 1
#   mpp_header_found        编译期能否找到 rk_mpi.h（SDK 目录或系统 include）
#   mpp_system_runtime_lib  只要系统路径（/usr/lib 等）里的 librockchip_mpp，不含仓库内副本
#   mpp_runtime_lib         打印运行期 librockchip_mpp 路径，找不到返回 1
#   mpp_staged_lib          打印 RUNTIME/.mpp-sdk/lib 里那份（可能已 patch 过）
#   mpp_soname              读 .so 的 SONAME —— 它才是写进 DT_NEEDED 的名字
#   mpp_lib_needed          列 .so 的 DT_NEEDED（判断容器还缺哪些传递依赖）
#   mpp_lib_versions        列 .so 的 Verneed 版本节点（自带 elf_hash 自检）
#   mpp_needs_glibc_patch   .so 是否要求容器没有的版本节点（实测为 GLIBC_2.29）
#   mpp_patch_glibc         改写 Verneed 后另存一份（入参文件永不修改）
#   mpp_version             尽力从 .so 里取出版本串，取不到打印空
#
# 为什么运行期库非得改一道手（与 rknn_sdk.sh 最大的差别）：
#   盒子宿主是 Ubuntu 20.04（glibc 2.31），板上那份 librockchip_mpp.so.0 的
#   .gnu.version_r 挂着 GLIBC_2.29 —— pow/log/log2 三个 libm 符号。
#   video-service 容器是 AlmaLinux 8.10（glibc 2.28），链接期直接报
#     version `GLIBC_2.29' not found, required by .../librockchip_mpp.so.1
#   ld.so 的启动版本审计不查符号：它把 DT_VERNEED 的 vn_file 字符串（"libm.so.6"）
#   解析成已加载的那一个对象，只在对象内部找版本节点。所以 -lm、
#   LD_PRELOAD shim 全都救不了（细节与被否掉的路线见 RUNTIME/tools/mpp_glibc_patch.c）。
#   唯一可行的办法是把版本节点重定向到 GLIBC_2.17（aarch64 上所有基础符号的基线），
#   这就是 mpp_patch_glibc / install_rk3588.sh mpp-setup 存在的理由。
#
# 另注意：mpp_sdk_probe 刻意不把 /usr 当候选。宿主头文件确实在 /usr/include/rockchip，
# 但 docker 编译模式下容器看不到宿主 rootfs，把 /usr 挂进容器只会顶掉容器自己的 /usr。
# 系统头文件只对 EASYAIOT_RUNTIME_BUILD_MODE=host 有意义，那条路径走 mpp_header_found。

[[ -n "${RUNTIME_MPP_SDK_LOADED:-}" ]] && return 0
RUNTIME_MPP_SDK_LOADED=1

# RUNTIME 模块目录（本文件位于 RUNTIME/scripts/）
MPP_SDK_HELPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MPP_RUNTIME_DIR="$(cd "${MPP_SDK_HELPER_DIR}/.." && pwd)"
MPP_STAGED_ROOT="${MPP_RUNTIME_DIR}/.mpp-sdk"
MPP_PATCH_SRC="${MPP_RUNTIME_DIR}/tools/mpp_glibc_patch.c"

# 容器缺的版本节点 -> 重定向目标（两个名字等长，改写落在原 .dynstr 槽位里）
MPP_GLIBIC_FROM="${MPP_GLIBIC_FROM:-GLIBC_2.29}"
MPP_GLIBIC_TO="${MPP_GLIBIC_TO:-GLIBC_2.17}"

# SDK 根目录候选。显式 MPP_SDK_ROOT 优先，其次是约定落点 RUNTIME/.mpp-sdk
# （docker 编译时它随 /src 一起进容器）。rk_mpi.h 的两种布局都要认，
# 与 RUNTIME/CMakeLists.txt 里 find_path(rockchip/rk_mpi.h) +
# find_path(rk_mpi.h) 两段一一对应：
#   <root>/include/rockchip/rk_mpi.h   —— 整理过的 SDK / mpp-setup 的产物
#   <root>/rockchip/rk_mpi.h           —— 直接把厂商 include/rockchip 当根
mpp_sdk_probe() {
  local root cand
  for root in "${MPP_SDK_ROOT:-}" "${MPP_STAGED_ROOT}" \
              /opt/rockchip-mpp /usr/local/rockchip-mpp \
              /opt/mpp /usr/local/mpp; do
    [[ -n "$root" && -d "$root" ]] || continue
    case "$root" in
      # 见文件头：系统前缀绝不接受，挂载会顶掉容器自己的 /usr
      /|/usr|/usr/local|/opt|/srv|/home|/var|/etc|/lib|/lib64) continue ;;
    esac
    for cand in "$root/include/rockchip/rk_mpi.h" "$root/rockchip/rk_mpi.h"; do
      if [[ -f "$cand" ]]; then
        printf '%s\n' "$root"
        return 0
      fi
    done
  done
  return 1
}

# 编译期头文件：SDK 里有，或盒子固件把开发包装进了系统路径（host 编译模式才用得上）
mpp_header_found() {
  local cand sdk
  for cand in /usr/include/rockchip/rk_mpi.h /usr/local/include/rockchip/rk_mpi.h \
              /usr/include/rk_mpi.h /usr/local/include/rk_mpi.h; do
    [[ -f "$cand" ]] && return 0
  done
  sdk="$(mpp_sdk_probe || true)"
  [[ -n "$sdk" ]] || return 1
  for cand in "$sdk/include/rockchip/rk_mpi.h" "$sdk/rockchip/rk_mpi.h"; do
    [[ -f "$cand" ]] && return 0
  done
  return 1
}

# 宿主系统路径里那份 librockchip_mpp（厂商 rootfs 只有带版本的 SONAME，没有 dev 符号链接）
mpp_system_runtime_lib() {
  local cand base dir
  for dir in /usr/lib /usr/lib64 /usr/local/lib /usr/lib/aarch64-linux-gnu \
             /usr/local/lib/aarch64-linux-gnu /oem/usr/lib /vendor/usr/lib; do
    for base in librockchip_mpp.so.0 librockchip_mpp.so.1 librockchip_mpp.so; do
      cand="$dir/$base"
      if [[ -e "$cand" ]]; then
        printf '%s\n' "$cand"
        return 0
      fi
    done
  done
  return 1
}

# mpp-setup 落到仓库里的那份（docker 编译/运行用的就是它，GLIBC 版本节点已重定向）
mpp_staged_lib() {
  local base="${MPP_STAGED_ROOT}/lib" cand
  for cand in "$base"/librockchip_mpp.so.1 "$base"/librockchip_mpp.so.0 "$base"/librockchip_mpp.so; do
    [[ -e "$cand" ]] && { printf '%s\n' "$cand"; return 0; }
  done
  return 1
}

# 运行期库：编译/链接以仓库内已 patch 的那份为先 —— 系统那份在容器里链不动。
mpp_runtime_lib() {
  local cand
  cand="$(mpp_staged_lib || true)"
  [[ -n "$cand" ]] && { printf '%s\n' "$cand"; return 0; }
  mpp_system_runtime_lib
}

# SONAME 才是 ld 写进 DT_NEEDED 的名字（板上 librockchip_mpp.so.0 的 SONAME 是 .so.1），
# 所以暂存目录里最终那份必须叫这个名字，否则容器里 ld.so 找不到。
mpp_soname() {
  local lib="${1:-}" out=""
  [[ -f "$lib" ]] || return 1
  if command -v readelf >/dev/null 2>&1; then
    out="$(readelf -d "$lib" 2>/dev/null \
           | awk '/\(SONAME\)/ {gsub(/[][()]/, "", $NF); print $NF; exit}')"
  fi
  if [[ -z "$out" ]] && command -v objdump >/dev/null 2>&1; then
    out="$(objdump -p "$lib" 2>/dev/null | awk '/SONAME/ {print $2; exit}')"
  fi
  [[ -n "$out" ]] || out="$(basename "$lib")"
  printf '%s\n' "$out"
}

# DT_NEEDED 列表：判断容器里是否还缺 libdrm.so.2 之类的传递依赖。
# 缺一个就整个 RUNTIME 起不来（直接链接的二进制，加载期解析失败没有回落可言）。
mpp_lib_needed() {
  local lib="${1:-}"
  [[ -f "$lib" ]] || return 1
  if command -v readelf >/dev/null 2>&1; then
    readelf -d "$lib" 2>/dev/null \
      | awk '/\(NEEDED\)/ {gsub(/[][()]/, "", $NF); print $NF}'
    return 0
  fi
  command -v objdump >/dev/null 2>&1 || return 1
  objdump -p "$lib" 2>/dev/null | awk '/NEEDED/ {print $2}'
}

# Verneed 全表（list 模式自带 elf_hash 自检：存表里的哈希能复现，才允许写）
mpp_lib_versions() {
  local lib="${1:-}" tool
  [[ -f "$lib" ]] || return 1
  tool="$(mpp_patch_tool || true)"
  [[ -n "$tool" ]] || { echo "mpp_glibc_patch 编译不了（缺 gcc/cc，且未预置 $MPP_PATCH_TOOL）" >&2; return 1; }
  "$tool" "$lib"
}

# 是否要求容器没有的版本节点
mpp_needs_glibc_patch() {
  local lib="${1:-}" want="${2:-$MPP_GLIBIC_FROM}"
  [[ -f "$lib" ]] || return 1
  mpp_lib_versions "$lib" 2>/dev/null | grep -q "version=$want" || return 1
  # 自检失败（MISMATCH）时不能改：mpp_glibc_patch 自己也会拒绝写，但这里先挡住
  mpp_lib_versions "$lib" 2>/dev/null | grep -q 'SELFTEST FAILED' && return 1
  return 0
}

# 预编译的修补工具优先（离线盒子可能没装 gcc）：MPP_PATCH_TOOL=/path/to/mpp_glibc_patch
MPP_PATCH_TOOL="${MPP_PATCH_TOOL:-}"

mpp_patch_tool() {
  if [[ -n "$MPP_PATCH_TOOL" && -x "$MPP_PATCH_TOOL" ]]; then
    printf '%s\n' "$MPP_PATCH_TOOL"
    return 0
  fi
  [[ -f "$MPP_PATCH_SRC" ]] || return 1
  local cc="" cand out_dir tool
  for cand in gcc cc clang; do
    command -v "$cand" >/dev/null 2>&1 && { cc="$(command -v "$cand")"; break; }
  done
  [[ -n "$cc" ]] || return 1
  # 缓存进暂存目录（本来就是 .gitignore 里的产物目录），没有可写位置就退回 TMPDIR
  if mkdir -p "$MPP_STAGED_ROOT/.build" 2>/dev/null; then
    out_dir="$MPP_STAGED_ROOT/.build"
  else
    out_dir="${TMPDIR:-/tmp}"
  fi
  tool="$out_dir/mpp_glibc_patch"
  [[ -x "$tool" && "$tool" -nt "$MPP_PATCH_SRC" ]] && { printf '%s\n' "$tool"; return 0; }
  "$cc" -O2 -o "$tool" "$MPP_PATCH_SRC" 2>/dev/null || return 1
  printf '%s\n' "$tool"
}

# mpp_patch_glibc IN.so OUT.so [FROM] [TO] —— 入参永不修改
mpp_patch_glibc() {
  local in="${1:-}" out="${2:-}" from="${3:-$MPP_GLIBIC_FROM}" to="${4:-$MPP_GLIBIC_TO}"
  [[ -f "$in" && -n "$out" ]] || { echo "usage: mpp_patch_glibc IN.so OUT.so [FROM [TO]]" >&2; return 1; }
  local tool
  tool="$(mpp_patch_tool || true)"
  [[ -n "$tool" ]] || { echo "找不到 mpp_glibc_patch（源文件 $MPP_PATCH_SRC，或预置 MPP_PATCH_TOOL）" >&2; return 1; }
  "$tool" "$in" "$out" "$from" "$to"
}

# MPP 的版本号只在 mpp_init 的日志里打印过，静态取一下：厂商包一般把
# MPP_VERSION 编进了 .rodata（形如 "mpp: 1.5.0" / "v1.4.0"）。取不到就打印空，
# 调用方按 unknown 处理 —— 只影响体检文案，不影响功能。
mpp_version() {
  local lib="${1:-}" raw=""
  [ -f "$lib" ] || return 0
  if command -v strings >/dev/null 2>&1; then
    raw="$(strings -a "$lib" 2>/dev/null \
          | grep -m1 -oiE '(mpp[^a-z]{0,3}version[ :]+|MPP_API_VERSION[ :]+)[0-9][0-9a-zA-Z._-]*' || true)"
  else
    raw="$(LC_ALL=C grep -a -m1 -oiE 'version[ :]+[0-9]+\.[0-9]+\.[0-9]+' "$lib" 2>/dev/null || true)"
  fi
  [ -n "$raw" ] || return 0
  printf '%s\n' "$raw" | grep -m1 -oE '[0-9]+\.[0-9]+\.[0-9]+' || true
}

# MPP 可用的判据之一：宿主有没有那个字符设备。老厂商内核叫 /dev/vpu_service。
mpp_device_ready() {
  [[ -e /dev/mpp_service || -e /dev/vpu_service ]]
}
