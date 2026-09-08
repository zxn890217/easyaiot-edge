#!/usr/bin/env bash
# ============================================
# RK3588 / RK356x：NPU 节点探测 + rkmpp 编解码链路 验证脚本
# ============================================
# 为什么要单独一个文件（而不是往 install_rk3588.sh 的 verify 里再加几条）：
#   install_rk3588.sh verify_* 查的是「部署有没有做对」——设备有没有透传、库有没有挂上、
#   服务有没有起来。这份脚本查的是它查不到的两件事：
#
#     A. 三处 NPU 判据实现是否仍然同源、同结论。
#        shell(VIDEO/scripts/npu_drm_nodes.sh) 决定 compose 的 devices: 白名单，
#        Python(VIDEO/app/services/runtime_config_service.py) 决定控制面是否下发 .rknn，
#        C++(RUNTIME/src/InferEngine.cpp) 决定 RUNTIME 是否选 RKNN 引擎。
#        三者一旦漂移，症状是「体检全绿、容器里 rknn_init 必失败」，或者反过来
#        「控制面误判本机有 NPU，只下发 .rknn 权重而 RUNTIME 加载不上」。
#        尤其是把 /dev/rga、/dev/dri/renderD* 当 NPU 证据这一类假阳性：只有交叉比对
#        三份实现的输出集合才回归得住，所以 A 段另用一个与它们无关的第二判据(oracle)。
#
#     B. rkmpp 编解码链路是否真能出流：链接期 DT_NEEDED、GLIBC 版本节点、设备 rw、
#        task ini 里的 hwaccel* 决策、VEPU 真编码一帧、/health 上报的 decode_ep/encode_ep。
#        librockchip_mpp 是链接期依赖（不像 librknnrt 走 dlopen 还能回落 ORT），
#        缺一个传递依赖整个 RUNTIME 都起不来，所以部署前后都得实证一次。
#
#   判据本身一律复用 RUNTIME/scripts/rknn_sdk.sh、RUNTIME/scripts/mpp_sdk.sh、
#   VIDEO/scripts/npu_drm_nodes.sh，本脚本只做「交叉比对 + 实证」，绝不另起炉灶。
#
# 用法：
#   bash RUNTIME/scripts/verify_rk_media.sh                      # 本机全量（宿主或容器内皆可）
#   bash RUNTIME/scripts/verify_rk_media.sh --no-probe           # 不编译 C 探针（只要静态证据）
#   bash RUNTIME/scripts/verify_rk_media.sh --container=video-service
#                                                                # 本机跑完 cp 进容器复跑并比对差异
#   bash RUNTIME/scripts/verify_rk_media.sh --inside-container   # 容器内模式（上一条会自动调用）
#   bash RUNTIME/scripts/verify_rk_media.sh --repo=/path/to/easyaiot-edge
#   bash RUNTIME/scripts/verify_rk_media.sh --report=/tmp/a.env  # 另存机器可读摘要（默认 ${TMPDIR:-/tmp}/rk_media_report.env）
#
# 退出码：0 = 无 FAIL（允许 SKIP）；1 = 有 FAIL
# 输出以 PASS/FAIL/SKIP 开头，便于 grep -E '^(FAIL|SKIP)'
# ============================================
set -eo pipefail

_SELF="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(cd "$(dirname "$_SELF")" && pwd)"

INSIDE_CONTAINER=0
NO_PROBE=0
CONTAINER=""
REPO_OPT=""
REPORT=""
CONTAINER_REPORT="/tmp/rk_media_container.env"

PASSES=0
FAILS=0
SKIPS=0

# 供 C 段比对的摘要字段（A/B 段填充）
SHELL_NODES=""
ORACLE_NODES=""
PY_NODES=""
NPU_EVIDENCE=0
MPP_LIB=""
MPP_SONAME=""
MPP_DLOPEN=""
RKNNRT_DLOPEN=""
MPP_DEVICE=0
DEVICE_RW=0
BIN_NEEDED=""
HEALTH_DECODE=""
HEALTH_ENCODE=""

usage() {
    local end
    # 文件头是 `# ===` / 标题 / `# ===` 三段，第一个分隔符在 NR=2/4，所以要跳过标题再看
    end="$(awk 'NR > 4 && /^# ={5,}$/ { print NR - 1; exit }' "$_SELF")"
    sed -n "3,${end:-36}p" "$_SELF" | sed 's/^#\{1,2\} \{0,1\}//'
}

hdr() { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }
pass() { printf 'PASS  %s\n' "$*"; PASSES=$((PASSES + 1)); }
fail() { printf 'FAIL  %s\n' "$*"; FAILS=$((FAILS + 1)); }
skip() { printf 'SKIP  %s\n' "$*"; SKIPS=$((SKIPS + 1)); }
note() { printf '      %s\n' "$*"; }
indent() { sed 's/^/        /'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --inside-container) INSIDE_CONTAINER=1; shift ;;
        --no-probe) NO_PROBE=1; shift ;;
        --container) CONTAINER="video-service"; shift ;;
        --container=*) CONTAINER="${1#*=}"; shift ;;
        --repo=*) REPO_OPT="${1#*=}"; shift ;;
        --report=*) REPORT="${1#*=}"; shift ;;
        --container-report=*) CONTAINER_REPORT="${1#*=}"; shift ;;
        -h | --help) usage; exit 0 ;;
        *)
            printf 'FAIL  未知参数: %s（--help 看用法）\n' "$1"
            exit 1
            ;;
    esac
done

# ------------------------------------------------------------ 目录与 helper ---

REPO=""
RT_DIR=""
resolve_roots() {
    local cand
    for cand in "$REPO_OPT" "$(cd "${SCRIPT_DIR}/../.." 2>/dev/null && pwd || true)" \
                /opt/easyaiot /app /src "$PWD"; do
        [[ -n "$cand" && -d "$cand" ]] || continue
        if [[ -f "$cand/RUNTIME/scripts/rknn_sdk.sh" ]]; then
            REPO="$cand"
            RT_DIR="$cand/RUNTIME"
            return 0
        fi
    done
    # 只把 RUNTIME 拷进容器（没有仓库根）：脚本自己就在 RUNTIME/scripts 下
    if [[ -f "$SCRIPT_DIR/rknn_sdk.sh" ]]; then
        RT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
        REPO="$(cd "$RT_DIR/.." && pwd)"
        return 0
    fi
    for cand in /opt/easyaiot/RUNTIME /app/RUNTIME ./RUNTIME; do
        if [[ -f "$cand/scripts/rknn_sdk.sh" ]]; then
            RT_DIR="$(cd "$cand" && pwd)"
            REPO="$(cd "$RT_DIR/.." && pwd)"
            return 0
        fi
    done
    return 1
}

TMP_DIR=""
cleanup() {
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        rm -rf "$TMP_DIR"
    fi
}

if ! resolve_roots; then
    printf 'FAIL  找不到 RUNTIME 目录（试过热推导与 /opt/easyaiot、/app、/src）；用 --repo=PATH 指定\n'
    exit 1
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rk_media.XXXXXX" 2>/dev/null || true)"
if [[ -z "$TMP_DIR" ]]; then
    TMP_DIR="/tmp/rk_media.$$"
    mkdir -p "$TMP_DIR"
fi
trap cleanup EXIT

PY_SRC="$REPO/VIDEO/app/services/runtime_config_service.py"
CPP_SRC="$RT_DIR/src/InferEngine.cpp"
RKNN_HELPER="$RT_DIR/scripts/rknn_sdk.sh"
MPP_HELPER="$RT_DIR/scripts/mpp_sdk.sh"
NPU_HELPER=""

for cand in "$REPO/VIDEO/scripts/npu_drm_nodes.sh" "$RT_DIR/../VIDEO/scripts/npu_drm_nodes.sh"; do
    if [[ -f "$cand" ]]; then
        NPU_HELPER="$(cd "$(dirname "$cand")" && pwd)/$(basename "$cand")"
        break
    fi
done
if [[ -f "$RKNN_HELPER" ]]; then
    # shellcheck source=/dev/null
    . "$RKNN_HELPER"
fi
if [[ -f "$MPP_HELPER" ]]; then
    # shellcheck source=/dev/null
    . "$MPP_HELPER"
fi
if [[ -n "$NPU_HELPER" ]]; then
    # shellcheck source=/dev/null
    . "$NPU_HELPER"
fi

PY="$(command -v python3 || command -v python || true)"
PY_HELPER="$TMP_DIR/rk_media_py.py"

# 控制面那份模块 import 时会拉起 Flask / SQLAlchemy，验证脚本不能直接 import。
# 用 ast 只把探针相关的函数与常量抽出来 exec —— 判据仍然是仓库里那一份原文：
# 抄一份到本脚本里的话，原文漂移了也不会报警，就白测了。
_emit_py_helper() {
    cat <<'PY'
"""Extract the RK/RKNN probe helpers from the control-plane module by AST.

Importing runtime_config_service pulls Flask + SQLAlchemy in; we only want the
device-probe predicates, so the exact source text of those definitions is
compiled into a private namespace instead.
"""
import ast
import functools
import os
import platform
import re
import sys

repo = os.environ.get("RK_REPO", ".")
src = os.path.join(repo, "VIDEO", "app", "services", "runtime_config_service.py")
mode = sys.argv[1] if len(sys.argv) > 1 else "nodes"

WANTED = {
    "_NPU_DEVICE_NODES", "_NPU_SYSFS_RE", "_RKNNRT_LIB_CANDIDATES",
    "_HWACCEL_VALUES", "_HWACCEL_ALIASES",
    "npu_drm_card_nodes", "rknn_host_available", "mpp_host_available",
    "_env_flag", "resolve_hwaccel_backend", "resolve_hwaccel_decode",
    "resolve_hwaccel_encode", "resolve_force_soft_av", "rknn_export_requested",
}


def load():
    with open(src, encoding="utf-8") as fh:
        text = fh.read()
    ns = {"os": os, "re": re, "platform": platform, "lru_cache": functools.lru_cache}
    for node in ast.parse(text).body:
        names = set()
        if isinstance(node, ast.Assign):
            names = {t.id for t in node.targets if isinstance(t, ast.Name)}
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            names = {node.name}
        if not names & WANTED:
            continue
        seg = ast.get_source_segment(text, node)
        if seg:
            exec(compile(seg, src, "exec"), ns)
    return ns


if not os.path.isfile(src):
    print("__NO_SOURCE__")
    sys.exit(0)
try:
    NS = load()
except Exception as exc:  # noqa: BLE001 - 诊断脚本：任何失败都要变成可读结论
    print("__PY_FAIL__ %r" % (exc,))
    sys.exit(0)

try:
    if mode == "nodes":
        for n in NS["npu_drm_card_nodes"]():
            print(n)
    elif mode == "devlist":
        for n in NS["_NPU_DEVICE_NODES"]:
            print(n)
    elif mode == "bools":
        print("rknn_host_available=%s" % NS["rknn_host_available"]())
        print("rknn_export_requested=%s" % NS["rknn_export_requested"]())
        print("mpp_host_available=%s" % NS["mpp_host_available"]())
        print("hwaccel=%s" % NS["resolve_hwaccel_backend"]())
        print("hwaccel_decode=%s" % NS["resolve_hwaccel_decode"]())
        print("hwaccel_encode=%s" % NS["resolve_hwaccel_encode"]())
        print("force_soft_av=%s" % NS["resolve_force_soft_av"]())
    elif mode == "dlopen":
        import ctypes
        try:
            ctypes.CDLL(sys.argv[2])
            print("OK")
        except OSError as err:
            print("ERR %s" % err)
    elif mode == "rknn_init":
        import ctypes
        lib, model = sys.argv[2], sys.argv[3]
        try:
            rt = ctypes.CDLL(lib)
        except OSError as err:
            print("NO_LIB %s" % err)
            sys.exit(0)
        try:
            with open(model, "rb") as fh:
                buf = fh.read()
        except OSError as err:
            print("NO_MODEL %s" % err)
            sys.exit(0)
        ctx = ctypes.c_void_p()
        try:
            rc = rt.rknn_init(ctypes.byref(ctx), ctypes.c_char_p(buf), len(buf), 0, 0)
        except Exception as err:  # noqa: BLE001
            print("NO_SYM %s" % err)
            sys.exit(0)
        if rc == 0 and ctx.value:
            try:
                rt.rknn_destroy(ctx)
            except Exception:  # noqa: BLE001
                pass
        print("RET %d" % rc)
    else:
        print("__PY_FAIL__ unknown mode %s" % mode)
except Exception as exc:  # noqa: BLE001
    print("__PY_FAIL__ %r" % (exc,))
sys.exit(0)
PY
}

run_py() {
    if [[ -z "$PY" || ! -f "$PY_HELPER" ]]; then
        return 1
    fi
    RK_REPO="$REPO" "$PY" "$PY_HELPER" "$@" 2>&1 || return 1
}

# ini_get FILE SECTION KEY —— 只认 `key=value`，注释行跳过
ini_get() {
    awk -v sec="$2" -v key="$3" '
        /^[[:space:]]*[#;]/ { next }
        /^[[:space:]]*\[/ {
            cur = $0
            gsub(/[][[:space:]]/, "", cur)
            next
        }
        cur == sec {
            k = $0
            sub(/=.*/, "", k)
            gsub(/[[:space:]]/, "", k)
            if (k == key) {
                v = $0
                sub(/^[^=]*=/, "", v)
                gsub(/[[:space:]]/, "", v)
                print v
                exit
            }
        }
    ' "$1" 2>/dev/null || true
}

json_str() { printf '%s' "$1" | grep -o "\"$2\":[[:space:]]*\"[^\"]*\"" | head -1 | cut -d'"' -f4 || true; }
json_bool() { printf '%s' "$1" | grep -o "\"$2\":[[:space:]]*\(true\|false\)" | head -1 | grep -o 'true\|false' || true; }

# 与三份实现无关的第二判据：只看 driver 目录基名与 platform 节点名。
# 用途是抓 npu_drm_nodes.sh 自己漂移（漏判 card 主节点 = 容器里 rknn_init 必失败）。
oracle_card_nodes() {
    local entry drv dev idx name
    for entry in /sys/class/drm/card*; do
        [[ -e "$entry" ]] || continue
        drv="$(readlink -f "$entry/device/driver" 2>/dev/null || true)"
        dev="$(readlink -f "$entry" 2>/dev/null || true)"
        name="$(basename "${drv:-none}" 2>/dev/null || echo none)"
        case "$name" in
            *rknpu* | *rknn* | *npu*) : ;;
            *)
                case "$dev" in
                    *.npu* | */npu*) : ;;
                    *) continue ;;
                esac
                ;;
        esac
        idx="${entry##*/}"
        idx="${idx#card}"
        if [[ "$idx" =~ ^[0-9]+$ ]]; then
            if [[ -e "/dev/dri/card$idx" ]]; then
                printf '/dev/dri/card%s\n' "$idx"
            fi
        fi
    done | sort -u
    return 0
}

find_runtime_bin() {
    local c
    for c in "$RT_DIR/build/RUNTIME" "$RT_DIR/bin/RUNTIME" "$RT_DIR/RUNTIME" \
             /opt/easyaiot/RUNTIME/build/RUNTIME /opt/easyaiot/RUNTIME/bin/RUNTIME \
             /app/build/RUNTIME ./RUNTIME; do
        if [[ -f "$c" && -x "$c" ]]; then
            printf '%s\n' "$c"
            return 0
        fi
    done
    return 1
}

find_runtime_override() {
    local c
    for c in "$REPO/VIDEO/.docker-compose.runtime.override.yaml" \
             "$RT_DIR/../VIDEO/.docker-compose.runtime.override.yaml" \
             /app/.docker-compose.runtime.override.yaml \
             /opt/easyaiot/VIDEO/.docker-compose.runtime.override.yaml; do
        if [[ -f "$c" ]]; then
            printf '%s\n' "$c"
            return 0
        fi
    done
    return 1
}

find_rknn_model() {
    local m f
    for m in "$RT_DIR"/data/models/*/model.rknn "$REPO"/VIDEO/data/models/*/model.rknn \
             /opt/easyaiot/RUNTIME/data/models/*/model.rknn /app/data/models/*/model.rknn; do
        if [[ -s "$m" ]]; then
            printf '%s\n' "$m"
            return 0
        fi
    done
    for f in "$RT_DIR"/config/task_*.ini; do
        if [[ -f "$f" ]]; then
            m="$(ini_get "$f" ai model_path)"
            if [[ "$m" == *.rknn && -s "$m" ]]; then
                printf '%s\n' "$m"
                return 0
            fi
        fi
    done
    return 1
}

locate_rknnrt() {
    local lib=""
    if declare -F rknn_runtime_lib >/dev/null 2>&1; then
        lib="$(rknn_runtime_lib 2>/dev/null || true)"
    fi
    if [[ -z "$lib" && -f /opt/easyaiot/rknn-lib/librknnrt.so ]]; then
        lib=/opt/easyaiot/rknn-lib/librknnrt.so
    fi
    if [[ -z "$lib" ]]; then
        return 1
    fi
    printf '%s\n' "$lib"
}

# ============================================================ A. NPU 节点探测 ===

section_a() {
    hdr "A. NPU 节点探测（三处实现交叉校验）"

    if [[ -z "$NPU_HELPER" ]]; then
        fail "找不到 VIDEO/scripts/npu_drm_nodes.sh，A 段没有判据基准"
        return 0
    fi
    if ! declare -F npu_drm_card_nodes >/dev/null 2>&1; then
        fail "$NPU_HELPER 里没有 npu_drm_card_nodes()"
        return 0
    fi
    pass "shell 判据可 source：$NPU_HELPER"

    SHELL_NODES="$(npu_drm_card_nodes 2>/dev/null | tr '\n' ' ')"
    SHELL_NODES="${SHELL_NODES% }"
    ORACLE_NODES="$(oracle_card_nodes | tr '\n' ' ')"
    ORACLE_NODES="${ORACLE_NODES% }"
    note "npu_drm_nodes.sh -> ${SHELL_NODES:-（无）}"
    note "独立 oracle      -> ${ORACLE_NODES:-（无）}"
    if [[ -n "$PY" && -f "$PY_SRC" ]]; then
        PY_NODES="$(run_py nodes | grep '^/dev/dri/' | tr '\n' ' ' || true)"
        PY_NODES="${PY_NODES% }"
        note "Python 控制面判据 -> ${PY_NODES:-（无）}"
    else
        skip "Python 判据未比对（$(if [[ -z "$PY" ]]; then echo '无 python3'; else echo "无 $PY_SRC"; fi)）"
    fi

    # A1 三处判据同结论
    if [[ "$SHELL_NODES" != "$ORACLE_NODES" ]]; then
        fail "shell 判据与独立 oracle 不一致：'${SHELL_NODES}' vs '${ORACLE_NODES}'"
        note "-> npu_drm_nodes.sh 漏判或误判 card 主节点；compose 的 devices: 白名单会跟着错"
    else
        pass "shell 判据与独立 oracle 一致（${SHELL_NODES:-无 card 节点}）"
    fi
    if [[ -n "$PY" && -f "$PY_SRC" ]]; then
        if printf '%s' "$PY_NODES" | grep -q '__PY_FAIL__\|__NO_SOURCE__\|Traceback'; then
            fail "Python 判据抽取执行失败：${PY_NODES}"
        elif [[ "$SHELL_NODES" != "$PY_NODES" ]]; then
            fail "shell 与 Python 控制面判据不一致：'${SHELL_NODES}' vs '${PY_NODES}'"
        else
            pass "shell 与 Python 控制面判据一致"
        fi
    fi

    # A2 判据里不许出现共享加速器（本次线上故障的根因，按假阳性回归对待）
    local bad
    # shellcheck disable=SC2086
    bad="$(printf '%s\n' ${SHELL_NODES} ${PY_NODES} | grep -E 'rga|renderD' | tr '\n' ' ' || true)"
    if [[ -n "$bad" ]]; then
        fail "card 判据输出里出现非 NPU 节点：${bad}"
    else
        pass "card 判据输出不含 /dev/rga、renderD*"
    fi

    local py_list
    py_list="$(run_py devlist | grep '^/' || true)"
    if [[ -z "$py_list" ]]; then
        skip "Python _NPU_DEVICE_NODES 未比对"
    elif printf '%s\n' "$py_list" | grep -Eq 'rga|renderD'; then
        # shellcheck disable=SC2086
        fail "Python _NPU_DEVICE_NODES 混进 RGA/renderD：$(printf '%s ' $py_list)"
    else
        pass "Python _NPU_DEVICE_NODES 只含 NPU 专属节点"
    fi

    if [[ -f "$CPP_SRC" ]]; then
        local cpp_list
        cpp_list="$(awk '
            /kNpuDevices\[\][[:space:]]*=[[:space:]]*\{/ { inblk = 1 }
            inblk {
                n = split($0, a, "\"")
                for (i = 2; i <= n; i += 2) if (a[i] ~ /^\//) print a[i]
                if ($0 ~ /\}/) inblk = 0
            }' "$CPP_SRC")"
        if [[ -z "$cpp_list" ]]; then
            skip "C++ kNpuDevices[] 没解析出来（数组改名/挪走了，改本脚本锚点）"
        elif printf '%s\n' "$cpp_list" | grep -Eq 'rga|renderD'; then
            # shellcheck disable=SC2086
            fail "C++ kNpuDevices[] 混进 RGA/renderD：$(printf '%s ' $cpp_list)"
        else
            # shellcheck disable=SC2086
            pass "C++ kNpuDevices[] 只含 NPU 专属节点：$(printf '%s ' $cpp_list)"
        fi
        if grep -q 'npuDrmCardNodes' "$CPP_SRC"; then
            pass "C++ hostHasRknn() 走 RKNPU card 主节点判据（rknpu v2 没有 /dev/rknpu）"
        else
            fail "$CPP_SRC 里没有 npuDrmCardNodes() —— RK3588 上 hostHasRknn() 会误判成无 NPU"
        fi
    else
        skip "C++ 判据未比对（找不到 $CPP_SRC，纯部署形态没有源码）"
    fi

    # A3 本机到底有没有 NPU 证据
    local node
    for node in /dev/rknpu /dev/rknpu_ll; do
        if [[ -e "$node" ]]; then
            NPU_EVIDENCE=1
            note "存在 $node"
        fi
    done
    for node in /sys/class/devfreq/*.npu; do
        if [[ -e "$node" ]]; then
            NPU_EVIDENCE=1
            note "存在 ${node}"
        fi
    done
    if [[ -n "$SHELL_NODES" ]]; then
        NPU_EVIDENCE=1
    fi
    if [[ "$NPU_EVIDENCE" -eq 1 ]]; then
        pass "本机有 NPU 证据（card 节点：${SHELL_NODES:-无}）"
    else
        note "本机没有任何 NPU 证据（x86 机器、或容器没透传 card 主节点时属正常）"
    fi

    # A4 控制面结论与证据必须自洽
    local pybool
    pybool="$(run_py bools | grep -v '^__' || true)"
    if [[ -z "$pybool" ]]; then
        skip "未比对控制面结论（无 python3 或无 $PY_SRC）"
    else
        local arch avail
        arch="$(uname -m 2>/dev/null || echo unknown)"
        avail="$(printf '%s\n' "$pybool" | grep -m1 '^rknn_host_available=' | cut -d= -f2 || true)"
        note "控制面结论（VIDEO 下发 ini 时用的就是这几个值）："
        printf '%s\n' "$pybool" | indent
        if [[ "$avail" == "True" && "$NPU_EVIDENCE" -eq 0 ]]; then
            fail "rknn_host_available=True 但本机没有任何 NPU 证据 —— 假阳性，控制面会只下发 .rknn"
        elif [[ "$avail" == "True" ]]; then
            pass "rknn_host_available=True 且确有 NPU 证据"
        elif [[ "$NPU_EVIDENCE" -eq 1 && "$arch" == "aarch64" ]]; then
            if [[ -n "$(locate_rknnrt || true)" ]]; then
                fail "有 NPU 证据且 librknnrt 定位得到，但 rknn_host_available=False —— 判据漏了 card 主节点？"
            else
                note "有 NPU 证据但没有 librknnrt.so：跑 install_rk3588.sh sdk-setup"
            fi
        else
            note "rknn_host_available=False（arch=${arch}）"
        fi
    fi

    # A5 运行中的 RUNTIME 实际打开了哪个 DRM 节点
    local rtpid fds
    rtpid="$(pgrep -f '[/ ]RUNTIME( |$)' 2>/dev/null | head -1 || true)"
    if [[ -n "$rtpid" && -d "/proc/$rtpid/fd" ]]; then
        fds="$(ls -l "/proc/$rtpid/fd" 2>/dev/null | grep -o '/dev/dri/[a-zA-Z0-9]*' | sort -u | tr '\n' ' ' || true)"
        note "RUNTIME pid=${rtpid} 打开的 DRM 节点：${fds:-（无 / 权限不足）}"
        if printf '%s' "$fds" | grep -q 'card'; then
            pass "运行期证据：librknnrt 确实打开了 card 主节点（$(printf '%s' "$fds" | grep -o '/dev/dri/card[0-9]*' | tr '\n' ' ')）"
        fi
    else
        note "没有运行中的 RUNTIME 进程（或读不了 /proc/*/fd），跳过 fd 取证"
    fi

    # A6 终极证据：用探测到的节点真跑一次 rknn_init
    local rklib model out
    rklib="$(locate_rknnrt || true)"
    model="$(find_rknn_model || true)"
    if [[ -z "$PY" ]]; then
        skip "rknn_init 实跑（无 python3）"
    elif [[ -z "$rklib" ]]; then
        note "librknnrt.so 未定位，跳过 rknn_init 实跑"
    elif [[ -z "$model" ]]; then
        note "没有现成 .rknn 模型，跳过 rknn_init 实跑（有模型时这一项才是硬证据）"
    else
        out="$(run_py rknn_init "$rklib" "$model" | tr '\n' ' ' || true)"
        if printf '%s' "$out" | grep -q 'failed to open rknn device\|failed to open rknpu'; then
            fail "rknn_init 打不开 NPU 设备（模型 $(basename "$model")）—— 十有八九是 card 主节点没透传"
            printf '%s\n' "$out" | cut -c1-200 | indent
        elif printf '%s' "$out" | grep -q 'RET 0'; then
            pass "rknn_init 成功（$(basename "$model")）—— 探测到的节点真能用"
        elif printf '%s' "$out" | grep -q 'RET'; then
            fail "rknn_init 返回 $(printf '%s' "$out" | grep -oE 'RET +-?[0-9]+' || true)（$rklib / $model）"
        else
            skip "rknn_init 未定论：${out}"
        fi
    fi

    a7_whitelist
}

# A7 探测结果有没有真的落到 compose 的 devices: 白名单里
# ensure_runtime_cpp.sh 用同一份 npu_drm_nodes.sh 生成这份 override；本次线上故障
# 的最终表现就是「宿主探到 card1，override 里只有 renderD*」。
a7_whitelist() {
    local ovr n missing
    ovr="$(find_runtime_override || true)"
    if [[ -z "$ovr" ]]; then
        note "没有 .docker-compose.runtime.override.yaml（还没跑过 ensure_runtime_cpp.sh），跳过白名单取证"
        return 0
    fi
    note "override：$ovr"
    missing=""
    # shellcheck disable=SC2086
    for n in ${SHELL_NODES}; do
        if ! grep -qF "${n}:${n}" "$ovr"; then
            missing="${missing} ${n}"
        fi
    done
    if [[ -n "$missing" ]]; then
        fail "${missing# } 在宿主探测结果里，但 override 的 devices: 没透传 —— 容器内 rknn_init 必报 failed to open rknn device"
        note "-> 重跑 bash VIDEO/scripts/ensure_runtime_cpp.sh（或 install.sh）后 docker compose up -d"
    elif [[ -n "$SHELL_NODES" ]]; then
        pass "RKNPU card 主节点已进 devices: 白名单（$SHELL_NODES）"
    elif grep -qE '/dev/dri/card[0-9]+:' "$ovr"; then
        note "override 里写了 card 节点，但本机现在探不到（换过机器/内核？重新生成一次更稳妥）"
    fi
    return 0
}

# ============================================================ B. 编解码链路 =====

section_b() {
    hdr "B. rkmpp 编解码链路"

    if ! declare -F mpp_runtime_lib >/dev/null 2>&1; then
        fail "找不到 $MPP_HELPER，B 段判据缺失"
        return 0
    fi

    # VPU 在不在，先定下来：B1「定位不到库」到底是 FAIL 还是正常，全看这个。
    # 否则在 x86 开发机上跑这份脚本永远红，A/B 段的回归价值就没了。
    if { declare -F mpp_device_ready >/dev/null 2>&1 && mpp_device_ready; } \
        || [[ -e /dev/mpp_service || -e /dev/vpu_service ]]; then
        MPP_DEVICE=1
    fi
    local arch
    arch="$(uname -m 2>/dev/null || echo unknown)"

    # B1 库与传递依赖
    MPP_LIB="$(mpp_runtime_lib 2>/dev/null || true)"
    if [[ -z "$MPP_LIB" ]]; then
        if [[ "$MPP_DEVICE" -eq 1 ]]; then
            fail "有 VPU 设备但定位不到 librockchip_mpp：跑 install_rk3588.sh mpp-setup 把宿主那份 patch 后暂存"
        elif [[ "$arch" == "aarch64" || "$arch" == "arm64" ]]; then
            fail "RK 板子上定位不到 librockchip_mpp（$arch）：librockchip_mpp 是链接期依赖，缺它 RUNTIME 直接起不来"
        else
            note "本机无 librockchip_mpp（arch=${arch}，非 RK 平台属正常；RK 板子上这一项是硬失败）"
        fi
    else
        MPP_SONAME="$(mpp_soname "$MPP_LIB" 2>/dev/null || true)"
        local ver
        ver="$(mpp_version "$MPP_LIB" 2>/dev/null || true)"
        pass "librockchip_mpp -> $MPP_LIB（SONAME=${MPP_SONAME:-?}，版本 ${ver:-unknown}）"
        note "DT_NEEDED: $(mpp_lib_needed "$MPP_LIB" 2>/dev/null | tr '\n' ' ')"
        local staged sys
        staged="$(mpp_staged_lib 2>/dev/null || true)"
        sys="$(mpp_system_runtime_lib 2>/dev/null || true)"
        note "暂存那份=${staged:-无}  系统那份=${sys:-无}"
        if [[ -z "$staged" && -n "$sys" ]]; then
            note "只有宿主系统那份可用：容器（glibc 2.28）链不动它，见 RUNTIME/tools/mpp_glibc_patch.c"
        fi
    fi

    # B2 GLIBC 版本节点
    if [[ -n "$MPP_LIB" ]]; then
        local vers
        vers="$(mpp_lib_versions "$MPP_LIB" 2>/dev/null || true)"
        if [[ -z "$vers" ]]; then
            skip "版本节点没取到（缺 gcc/cc，也没预置 MPP_PATCH_TOOL）"
        elif printf '%s' "$vers" | grep -q "version=${MPP_GLIBIC_FROM:-GLIBC_2.29}"; then
            fail "$MPP_LIB 仍要求 ${MPP_GLIBIC_FROM:-GLIBC_2.29} —— 容器里 dlopen 必失败：跑 mpp-setup 或 mpp_patch_glibc"
        elif printf '%s' "$vers" | grep -q 'SELFTEST FAILED'; then
            fail "elf_hash 自检失败：$MPP_LIB 的 verneed 表被改坏了（拿原始库重跑 mpp_patch_glibc）"
        else
            pass "$MPP_LIB 不再要求 ${MPP_GLIBIC_FROM:-GLIBC_2.29}"
        fi
    fi

    # B3 dlopen 实证
    if [[ -n "$PY" && -f "$PY_HELPER" ]]; then
        if [[ -n "$MPP_LIB" ]]; then
            MPP_DLOPEN="$(LD_LIBRARY_PATH="$(dirname "$MPP_LIB"):${LD_LIBRARY_PATH:-}" \
                run_py dlopen "$(basename "$MPP_LIB")" | tail -1 || true)"
            if [[ "$MPP_DLOPEN" == "OK" ]]; then
                pass "dlopen $(basename "$MPP_LIB") 成功"
            elif [[ "$MPP_DLOPEN" == ERR* ]]; then
                fail "dlopen $(basename "$MPP_LIB") 失败：${MPP_DLOPEN}"
                note "-> 核对 DT_NEEDED 里的 libdrm.so.2 等传递依赖，以及 .mpp-sdk/lib 在不在 LD_LIBRARY_PATH"
            else
                skip "dlopen $(basename "$MPP_LIB") 未定论（探针无输出）"
            fi
        fi
        local rklib
        rklib="$(locate_rknnrt || true)"
        if [[ -n "$rklib" ]]; then
            RKNNRT_DLOPEN="$(LD_LIBRARY_PATH="$(dirname "$rklib"):${LD_LIBRARY_PATH:-}" \
                run_py dlopen "$(basename "$rklib")" | tail -1 || true)"
            if [[ "$RKNNRT_DLOPEN" == "OK" ]]; then
                pass "dlopen librknnrt.so 成功（$rklib）"
            elif [[ "$RKNNRT_DLOPEN" == ERR* ]]; then
                fail "dlopen librknnrt.so 失败：${RKNNRT_DLOPEN} —— RUNTIME 会静默回落 ONNX Runtime"
            else
                skip "dlopen librknnrt.so 未定论（探针无输出）"
            fi
        fi
    else
        skip "dlopen 实证（无 python3）"
    fi

    # B4 设备节点 rw / memlock
    if declare -F npu_device_nodes >/dev/null 2>&1; then
        local dev n_bad=""
        # shellcheck disable=SC2046
        for dev in $(npu_device_nodes); do
            if [[ -r "$dev" && -w "$dev" ]]; then
                DEVICE_RW=$((DEVICE_RW + 1))
            else
                n_bad="${n_bad} ${dev}"
            fi
        done
        note "可读写设备节点 ${DEVICE_RW} 个${n_bad:+，不可读写:${n_bad}}"
    fi
    if declare -F mpp_device_ready >/dev/null 2>&1 && mpp_device_ready; then
        MPP_DEVICE=1
        pass "VPU 设备就绪（/dev/mpp_service 或 /dev/vpu_service）"
    elif [[ "$INSIDE_CONTAINER" -eq 1 ]]; then
        fail "容器里没有 /dev/mpp_service —— compose 没透传，硬解只能回落 CPU"
    elif [[ "$arch" == "aarch64" || "$arch" == "arm64" ]]; then
        fail "RK 板子上没有 /dev/mpp_service / /dev/vpu_service —— 内核没开 RGA/MPP 或 dtb 缺节点，整条硬解链路不可用"
    else
        note "没有 /dev/mpp_service（非 RK 平台属正常；RK 板子上这就是硬解开关）"
    fi
    if [[ -e /dev/dma_heap/system || -d /dev/dma_heap ]]; then
        pass "存在 /dev/dma_heap/*（MppEnv buffer group 的第二选择）"
    else
        note "无 /dev/dma_heap/*：buffer group 只能走 DRM 或 NORMAL（后者多一次 memcpy）"
    fi
    # B4.5 编解码设备是否真的进了容器 devices: 白名单（A7 的编解码 counterpart）
    if [[ "$INSIDE_CONTAINER" -eq 0 ]]; then
        local ovr2="" want="" have="" w d
        ovr2="$(find_runtime_override || true)"
        if [[ -z "$ovr2" ]]; then
            note "无 override 文件，跳过编解码设备的透传取证"
        else
            if [[ -e /dev/mpp_service ]]; then
                want="${want} /dev/mpp_service"
            fi
            if [[ -e /dev/vpu_service ]]; then
                want="${want} /dev/vpu_service"
            fi
            for d in /dev/dma_heap/*; do
                if [[ -e "$d" ]]; then
                    want="${want} ${d}"
                fi
            done
            # shellcheck disable=SC2086
            for w in ${want}; do
                if ! grep -qF "${w}:${w}" "$ovr2"; then
                    have="${have} ${w}"
                fi
            done
            if [[ -n "$have" ]]; then
                fail "宿主有 ${have# } 但 override 的 devices: 没透传 —— 容器内 MppEnv 探测失败，硬编硬解整体回落 CPU"
                note "-> 重跑 bash VIDEO/scripts/ensure_runtime_cpp.sh 后 docker compose up -d"
            elif [[ -n "$want" ]]; then
                pass "编解码所需设备都在 devices: 白名单里（${want# }）"
            else
                note "宿主没有 /dev/mpp_service、/dev/dma_heap/*，无透传需求"
            fi
        fi
    fi
    local meml
    meml="$(ulimit -l 2>/dev/null || echo unknown)"
    note "ulimit -l (memlock) = $meml"
    if [[ "$meml" == "unlimited" ]]; then
        pass "memlock 不受限（DRM/DMABUF 缓冲不会 pin 失败）"
    else
        note "memlock=$meml KB：DRM buffer group 申请大帧缓冲时可能 EPERM"
    fi

    # B5 RUNTIME 二进制的链接期证据
    local bin
    bin="$(find_runtime_bin || true)"
    if [[ -z "$bin" ]]; then
        skip "找不到 RUNTIME 二进制（还没编译：bash RUNTIME/scripts/build_linux.sh）"
    else
        note "RUNTIME 二进制：$bin"
        "$bin" --version 2>&1 | head -2 | indent || true
        local needed
        # 复用 mpp_sdk.sh 那份解析（它锁了 C locale 并只取方括号内的值）：
        # readelf 的 "Shared library:" 标签在 zh_CN 下会与值粘成一个字段，按 $NF 取会连着译文一起拿
        if declare -F mpp_lib_needed >/dev/null 2>&1; then
            needed="$(mpp_lib_needed "$bin" 2>/dev/null | tr '\n' ' ' || true)"
        elif command -v readelf >/dev/null 2>&1; then
            needed="$(LC_ALL=C readelf -d "$bin" 2>/dev/null | awk -F'[][]' '/\(NEEDED\)/ {print $2}' | tr '\n' ' ' || true)"
        elif command -v objdump >/dev/null 2>&1; then
            needed="$(LC_ALL=C objdump -p "$bin" 2>/dev/null | awk '/^[[:space:]]*NEEDED[[:space:]]/ {print $2}' | tr '\n' ' ' || true)"
        fi
        BIN_NEEDED="${needed// /,}"
        if [[ -z "$needed" ]]; then
            skip "无 readelf/objdump，跳过 DT_NEEDED 检查"
        elif printf '%s' "$needed" | grep -q 'rockchip_mpp'; then
            pass "RUNTIME 已链接 librockchip_mpp（rkmpp 后端确实编进了二进制）"
        elif [[ "$MPP_DEVICE" -eq 1 ]]; then
            fail "有 VPU 设备但 RUNTIME 未链接 librockchip_mpp —— 编译期缺 RUNTIME_WITH_MPP/SDK，编解码全走 CPU"
        else
            note "RUNTIME 未链接 librockchip_mpp（本机无 VPU 时属正常）"
        fi
        if command -v ldd >/dev/null 2>&1; then
            local ldd_out miss
            # LC_ALL=C：ldd 的 "not found" 在 zh_CN 下会译成中文，grep 直接失效
            ldd_out="$(LC_ALL=C ldd "$bin" 2>&1 || true)"
            if ! printf '%s' "$ldd_out" | grep -qE '\.so|statically linked|not a dynamic'; then
                skip "ldd 读不出 $bin 的依赖（架构不匹配 / 非 ELF），跳过动态库解析检查"
            else
                miss="$(printf '%s\n' "$ldd_out" | grep 'not found' | awk '{print $1}' | tr '\n' ' ' || true)"
                if [[ -n "$miss" ]]; then
                    fail "ldd 未解析：${miss} —— 直接链接的二进制没有回落路径，RUNTIME 起不来"
                else
                    pass "ldd 全部解析（$(printf '%s\n' "$ldd_out" | grep -c '=>') 个依赖）"
                fi
            fi
        fi
    fi

    # B6 task ini 的编解码决策
    local f
    for f in "$RT_DIR"/config/task_*.ini; do
        if [[ -f "$f" ]]; then
            b6_ini "$f"
        fi
    done
    note "当前 shell 的覆盖变量：RUNTIME_HWACCEL=${RUNTIME_HWACCEL:-} RUNTIME_HWACCEL_DECODE=${RUNTIME_HWACCEL_DECODE:-} RUNTIME_HWACCEL_ENCODE=${RUNTIME_HWACCEL_ENCODE:-} RUNTIME_FORCE_SOFT_AV=${RUNTIME_FORCE_SOFT_AV:-} RUNTIME_INFER_BACKEND=${RUNTIME_INFER_BACKEND:-}"

    # B7 真编一帧（VEPU 出流）
    b7_encode_probe

    # B8 /health 运行期上报
    for f in "$RT_DIR"/config/task_*.ini; do
        if [[ -f "$f" ]]; then
            b8_health "$f"
        fi
    done
}

b6_ini() {
    local f="$1" base hw dec enc soft pfg infer
    base="$(basename "$f")"
    hw="$(ini_get "$f" ai hwaccel)"
    dec="$(ini_get "$f" ai hwaccel_decode)"
    enc="$(ini_get "$f" ai hwaccel_encode)"
    soft="$(ini_get "$f" ai force_soft_av)"
    pfg="$(ini_get "$f" ai prefer_gpu)"
    infer="$(ini_get "$f" ai infer_backend)"
    if [[ -z "$hw" ]]; then
        fail "$base 的 [ai] 缺 hwaccel（旧配置残留？让 VIDEO 重新下发，或显式写 auto）"
        return 0
    fi
    if [[ "$hw" != "auto" && "$hw" != "cuda" && "$hw" != "rkmpp" && "$hw" != "none" ]]; then
        fail "$base hwaccel=$hw 非法（RUNTIME 只认 auto|cuda|rkmpp|none）"
        return 0
    fi
    pass "$base: hwaccel=$hw decode=${dec:-?} encode=${enc:-?} force_soft_av=${soft:-?} infer_backend=${infer:-auto}"
    # prefer_gpu / force_cpu 只管 ONNX Runtime 的设备；把它们和编解码重新耦合就是本次修的 bug
    if [[ "$hw" == "none" && "$pfg" == "false" && "$MPP_DEVICE" -eq 1 ]]; then
        fail "$base 有 VPU 却写死 hwaccel=none、且 prefer_gpu=false —— 编解码后端又被推理设备联动掉了"
    fi
    if [[ "$soft" == "true" && "$pfg" == "false" && "$MPP_DEVICE" -eq 1 ]]; then
        fail "$base force_soft_av=true 且 prefer_gpu=false —— 同一个耦合症状的另一种写法"
    fi
    if [[ "$hw" == "rkmpp" && -z "$dec" ]]; then
        fail "$base hwaccel=rkmpp 但没有 hwaccel_decode 这一行（配置面应显式写出，别依赖默认值）"
    fi
    # 控制面写 ini 时是 hwaccel_decode = (not force_soft_av) and resolve_hwaccel_decode()，
    # 两份同时为 true 说明这份 ini 不是当前控制面生成的（手改/旧版本残留），行为不可预期。
    if [[ "$soft" == "true" && "$dec" == "true" ]]; then
        fail "$base force_soft_av=true 但 hwaccel_decode=true —— 与 $(basename "$PY_SRC") 的与运算不一致，重新下发 ini"
    fi
    if [[ "$hw" == "rkmpp" && "$MPP_DEVICE" -eq 0 ]]; then
        note "$base 写死 hwaccel=rkmpp 而本机没有 VPU 设备：MppEnv 探测失败会回落软解（不算错，但是白配）"
    fi
}

b7_encode_probe() {
    local src="$RT_DIR/tools/mpp_enc_probe.c"
    if [[ "$NO_PROBE" -eq 1 ]]; then
        skip "跳过 mpp_enc_probe（--no-probe）"
        return 0
    fi
    if [[ ! -f "$src" ]]; then
        skip "没有 $src，跳过实机编码探针"
        return 0
    fi
    if ! command -v gcc >/dev/null 2>&1; then
        skip "无 gcc，跳过实机编码探针（离线盒子常见，静态证据已能定位到链接期）"
        return 0
    fi
    if declare -F mpp_header_found >/dev/null 2>&1 && ! mpp_header_found; then
        skip "无 MPP 头文件（rk_mpi.h），跳过编码探针：export MPP_SDK_ROOT=/path 后重试"
        return 0
    fi
    local inc="" sdk="" out="$TMP_DIR/mpp_enc_probe"
    if [[ -f /usr/include/rockchip/rk_mpi.h ]]; then
        inc="-I/usr/include"
    elif declare -F mpp_sdk_probe >/dev/null 2>&1; then
        sdk="$(mpp_sdk_probe 2>/dev/null || true)"
        if [[ -n "$sdk" ]]; then
            if [[ -f "$sdk/include/rockchip/rk_mpi.h" ]]; then
                inc="-I$sdk/include"
            elif [[ -f "$sdk/rockchip/rk_mpi.h" ]]; then
                inc="-I$sdk"
            fi
        fi
    fi
    local link=""
    if [[ -n "$MPP_LIB" ]]; then
        # 板上只有 .so.0/.so.1，没有 dev 符号链接，所以 -l: 直接按文件名链
        link="-L$(dirname "$MPP_LIB") -l:$(basename "$MPP_LIB")"
    else
        link="-lrockchip_mpp"
    fi
    # 两个 -Werror 是必需的：MPP 的 buffer API 多是宏包装，参数错位只会 warning，
    # 然后把野指针交给 VEPU（详见 mpp_enc_probe.c 文件头）。
    # shellcheck disable=SC2086
    if ! gcc -O2 $inc "$src" -o "$out" $link \
        -Werror=int-conversion -Werror=implicit-function-declaration >"$TMP_DIR/gcc.log" 2>&1; then
        fail "mpp_enc_probe 编译失败：$(tail -3 "$TMP_DIR/gcc.log" | tr '\n' ' ')"
        return 0
    fi
    if ! "$out" >"$TMP_DIR/stage1.log" 2>&1; then
        fail "探针 stage 1（mpp_create/mpp_init）失败：$(tail -3 "$TMP_DIR/stage1.log" | tr '\n' ' ')"
        return 0
    fi
    pass "探针 stage 1 通过（MPP 能给这颗 SoC 的编码器上下文）"
    grep -iE 'coding|buffer group|buf group|DRM|DMA_HEAP|NORMAL' "$TMP_DIR/stage1.log" | head -6 | indent || true

    local bs="$TMP_DIR/out.264"
    if ! "$out" encode "$bs" >"$TMP_DIR/stage2.log" 2>&1; then
        fail "探针 stage 2（真出一帧）失败：$(tail -5 "$TMP_DIR/stage2.log" | tr '\n' ' ')"
        note "-> stage 2 会先报 buffer group 走 DRM / DMA_HEAP 还是 NORMAL，那决定容器要挂什么节点"
        tail -12 "$TMP_DIR/stage2.log" | indent || true
        return 0
    fi
    grep -iE 'buffer group|alloc|sps|pps|frame|OK|FAIL' "$TMP_DIR/stage2.log" | tail -8 | indent || true
    b7_stream_sanity "$bs"
}

b7_stream_sanity() {
    local f="$1" hex b nal
    if [[ ! -s "$f" ]]; then
        fail "stage 2 rc=0 但没有码流文件（$f）—— rc=0 从来不算证据，见 mpp_enc_probe.c 注释"
        return 0
    fi
    hex="$(LC_ALL=C head -c 8 "$f" | od -An -tx1 | tr -d ' \n')"
    if [[ "$hex" == 00000001* ]]; then
        b="${hex:8:2}"
    elif [[ "$hex" == 000001* ]]; then
        b="${hex:6:2}"
    else
        fail "码流开头不是 H.264 起始码：$hex"
        return 0
    fi
    nal=$((16#$b & 0x1F))
    case "$nal" in
        7 | 8 | 5 | 1)
            pass "VEPU 真出流：$(wc -c <"$f" | tr -d ' ') 字节，首个 NAL 类型 $nal（7=SPS 8=PPS 5=IDR）"
            ;;
        *)
            fail "首个 NAL 类型 $nal，不是 H.264 编码输出"
            ;;
    esac
}

b8_health() {
    local f="$1" port body dec enc hw ep
    port="$(ini_get "$f" task control_port)"
    if [[ -z "$port" ]]; then
        note "$(basename "$f") 没有 [task] control_port，跳过 /health 取证"
        return 0
    fi
    if ! command -v curl >/dev/null 2>&1; then
        skip "无 curl，跳过 /health 取证"
        return 0
    fi
    body="$(curl -fsS --max-time 3 "http://127.0.0.1:${port}/health" 2>/dev/null || true)"
    if [[ -z "$body" ]]; then
        note "端口 $port 上没有 /health 响应（任务未启动，或端口越界被回退成 8000）"
        return 0
    fi
    dec="$(json_str "$body" decode_ep)"
    enc="$(json_str "$body" encode_ep)"
    hw="$(json_str "$body" hwaccel)"
    ep="$(json_str "$body" infer_ep)"
    HEALTH_DECODE="$dec"
    HEALTH_ENCODE="$enc"
    note "/health(:$port) infer_ep=$ep decode_ep=$dec encode_ep=$enc hwaccel=$hw force_soft_av=$(json_bool "$body" force_soft_av) prefer_gpu=$(json_bool "$body" prefer_gpu)"
    case "$dec" in
        cuda | rkmpp | cpu | none) : ;;
        *) fail "decode_ep='$dec' 不在已知取值里（Detech.cpp 的上报字段漂移了）" ;;
    esac
    case "$enc" in
        h264_nvenc | h264_rkmpp | libx264 | copy | none) : ;;
        *) fail "encode_ep='$enc' 不在已知取值里" ;;
    esac
    if [[ "$hw" == "rkmpp" ]]; then
        if [[ "$dec" == "rkmpp" ]]; then
            pass "运行期硬解生效（decode_ep=rkmpp）"
        elif [[ "$dec" == "cpu" ]]; then
            fail "要的是 rkmpp 但 decode_ep=cpu —— 后端探测失败回落软解，对照 B2/B3 与启动日志 [MPP]/[HWACCEL]"
        fi
        if [[ "$enc" == "h264_rkmpp" ]]; then
            pass "运行期硬编生效（encode_ep=h264_rkmpp）"
        elif [[ "$enc" == "libx264" ]]; then
            fail "要的是 rkmpp 但 encode_ep=libx264（1080p25 软编约吃 1.5 个 A76），看 RTMPEncoder 初始化日志"
        fi
    fi
    if [[ "$NPU_EVIDENCE" -eq 1 && "$ep" == "cpu" ]]; then
        fail "有 NPU 证据但 infer_ep=cpu —— 权重或 librknnrt 没到位（对照 A 段结论与 [RKNN] LoadModel 日志）"
    fi
}

# ======================================================= C. 容器内外一致性 ======

section_c() {
    hdr "C. 容器内外一致性"
    if [[ -z "$CONTAINER" ]]; then
        note "未指定 --container=<name>，跳过（想比对容器内外就加 --container=video-service）"
        return 0
    fi
    if ! command -v docker >/dev/null 2>&1; then
        skip "docker 不可用"
        return 0
    fi
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "$CONTAINER"; then
        skip "容器 $CONTAINER 没在运行"
        return 0
    fi
    local dst="/tmp/rk_verify"
    local self="$SCRIPT_DIR/$(basename "$_SELF")"
    # 容器里通常只有 RUNTIME 的部署产物，没有仓库根。把判据 helper 按仓库布局一起拷进去，
    # 容器内那份才能用与宿主完全相同的原文（同一套判据）跑，而不是各说各话。
    docker exec "$CONTAINER" mkdir -p "$dst/RUNTIME/scripts" >/dev/null 2>&1 || true
    if ! docker exec "$CONTAINER" mkdir -p "$dst/VIDEO/scripts" >/dev/null 2>&1; then
        fail "容器 $CONTAINER 里建不了 $dst"
        return 0
    fi
    local pair
    for pair in "$self:$dst/RUNTIME/scripts/$(basename "$self")" \
        "$RKNN_HELPER:$dst/RUNTIME/scripts/rknn_sdk.sh" \
        "$MPP_HELPER:$dst/RUNTIME/scripts/mpp_sdk.sh"; do
        if [[ ! -f "${pair%%:*}" ]]; then
            skip "宿主缺 ${pair%%:*}，不拷入容器"
            continue
        fi
        if ! docker cp "${pair%%:*}" "$CONTAINER:${pair#*:}" >/dev/null 2>&1; then
            fail "docker cp ${pair%%:*} -> $CONTAINER:${pair#*:} 失败"
            return 0
        fi
    done
    if [[ -n "$NPU_HELPER" && -f "$NPU_HELPER" ]]; then
        docker cp "$NPU_HELPER" "$CONTAINER:$dst/VIDEO/scripts/npu_drm_nodes.sh" >/dev/null 2>&1 || true
    fi
    if ! docker exec "$CONTAINER" bash "$dst/RUNTIME/scripts/$(basename "$self")" \
        --inside-container --repo="$dst" --no-probe \
        --container-report="$CONTAINER_REPORT" >"$TMP_DIR/container.log" 2>&1; then
        note "容器内自检有 FAIL（容器内完整输出）："
        grep -E '^(FAIL|SKIP)' "$TMP_DIR/container.log" | indent || true
        local cres
        cres="$(docker exec "$CONTAINER" grep -m1 '^result=' "$CONTAINER_REPORT" 2>/dev/null | cut -d= -f2- || true)"
        note "容器内小结：${cres:-（读不到 $CONTAINER_REPORT）}"
    else
        note "容器内输出（末 20 行）："
        tail -20 "$TMP_DIR/container.log" | indent || true
    fi
    if ! docker exec "$CONTAINER" cat "$CONTAINER_REPORT" >"$TMP_DIR/container.env" 2>/dev/null; then
        fail "取不到容器内的报告 $CONTAINER_REPORT"
        return 0
    fi
    c_compare
    return 0
}

c_compare() {
    local key hostv contv
    write_report "$TMP_DIR/host.env"
    for key in npu_card_nodes mpp_lib mpp_soname mpp_dlopen rknnrt_dlopen mpp_device \
        device_rw health_decode health_encode; do
        hostv="$(grep -m1 "^${key}=" "$TMP_DIR/host.env" | cut -d= -f2- || true)"
        contv="$(grep -m1 "^${key}=" "$TMP_DIR/container.env" | cut -d= -f2- || true)"
        printf '        %-16s host=[%s] container=[%s]\n' "$key" "$hostv" "$contv"
    done

    contv="$(grep -m1 '^npu_card_nodes=' "$TMP_DIR/container.env" | cut -d= -f2- || true)"
    if [[ -n "$SHELL_NODES" && -z "$contv" ]]; then
        fail "宿主有 RKNPU card 主节点（$SHELL_NODES），容器里没有 —— devices: 白名单没精确到 major:minor，容器内 rknn_init 必失败"
    elif [[ -n "$SHELL_NODES" && "$SHELL_NODES" != "$contv" ]]; then
        fail "容器内外 card 节点集合不同：'$SHELL_NODES' vs '$contv'"
    elif [[ -n "$SHELL_NODES" ]]; then
        pass "容器内外 RKNPU card 节点一致（$SHELL_NODES）"
    fi

    if [[ "$MPP_DLOPEN" == "OK" ]]; then
        local cd
        cd="$(grep -m1 '^mpp_dlopen=' "$TMP_DIR/container.env" | cut -d= -f2- || true)"
        if printf '%s' "$cd" | grep -q '^ERR'; then
            fail "宿主 dlopen librockchip_mpp 成功、容器里失败（${cd}）—— 挂进去的多半是板上那份 GLIBC_2.29 的库，不是 .mpp-sdk/lib 里 patch 过的"
        elif [[ -n "$cd" ]]; then
            pass "容器内 dlopen librockchip_mpp 成功"
        fi
    fi

    local list
    list="$(docker exec "$CONTAINER" cat /sys/fs/cgroup/devices/devices.list 2>/dev/null \
        || docker exec "$CONTAINER" cat /sys/fs/cgroup/devices.list 2>/dev/null || true)"
    if [[ -z "$list" ]]; then
        skip "读不到容器 cgroup 的 devices.list（cgroup v2 或权限不足）"
    elif printf '%s' "$list" | grep -Eq '^c 226:[0-9]+ rwm'; then
        pass "容器 cgroup 已授予 DRM 主设备(226) rwm"
    elif printf '%s' "$list" | grep -Eq '^c 226:[0-9*]+ m$'; then
        fail "容器 cgroup 对 226 只有 m（mknod）没有 rwm —— 正是 devices: 写通配的典型表现，必须精确到 major:minor"
    else
        note "容器 cgroup 里没有任何 226:* 条目"
    fi
    return 0
}

# ------------------------------------------------------------ 机器可读摘要 ---

write_report() {
    local path="$1"
    {
        printf 'repo=%s\n' "$REPO"
        printf 'runtime_dir=%s\n' "$RT_DIR"
        printf 'inside_container=%s\n' "$INSIDE_CONTAINER"
        printf 'npu_card_nodes=%s\n' "$SHELL_NODES"
        printf 'oracle_card_nodes=%s\n' "$ORACLE_NODES"
        printf 'python_card_nodes=%s\n' "$PY_NODES"
        printf 'npu_evidence=%s\n' "$NPU_EVIDENCE"
        printf 'device_rw=%s\n' "$DEVICE_RW"
        printf 'mpp_device=%s\n' "$MPP_DEVICE"
        printf 'mpp_lib=%s\n' "$MPP_LIB"
        printf 'mpp_soname=%s\n' "$MPP_SONAME"
        printf 'mpp_glibc_from=%s\n' "${MPP_GLIBIC_FROM:-GLIBC_2.29}"
        printf 'mpp_dlopen=%s\n' "$MPP_DLOPEN"
        printf 'rknnrt_dlopen=%s\n' "$RKNNRT_DLOPEN"
        printf 'bin_needed=%s\n' "$BIN_NEEDED"
        printf 'health_decode=%s\n' "$HEALTH_DECODE"
        printf 'health_encode=%s\n' "$HEALTH_ENCODE"
        printf 'result=PASS:%d FAIL:%d SKIP:%d\n' "$PASSES" "$FAILS" "$SKIPS"
    } >"$path" 2>/dev/null || true
}

# ------------------------------------------------------------------ 主流程 ---

if [[ -n "$PY" ]]; then
    _emit_py_helper >"$PY_HELPER"
fi

MODE_TEXT="本机模式"
if [[ "$INSIDE_CONTAINER" -eq 1 ]]; then
    MODE_TEXT="容器内模式"
fi

hdr "RK3588/RK356x NPU 探测与编解码链路验证（${MODE_TEXT}）"
note "repo=$REPO"
note "RUNTIME=$RT_DIR"
note "python=${PY:-无}  arch=$(uname -m 2>/dev/null || echo '?')"

section_a
section_b

if [[ "$INSIDE_CONTAINER" -eq 0 ]]; then
    section_c
fi

# 摘要默认落在 TMP_DIR 之外：TMP_DIR 会被 EXIT trap 删掉，指到里面的路径等于给一个假文件
RPT="$REPORT"
if [[ -z "$RPT" ]]; then
    RPT="${TMPDIR:-/tmp}/rk_media_report.env"
fi
write_report "$RPT"

hdr "小结"
printf 'PASS=%d  FAIL=%d  SKIP=%d   摘要：%s\n' "$PASSES" "$FAILS" "$SKIPS" "$RPT"
if [[ "$FAILS" -gt 0 ]]; then
    printf '有 %d 项 FAIL：逐条看上面的 FAIL 行，或用 grep -E "^FAIL" 过滤\n' "$FAILS"
    exit 1
fi
exit 0
