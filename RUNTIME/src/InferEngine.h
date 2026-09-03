#ifndef INFER_ENGINE_H
#define INFER_ENGINE_H

#include "Datatype.h"

#include <memory>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

/** Backend selection for [ai] infer_backend. */
namespace infer_backend {
constexpr const char* kAuto = "auto";
constexpr const char* kOnnx = "onnx";
constexpr const char* kRknn = "rknn";
}  // namespace infer_backend

/** RKNN NPU core masks (mirrors rknn_api.h RKNN_NPU_CORE_*). */
namespace npu_core {
constexpr int kAuto = 0;
constexpr int kCore0 = 1;
constexpr int kCore1 = 2;
constexpr int kCore2 = 4;
constexpr int kAll = 7;
}  // namespace npu_core

/**
 * Per-instance load options. `npuCoreMask` / `gpuDeviceId` are hardware targets;
 * an engine is expected to degrade to the cheapest working option instead of failing
 * (RUNTIME keeps the stream alive even when the accelerator is missing).
 */
struct EngineLoadOptions {
    bool preferGpu{true};
    bool forceCpu{false};
    int gpuDeviceId{0};
    /** 0 = auto (all cores), otherwise a bit mask of npu_core::* values. */
    int npuCoreMask{npu_core::kAuto};
    /** Index of this engine inside the pool — used to spread work across NPU cores. */
    int instanceIndex{0};
};

/**
 * Detection inference engine contract.
 *
 * Implementations: YoloEngine (ONNX Runtime CPU/CUDA EP) and RknnEngine (Rockchip NPU).
 */
class InferEngine {
public:
    virtual ~InferEngine() = default;

    /** @return 0 on success; non-zero keeps the caller from starting the pool. */
    virtual int LoadModel(const std::string& model_path,
                          const std::vector<std::string>& model_class,
                          const EngineLoadOptions& options) = 0;

    /** image is BGR CV_8UC3 as decoded by the pipeline. */
    virtual int Run(cv::Mat& image, std::vector<DetectObject>& objects) = 0;

    /** "cuda" | "cpu" | "rknn" | "rknn-cpu" | "none" */
    virtual const std::string& inferEp() const = 0;
    /** "detect" | "end2end" | "unknown" */
    virtual const std::string& modelLayout() const = 0;
    /** Resolved weights file actually handed to the runtime. */
    virtual const std::string& loadedModelPath() const = 0;

    virtual void setScoreThreshold(float threshold) = 0;
};

/** "auto"|"onnx"|"cuda"|"cpu"|"rknn"|"npu" → canonical "auto"|"onnx"|"rknn". */
std::string normalizeInferBackend(const std::string& backend);

/** Best-effort host detection used by infer_backend=auto. */
bool hostHasRknn();

/** True when a .rknn weight already exists for `model_path` (NPU presence is checked by hostHasRknn). */
bool rknnModelAvailable(const std::string& model_path);

/**
 * Engine factory.
 *
 * @param backend     infer_backend value ("auto"/"onnx"/"rknn")
 * @param model_path  configured weights path (used by "auto" to pick a backend)
 * @param options     load options (core mask, device id, ...)
 */
std::shared_ptr<InferEngine> createInferEngine(const std::string& backend,
                                               const std::string& model_path,
                                               const EngineLoadOptions& options);

#endif  // INFER_ENGINE_H
