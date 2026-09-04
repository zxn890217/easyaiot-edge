#!/usr/bin/env bash
# ============================================
# RK3588/RK356x：从 sysfs 精确识别 RKNPU 的 DRM card 主节点
# ============================================
# 为什么要单独一个文件：
#   RUNTIME/scripts/rknn_sdk.sh（体检/安装器）与 VIDEO/scripts/ensure_runtime_cpp.sh
#   （生成 compose override）都需要这份判据，逻辑一旦漂移就会出现「体检通过、容器内用不了」。
#
# 实测结论（RK3588 + rknpu 0.9.8 + librknnrt 2.3.0）：
#   librknnrt 打开的是 card 主节点 —— 宿主侧 rknn_init 后 /proc/self/fd 指向
#   /dev/dri/card1（226:1，sysfs 驱动名 RKNPU），而不是 renderD129。
#   docker 的 devices: 白名单精确到 major:minor（cgroup v1 里 `c *:* m` 只允许 mknod、
#   不允许 rwm），所以只透传 renderD* 时，容器内 rknn_init 必然失败在
#   「failed to open rknpu module / failed to open rknn device」。
#
# renderD* 不在这里输出：调用方本来就按 glob 全量透传（MPP/RGA 解码要用 rockchip-drm
# 那张卡的 render 节点），这里只负责补充「NPU 专属的 card 主节点」。
#
# 用法：
#   source VIDEO/scripts/npu_drm_nodes.sh
#   npu_drm_card_nodes        # 逐行输出存在的 NPU 所属 /dev/dri/card*
#   非 RK 平台 / 内核没登记驱动名时不输出任何内容，调用方按「没有额外节点」处理。
#
# 注意：本文件会被 set -euo pipefail 的脚本 source，内部不用裸全局变量、不依赖 glob。
# ============================================

npu_drm_card_nodes() {
  local entry drv idx devpath hay
  for entry in /sys/class/drm/card*; do
    [ -e "$entry" ] || continue
    drv="$(readlink -f "$entry/device/driver" 2>/dev/null \
           || readlink "$entry/device/driver" 2>/dev/null || true)"
    devpath="$(readlink -f "$entry" 2>/dev/null || true)"
    # 驱动名与设备路径都看：厂商内核可能把驱动登记成 rknpu / rockchip-rknpu，
    # 而 sysfs 路径里通常带 platform 节点名（fe440000.npu 之类）。
    # rockchip-drm 那张显示卡的路径不含 npu/rknn，不会误判。
    hay="$(printf '%s %s' "${drv##*/}" "$devpath" | tr '[:upper:]' '[:lower:]')"
    [[ "$hay" =~ (rknpu|rknn|[.]npu|/npu) ]] || continue
    idx="${entry##*/}"
    idx="${idx#card}"
    [[ "$idx" =~ ^[0-9]+$ ]] || continue
    # 用 if 而不是 `[ -e ] && printf`：后者在条件不成立时整体返回 1，
    # 会被调用方的 set -e + pipefail 当成致命错误提前掐断循环。
    if [ -e "/dev/dri/card${idx}" ]; then
      printf '/dev/dri/card%s\n' "$idx"
    fi
  done | sort -u
  return 0
}
