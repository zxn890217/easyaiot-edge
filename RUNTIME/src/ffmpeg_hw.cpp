#include "ffmpeg_hw.h"

#include <cstdio>
#include <vector>
#include <glog/logging.h>

extern "C" {
#include "libavutil/error.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
}

#ifdef RUNTIME_WITH_MPP
#include "mpp/H264Bits.h"
#include "mpp/MppDecoder.h"
#endif

namespace runtime {

namespace {

enum AVPixelFormat getHwFormatCuda(AVCodecContext* /*ctx*/, const enum AVPixelFormat* pixFmts) {
    for (const enum AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_CUDA) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

bool openSoftDecoder(AVCodecContext** codecCtxOut, AVCodecParameters* par) {
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        LOG(ERROR) << "[HW] avcodec_find_decoder failed codec_id=" << par->codec_id;
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        return false;
    }
    if (avcodec_parameters_to_context(ctx, par) != 0) {
        avcodec_free_context(&ctx);
        return false;
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return false;
    }
    *codecCtxOut = ctx;
    return true;
}

bool openCudaDecoder(AVCodecContext** codecCtxOut,
                     AVCodecParameters* par,
                     int deviceId,
                     HwDecodeState* state) {
    if (!createCudaHwDevice(&state->hwDeviceCtx, deviceId)) {
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        releaseHwDecodeState(state);
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        releaseHwDecodeState(state);
        return false;
    }
    if (avcodec_parameters_to_context(ctx, par) != 0) {
        avcodec_free_context(&ctx);
        releaseHwDecodeState(state);
        return false;
    }

    ctx->hw_device_ctx = av_buffer_ref(state->hwDeviceCtx);
    if (!ctx->hw_device_ctx) {
        avcodec_free_context(&ctx);
        releaseHwDecodeState(state);
        return false;
    }
    ctx->get_format = getHwFormatCuda;

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        LOG(WARNING) << "[HW] CUDA decoder open failed, will fall back to software";
        avcodec_free_context(&ctx);
        releaseHwDecodeState(state);
        return false;
    }

    *codecCtxOut = ctx;
    state->usingCuda = true;
    state->decodeEp = "cuda";
    LOG(INFO) << "[HW] NVDEC enabled decode_ep=cuda device_id=" << deviceId
              << " codec=" << codec->name;
    return true;
}

#ifdef RUNTIME_WITH_MPP

// rkvdec is picky about coding types, and the two the probe confirmed on this
// board are exactly the two IP cameras emit. Anything else goes to software
// rather than risk an MppCtx that silently produces nothing.
bool mppCodingFor(enum AVCodecID id, MppCodingType* coding, const char** name) {
    switch (id) {
        case AV_CODEC_ID_H264:
            *coding = MPP_VIDEO_CodingAVC;
            *name = "h264";
            return true;
        case AV_CODEC_ID_HEVC:
            *coding = MPP_VIDEO_CodingHEVC;
            *name = "hevc";
            return true;
        default:
            return false;
    }
}

void appendStartCode(std::vector<uint8_t>* out, const uint8_t* nal, size_t len) {
    static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    out->insert(out->end(), kStartCode, kStartCode + 4);
    out->insert(out->end(), nal, nal + len);
}

// Shared walker for the "count, then (be16 length, NAL)*" runs inside both the
// AVC and the hvcC configuration records.
bool appendBe16NalArray(const uint8_t* d, size_t size, size_t* pos, int count,
                        std::vector<uint8_t>* out) {
    for (int i = 0; i < count; ++i) {
        if (*pos + 2 > size) {
            return false;
        }
        const size_t len = (static_cast<size_t>(d[*pos]) << 8) | d[*pos + 1];
        *pos += 2;
        if (len == 0 || *pos + len > size) {
            return false;
        }
        appendStartCode(out, d + *pos, len);
        *pos += len;
    }
    return true;
}

/** AVCDecoderConfigurationRecord -> Annex-B parameter sets. */
bool h264ConfigToAnnexB(const uint8_t* d, size_t size, std::vector<uint8_t>* out) {
    if (size < 7 || d[0] != 1) {
        return false;
    }
    size_t pos = 6;
    if (!appendBe16NalArray(d, size, &pos, d[5] & 0x1f, out)) {
        return false;
    }
    if (pos + 1 > size) {
        return false;
    }
    const int numPps = d[pos++];
    return appendBe16NalArray(d, size, &pos, numPps, out) && !out->empty();
}

/** HEVCDecoderConfigurationRecord -> Annex-B parameter sets. */
bool hevcConfigToAnnexB(const uint8_t* d, size_t size, std::vector<uint8_t>* out) {
    if (size < 23 || d[0] != 1) {
        return false;
    }
    size_t pos = 23;
    const int numArrays = d[22];
    for (int i = 0; i < numArrays; ++i) {
        if (pos + 3 > size) {
            return false;
        }
        const int numNalus = (static_cast<size_t>(d[pos + 1]) << 8) | d[pos + 2];
        pos += 3;  // skips array_completeness | NAL_unit_type as well
        if (!appendBe16NalArray(d, size, &pos, numNalus, out)) {
            return false;
        }
    }
    return !out->empty();
}

/**
 * RTSP/RTMP H.264 frequently carries SPS/PPS only in the SDP fmtp line, i.e. in
 * AVCodecParameters::extradata and never in-band. rkvdec cannot start without
 * them, so the parameter sets are handed over as one Annex-B packet first.
 */
bool extradataToAnnexB(AVCodecParameters* par, std::vector<uint8_t>* out) {
    const uint8_t* d = par->extradata;
    const size_t size = static_cast<size_t>(par->extradata_size);
    if (!d || size == 0) {
        return true;  // nothing to prime with, in-band parameter sets will do
    }
    if (mpp::looksLikeAnnexB(d, size)) {
        out->assign(d, d + size);
        return true;
    }
    if (par->codec_id == AV_CODEC_ID_H264) {
        return h264ConfigToAnnexB(d, size, out);
    }
    if (par->codec_id == AV_CODEC_ID_HEVC) {
        return hevcConfigToAnnexB(d, size, out);
    }
    return false;
}

bool primeDecoder(mpp::MppDecoder* dec, const std::vector<uint8_t>& annexb) {
    bool retry = false;
    for (int guard = 0; guard < 16; ++guard) {
        retry = false;
        if (dec->send(annexb.data(), annexb.size(), 0, &retry)) {
            return true;
        }
        if (!retry) {
            return false;
        }
        // Input slot still busy: only rkvdec draining a parameter-set packet can
        // free it, so pump once and try again.
        mpp::MppDecoder::Frame frame;
        if (!dec->receive(&frame)) {
            return false;
        }
        dec->release();
    }
    return false;
}

bool openMppDecoder(AVCodecContext** codecCtxOut, AVCodecParameters* par, HwDecodeState* state) {
    MppCodingType coding = MPP_VIDEO_CodingUnused;
    const char* codecName = nullptr;
    if (!mppCodingFor(par->codec_id, &coding, &codecName)) {
        LOG(INFO) << "[HW] rkmpp has no rkvdec profile for codec_id=" << par->codec_id;
        return false;
    }
    if (par->width <= 0 || par->height <= 0) {
        LOG(WARNING) << "[HW] rkmpp needs a coded size, got "
                     << par->width << "x" << par->height;
        return false;
    }

    mpp::MppDecoder* dec = new mpp::MppDecoder();
    if (!dec->open(static_cast<int>(coding), par->width, par->height)) {
        LOG(WARNING) << "[HW] rkvdec open failed: " << dec->lastError();
        delete dec;
        return false;
    }

    std::vector<uint8_t> annexb;
    if (!extradataToAnnexB(par, &annexb)) {
        LOG(WARNING) << "[HW] rkmpp: unrecognized " << codecName
                     << " extradata (" << par->extradata_size
                     << "B), relying on in-band parameter sets";
    } else if (!annexb.empty() && !primeDecoder(dec, annexb)) {
        LOG(WARNING) << "[HW] rkvdec failed to accept the parameter sets: "
                     << dec->lastError();
        delete dec;
        return false;
    }

    *codecCtxOut = nullptr;  // no FFmpeg decode on this path, demux only
    state->mpp = dec;
    state->usingMpp = true;
    state->codedWidth = par->width;
    state->codedHeight = par->height;
    state->decodeEp = "rkmpp";
    LOG(INFO) << "[HW] rkvdec enabled decode_ep=rkmpp codec=" << codecName
              << " " << par->width << "x" << par->height;
    return true;
}

#endif  // RUNTIME_WITH_MPP

}  // namespace

bool createCudaHwDevice(AVBufferRef** out, int deviceId) {
    if (!out) {
        return false;
    }
    *out = nullptr;

    enum AVHWDeviceType type = av_hwdevice_find_type_by_name("cuda");
    if (type == AV_HWDEVICE_TYPE_NONE) {
        LOG(INFO) << "[HW] FFmpeg build has no CUDA hwdevice";
        return false;
    }

    char device[16];
    snprintf(device, sizeof(device), "%d", deviceId < 0 ? 0 : deviceId);
    int ret = av_hwdevice_ctx_create(out, type, device, nullptr, 0);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG(WARNING) << "[HW] av_hwdevice_ctx_create(cuda) failed: " << errbuf;
        *out = nullptr;
        return false;
    }
    return true;
}

bool openVideoDecoder(AVCodecContext** codecCtxOut,
                      AVCodecParameters* par,
                      bool preferHw,
                      bool forceSoft,
                      int deviceId,
                      HwDecodeState* state,
                      const std::string& hwaccel) {
    if (!codecCtxOut || !par || !state) {
        return false;
    }
    *codecCtxOut = nullptr;
    releaseHwDecodeState(state);
    state->usingCuda = false;
    state->decodeEp = "cpu";

    const std::string family = hwaccel.empty() ? std::string("auto") : hwaccel;
    const bool tryHw = preferHw && !forceSoft;
    if (tryHw) {
#ifdef RUNTIME_WITH_MPP
        if (family == "auto" || family == "rkmpp") {
            if (openMppDecoder(codecCtxOut, par, state)) {
                return true;
            }
            if (family == "rkmpp") {
                LOG(WARNING) << "[HW] hwaccel=rkmpp unavailable, falling back to software decode";
            } else {
                LOG(INFO) << "[HW] rkmpp unavailable, trying CUDA";
            }
        }
#endif
        if (family == "auto" || family == "cuda") {
            if (openCudaDecoder(codecCtxOut, par, deviceId, state)) {
                return true;
            }
        }
        LOG(INFO) << "[HW] Falling back to software decode";
    }

    if (!openSoftDecoder(codecCtxOut, par)) {
        LOG(ERROR) << "[HW] Software decoder open failed";
        return false;
    }
    state->usingCuda = false;
    state->decodeEp = "cpu";
    LOG(INFO) << "[HW] Using software decode decode_ep=cpu";
    return true;
}

void releaseHwDecodeState(HwDecodeState* state) {
    if (!state) {
        return;
    }
    if (state->hwDeviceCtx) {
        av_buffer_unref(&state->hwDeviceCtx);
        state->hwDeviceCtx = nullptr;
    }
#ifdef RUNTIME_WITH_MPP
    delete state->mpp;
    state->mpp = nullptr;
#endif
    state->usingCuda = false;
    state->usingMpp = false;
    state->codedWidth = 0;
    state->codedHeight = 0;
}

bool isCudaHwFrame(const AVFrame* frame) {
    return frame && frame->format == AV_PIX_FMT_CUDA;
}

AVFrame* ensureSoftwareFrame(AVFrame* src, AVFrame* dst) {
    if (!src || !dst) {
        return nullptr;
    }
    if (!isCudaHwFrame(src)) {
        return src;
    }
    av_frame_unref(dst);
    int ret = av_hwframe_transfer_data(dst, src, 0);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG(WARNING) << "[HW] av_hwframe_transfer_data failed: " << errbuf;
        return nullptr;
    }
    dst->pts = src->pts;
    dst->pkt_dts = src->pkt_dts;
    return dst;
}

}  // namespace runtime
