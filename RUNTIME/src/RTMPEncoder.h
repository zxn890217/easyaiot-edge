#ifndef RTMP_ENCODER_H
#define RTMP_ENCODER_H

#include <cstdint>
#include <string>
#include <opencv2/opencv.hpp>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
}

/*
 * Held behind a pointer so rockchip/rk_mpi.h never leaks into every TU that
 * includes this header. The destructor lives in RTMPEncoder.cpp.
 */
namespace runtime {
namespace mpp {
class MppEncoder;
}  // namespace mpp
}  // namespace runtime

struct RtmpEncoderOptions {
    bool preferHw{true};
    bool forceSoft{false};
    int gpuDeviceId{0};
    std::string nvencPreset{"p3"};
    /** Target ABR bitrate in bits/sec; <=0 = auto by resolution (align VIDEO clarity). */
    int64_t bitRate{0};
    /** GOP size in frames; <=0 = 2 * fps (keyframe ~every 2s). */
    int gopSize{0};
    /**
     * Hardware codec family: auto | cuda | rkmpp | none.
     * "auto" tries VEPU (rkmpp) first, then h264_nvenc, then libx264. On an
     * RK3588 box there is no CUDA device, so without this knob the CPU encoder
     * was the only option and ate a third of the box.
     */
    std::string hwaccel{"auto"};
};

/**
 * RTMP推流编码器
 * 功能：将OpenCV Mat图像编码为H.264并推送到RTMP服务器
 * 优先硬件编码器（rkmpp/VEPU 或 h264_nvenc），失败回退 libx264
 */
class RTMPEncoder {
public:
    RTMPEncoder();
    ~RTMPEncoder();

    bool init(const std::string& rtmpUrl, int width, int height, int fps,
              const RtmpEncoderOptions& opts = RtmpEncoderOptions());

    bool encodeAndPush(const cv::Mat& frame);

    void release();

    bool isInitialized() const { return _initialized; }

    /** rkmpp | h264_nvenc | libx264 | none */
    const std::string& encodeEp() const { return _encodeEp; }

private:
    bool openEncoder(const AVCodec* codec, bool isNvenc, const RtmpEncoderOptions& opts);
#ifdef RUNTIME_WITH_MPP
    /** Open VEPU and publish its SPS/PPS as the stream's AVCC extradata. */
    bool openMppEncoder(const RtmpEncoderOptions& opts);
    /** BGR -> NV12 straight into the VEPU input buffer, then mux what came back. */
    bool encodeAndPushMpp(const cv::Mat& frame);
#endif
    /** Describe the elementary stream to the FLV muxer (both encoder families). */
    bool setupVideoStream();
    /** Mux one already-encoded H.264 AVCC access unit. */
    bool writeAvccPacket(const uint8_t* data, int size, int64_t frameIndex, bool key);
    static int alignDim(int v, int align = 16);
    static int64_t defaultBitRate(int width, int height);

    AVFormatContext* _outputCtx;    // 输出格式上下文
    AVCodecContext* _codecCtx;      // 编码器上下文（rkmpp 路径下保持 null）
    AVStream* _videoStream;         // 视频流
    SwsContext* _swsCtx;            // 颜色空间转换上下文
    AVFrame* _yuvFrame;             // YUV帧（rkmpp 直接写 MppBuffer，不使用）
    AVPacket* _packet;              // 编码后的数据包
    runtime::mpp::MppEncoder* _mpp; // VEPU 编码器（未编译 MPP 后端时恒为 null）

    int64_t _frameIndex;
    int _srcWidth;                  // OpenCV 输入宽
    int _srcHeight;
    int _encWidth;                  // 编码器宽（NVENC 16 对齐）
    int _encHeight;
    int _fps;
    std::string _rtmpUrl;
    std::string _encodeEp{"none"};
    bool _initialized;
};

#endif // RTMP_ENCODER_H
