#ifndef RUNTIME_MPP_MPPDECODER_H
#define RUNTIME_MPP_MPPDECODER_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "mpp/MppEnv.h"

namespace runtime {
namespace mpp {

/**
 * RK3588 rkvdec H.264/H.265 decoder driven straight through librockchip_mpp.
 *
 * Like the encoder, there is no FFmpeg route here: neither FFmpeg build on the
 * board carries the rkmpp wrappers. rkvdec takes Annex-B in and hands back NV12
 * in a buffer it allocated from our external buffer group, which is exactly the
 * layout sws_scale wants for the detect path.
 *
 * Reference accounting follows what the probe proved on the encode side: an
 * MppFrame holds its own reference on the buffer attached to it and
 * mpp_frame_deinit() drops it. We never called mpp_buffer_get() for a decoded
 * buffer, so we must never call mpp_buffer_put() on one either - doing that
 * would corrupt the group.
 *
 * Input side is trickier: mpp_packet_deinit() only drops our reference on the
 * packet shell, the payload bytes stay ours to keep alive. A ring of staging
 * buffers covers the window where rkvdec still reads them.
 */
class MppDecoder {
public:
    /** A decoded NV12 picture, valid until the next receive() or close(). */
    struct Frame {
        const uint8_t* planes[2] = {nullptr, nullptr};
        int linesize[2] = {0, 0};
        int width = 0;   // visible
        int height = 0;
        int64_t pts = 0;
    };

    MppDecoder();
    ~MppDecoder();

    MppDecoder(const MppDecoder&) = delete;
    MppDecoder& operator=(const MppDecoder&) = delete;

    static bool hostSupported(std::string* reason = nullptr);

    /**
     * coding is MPP_VIDEO_CodingAVC / MPP_VIDEO_CodingHEVC - mapped by the
     * caller from the AVCodecID so this class stays free of FFmpeg headers.
     * width/height are only hints used for logging and pre-sizing.
     */
    bool open(int coding, int widthHint, int heightHint);
    void close();

    bool isOpen() const { return ctx_ != nullptr; }
    const std::string& lastError() const { return lastError_; }
    /** Picture geometry as rkvdec last reported it. */
    int codedWidth() const { return width_; }
    int codedHeight() const { return height_; }

    /**
     * Hand one access unit to rkvdec. On false, `retry` reports whether the
     * bytes are queued internally and the call should be repeated after
     * receive() has drained something (input queue full) versus a hard error.
     * `data` may be freed as soon as this returns.
     */
    bool send(const uint8_t* data, size_t len, int64_t pts, bool* retry);

    /**
     * Take one decoded picture. Returns false when nothing is ready, when the
     * decoder is renegotiating geometry, or on a corrupt frame - all of which
     * are normal and need no action beyond trying again later.
     */
    bool receive(Frame* out);
    /** Drop the picture returned by the last receive(). */
    void release();

    uint64_t framesOut() const { return framesOut_; }

    /**
     * True once rkvdec has reported an output format we cannot feed to sws
     * (tiled, 10-bit, ...). receive() then never returns a picture again, so the
     * caller should give up on hardware decode rather than run blind.
     */
    bool unsupported() const { return unsupportedFormat_; }

private:
    static constexpr size_t kInputRing = 8;

    bool stage(const uint8_t* data, size_t len, int64_t pts);
    bool putPending();
    bool accept(MppFrame frame, Frame* out);

    MppCtx ctx_ = nullptr;
    MppApi* mpi_ = nullptr;
    MppBufferGroup group_ = nullptr;
    int coding_ = 0;

    int width_ = 0;
    int height_ = 0;
    int horStride_ = 0;
    int verStride_ = 0;

    std::vector<uint8_t> ring_[kInputRing];
    size_t ringNext_ = 0;
    MppPacket pending_ = nullptr;  // staged packet not yet accepted by rkvdec

    MppFrame current_ = nullptr;   // the frame backing the last receive()
    std::string lastError_;
    bool unsupportedFormat_ = false;
    uint64_t framesOut_ = 0;
    uint64_t putFails_ = 0;
    uint64_t infoChanges_ = 0;
};

}  // namespace mpp
}  // namespace runtime

#endif  // RUNTIME_MPP_MPPDECODER_H
