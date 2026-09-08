#!/usr/bin/env bash
# ============================================
# 真正拉起 RUNTIME --version。文件存在不算就绪。
# CUDA 驱动库缺失视为 CPU 可用；glibc / libstdc++ / 其它 .so 缺失则失败。
#
# 用法:
#   bash smoke_runtime.sh [/opt/easyaiot/RUNTIME]
# ============================================
set -u

ROOT="${1:-}"
if [[ -z "$ROOT" ]]; then
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi

BIN="${RUNTIME_BIN:-$ROOT/bin/RUNTIME}"
ENV_SH="$ROOT/env.sh"

if [[ ! -x "$BIN" ]]; then
  echo "SMOKE_FAIL: 二进制不存在或不可执行: $BIN" >&2
  exit 1
fi

unset LD_LIBRARY_PATH LD_PRELOAD
if [[ -f "$ENV_SH" ]]; then
  # shellcheck disable=SC1090
  . "$ENV_SH"
fi

cuda_noise='libcuda\.so|libcudart|libcublas|libcudnn|libnvinfer|libnvrtc|libnvidia'

if command -v ldd >/dev/null 2>&1; then
  # LC_ALL=C：ldd 的 "not found" 会随 locale 翻译（zh_CN 下是「未找到」），
  # 不锁住的话这条 grep 永远不命中，缺库的二进制会被判成 smoke 通过
  miss="$(LC_ALL=C ldd "$BIN" 2>/dev/null | grep 'not found' | grep -Ev "$cuda_noise" || true)"
  if [[ -n "$miss" ]]; then
    echo "SMOKE_FAIL: 动态库缺失（非 CUDA）:" >&2
    echo "$miss" >&2
    exit 2
  fi
fi

# 注意别写成 `out="$(...)" || true; ec=$?`：|| true 会把 $? 变成 0，
# 于是下面的分支永远走 SMOKE_OK，RUNTIME 起不来也报通过。
ec=0
out="$("$BIN" --version 2>&1)" || ec=$?
echo "$out"

if [[ "$ec" -eq 0 ]]; then
  echo "SMOKE_OK: $BIN"
  exit 0
fi

# 动态链接器失败
# 动态链接失败的特征。这里刻意不用裸 "not found"：链上 MPP 之后
# RUNTIME --version 会打 "mpp_platform: can not found match soc name"（容器里
# 读不到设备树），那是告警不是缺库，会被误判成加载失败。ldd 的缺库行长成
# "libfoo.so => not found"，符号版本不兼容行长成 "version `GLIBC_2.29' not found"。
dyn_fail='=>[[:space:]]*not found|version .*not found|cannot open shared object file|GLIBCXX_[0-9]|CXXABI_[0-9]|GLIBC_[0-9]'

if echo "$out" | grep -Eqi "$dyn_fail"; then
  if echo "$out" | grep -Ev "$cuda_noise" | grep -Eqi "$dyn_fail"; then
    echo "SMOKE_FAIL: 无法执行 --version (exit=$ec)" >&2
    exit 3
  fi
  echo "SMOKE_OK: $BIN（仅缺 CUDA 驱动库，可作 CPU 执行器）"
  exit 0
fi

if [[ "$ec" -eq 127 ]]; then
  echo "SMOKE_FAIL: 无法执行 --version (exit=127)" >&2
  exit 3
fi

# --version 正常应返回 0；其它退出码也视为不可用
echo "SMOKE_FAIL: --version 退出码 $ec" >&2
exit 3
