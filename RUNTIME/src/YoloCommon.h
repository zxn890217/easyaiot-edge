#ifndef YOLO_COMMON_H
#define YOLO_COMMON_H

#include "Datatype.h"

#include <cstdint>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

/**
 * Shared helpers for every YOLO detection backend (ONNX Runtime, RKNN NPU, ...).
 *
 * The preprocessing geometry, output-layout sniffing and NMS/end2-end decoding are
 * backend independent, so they live here to guarantee that switching infer_backend
 * never changes the detections a task produces.
 */
namespace yolocore {

/** Mutable class-name table (defaults to COCO-80, overwritten per model load). */
std::vector<std::string>& classes();
std::string classNameOf(int idx);
/** Replace the global table when `in` is non-empty; returns resulting size. */
size_t applyClasses(const std::vector<std::string>& in);

std::string toLower(std::string s);
std::string replaceExt(const std::string& path, const std::string& newExt);
bool fileExists(const std::string& path);
/** Strip CR/LF + surrounding blanks. */
std::string trim(const std::string& s);
bool hasSuffix(const std::string& lowerPath, const std::string& lowerExt);

/**
 * Locate a helper script under RUNTIME/scripts (env override → install roots → cwd).
 * `scriptName` e.g. "ensure_onnx_model.py". Returns "" when not found.
 */
std::string findRuntimeScript(const char* envOverride, const std::string& scriptName);

/** Run `python3 <script> <args...>` streaming stdout/stderr into glog with `logTag`. */
bool runPythonScript(const std::string& logTag, const std::string& scriptPath,
                     const std::vector<std::string>& args);

/** Read a "<model>.names" sidecar (one label per line) if present. */
std::vector<std::string> readNamesSidecar(const std::string& modelPath);

/** Square letterbox pad: right/bottom zero padding up to max(w, h). */
cv::Mat squarePad(const cv::Mat& image, float& xFactorOut, float& yFactorOut,
                  int inputW, int inputH);

/** True when the last output dim is 6 → YOLO26 end-to-end [N, 6] tensor. */
bool isEndToEndShape(const std::vector<int64_t>& dims);

/**
 * Decode a float32 detection tensor into image-space boxes.
 *
 * @param data     row-major float output (already dequantized by the backend)
 * @param dims     output shape ([1, 4+C, N] / [1, N, 4+C] / [1, N, 6])
 * @param end2End  force end-to-end parsing; when false it is derived from `dims`
 */
int postprocess(const float* data,
                const std::vector<int64_t>& dims,
                bool end2End,
                float xFactor,
                float yFactor,
                float scoreThreshold,
                float nmsThreshold,
                const std::string& logTag,
                std::vector<DetectObject>& detections);

}  // namespace yolocore

#endif
