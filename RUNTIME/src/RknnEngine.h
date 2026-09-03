#ifndef RKNN_ENGINE_H
#define RKNN_ENGINE_H

#include "InferEngine.h"
#include "YoloCommon.h"

/**
 * Rockchip NPU (RKNN) detection engine for RK3588 / RK356x / RV11xx.
 *
 * Requires librknnrt + rknn_api.h (rknpu2 runtime) and a build with
 * -DRUNTIME_WITH_RKNN=ON. Model weights must be .rknn (produced by
 * RUNTIME/scripts/ensure_rknn_model.py on the x86_64 control plane); .onnx/.pt
 * inputs are converted on demand when the toolkit is present.
 *
 * I/O contract with the NPU:
 *  - input : uint8 NHWC RGB, square-letterboxed to the exported imgsz, raw 0..255
 *            (the /255 normalization is baked into the .rknn graph by mean/std)
 *  - output: float32 (want_float = 1), decoded through yolocore::postprocess so the
 *            result is identical to the ONNX Runtime path.
 */
class RknnEngine : public InferEngine {
public:
    RknnEngine();
    ~RknnEngine() override;

    int LoadModel(const std::string& model_path,
                  const std::vector<std::string>& model_class,
                  const EngineLoadOptions& options) override;

    int Run(cv::Mat& image, std::vector<DetectObject>& objects) override;

    const std::string& inferEp() const override { return inferEp_; }
    const std::string& modelLayout() const override { return modelLayout_; }
    const std::string& loadedModelPath() const override { return loadedModelPath_; }
    void setScoreThreshold(float threshold) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool ready_{false};
    bool end2end_{false};
    int input_side_{0};
    int input_channels_{3};
    std::string inferEp_{"none"};
    std::string modelLayout_{"unknown"};
    std::string loadedModelPath_;
    float scoreThreshold_{0.25f};
    float nmsThreshold_{0.45f};
};

#endif  // RKNN_ENGINE_H
