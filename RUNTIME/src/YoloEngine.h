#ifndef YOLO_ENGINE_H
#define YOLO_ENGINE_H

#include "InferEngine.h"

#include <memory>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>
#include <onnxruntime_cxx_api.h>

/**
 * Ultralytics YOLO detect engine (ONNX Runtime CPU / CUDA EP).
 * Supports:
 *  - YOLOv8 / YOLO11 classic detect export: [1, 4+C, N] → NMS
 *  - YOLO26 end2end export: [1, N, 6] = [x1,y1,x2,y2,conf,cls] (no NMS)
 *  - .pt paths: auto-export to sibling .onnx via RUNTIME/scripts/ensure_onnx_model.py
 *
 * Pre/post-processing is shared with the other backends through yolocore (YoloCommon.h)
 * so switching infer_backend does not change detections.
 */
class YoloEngine : public InferEngine {
public:
    YoloEngine();
    ~YoloEngine() override;

    int LoadModel(const std::string& model_path,
                  const std::vector<std::string>& model_class,
                  const EngineLoadOptions& options) override;

    int Run(cv::Mat& image, std::vector<DetectObject>& objects) override;

    const std::string& inferEp() const override { return inferEp_; }
    const std::string& modelLayout() const override { return modelLayout_; }
    const std::string& loadedModelPath() const override { return loadedModelPath_; }
    void setScoreThreshold(float threshold) override;

private:
    int Inference(const cv::Mat& image, std::vector<DetectObject>& objects);
    int createSession(const std::string& model_path, bool use_cuda, int gpu_device_id);
    static std::string ensureOnnxPath(const std::string& model_path);
    void loadNamesFromOnnxMetadata();

    bool ready_{false};
    bool end2end_{false};
    std::string inferEp_{"none"};
    std::string modelLayout_{"unknown"};
    std::string loadedModelPath_;
    float scoreThreshold_{0.25f};
    float nmsThreshold_{0.45f};

    Ort::Env onnxEnv{nullptr};
    Ort::SessionOptions onnxSessionOptions{nullptr};
    Ort::Session onnxSession{nullptr};
};

#endif
