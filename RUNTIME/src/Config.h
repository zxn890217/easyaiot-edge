//
// Created by basiclab on 25-10-15.
//

#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <opencv2/opencv.hpp>

struct DeviceStreamConfig {
    std::string deviceId;
    std::string deviceName;
    std::string rtspUrl;
};

typedef struct Config {
    std::string rtspUrl;
    std::string rtmpUrl;
    std::string hookHttpUrl;
    bool enableRtmp{false};
    bool enableAI{true};
    bool enableDrawRtmp{true};
    bool enableAlarm{true};
    std::map<std::string, std::string> modelPaths;
    std::map<std::string, std::string> modelClasses;
    std::map<std::string, std::vector<std::vector<cv::Point>>> regions;
    int threadNums{2};

    int videoWidth{1920};
    int videoHeight{1080};
    int rtmpFps{25};
    /** RTMP ABR bitrate bits/sec; 0 = auto by encode resolution. Accepts ini/env like 4500k. */
    int64_t videoBitRate{0};
    /** Encoder GOP frames; 0 = 2 * fps. */
    int videoGopSize{0};

    float alarmConfidenceThreshold{0.5f};
    int alarmCooldownTime{30};
    /** 告警触发类别；为空时任意检测均可触发（与 VIDEO alert_class_filter.py 一致） */
    std::vector<std::string> alertClassNames;

    std::string taskId;
    int controlPort{8000};

    // VIDEO 平台对接（[video_task]）
    std::string deviceId;
    std::string deviceName;
    std::string taskType{"realtime"};  // realtime | snap | snapshot | patrol
    std::string algorithmName{"detection"};
    std::string heartbeatUrl;
    std::string alertHookUrl;  // deprecated: events use MQTT; kept for HTTP fallback
    std::string logPath;
    std::string alertImageDir;
    int heartbeatIntervalSec{10};
    bool headless{true};
    int frameSkip{8};  // realtime: infer every N frames; snap fallback interval sec

    // Algorithm event bus (MQTT → iot-sink); heartbeat stays HTTP → VIDEO
    std::string algoBusTransport;  // empty/mqtt default; http/off disables
    std::string mqttBrokerUrls;
    std::string mqttUsername;
    std::string mqttPassword;
    std::string mqttClientId;
    std::string mqttTenant;
    std::string computeNodeId;

    // snap
    std::string cronExpression;

    // patrol
    std::string patrolMode{"pool"};  // pool | rotate
    int patrolIntervalSec{10};
    int patrolPoolSize{4};

    // multi-device (snap/patrol; realtime may also list primary first)
    std::vector<DeviceStreamConfig> devices;

    // AI execution backend: prefer CUDA EP, fallback CPU
    bool preferGpu{true};
    bool forceCpu{false};
    int gpuDeviceId{0};

    /** [ai] infer_backend: auto | onnx | rknn (Rockchip NPU, e.g. RK3588) */
    std::string inferBackend{"auto"};
    /**
     * [ai] npu_core_mask: 0/auto = driver decides, 7/all = 3 cores,
     * per_thread = pin engine i to core i%3, otherwise a bit mask (1/2/4/3/5/6).
     */
    int npuCoreMask{0};

    /**
     * Hardware codec family: auto | cuda | rkmpp | none.
     * auto lets each backend probe the board (NVDEC/NVENC vs rkvdec/VEPU).
     * This is deliberately independent from prefer_gpu / force_cpu, which only
     * steer ONNX Runtime: an RK3588 box has no CUDA device yet still needs the
     * VPU for decode and encode, otherwise libx264 eats 30-40 % of the CPU.
     */
    std::string hwaccel{"auto"};
    /** Hardware decode (NVDEC / rkvdec). Soft fallback on failure. */
    bool hwaccelDecode{true};
    /** Hardware encode (NVENC / VEPU). libx264 fallback on failure. */
    bool hwaccelEncode{true};

    // Master switch and kill switch for both of the above
    bool preferHwaccel{true};
    bool forceSoftAv{false};
    int hwaccelDeviceId{-1};  // <0 → use gpuDeviceId after parse (CUDA only)
    std::string nvencPreset{"p3"};
} Config;

/** Effective hardware decode request after the master / kill switches. */
inline bool hwaccelDecodeEnabled(const Config& c) {
    return c.preferHwaccel && !c.forceSoftAv && c.hwaccelDecode && c.hwaccel != "none";
}

/** Effective hardware encode request after the master / kill switches. */
inline bool hwaccelEncodeEnabled(const Config& c) {
    return c.preferHwaccel && !c.forceSoftAv && c.hwaccelEncode && c.hwaccel != "none";
}

#endif //CONFIG_H
