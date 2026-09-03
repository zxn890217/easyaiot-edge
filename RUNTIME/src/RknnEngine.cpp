#include "RknnEngine.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <glog/logging.h>
#include <json/json.h>
#include <sstream>

#include "rknn_api.h"

namespace {

constexpr int kMaxRknnDims = 8;

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    const std::streamsize size = f.tellg();
    if (size <= 0) {
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), size));
}

/** Optional sidecar written by ensure_rknn_model.py: {"model_layout": "...", ...} */
Json::Value readSidecar(const std::string& rknnPath) {
    Json::Value root;
    const std::string jsonPath = yolocore::replaceExt(rknnPath, ".rknn.json");
    if (!yolocore::fileExists(jsonPath)) {
        return root;
    }
    std::ifstream ifs(jsonPath);
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, ifs, &root, &errs)) {
        LOG(WARNING) << "[RKNN] Bad sidecar " << jsonPath << ": " << errs;
        return Json::Value();
    }
    return root;
}

std::vector<int64_t> dimsOf(const rknn_tensor_attr& attr) {
    std::vector<int64_t> dims;
    const size_t cap = std::min<size_t>(attr.n_dims, kMaxRknnDims);
    for (size_t i = 0; i < cap; ++i) {
        dims.push_back(static_cast<int64_t>(attr.dims[i]));
    }
    // Drop the leading batch axis so downstream code sees a plain 2-D matrix.
    while (dims.size() > 2 && dims.front() == 1) {
        dims.erase(dims.begin());
    }
    return dims;
}

/** Input geometry from an NCHW/NHWC attr, tolerant of the RKNN dim ordering. */
void inputGeometry(const rknn_tensor_attr& attr, int& sideOut, int& channelsOut) {
    int side = 0;
    int channels = 0;
    const size_t cap = std::min<size_t>(attr.n_dims, kMaxRknnDims);
    for (size_t i = 0; i < cap; ++i) {
        const int d = static_cast<int>(attr.dims[i]);
        if (d <= 1) {
            continue;
        }
        if (d > side) {
            side = d;
        }
        if (channels == 0 || d < channels) {
            channels = d;
        }
    }
    sideOut = side;
    channelsOut = (channels > 0 && channels <= 4) ? channels : 3;
}

/** Core mask for one pool instance. */
rknn_core_mask coreMaskFor(const EngineLoadOptions& opt) {
    int mask = opt.npuCoreMask;
    if (mask == -1) {  // per-thread: pin engine i to core i % 3
        mask = 1 << (opt.instanceIndex % 3);
    }
    if (mask < 0 || mask > npu_core::kAll) {
        mask = npu_core::kAuto;
    }
    return static_cast<rknn_core_mask>(mask);
}

const char* coreMaskName(rknn_core_mask m) {
    switch (m) {
        case RKNN_NPU_CORE_AUTO: return "auto";
        case RKNN_NPU_CORE_0: return "core0";
        case RKNN_NPU_CORE_1: return "core1";
        case RKNN_NPU_CORE_2: return "core2";
        case RKNN_NPU_CORE_0_1: return "core0+1";
        case RKNN_NPU_CORE_0_1_2: return "core0+1+2";
        default: return "custom";
    }
}

}  // namespace

struct RknnEngine::Impl {
    rknn_context ctx{0};
    std::vector<uint8_t> blob;
    std::vector<rknn_tensor_attr> inputAttrs;
    std::vector<rknn_tensor_attr> outputAttrs;
    std::vector<uint8_t> rgbScratch;
    std::vector<int64_t> outDims;
    bool hasOutputShape{false};

    ~Impl() { release(); }

    void release() {
        if (ctx) {
            rknn_destroy(ctx);
            ctx = 0;
        }
        inputAttrs.clear();
        outputAttrs.clear();
        outDims.clear();
        hasOutputShape = false;
    }
};

RknnEngine::RknnEngine() : impl_(std::make_unique<Impl>()) {}

RknnEngine::~RknnEngine() {
    if (impl_) {
        impl_->release();
    }
}

void RknnEngine::setScoreThreshold(float threshold) {
    if (threshold > 0.0f && threshold <= 1.0f) {
        scoreThreshold_ = threshold;
    }
}

namespace {

/** Resolve a .rknn weight, converting .onnx/.pt in place when possible. */
std::string ensureRknnPath(const std::string& model_path) {
    const std::string lower = yolocore::toLower(model_path);
    if (yolocore::hasSuffix(lower, ".rknn")) {
        if (yolocore::fileExists(model_path)) {
            return model_path;
        }
        return "";
    }

    const std::string sibling = yolocore::replaceExt(model_path, ".rknn");
    if (yolocore::fileExists(sibling)) {
        LOG(INFO) << "[RKNN] Using existing RKNN beside weights: " << sibling;
        return sibling;
    }

    const std::string script =
        yolocore::findRuntimeScript("RUNTIME_ENSURE_RKNN_SCRIPT", "ensure_rknn_model.py");
    if (script.empty()) {
        LOG(ERROR) << "[RKNN] No .rknn found and ensure_rknn_model.py is unavailable: " << model_path;
        return "";
    }
    if (!yolocore::runPythonScript("RKNN", script, {"--input", model_path, "--output", sibling})) {
        LOG(ERROR) << "[RKNN] Conversion failed for " << model_path;
        return "";
    }
    if (!yolocore::fileExists(sibling)) {
        LOG(ERROR) << "[RKNN] Conversion produced no file at " << sibling;
        return "";
    }
    LOG(INFO) << "[RKNN] Conversion OK: " << sibling;
    return sibling;
}

}  // namespace

int RknnEngine::LoadModel(const std::string& model_path,
                          const std::vector<std::string>& model_class,
                          const EngineLoadOptions& options) {
    try {
        const std::string rknn_path = ensureRknnPath(model_path);
        if (rknn_path.empty()) {
            LOG(ERROR) << "[RKNN] No loadable .rknn for: " << model_path;
            inferEp_ = "none";
            return -3;
        }
        loadedModelPath_ = rknn_path;

        if (!readFile(rknn_path, impl_->blob)) {
            LOG(ERROR) << "[RKNN] Failed to read model file: " << rknn_path;
            inferEp_ = "none";
            return -4;
        }

        const int init_ret = rknn_init(&impl_->ctx, impl_->blob.data(),
                                       static_cast<uint32_t>(impl_->blob.size()), 0, nullptr);
        if (init_ret < 0) {
            LOG(ERROR) << "[RKNN] rknn_init failed ret=" << init_ret
                       << " (" << rknn_path << "). Check librknnrt / driver and"
                          " that the model was built for this chip.";
            impl_->ctx = 0;
            inferEp_ = "none";
            return -5;
        }

        rknn_sdk_version sdk{};
        if (rknn_query(impl_->ctx, RKNN_QUERY_SDK_VERSION, &sdk, sizeof(sdk)) == 0) {
            LOG(INFO) << "[RKNN] api_version=" << sdk.api_version << " drv_version=" << sdk.drv_version;
        }

        rknn_core_mask mask = coreMaskFor(options);
        const int mask_ret = rknn_set_core_mask(impl_->ctx, mask);
        if (mask_ret < 0) {
            LOG(WARNING) << "[RKNN] rknn_set_core_mask(" << coreMaskName(mask)
                         << ") ret=" << mask_ret << "; continuing with driver default";
        } else {
            LOG(INFO) << "[RKNN] NPU core binding: " << coreMaskName(mask)
                      << " (instance=" << options.instanceIndex << ")";
        }

        rknn_input_output_num io_num{};
        if (rknn_query(impl_->ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != 0) {
            LOG(ERROR) << "[RKNN] RKNN_QUERY_IN_OUT_NUM failed";
            impl_->release();
            inferEp_ = "none";
            return -6;
        }
        if (io_num.n_input == 0) {
            LOG(ERROR) << "[RKNN] Model reports zero inputs";
            impl_->release();
            inferEp_ = "none";
            return -6;
        }

        impl_->inputAttrs.resize(io_num.n_input);
        for (uint32_t i = 0; i < io_num.n_input; ++i) {
            std::memset(&impl_->inputAttrs[i], 0, sizeof(rknn_tensor_attr));
            impl_->inputAttrs[i].index = i;
            if (rknn_query(impl_->ctx, RKNN_QUERY_INPUT_ATTR, &impl_->inputAttrs[i],
                           sizeof(rknn_tensor_attr)) != 0) {
                LOG(ERROR) << "[RKNN] RKNN_QUERY_INPUT_ATTR failed for input " << i;
                impl_->release();
                inferEp_ = "none";
                return -6;
            }
        }
        impl_->outputAttrs.resize(io_num.n_output);
        for (uint32_t i = 0; i < io_num.n_output; ++i) {
            std::memset(&impl_->outputAttrs[i], 0, sizeof(rknn_tensor_attr));
            impl_->outputAttrs[i].index = i;
            if (rknn_query(impl_->ctx, RKNN_QUERY_OUTPUT_ATTR, &impl_->outputAttrs[i],
                           sizeof(rknn_tensor_attr)) != 0) {
                LOG(WARNING) << "[RKNN] RKNN_QUERY_OUTPUT_ATTR failed for output " << i;
                impl_->outputAttrs.clear();
                break;
            }
        }

        inputGeometry(impl_->inputAttrs[0], input_side_, input_channels_);
        if (input_side_ <= 0) {
            LOG(ERROR) << "[RKNN] Unable to derive input size from model attrs";
            impl_->release();
            inferEp_ = "none";
            return -7;
        }
        impl_->rgbScratch.assign(static_cast<size_t>(input_side_) * input_side_ * input_channels_, 0);

        // Layout: trust the export sidecar (authoritative), else infer from the shape.
        const Json::Value sidecar = readSidecar(rknn_path);
        const std::string sidecarLayout =
            sidecar.get("model_layout", "").asString();
        if (!impl_->outputAttrs.empty()) {
            impl_->outDims = dimsOf(impl_->outputAttrs[0]);
            impl_->hasOutputShape = !impl_->outDims.empty();
        }
        if (sidecarLayout == "end2end" || sidecarLayout == "detect") {
            end2end_ = (sidecarLayout == "end2end");
        } else {
            end2end_ = impl_->hasOutputShape && yolocore::isEndToEndShape(impl_->outDims);
        }
        if (end2end_) {
            modelLayout_ = "end2end";
            int64_t numel = 1;
            for (int64_t d : impl_->outDims) {
                numel *= d;
            }
            if (numel > 6) {
                impl_->outDims = {numel / 6, 6};
            }
            if (scoreThreshold_ < 0.20f) {
                scoreThreshold_ = 0.20f;
            }
        } else {
            modelLayout_ = "detect";
        }

        if (!model_class.empty()) {
            yolocore::applyClasses(model_class);
            LOG(INFO) << "[RKNN] Using " << model_class.size() << " classes from names file";
        } else {
            const std::vector<std::string> names = yolocore::readNamesSidecar(rknn_path);
            yolocore::applyClasses(names);
        }

        inferEp_ = "rknn";
        ready_ = true;
        LOG(INFO) << "[RKNN] Model loaded infer_ep=rknn input=" << input_channels_ << "x" << input_side_
                  << "x" << input_side_ << " layout=" << modelLayout_ << " model=" << rknn_path;
        return 0;
    } catch (const std::exception& e) {
        LOG(ERROR) << "[RKNN] Exception loading model: " << e.what();
        impl_->release();
        inferEp_ = "none";
        ready_ = false;
        return -2;
    }
}

int RknnEngine::Run(cv::Mat& image, std::vector<DetectObject>& detections) {
    detections.clear();
    if (!ready_ || !impl_->ctx) {
        return -1;
    }

    float x_factor = 1.0f;
    float y_factor = 1.0f;
    const cv::Mat padded = yolocore::squarePad(image, x_factor, y_factor, input_side_, input_side_);

    cv::Mat resized;
    cv::resize(padded, resized, cv::Size(input_side_, input_side_), 0, 0, cv::INTER_LINEAR);
    cv::Mat rgb;
    if (input_channels_ == 3) {
        cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    } else {
        rgb = resized;
    }
    if (!rgb.isContinuous()) {
        rgb = rgb.clone();
    }

    rknn_input in{};
    in.index = 0;
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;
    in.size = static_cast<uint32_t>(rgb.total() * rgb.elemSize());
    in.pass_through = 0;
    in.buf = rgb.data;

    if (rknn_inputs_set(impl_->ctx, 1, &in) < 0) {
        LOG(ERROR) << "[RKNN] rknn_inputs_set failed";
        return -1;
    }
    if (rknn_run(impl_->ctx, nullptr) < 0) {
        LOG(ERROR) << "[RKNN] rknn_run failed (NPU busy or model/driver mismatch)";
        return -1;
    }

    if (impl_->outputAttrs.empty()) {
        LOG(ERROR) << "[RKNN] Output attributes unavailable for postprocess";
        return -1;
    }

    std::vector<rknn_output> outputs(impl_->outputAttrs.size());
    for (size_t i = 0; i < outputs.size(); ++i) {
        std::memset(&outputs[i], 0, sizeof(rknn_output));
        outputs[i].index = static_cast<uint32_t>(i);
        outputs[i].want_float = 1;  // dequantize on the driver side
        outputs[i].is_prealloc = 0;
    }
    if (rknn_outputs_get(impl_->ctx, static_cast<uint32_t>(outputs.size()), outputs.data(), nullptr) < 0) {
        LOG(ERROR) << "[RKNN] rknn_outputs_get failed";
        return -1;
    }

    std::vector<int64_t> dims = impl_->hasOutputShape ? impl_->outDims : dimsOf(impl_->outputAttrs[0]);
    const int rc = yolocore::postprocess(static_cast<const float*>(outputs[0].buf), dims, end2end_,
                                         x_factor, y_factor, scoreThreshold_, nmsThreshold_, "RKNN",
                                         detections);

    rknn_outputs_release(impl_->ctx, static_cast<uint32_t>(outputs.size()), outputs.data());

    static int debug_count = 0;
    if (debug_count < 3) {
        LOG(INFO) << "[RKNN] layout=" << modelLayout_ << " detections=" << detections.size();
        debug_count++;
    }
    return rc;
}
