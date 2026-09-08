#include "RTMPEncoder.h"
#include <climits>
#include <cstdio>
#include <cstring>
#include <glog/logging.h>
#include <algorithm>
#include <vector>

#ifdef RUNTIME_WITH_MPP
#include "mpp/MppEncoder.h"
#endif

RTMPEncoder::RTMPEncoder()
    : _outputCtx(nullptr)
    , _codecCtx(nullptr)
    , _videoStream(nullptr)
    , _swsCtx(nullptr)
    , _yuvFrame(nullptr)
    , _packet(nullptr)
    , _mpp(nullptr)
    , _frameIndex(0)
    , _srcWidth(0)
    , _srcHeight(0)
    , _encWidth(0)
    , _encHeight(0)
    , _fps(0)
    , _initialized(false)
{
}

RTMPEncoder::~RTMPEncoder() {
    release();
}

int RTMPEncoder::alignDim(int v, int align) {
    if (v <= 0) return align;
    return (v + align - 1) / align * align;
}

int64_t RTMPEncoder::defaultBitRate(int width, int height) {
    // RUNTIME keeps source resolution (often 1080p/4K). VIDEO's Python path often
    // scales to 720p @ 3500k; at full res we need higher ABR or the picture looks soft.
    const int64_t pixels = static_cast<int64_t>(std::max(1, width)) * std::max(1, height);
    if (pixels <= 640LL * 360) {
        return 1500000;
    }
    if (pixels <= 1280LL * 720) {
        return 3500000;
    }
    if (pixels <= 1920LL * 1080) {
        return 4500000;
    }
    if (pixels <= 2560LL * 1440) {
        return 6000000;
    }
    return 8000000;
}

bool RTMPEncoder::openEncoder(const AVCodec* codec, bool isNvenc, const RtmpEncoderOptions& opts) {
    _codecCtx = avcodec_alloc_context3(codec);
    if (!_codecCtx) {
        LOG(ERROR) << "[RTMP] Failed to allocate codec context";
        return false;
    }

    const int64_t bitRate = opts.bitRate > 0 ? opts.bitRate : defaultBitRate(_encWidth, _encHeight);
    const int gop = opts.gopSize > 0 ? opts.gopSize : std::max(1, _fps * 2);

    _codecCtx->width = _encWidth;
    _codecCtx->height = _encHeight;
    _codecCtx->time_base = AVRational{1, _fps};
    _codecCtx->framerate = AVRational{_fps, 1};
    _codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    _codecCtx->bit_rate = bitRate;
    // Align VIDEO: ~2s keyframe interval; short GOP wastes bitrate on I-frames → blurrier P-frames.
    _codecCtx->gop_size = gop;
    _codecCtx->keyint_min = std::max(1, _fps);
    _codecCtx->max_b_frames = 0;
    // bufsize 2x bitrate avoids RC starvation that makes the picture mushy under motion.
    _codecCtx->rc_buffer_size = static_cast<int>(std::min<int64_t>(bitRate * 2, INT_MAX));
    _codecCtx->rc_max_rate = bitRate;
    // Do not clamp rc_min_rate: hard min forces CBR and hurts perceived clarity.

    if (_outputCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        _codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (isNvenc) {
        const std::string preset = opts.nvencPreset.empty() ? "p3" : opts.nvencPreset;
        av_opt_set(_codecCtx->priv_data, "preset", preset.c_str(), 0);
        av_opt_set(_codecCtx->priv_data, "tune", "ll", 0);
        av_opt_set(_codecCtx->priv_data, "rc", "vbr", 0);
        av_opt_set(_codecCtx->priv_data, "profile", "main", 0);
        // Mild CQ bias toward clarity while still respecting bit_rate / rc_max_rate.
        av_opt_set(_codecCtx->priv_data, "cq", "19", 0);
        char gpuBuf[16];
        snprintf(gpuBuf, sizeof(gpuBuf), "%d", opts.gpuDeviceId < 0 ? 0 : opts.gpuDeviceId);
        av_opt_set(_codecCtx->priv_data, "gpu", gpuBuf, 0);
        _codecCtx->thread_count = 1;
    } else {
        _codecCtx->thread_count = 4;
        av_opt_set(_codecCtx->priv_data, "preset", "veryfast", 0);
        av_opt_set(_codecCtx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(_codecCtx->priv_data, "profile", "main", 0);
        // ABR only (no CRF): mixing CRF with bit_rate/rc_* fights and often looks worse live.
    }

    int ret = avcodec_open2(_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG(WARNING) << "[RTMP] Failed to open codec " << codec->name << ": " << errbuf;
        avcodec_free_context(&_codecCtx);
        _codecCtx = nullptr;
        return false;
    }

    LOG(INFO) << "[RTMP] Codec open " << codec->name
              << " bitrate=" << (bitRate / 1000) << "k"
              << " gop=" << gop
              << " bufsize=" << (_codecCtx->rc_buffer_size / 1000) << "k"
              << " " << _encWidth << "x" << _encHeight << "@" << _fps << "fps";
    return true;
}

bool RTMPEncoder::setupVideoStream() {
    _videoStream = avformat_new_stream(_outputCtx, nullptr);
    if (!_videoStream) {
        LOG(ERROR) << "[RTMP] Failed to create video stream";
        return false;
    }

#ifdef RUNTIME_WITH_MPP
    if (_mpp) {
        _videoStream->time_base = AVRational{1, _fps};
        _videoStream->avg_frame_rate = AVRational{_fps, 1};

        // There is no AVCodecContext to copy from, so describe the elementary
        // stream by hand. FLV insists on an AVCC AVCDecoderConfigurationRecord
        // in extradata, which is exactly what MppEncoder::extradata() holds.
        AVCodecParameters* par = _videoStream->codecpar;
        par->codec_type = AVMEDIA_TYPE_VIDEO;
        par->codec_id = AV_CODEC_ID_H264;
        par->width = _mpp->width();
        par->height = _mpp->height();
        // Coded surface format; the NV12 <-> I420 distinction never reaches the
        // container, so report what a decoder will hand out.
        par->format = AV_PIX_FMT_YUV420P;
        par->framerate = _videoStream->avg_frame_rate;

        const std::vector<uint8_t>& extra = _mpp->extradata();
        if (extra.empty()) {
            LOG(ERROR) << "[RTMP] rkmpp produced no SPS/PPS extradata";
            return false;
        }
        par->extradata = static_cast<uint8_t*>(
            av_malloc(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!par->extradata) {
            par->extradata_size = 0;
            LOG(ERROR) << "[RTMP] Failed to allocate extradata";
            return false;
        }
        memcpy(par->extradata, extra.data(), extra.size());
        memset(par->extradata + extra.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        par->extradata_size = static_cast<int>(extra.size());

        LOG(INFO) << "[RTMP] Stream ready encode_ep=rkmpp " << par->width << "x" << par->height
                  << "@" << _fps << "fps avcc_header=" << extra.size() << "B";
        return true;
    }
#endif

    _videoStream->time_base = _codecCtx->time_base;
    _videoStream->avg_frame_rate = _codecCtx->framerate;

    const int ret = avcodec_parameters_from_context(_videoStream->codecpar, _codecCtx);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG(ERROR) << "[RTMP] Failed to copy codec parameters: " << errbuf;
        return false;
    }
    return true;
}

int64_t RTMPEncoder::nextWallTick(int64_t captureNs) {
    // Wall-clock PTS derivation, expressed in **milliseconds** (i.e. a fixed
    // AVRational{1, 1000} time base) so it stays independent of whatever the
    // muxer happens to rewrite _videoStream->time_base to after write_header.
    // writeAvccPacket() rescales these ms into the current stream time_base at
    // the exact moment it needs one, and the libx264/NVENC path rescales into
    // _codecCtx->time_base before avcodec_receive_packet() runs its own
    // rescale.  This mirrors VIDEO's Python relay default of `-fps_mode vfr`,
    // so a slow inference loop no longer stretches or squeezes the timeline
    // and the player stops seeing "the last few seconds repeated / jittering".
    if (captureNs <= 0) {
        return -1;
    }
    if (_firstCaptureNs == 0) {
        _firstCaptureNs = captureNs;
    }
    int64_t elapsedNs = captureNs - _firstCaptureNs;
    if (elapsedNs < 0) {
        // Backwards clock jump (should not happen with MONOTONIC, defensive only).
        elapsedNs = 0;
    }
    int64_t ms = elapsedNs / 1000000;
    if (_lastWallTick >= 0 && ms <= _lastWallTick) {
        ms = _lastWallTick + 1;
    }
    _lastWallTick = ms;
    return ms;
}

bool RTMPEncoder::writeAvccPacket(const uint8_t* data, int size, int64_t ptsMs, bool key) {
    if (!_outputCtx || !_videoStream || !_packet) {
        return false;
    }

    int ret = av_new_packet(_packet, size);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG(ERROR) << "[RTMP] Failed to allocate mux packet: " << errbuf;
        return false;
    }
    memcpy(_packet->data, data, static_cast<size_t>(size));

    // `ptsMs` is a millisecond-unit wall-clock tick produced by nextWallTick().
    // We rescale here (rather than earlier) because FLV rewrites
    // _videoStream->time_base to {1, 1000} during avformat_write_header(),
    // which would otherwise double-scale MPP-path pts and make the timeline
    // grow at ~fps/1000 of real time -- exactly the "recent-seconds jitter +
    // repeated frames" symptom we are fixing.
    const int64_t ts = av_rescale_q(ptsMs, AVRational{1, 1000}, _videoStream->time_base);
    _packet->pts = ts;
    _packet->dts = ts;  // VEPU and libx264 both run with no B frames
    _packet->duration = av_rescale_q(1, AVRational{1, 1000}, _videoStream->time_base);
    _packet->stream_index = _videoStream->index;
    _packet->flags = key ? AV_PKT_FLAG_KEY : 0;

    ret = av_interleaved_write_frame(_outputCtx, _packet);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG(ERROR) << "[RTMP] Failed to write frame: " << errbuf;
        av_packet_unref(_packet);
        return false;
    }
    av_packet_unref(_packet);
    return true;
}

#ifdef RUNTIME_WITH_MPP
bool RTMPEncoder::openMppEncoder(const RtmpEncoderOptions& opts) {
    std::string reason;
    if (!runtime::mpp::MppEncoder::hostSupported(&reason)) {
        LOG(INFO) << "[RTMP] rkmpp skipped: " << reason;
        return false;
    }

    const int64_t bitRate = opts.bitRate > 0 ? opts.bitRate : defaultBitRate(_encWidth, _encHeight);
    const int gop = opts.gopSize > 0 ? opts.gopSize : std::max(1, _fps * 2);

    runtime::mpp::MppEncoder* enc = new runtime::mpp::MppEncoder();
    if (!enc->open(_encWidth, _encHeight, _fps,
                   static_cast<int>(std::min<int64_t>(bitRate, INT_MAX)), gop)) {
        LOG(WARNING) << "[RTMP] VEPU open failed: " << enc->lastError();
        delete enc;
        return false;
    }
    _mpp = enc;

    LOG(INFO) << "[RTMP] Using rkmpp (VEPU)"
              << " " << _mpp->width() << "x" << _mpp->height()
              << " stride=" << _mpp->horStride() << "x" << _mpp->verStride()
              << " bitrate=" << (bitRate / 1000) << "k"
              << " gop=" << gop << "@" << _fps << "fps";
    return true;
}

bool RTMPEncoder::encodeAndPushMpp(const cv::Mat& frame, int64_t captureNs) {
    cv::Mat bgr = frame;
    if (frame.cols != _srcWidth || frame.rows != _srcHeight) {
        cv::resize(frame, bgr, cv::Size(_srcWidth, _srcHeight), 0, 0, cv::INTER_AREA);
    }

    // VEPU is asynchronous: it holds up to kInputSlots frames. Recycle eagerly so
    // the common steady-state case never trips the "queue full" path below.
    std::vector<runtime::mpp::MppEncPacket> packets;
    _mpp->drain(&packets);

    uint8_t* luma = _mpp->acquireInput();
    if (!luma) {
        LOG_EVERY_N(WARNING, 60) << "[RTMP] VEPU input queue full, frame dropped";
        for (size_t i = 0; i < packets.size(); ++i) {
            const runtime::mpp::MppEncPacket& pkt = packets[i];
            writeAvccPacket(pkt.data.data(), static_cast<int>(pkt.data.size()), pkt.pts, pkt.key);
        }
        return false;
    }
    uint8_t* dstData[2] = {luma, _mpp->acquireInputChroma()};
    const int stride = _mpp->horStride();
    int dstLinesize[2] = {stride, stride};

    const uint8_t* srcData[1] = {bgr.data};
    int srcLinesize[1] = {static_cast<int>(bgr.step[0])};

    if (sws_scale(_swsCtx, srcData, srcLinesize, 0, _srcHeight, dstData, dstLinesize) < 0) {
        LOG(ERROR) << "[RTMP] Failed to convert BGR to NV12";
        return false;
    }

    // Wall-clock tick in the mux time_base.  MppEncoder echoes it back on the
    // corresponding MppEncPacket so muxing sees a true VFR stream.  -1 tells
    // MppEncoder to keep using its legacy per-frame counter.
    const int64_t wallTick = nextWallTick(captureNs);

    if (!_mpp->submit(wallTick)) {
        LOG(ERROR) << "[RTMP] VEPU submit failed: " << _mpp->lastError();
        return false;
    }
    _frameIndex++;

    _mpp->drain(&packets);
    for (size_t i = 0; i < packets.size(); ++i) {
        const runtime::mpp::MppEncPacket& pkt = packets[i];
        if (pkt.data.empty()) continue;
        if (!writeAvccPacket(pkt.data.data(), static_cast<int>(pkt.data.size()),
                             pkt.pts, pkt.key)) {
            return false;
        }
    }
    return true;
}
#endif  // RUNTIME_WITH_MPP

bool RTMPEncoder::init(const std::string& rtmpUrl, int width, int height, int fps,
                       const RtmpEncoderOptions& opts) {
    if (_initialized) {
        LOG(WARNING) << "[RTMP] Encoder already initialized";
        return true;
    }

    _rtmpUrl = rtmpUrl;
    _srcWidth = width;
    _srcHeight = height;
    _fps = fps > 0 ? fps : 25;
    _encodeEp = "none";

    const std::string family = opts.hwaccel.empty() ? std::string("auto") : opts.hwaccel;
    const bool wantHw = opts.preferHw && !opts.forceSoft && family != "none";
    const bool tryNvenc = wantHw && (family == "auto" || family == "cuda");
    // Stays false in a build without the MPP backend, so hwaccel=rkmpp there
    // correctly degrades to the software path below.
    bool tryRkmpp = false;
#ifdef RUNTIME_WITH_MPP
    tryRkmpp = wantHw && (family == "auto" || family == "rkmpp");
#endif
    if (wantHw && !tryRkmpp && !tryNvenc) {
        LOG(WARNING) << "[RTMP] hwaccel=" << family << " unusable here, using software encode";
    }

    if (wantHw) {
        // Both hardware encoders want an aligned picture; libx264 copes with odd sizes.
        _encWidth = alignDim(width);
        _encHeight = alignDim(height);
    } else {
        _encWidth = width;
        _encHeight = height;
    }

    LOG(INFO) << "[RTMP] Initializing encoder: " << rtmpUrl
              << " (" << width << "x" << height << " -> " << _encWidth << "x" << _encHeight
              << "@" << _fps << "fps)"
              << " hwaccel=" << family
              << " prefer_hw=" << (opts.preferHw ? "true" : "false")
              << " force_soft=" << (opts.forceSoft ? "true" : "false")
              << " bitrate_hint=" << (opts.bitRate > 0 ? opts.bitRate / 1000 : 0) << "k";

    int ret = avformat_alloc_output_context2(&_outputCtx, nullptr, "flv", rtmpUrl.c_str());
    if (ret < 0 || !_outputCtx) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG(ERROR) << "[RTMP] Failed to create output context: " << errbuf;
        return false;
    }

    bool opened = false;
#ifdef RUNTIME_WITH_MPP
    if (tryRkmpp) {
        if (openMppEncoder(opts)) {
            _encodeEp = "rkmpp";
            opened = true;
        } else {
            LOG(WARNING) << "[RTMP] rkmpp (VEPU) unavailable, falling back to FFmpeg encode";
            _encWidth = width;
            _encHeight = height;
        }
    }
#endif

    if (!opened && tryNvenc) {
        const AVCodec* nvenc = avcodec_find_encoder_by_name("h264_nvenc");
        if (nvenc) {
            if (openEncoder(nvenc, true, opts)) {
                _encodeEp = "h264_nvenc";
                opened = true;
                LOG(INFO) << "[RTMP] Using h264_nvenc preset=" << opts.nvencPreset
                          << " gpu=" << opts.gpuDeviceId;
            } else {
                LOG(WARNING) << "[RTMP] h264_nvenc open failed, falling back to libx264";
            }
        } else {
            LOG(INFO) << "[RTMP] h264_nvenc not found in FFmpeg, using libx264";
        }
        if (!opened) {
            _encWidth = width;
            _encHeight = height;
        }
    }

    if (!opened) {
        const AVCodec* soft = avcodec_find_encoder_by_name("libx264");
        if (!soft) {
            soft = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        if (!soft) {
            LOG(ERROR) << "[RTMP] H.264 codec not found";
            release();
            return false;
        }
        if (!openEncoder(soft, false, opts)) {
            LOG(ERROR) << "[RTMP] Failed to open libx264";
            release();
            return false;
        }
        _encodeEp = "libx264";
    }

    if (!setupVideoStream()) {
        release();
        return false;
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtmp_buffer", "100", 0);
    av_dict_set(&options, "rtmp_live", "live", 0);
    av_dict_set(&options, "buffer_size", "65536", 0);

    if (!((_outputCtx->oformat->flags & AVFMT_NOFILE))) {
        ret = avio_open2(&_outputCtx->pb, rtmpUrl.c_str(), AVIO_FLAG_WRITE, nullptr, &options);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG(ERROR) << "[RTMP] Failed to open RTMP URL: " << errbuf
                      << " (URL: " << rtmpUrl << ")";
            av_dict_free(&options);
            release();
            return false;
        }
    }
    av_dict_free(&options);

    AVDictionary* muxer_opts = nullptr;
    av_dict_set(&muxer_opts, "flvflags", "no_duration_filesize", 0);
    av_dict_set(&muxer_opts, "fflags", "nobuffer", 0);

    ret = avformat_write_header(_outputCtx, &muxer_opts);
    av_dict_free(&muxer_opts);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG(ERROR) << "[RTMP] Failed to write header: " << errbuf;
        release();
        return false;
    }

    // BGR -> YUV；NVENC 16 对齐偶发缩放时用 bicubic，比 bilinear 更锐。
    // VEPU 只吃 NV12（半平面），且目标行距是它自己算出的 hor_stride，
    // 所以 rkmpp 路径下 dst 平面每帧从 MppBuffer 现取，见 encodeAndPushMpp()。
    const AVPixelFormat dstFmt = (_encodeEp == "rkmpp") ? AV_PIX_FMT_NV12
                                                        : AV_PIX_FMT_YUV420P;
    _swsCtx = sws_getContext(
        _srcWidth, _srcHeight, AV_PIX_FMT_BGR24,
        _encWidth, _encHeight, dstFmt,
        SWS_BICUBIC, nullptr, nullptr, nullptr
    );
    if (!_swsCtx) {
        LOG(ERROR) << "[RTMP] Failed to create sws context";
        release();
        return false;
    }

    if (!_mpp) {
        _yuvFrame = av_frame_alloc();
        if (!_yuvFrame) {
            LOG(ERROR) << "[RTMP] Failed to allocate YUV frame";
            release();
            return false;
        }

        _yuvFrame->format = dstFmt;
        _yuvFrame->width = _encWidth;
        _yuvFrame->height = _encHeight;

        ret = av_frame_get_buffer(_yuvFrame, 0);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG(ERROR) << "[RTMP] Failed to allocate frame buffer: " << errbuf;
            release();
            return false;
        }
    }

    _packet = av_packet_alloc();
    if (!_packet) {
        LOG(ERROR) << "[RTMP] Failed to allocate packet";
        release();
        return false;
    }

    _initialized = true;
    _frameIndex = 0;
    _firstCaptureNs = 0;
    _lastWallTick = -1;

    LOG(INFO) << "[RTMP] Encoder initialized successfully encode_ep=" << _encodeEp
              << " url=" << rtmpUrl;
    return true;
}

bool RTMPEncoder::encodeAndPush(const cv::Mat& frame, int64_t captureNs) {
    if (!_initialized) {
        LOG(ERROR) << "[RTMP] Encoder not initialized";
        return false;
    }

    if (frame.empty()) {
        LOG(WARNING) << "[RTMP] Empty frame received";
        return false;
    }

#ifdef RUNTIME_WITH_MPP
    if (_mpp) {
        return encodeAndPushMpp(frame, captureNs);
    }
#endif

    cv::Mat bgr = frame;
    if (frame.cols != _srcWidth || frame.rows != _srcHeight) {
        // Unexpected size: scale to encoder source geometry
        cv::resize(frame, bgr, cv::Size(_srcWidth, _srcHeight), 0, 0, cv::INTER_AREA);
    }

    const uint8_t* srcData[1] = {bgr.data};
    int srcLinesize[1] = {static_cast<int>(bgr.step[0])};

    int ret = sws_scale(_swsCtx, srcData, srcLinesize, 0, _srcHeight,
                       _yuvFrame->data, _yuvFrame->linesize);
    if (ret < 0) {
        LOG(ERROR) << "[RTMP] Failed to convert color space";
        return false;
    }

    // Wall-clock tick in the codec time_base (mirrors the MPP path above).
    // x264/NVENC are configured with no B frames, so input and output picture
    // order stay 1:1 and the avcodec-internal PTS we assign here reaches
    // av_packet_rescale_ts() unchanged on the mux side.  When captureNs is
    // unavailable we fall back to the legacy frame-index CFR, matching the
    // previous behaviour.
    const int64_t wallTick = nextWallTick(captureNs);
    if (wallTick >= 0) {
        // `wallTick` is in milliseconds; x264/NVENC expect pts in
        // _codecCtx->time_base (typically {1, 1/fps} or {1, 1000000}).
        _yuvFrame->pts = av_rescale_q(wallTick, AVRational{1, 1000}, _codecCtx->time_base);
    } else {
        _yuvFrame->pts = _frameIndex;
    }
    _frameIndex++;

    ret = avcodec_send_frame(_codecCtx, _yuvFrame);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG(ERROR) << "[RTMP] Failed to send frame: " << errbuf;
        return false;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(_codecCtx, _packet);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG(ERROR) << "[RTMP] Failed to receive packet: " << errbuf;
            return false;
        }

        av_packet_rescale_ts(_packet, _codecCtx->time_base, _videoStream->time_base);
        _packet->stream_index = _videoStream->index;

        ret = av_interleaved_write_frame(_outputCtx, _packet);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG(ERROR) << "[RTMP] Failed to write frame: " << errbuf;
            av_packet_unref(_packet);
            return false;
        }

        av_packet_unref(_packet);
    }

    return true;
}

void RTMPEncoder::release() {
    if (!_initialized && !_outputCtx) {
        return;
    }

    LOG(INFO) << "[RTMP] Releasing encoder resources encode_ep=" << _encodeEp;

#ifdef RUNTIME_WITH_MPP
    if (_mpp) {
        // VEPU holds the last few frames in flight; push EOS and collect them
        // before the trailer, otherwise the stream ends short.
        std::vector<runtime::mpp::MppEncPacket> tail;
        bool drained = false;
        for (int guard = 0; guard < 64 && !drained; ++guard) {
            drained = _mpp->flush(&tail);
        }
        if (_initialized && _outputCtx && _videoStream) {
            for (size_t i = 0; i < tail.size(); ++i) {
                const runtime::mpp::MppEncPacket& pkt = tail[i];
                if (pkt.data.empty()) continue;
                writeAvccPacket(pkt.data.data(), static_cast<int>(pkt.data.size()),
                                pkt.pts, pkt.key);
            }
        }
        if (!drained) {
            LOG(WARNING) << "[RTMP] VEPU flush incomplete, pending=" << _mpp->freeInputSlots();
        }
        delete _mpp;
        _mpp = nullptr;
    }
#endif

    if (_codecCtx && _initialized) {
        avcodec_send_frame(_codecCtx, nullptr);

        while (true) {
            int ret = avcodec_receive_packet(_codecCtx, _packet);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                break;
            }
            if (ret >= 0) {
                av_packet_rescale_ts(_packet, _codecCtx->time_base, _videoStream->time_base);
                _packet->stream_index = _videoStream->index;
                av_interleaved_write_frame(_outputCtx, _packet);
                av_packet_unref(_packet);
            }
        }
    }

    if (_outputCtx && _initialized) {
        av_write_trailer(_outputCtx);
    }

    if (_outputCtx) {
        if (!(_outputCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&_outputCtx->pb);
        }
        avformat_free_context(_outputCtx);
        _outputCtx = nullptr;
    }

    if (_codecCtx) {
        avcodec_free_context(&_codecCtx);
        _codecCtx = nullptr;
    }

    if (_swsCtx) {
        sws_freeContext(_swsCtx);
        _swsCtx = nullptr;
    }

    if (_yuvFrame) {
        av_frame_free(&_yuvFrame);
        _yuvFrame = nullptr;
    }

    if (_packet) {
        av_packet_free(&_packet);
        _packet = nullptr;
    }

    _initialized = false;
    _frameIndex = 0;
    _encodeEp = "none";

    LOG(INFO) << "[RTMP] Encoder resources released";
}
