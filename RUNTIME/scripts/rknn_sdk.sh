#!/usr/bin/env bash
# ============================================
# Rockchip NPU（rknpu2）探测 —— 编译期与部署期共用
# ============================================
# 由 RUNTIME/install_linux.sh 与仓库根的 RK3588 盒子脚本 source，避免探测路径各写一份。
#
# 导出函数：
#   rknn_sdk_probe       打印可用的 rknpu2 SDK 根目录（其下能找到 rknn_api.h），找不到返回 1
#   rknn_header_found    编译期能否找到 rknn_api.h（SDK 目录或系统 include）
#   rknn_runtime_lib     打印运行期 librknnrt.so 路径，找不到返回 1
#   rknn_system_runtime_lib  只要系统路径（/usr/lib 等）里那份，不含 SDK 目录副本
#   librknnrt_version    从 librknnrt.so 里取出版本号（比对 SDK 与盒子驱动用）
#   npu_device_nodes     逐行打印存在的 NPU / RGA / MPP 设备节点
#   npu_core_mask_normalize  把可读别名（all/core0/core0_1...）归一化成 RUNTIME 认得的值

[[ -n "${RUNTIME_RKNN_SDK_LOADED:-}" ]] && return 0
RUNTIME_RKNN_SDK_LOADED=1

# RUNTIME 模块目录（本文件位于 RUNTIME/scripts/）
RKNN_SDK_HELPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RKNN_RUNTIME_DIR="$(cd "${RKNN_SDK_HELPER_DIR}/.." && pwd)"

# SDK 根目录候选。显式 RKNN_SDK_ROOT 优先，其次解压到仓库内（docker 编译时随 /src 一起进容器）
# 与厂包常见落点。rknn_api.h 的实际位置有三种布局，都要认：
#   <root>/include/rknn_api.h                              —— 自己整理过的 SDK
#   <root>/rknpu2/include/rknn_api.h                       —— 整包 rknpu2 目录
#   <root>/librknn_api/include/rknn_api.h                  —— rknpu2/runtime/RK3588/Linux 布局
rknn_sdk_probe() {
  local root cand
  for root in "${RKNN_SDK_ROOT:-}" \
              "${RKNN_RUNTIME_DIR}/.rknn-sdk" \
              /opt/rknpu2 /usr/local/rknpu2 \
              /opt/librknn_api /usr/local/librknn_api \
              /opt/rknn /usr/local/rknn; do
    [[ -n "$root" && -d "$root" ]] || continue
    for cand in "$root/include/rknn_api.h" \
                "$root/rknpu2/include/rknn_api.h" \
                "$root/librknn_api/include/rknn_api.h"; do
      if [[ -f "$cand" ]]; then
        printf '%s\n' "$root"
        return 0
      fi
    done
  done
  return 1
}

# 编译期头文件：SDK 里有，或盒子固件把开发包装进了系统路径
rknn_header_found() {
  local cand sdk
  for cand in /usr/include/rknn_api.h /usr/local/include/rknn_api.h \
              /usr/include/rknn/rknn_api.h /usr/local/include/rknn/rknn_api.h; do
    [[ -f "$cand" ]] && return 0
  done
  sdk="$(rknn_sdk_probe || true)"
  [[ -n "$sdk" ]] || return 1
  for cand in "$sdk/include/rknn_api.h" "$sdk/rknpu2/include/rknn_api.h" \
              "$sdk/librknn_api/include/rknn_api.h"; do
    [[ -f "$cand" ]] && return 0
  done
  return 1
}

# 盒子系统路径里那份 librknnrt.so（由 rknpu2 驱动/厂包安装，运行期 ld.so 真正加载的就是它）。
# 不含 SDK 目录内的副本 —— 比对驱动与 SDK 版本时必须只用这一个来源。
rknn_system_runtime_lib() {
  local cand
  for cand in /usr/lib/librknnrt.so /usr/local/lib/librknnrt.so \
              /usr/lib/aarch64-linux-gnu/librknnrt.so \
              /oem/usr/lib/librknnrt.so /vendor/usr/lib/librknnrt.so; do
    if [[ -f "$cand" ]]; then
      printf '%s\n' "$cand"
      return 0
    fi
  done
  return 1
}

# 运行期库：正常由盒子上的 rknpu2 驱动提供；离线/精简镜像可能只落在 SDK 目录里
rknn_runtime_lib() {
  local cand sdk
  cand="$(rknn_system_runtime_lib || true)"
  [[ -n "$cand" ]] && { printf '%s\n' "$cand"; return 0; }
  sdk="$(rknn_sdk_probe || true)"
  [[ -n "$sdk" ]] || return 1
  for cand in "$sdk/lib/librknnrt.so" "$sdk/rknpu2/lib/librknnrt.so" \
              "$sdk/librknn_api/aarch64/librknnrt.so" "$sdk/aarch64/librknnrt.so"; do
    if [[ -f "$cand" ]]; then
      printf '%s\n' "$cand"
      return 0
    fi
  done
  return 1
}

# librknnrt.so 里带一行 "librknnrt version: 1.6.0 (…)"，取出版本号用于比对驱动/SDK。
# 优先 strings（二进制友好）；没有 binutils 时退回 grep -a。都取不到就打印空。
librknnrt_version() {
  local lib="${1:-}" raw=""
  [ -f "$lib" ] || return 0
  if command -v strings >/dev/null 2>&1; then
    raw="$(strings -a "$lib" 2>/dev/null | grep -m1 -i 'librknnrt version' || true)"
  else
    raw="$(LC_ALL=C grep -a -m1 -o 'librknnrt version: *[0-9][0-9.]*' "$lib" 2>/dev/null || true)"
  fi
  [ -n "$raw" ] || return 0
  printf '%s\n' "$raw" | grep -m1 -oE '[0-9]+\.[0-9]+\.[0-9]+' || true
}

# 只列真实存在的节点：docker-compose 的 devices: 写不存在的节点会直接起不来服务
npu_device_nodes() {
  local node seen=" "
  for node in /dev/rga /dev/rknpu /dev/rknpu_ll /dev/mpp_service /dev/dri/renderD*; do
    [[ -e "$node" ]] || continue
    case "$seen" in *" $node "*) continue ;; esac
    seen="${seen}${node} "
    printf '%s\n' "$node"
  done
}

# NPU 核绑定：接受可读别名与数字掩码，非法值返回 1（调用方决定是报错还是回落 auto）。
# 取值与 RUNTIME/src/ConfigParser.cpp 的 parseNpuCoreMask 保持一致：
#   auto                 交给驱动调度（RKNN_NPU_CORE_AUTO）
#   all / core0_1_2 / 7  三核同时跑（RKNN_NPU_CORE_0_1_2）
#   per_thread / -1      每个推理线程绑一个核（engine i → core i % 3）
#   core0..core2、core0_1、core0_2、core1_2、数字掩码   显式绑定
npu_core_mask_normalize() {
  local v
  v="$(printf '%s' "${1:-auto}" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')"
  case "$v" in
    ""|auto|default|driver) printf 'auto\n' ;;
    all|every|0_1_2|core0_1_2) printf 'all\n' ;;
    per_thread|per-thread|round_robin|round-robin) printf 'per_thread\n' ;;
    core0|npu0|1)   printf 'core0\n' ;;
    core1|npu1|2)   printf 'core1\n' ;;
    core2|npu2|4)   printf 'core2\n' ;;
    core0_1|0_1|3)  printf 'core0_1\n' ;;
    core0_2|0_2|5)  printf 'core0_2\n' ;;
    core1_2|1_2|6)  printf 'core1_2\n' ;;
    *)
      # 其余只放行 C++ 侧能 stol 解析的整数（含 0x 十六进制）
      if [[ "$v" =~ ^(0[xX][0-9a-fA-F]+|-?[0-9]+)$ ]]; then
        printf '%s\n' "$v"
      else
        return 1
      fi
      ;;
  esac
}
