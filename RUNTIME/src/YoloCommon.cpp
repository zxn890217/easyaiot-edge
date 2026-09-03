#include "YoloCommon.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <glog/logging.h>

namespace yolocore {
namespace {

const char* kCoco[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"};

std::vector<std::string>& classesRef() {
    static std::vector<std::string> g_classes(std::begin(kCoco), std::end(kCoco));
    return g_classes;
}

const char* pythonExecutable() {
    const char* py = std::getenv("RUNTIME_PYTHON");
    if (!py || !*py) {
        py = std::getenv("EASYAIOT_PYTHON");
    }
    if (!py || !*py) {
        py = "python3";
    }
    return py;
}

}  // namespace

std::vector<std::string>& classes() {
    return classesRef();
}

std::string classNameOf(int idx) {
    const auto& names = classesRef();
    if (idx >= 0 && idx < static_cast<int>(names.size())) {
        return names[static_cast<size_t>(idx)];
    }
    return "class_" + std::to_string(idx);
}

size_t applyClasses(const std::vector<std::string>& in) {
    if (!in.empty()) {
        classesRef() = in;
    }
    return classesRef().size();
}

std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string replaceExt(const std::string& path, const std::string& newExt) {
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return path + newExt;
    }
    return path.substr(0, dot) + newExt;
}

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return "";
    }
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool hasSuffix(const std::string& lowerPath, const std::string& lowerExt) {
    if (lowerExt.size() > lowerPath.size()) {
        return false;
    }
    return lowerPath.compare(lowerPath.size() - lowerExt.size(), lowerExt.size(), lowerExt) == 0;
}

std::string findRuntimeScript(const char* envOverride, const std::string& scriptName) {
    if (envOverride) {
        if (const char* env = std::getenv(envOverride)) {
            if (fileExists(env)) {
                return env;
            }
        }
    }
    const char* roots[] = {
        std::getenv("EASYAIOT_RUNTIME_INSTALL_DIR"),
        std::getenv("RUNTIME_ROOT"),
        std::getenv("EASYAIOT_ROOT"),
        nullptr,
    };
    for (int i = 0; roots[i]; ++i) {
        std::string p = std::string(roots[i]) + "/scripts/" + scriptName;
        if (fileExists(p)) {
            return p;
        }
        p = std::string(roots[i]) + "/RUNTIME/scripts/" + scriptName;
        if (fileExists(p)) {
            return p;
        }
    }
    const std::string candidates[] = {
        "RUNTIME/scripts/" + scriptName,
        "scripts/" + scriptName,
        "/opt/easyaiot/RUNTIME/scripts/" + scriptName,
    };
    for (const auto& c : candidates) {
        if (fileExists(c)) {
            return c;
        }
    }
    return "";
}

bool runPythonScript(const std::string& logTag, const std::string& scriptPath,
                     const std::vector<std::string>& args) {
    std::string cmd = std::string("\"") + pythonExecutable() + "\" \"" + scriptPath + "\"";
    for (const auto& a : args) {
        cmd += " \"" + a + "\"";
    }
    cmd += " 2>&1";
    LOG(INFO) << "[" << logTag << "] exec: " << cmd;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        LOG(ERROR) << "[" << logTag << "] failed to spawn " << scriptPath;
        return false;
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        const std::string line = trim(buf);
        if (!line.empty()) {
            LOG(INFO) << "[" << logTag << "-OUT] " << line;
        }
    }
    const int rc = pclose(pipe);
    if (rc != 0) {
        LOG(ERROR) << "[" << logTag << "] script exited rc=" << rc;
        return false;
    }
    return true;
}

std::vector<std::string> readNamesSidecar(const std::string& modelPath) {
    std::vector<std::string> names;
    const std::string namesPath = replaceExt(modelPath, ".names");
    if (!fileExists(namesPath)) {
        return names;
    }
    std::ifstream ifs(namesPath);
    std::string line;
    while (std::getline(ifs, line)) {
        const std::string v = trim(line);
        if (!v.empty()) {
            names.push_back(v);
        }
    }
    if (!names.empty()) {
        LOG(INFO) << "[YOLO] Loaded " << names.size() << " class names from " << namesPath;
    }
    return names;
}

cv::Mat squarePad(const cv::Mat& image, float& xFactorOut, float& yFactorOut,
                  int inputW, int inputH) {
    const int side = std::max(image.rows, image.cols);
    cv::Mat mask = cv::Mat::zeros(cv::Size(side, side), CV_8UC3);
    image.copyTo(mask(cv::Rect(0, 0, image.cols, image.rows)));
    xFactorOut = mask.cols / static_cast<float>(inputW);
    yFactorOut = mask.rows / static_cast<float>(inputH);
    return mask;
}

bool isEndToEndShape(const std::vector<int64_t>& dims) {
    return !dims.empty() && dims.back() == 6;
}

int postprocess(const float* data,
                const std::vector<int64_t>& dims,
                bool end2End,
                float xFactor,
                float yFactor,
                float scoreThreshold,
                float nmsThreshold,
                const std::string& logTag,
                std::vector<DetectObject>& detections) {
    detections.clear();
    if (!data) {
        LOG(ERROR) << "[" << logTag << "] null output tensor";
        return -1;
    }

    if (end2End) {
        // [1, N, 6] or [N, 6] with rows = (x1, y1, x2, y2, conf, cls)
        int64_t rows = 0;
        if (dims.size() == 3) {
            rows = dims[1];
        } else if (dims.size() == 2) {
            rows = dims[0];
        } else {
            LOG(ERROR) << "[" << logTag << "] unexpected end2end rank=" << dims.size();
            return -1;
        }
        for (int64_t i = 0; i < rows; ++i) {
            const float* row = data + i * 6;
            const float score = row[4];
            if (score < scoreThreshold) {
                continue;
            }
            const int cls = static_cast<int>(row[5]);
            DetectObject det;
            det.x1 = static_cast<int>(row[0] * xFactor);
            det.y1 = static_cast<int>(row[1] * yFactor);
            det.x2 = static_cast<int>(row[2] * xFactor);
            det.y2 = static_cast<int>(row[3] * yFactor);
            det.class_id = cls;
            det.class_name = classNameOf(cls);
            det.class_score = score;
            detections.push_back(det);
        }
        return 0;
    }

    // Ultralytics detect: [4+C, N] (transposed) or [N, 4+C]
    int64_t d1 = 0;
    int64_t d2 = 0;
    if (dims.size() == 3) {
        d1 = dims[1];
        d2 = dims[2];
    } else if (dims.size() == 2) {
        d1 = dims[0];
        d2 = dims[1];
    } else {
        LOG(ERROR) << "[" << logTag << "] unexpected detect rank=" << dims.size();
        return -1;
    }

    cv::Mat det_output;
    if (d1 < d2) {
        cv::Mat dout(static_cast<int>(d1), static_cast<int>(d2), CV_32F, const_cast<float*>(data));
        det_output = dout.t();
    } else {
        det_output = cv::Mat(static_cast<int>(d1), static_cast<int>(d2), CV_32F, const_cast<float*>(data));
    }

    const int output_dim = det_output.cols;
    std::vector<cv::Rect> boxes;
    std::vector<int> classIds;
    std::vector<float> confidences;
    for (int i = 0; i < det_output.rows; ++i) {
        if (output_dim <= 4) {
            break;
        }
        cv::Mat classes_scores = det_output.row(i).colRange(4, output_dim);
        cv::Point classIdPoint;
        double score = 0.0;
        cv::minMaxLoc(classes_scores, nullptr, &score, nullptr, &classIdPoint);
        if (score > scoreThreshold) {
            const float cx = det_output.at<float>(i, 0);
            const float cy = det_output.at<float>(i, 1);
            const float ow = det_output.at<float>(i, 2);
            const float oh = det_output.at<float>(i, 3);
            cv::Rect box;
            box.x = static_cast<int>((cx - 0.5f * ow) * xFactor);
            box.y = static_cast<int>((cy - 0.5f * oh) * yFactor);
            box.width = static_cast<int>(ow * xFactor);
            box.height = static_cast<int>(oh * yFactor);
            boxes.push_back(box);
            classIds.push_back(classIdPoint.x);
            confidences.push_back(static_cast<float>(score));
        }
    }

    std::vector<int> indexes;
    cv::dnn::NMSBoxes(boxes, confidences, scoreThreshold, nmsThreshold, indexes);
    for (int index : indexes) {
        DetectObject detection;
        const int idx = classIds[static_cast<size_t>(index)];
        const cv::Rect& box = boxes[static_cast<size_t>(index)];
        detection.x1 = box.x;
        detection.y1 = box.y;
        detection.x2 = box.x + box.width;
        detection.y2 = box.y + box.height;
        detection.class_id = idx;
        detection.class_name = classNameOf(idx);
        detection.class_score = confidences[static_cast<size_t>(index)];
        detections.push_back(detection);
    }
    return 0;
}

}  // namespace yolocore
