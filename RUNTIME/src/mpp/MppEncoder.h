#ifndef RUNTIME_MPP_MPPENCODER_H
#define RUNTIME_MPP_MPPENCODER_H

#include <deque>
#include <string>
#include <vector>

#include "mpp/H264Bits.h"
#include "mpp/MppEnv.h"

namespace runtime {
namespace mpp {

/** One encoded access unit, already rewritten to AVCC for the FLV muxer. */
struct MppEncPacket {
    std::vector<uint8_t> data;  ///< 4-byte length prefixed NALs, start codes stripped
    int64_t pts = 0;            ///< frame index, same units as the matching submit()
    bool key = false;           ///< contains an IDR slice
};

/**
 * RK3588 VEPU H.264 encoder driven straight through librockchip_mpp.
 *
 * Neither FFmpeg build on the board was compiled with the rkmpp wrappers, so
 * there is no avcodec path to hardware encode; the alternative was
 * h264_v4l2m2m (the vendor /dev/video-enc0 node is a 4 byte stub) or keeping
 * libx264 on the CPU, which is what costs us 30-40 % of the box.
 *
 * The call sequence is a direct transplant of RUNTIME/tools/mpp_enc_probe.c
 * stage 2, which is the exact API shape proven on this board:
 *   mpp_create -> mpp_init(MPP_CTX_ENC, MPP_VIDEO_CodingAVC)
 *   -> buffer group (DRM/DMA_HEAP/NORMAL probed)
 *   -> MPP_ENC_GET_CFG, mutate string keys, MPP_ENC_SET_CFG
 *   -> MPP_ENC_GET_HDR_SYNC for SPS/PPS
 *   -> encode_put_frame / encode_get_packet (the combined mpi->encode() is
 *      rejected by this MPP vintage)
 *
 * Feeding is zero copy: the caller writes NV12 straight into the MppBuffer
 * VEPU will DMA from, so a 1080p frame never gets copied. Because VEPU reads
 * that buffer asynchronously we keep a small ring of them and only recycle a
 * slot once the matching bitstream comes back.
 *
 * Output is drained without ever calling encode_get_packet() when nothing is
 * owed. That keeps us safe under either poll semantics: this MPP build was not
 * probed with MPP_SET_OUTPUT_TIMEOUT, so a blocking get is a real possibility
 * and an unbounded drain loop could hang the infer thread.
 */
class MppEncoder {
public:
    static constexpr int kInputSlots = 3;

    MppEncoder();
    ~MppEncoder();

    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;

    /** Cheap pre-flight so the caller can log a reason before calling open(). */
    static bool hostSupported(std::string* reason = nullptr);

    /**
     * width/height are the visible picture; the strides are derived with the
     * VEPU alignment rules (hor 64, ver 16). gop == 0 means "fps * 2".
     */
    bool open(int width, int height, int fps, int bitRate, int gop);
    void close();

    bool isOpen() const { return ctx_ != nullptr; }
    const std::string& lastError() const { return lastError_; }

    int width() const { return width_; }
    int height() const { return height_; }
    int horStride() const { return horStride_; }
    int verStride() const { return verStride_; }

    /** NV12 luma plane of the free input slot, or NULL when none is free. */
    uint8_t* acquireInput();
    /** Chroma plane of the same slot (NV12: interleaved UV, horStride * h/2). */
    uint8_t* acquireInputChroma();
    /** Hand what acquireInput() filled over to VEPU. */
    bool submit();
    /** Number of input slots currently available. */
    int freeInputSlots() const { return static_cast<int>(freeSlots_.size()); }

    /** Collect every packet VEPU already owes us. Never blocks on an empty queue. */
    void drain(std::vector<MppEncPacket>* out);

    /**
     * Push EOS and keep draining. Returns false while frames are still in
     * flight, true once everything has come back (or on error).
     */
    bool flush(std::vector<MppEncPacket>* out);

    /** AVCDecoderConfigurationRecord for codecpar->extradata (empty until open). */
    const std::vector<uint8_t>& extradata() const { return extradata_; }

    /** Packets produced / bytes emitted, for the encode_ep telemetry. */
    uint64_t packetsOut() const { return packetsOut_; }

private:
    struct Slot {
        MppBuffer buf = nullptr;
        MppFrame frame = nullptr;
        uint8_t* ptr = nullptr;
    };

    struct InFlight {
        size_t slot;
        int64_t pts;
    };

    bool configure();
    bool extractHeader();
    bool allocSlots();
    void freeSlots();
    bool toAvcc(const uint8_t* annexb, size_t len, MppEncPacket* pkt);

    MppCtx ctx_ = nullptr;
    MppApi* mpi_ = nullptr;
    MppBufferGroup group_ = nullptr;
    MppEncCfg cfg_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int horStride_ = 0;
    int verStride_ = 0;
    int fps_ = 0;
    int bitRate_ = 0;
    int gop_ = 0;
    size_t frameSize_ = 0;

    std::vector<Slot> slots_;
    std::deque<size_t> freeSlots_;
    std::deque<InFlight> pending_;  // submitted frames still inside VEPU, FIFO
    int64_t nextPts_ = 0;
    bool eosSent_ = false;

    std::vector<uint8_t> extradata_;
    std::string lastError_;
    uint64_t packetsOut_ = 0;
    uint64_t avccFailures_ = 0;
};

}  // namespace mpp
}  // namespace runtime

#endif  // RUNTIME_MPP_MPPENCODER_H
