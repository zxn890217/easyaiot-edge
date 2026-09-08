#include "InferEngine.h"

#include <glog/logging.h>

#include "YoloCommon.h"
#include "YoloEngine.h"

#ifdef RUNTIME_WITH_RKNN
#include "RknnEngine.h"
#endif

#ifdef __linux__
#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>
#endif

namespace {

#ifdef RUNTIME_WITH_RKNN
#ifdef __linux__
/**
 * NPU-only evidence. Never list shared accelerators here.
 *
 * /dev/rga and /dev/dri/renderD128 used to be candidates and both were wrong:
 * RGA is the 2D blitter, renderD128 belongs to the rockchip-drm display core.
 * A container that mounts exactly the rkmpp decode set (/dev/mpp_service,
 * /dev/rga, /dev/dri/renderD128) was therefore declared "NPU present", picked
 * the rknn engine for want of a better signal and then died inside
 * rknn_init(). The RKNPU DRM card master node is handled separately below.
 */
const char* kNpuDevices[] = {
    "/dev/rknpu",
    "/dev/rknpu_ll",
    "/sys/class/devfreq/fdab0000.npu",  // RK3588
    "/sys/class/devfreq/ff800000.npu",  // RK356x / RK3576
    nullptr,
};

std::string toLowerStr(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool realPathOf(const std::string& path, std::string& out) {
    char buf[PATH_MAX];
    if (!::realpath(path.c_str(), buf)) {
        return false;
    }
    out.assign(buf);
    return true;
}

/** Same predicate as VIDEO/scripts/npu_drm_nodes.sh. */
bool looksLikeRknpu(const std::string& hay) {
    return hay.find("rknpu") != std::string::npos || hay.find("rknn") != std::string::npos
        || hay.find(".npu") != std::string::npos || hay.find("/npu") != std::string::npos;
}

/**
 * /dev/dri/card* master nodes owned by the RKNPU DRM driver.
 *
 * Measured on RK3588 (rknpu 0.9.8 + librknnrt 2.3.0): rknn_init() ends up with
 * an fd on /dev/dri/card1 (226:1, sysfs driver name RKNPU) - not on a
 * renderD12x node. renderD* is deliberately *not* treated as NPU evidence
 * because the display core exposes one too.
 */
std::vector<std::string> npuDrmCardNodes() {
    std::vector<std::string> nodes;
    DIR* dir = opendir("/sys/class/drm");
    if (!dir) {
        return nodes;
    }
    while (dirent* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name.compare(0, 4, "card") != 0) {
            continue;
        }
        const std::string index = name.substr(4);
        if (index.empty() || index.find_first_not_of("0123456789") != std::string::npos) {
            continue;  // card0-HDMI-A-1 etc. are connectors, not the base device
        }
        const std::string sysfs = "/sys/class/drm/" + name;
        std::string driver;
        std::string devpath;
        realPathOf(sysfs + "/device/driver", driver);
        realPathOf(sysfs, devpath);
        // Both the bound driver basename and the sysfs path are matched: vendor
        // kernels register it as rknpu / rockchip-rknpu while the platform path
        // carries the node name (fe440000.npu). rockchip-drm matches neither.
        if (!looksLikeRknpu(toLowerStr(driver + " " + devpath))) {
            continue;
        }
        const std::string node = "/dev/dri/" + name;
        if (::access(node.c_str(), R_OK | W_OK) == 0) {
            nodes.push_back(node);
        }
    }
    closedir(dir);
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    return nodes;
}
#endif  // __linux__
#endif  // RUNTIME_WITH_RKNN

}  // namespace

std::string normalizeInferBackend(const std::string& backend) {
    const std::string v = yolocore::trim(yolocore::toLower(backend));
    if (v.empty() || v == "auto" || v == "default") {
        return infer_backend::kAuto;
    }
    if (v == "rknn" || v == "npu" || v == "rockchip" || v == "rknpu") {
        return infer_backend::kRknn;
    }
    if (v == "onnx" || v == "onnxruntime" || v == "ort" || v == "cpu" || v == "cuda" || v == "gpu") {
        return infer_backend::kOnnx;
    }
    LOG(WARNING) << "[INFER] Unknown infer_backend='" << backend << "', falling back to auto";
    return infer_backend::kAuto;
}

bool hostHasRknn() {
#ifdef RUNTIME_WITH_RKNN
#ifndef __linux__
    // dlopen/RTLD_* and the rknpu device nodes only exist on Linux; RUNTIME_WITH_RKNN is
    // meaningless anywhere else, so report "no NPU" and let the ONNX path take over.
    return false;
#else
    static const bool cached = [] {
        void* handle = dlopen("librknnrt.so", RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            handle = dlopen("/usr/lib/librknnrt.so", RTLD_NOW | RTLD_GLOBAL);
        }
        if (!handle) {
            LOG(WARNING) << "[INFER] librknnrt.so is not loadable on this host";
            return false;
        }
        // dlclose is intentionally skipped: the runtime keeps driver state in process.
        for (int i = 0; kNpuDevices[i]; ++i) {
            if (access(kNpuDevices[i], R_OK | W_OK) == 0) {
                LOG(INFO) << "[INFER] RKNN NPU detected via " << kNpuDevices[i];
                return true;
            }
        }
        // rknpu v2 (RK3588 & co.) has no /dev/rknpu: the driver is reached through
        // the RKNPU DRM *card* master node. Missing this is what made an
        // NPU-capable host look like a CPU-only one.
        for (const std::string& node : npuDrmCardNodes()) {
            LOG(INFO) << "[INFER] RKNN NPU detected via " << node << " (RKNPU DRM card node)";
            return true;
        }
        LOG(WARNING) << "[INFER] librknnrt.so present but no NPU device node was found "
                     << "(checked /dev/rknpu*, the *.npu devfreq dirs and /sys/class/drm/card*)";
        return false;
    }();
    return cached;
#endif
#else
    return false;
#endif
}

bool rknnModelAvailable(const std::string& model_path) {
    const std::string lower = yolocore::toLower(model_path);
    if (yolocore::hasSuffix(lower, ".rknn")) {
        return yolocore::fileExists(model_path);
    }
    return yolocore::fileExists(yolocore::replaceExt(model_path, ".rknn"));
}

std::shared_ptr<InferEngine> createInferEngine(const std::string& backend,
                                               const std::string& model_path,
                                               const EngineLoadOptions& options) {
    const std::string normalized = normalizeInferBackend(backend);

#ifdef RUNTIME_WITH_RKNN
    // Explicit rknn still needs a resolvable weight: rknn-toolkit2 is x86_64-only, so an
    // edge node cannot lazily convert .onnx/.pt and must fall back instead of failing hard.
    const bool rknn_weight_ready = hostHasRknn() && rknnModelAvailable(model_path);
    const bool use_rknn = rknn_weight_ready && normalized != infer_backend::kOnnx;
    if (use_rknn) {
        if (normalized == infer_backend::kAuto) {
            LOG(INFO) << "[INFER] infer_backend=auto → rknn (NPU present, .rknn weight available)";
        }
        return std::make_shared<RknnEngine>();
    }
    if (normalized == infer_backend::kRknn) {
        LOG(WARNING) << "[INFER] infer_backend=rknn unavailable ("
                     << (hostHasRknn() ? "no .rknn weight resolvable" : "no NPU runtime")
                     << ") for " << model_path << "; falling back to ONNX Runtime";
    }
#else
    if (normalized == infer_backend::kRknn) {
        LOG(ERROR) << "[INFER] infer_backend=rknn requested but RUNTIME was built without "
                      "RUNTIME_WITH_RKNN; using ONNX Runtime instead";
    }
#endif
    return std::make_shared<YoloEngine>();
}
