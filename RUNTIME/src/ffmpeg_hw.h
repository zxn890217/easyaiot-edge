#ifndef RUNTIME_FFMPEG_H
#define RUNTIME_FFMPEG_H

#include <string>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/buffer.h"
#include "libavutil/hwcontext.h"
#include "libavutil/pixfmt.h"
}

namespace runtime {
namespace mpp {
class MppDecoder;
}  // namespace mpp

struct HwDecodeState {
    AVBufferRef* hwDeviceCtx{nullptr};
    bool usingCuda{false};
    /**
     * rkvdec owns the decode. The AVCodecContext output pointer stays null
     * because FFmpeg never decodes in this mode - demux only. Neither FFmpeg
     * build on the board ships the rkmpp wrappers, hence the direct MPP path.
     */
    bool usingMpp{false};
    mpp::MppDecoder* mpp{nullptr};
    /** Coded geometry from AVCodecParameters (codecCtx is null on the MPP path). */
    int codedWidth{0};
    int codedHeight{0};
    std::string decodeEp{"cpu"};
};

/** Create CUDA hwdevice context for deviceId. Returns true on success. */
bool createCudaHwDevice(AVBufferRef** out, int deviceId);

/**
 * Open a video decoder. When preferHw && !forceSoft, tries the hardware family
 * named by hwaccel ("auto" = rkmpp then cuda). On failure falls back to software
 * decode once.
 *
 * On success *codecCtxOut is allocated and opened, EXCEPT on the rkmpp path
 * where it is set to null and state->usingMpp is true; callers must then pull
 * pictures from state->mpp instead of the codec context.
 */
bool openVideoDecoder(AVCodecContext** codecCtxOut,
                      AVCodecParameters* par,
                      bool preferHw,
                      bool forceSoft,
                      int deviceId,
                      HwDecodeState* state,
                      const std::string& hwaccel = "auto");

void releaseHwDecodeState(HwDecodeState* state);

/** True if frame is a CUDA hw frame. */
bool isCudaHwFrame(const AVFrame* frame);

/**
 * If src is CUDA, transfer to *dst (must be allocated empty AVFrame).
 * Returns pointer to software frame to feed sws (dst on transfer, else src).
 * On transfer failure returns nullptr.
 */
AVFrame* ensureSoftwareFrame(AVFrame* src, AVFrame* dst);

}  // namespace runtime

#endif
