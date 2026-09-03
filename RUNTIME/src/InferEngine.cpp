#include "InferEngine.h"

#include <glog/logging.h>

#include "YoloCommon.h"
#include "YoloEngine.h"

#ifdef RUNTIME_WITH_RKNN
#include "RknnEngine.h"
#endif

#ifdef __linux__
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace {

#ifdef RUNTIME_WITH_RKNN
/** Candidate NPU device nodes across rknpu/rknpu2 kernel drivers. */
const char* kNpuDevices[] = {
    "/dev/rga",
    "/dev/dri/renderD128",
    "/dev/rknpu",
    "/sys/class/devfreq/fdab0000.npu",
    "/sys/class/devfreq/ff800000.npu",
    nullptr,
};
#endif

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
        LOG(WARNING) << "[INFER] librknnrt.so present but no NPU device node was found";
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
