#include "mpp/MppDecoder.h"

#include <cstring>

#include <glog/logging.h>

namespace runtime {
namespace mpp {
namespace {

// Bounded work per receive() call so a stream full of info-change frames can
// never spin the pull loop.
constexpr int kGetRounds = 4;

bool isNv12(MppFrameFormat fmt) {
    // MPP packs colour-depth and vendor flags above the low 16 bits, so mask
    // before comparing (10-bit NV12 must not be mistaken for 8-bit).
    return (static_cast<unsigned>(fmt) & 0xffffu) == static_cast<unsigned>(MPP_FMT_YUV420SP);
}

}  // namespace

MppDecoder::MppDecoder() = default;

MppDecoder::~MppDecoder() { close(); }

bool MppDecoder::hostSupported(std::string* reason) { return MppEnv::hostSupported(reason); }

bool MppDecoder::open(int coding, int widthHint, int heightHint) {
    close();
    lastError_.clear();

    std::string reason;
    if (!MppEnv::hostSupported(&reason)) {
        lastError_ = "host has no rockchip codec device (" + reason + ")";
        LOG(WARNING) << "[MPP-DEC] " << lastError_;
        return false;
    }

    if (mpp_create(&ctx_, &mpi_) || !mpi_) {
        lastError_ = "mpp_create failed";
        LOG(ERROR) << "[MPP-DEC] " << lastError_;
        close();
        return false;
    }

    // Output has to be drained opportunistically from the single pull thread, so
    // the get port must not park on a condition variable waiting for input.
    MppPollType timeout = MPP_POLL_NON_BLOCK;
    if (mpi_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout)) {
        LOG(WARNING) << "[MPP-DEC] MPP_SET_OUTPUT_TIMEOUT rejected; hardware decode may "
                        "block, set hwaccel_decode=false if the pipeline stalls";
    }

    const int rc = mpp_init(ctx_, MPP_CTX_DEC, static_cast<MppCodingType>(coding));
    if (rc) {
        lastError_ = "mpp_init(MPP_CTX_DEC, coding=" + std::to_string(coding) +
                     ") rc=" + std::to_string(rc);
        LOG(ERROR) << "[MPP-DEC] " << lastError_;
        close();
        return false;
    }
    coding_ = coding;
    width_ = widthHint > 0 ? widthHint : 0;
    height_ = heightHint > 0 ? heightHint : 0;

    group_ = MppEnv::openBufferGroup("dec");
    if (!group_) {
        lastError_ = "no MPP buffer group";
        close();
        return false;
    }
    const int grc = mpi_->control(ctx_, MPP_DEC_SET_EXT_BUF_GROUP, group_);
    if (grc) {
        lastError_ = "control(MPP_DEC_SET_EXT_BUF_GROUP) rc=" + std::to_string(grc);
        LOG(ERROR) << "[MPP-DEC] " << lastError_;
        close();
        return false;
    }

    LOG(INFO) << "[MPP-DEC] rkvdec ready coding=" << coding_ << " hint=" << width_ << "x"
              << height_;
    return true;
}

void MppDecoder::close() {
    release();
    if (pending_) {
        mpp_packet_deinit(&pending_);
        pending_ = nullptr;
    }
    for (size_t i = 0; i < kInputRing; ++i) {
        ring_[i].clear();
        ring_[i].shrink_to_fit();
    }
    ringNext_ = 0;
    // The decoded buffers belong to the group, not to us: dropping the group is
    // how they are reclaimed, and it has to happen after every frame is gone.
    if (ctx_) {
        mpp_destroy(ctx_);
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
    if (group_) {
        mpp_buffer_group_put(group_);
        group_ = nullptr;
    }
    unsupportedFormat_ = false;
    putFails_ = 0;
    infoChanges_ = 0;
}

bool MppDecoder::stage(const uint8_t* data, size_t len, int64_t pts) {
    // Rotate the staging ring so rkvdec can still be reading the previous
    // payload while the caller reuses its own AVPacket.
    ringNext_ = (ringNext_ + 1) % kInputRing;
    std::vector<uint8_t>& store = ring_[ringNext_];
    if (store.size() < len) store.resize(len);
    std::memcpy(store.data(), data, len);

    MppPacket packet = nullptr;
    if (mpp_packet_init(&packet, store.data(), len) || !packet) {
        lastError_ = "mpp_packet_init failed";
        LOG(WARNING) << "[MPP-DEC] " << lastError_;
        return false;
    }
    mpp_packet_set_pts(packet, static_cast<RK_S64>(pts));
    pending_ = packet;
    return true;
}

bool MppDecoder::putPending() {
    if (!pending_) return false;
    const int rc = mpi_->decode_put_packet(ctx_, pending_);
    if (rc == 0) {
        mpp_packet_deinit(&pending_);
        pending_ = nullptr;
        return true;
    }
    if (putFails_++ == 0) {
        LOG(WARNING) << "[MPP-DEC] decode_put_packet rc=" << rc
                     << " (input queue full, will retry after draining)";
    }
    return false;
}

bool MppDecoder::send(const uint8_t* data, size_t len, int64_t pts, bool* retry) {
    if (retry) *retry = false;
    if (!ctx_) return false;
    if (!data || len == 0) return false;

    if (pending_ && !putPending()) {
        if (retry) *retry = true;
        return false;
    }
    if (!stage(data, len, pts)) return false;
    if (putPending()) return true;

    if (retry) *retry = true;
    return false;
}

void MppDecoder::release() {
    if (current_) {
        // Drops the frame's own reference on the buffer it was decoded into; we
        // never took one, so there is no matching mpp_buffer_put here.
        mpp_frame_deinit(&current_);
        current_ = nullptr;
    }
}

bool MppDecoder::receive(Frame* out) {
    if (!ctx_ || !out) return false;
    if (current_) {
        LOG(WARNING) << "[MPP-DEC] receive() while a frame is still held; call release()";
        return false;
    }
    if (unsupportedFormat_) return false;

    for (int round = 0; round < kGetRounds; ++round) {
        MppFrame frame = nullptr;
        const int rc = mpi_->decode_get_frame(ctx_, &frame);
        if (rc) return false;
        if (!frame) return false;

        if (mpp_frame_get_info_change(frame)) {
            const int w = static_cast<int>(mpp_frame_get_width(frame));
            const int h = static_cast<int>(mpp_frame_get_height(frame));
            const int hs = static_cast<int>(mpp_frame_get_hor_stride(frame));
            const int vs = static_cast<int>(mpp_frame_get_ver_stride(frame));
            mpp_frame_deinit(&frame);
            if (w > 0 && h > 0) {
                width_ = w;
                height_ = h;
                horStride_ = hs;
                verStride_ = vs;
            }
            ++infoChanges_;
            const int rrc = mpi_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
            if (rrc) {
                lastError_ = "control(MPP_DEC_SET_INFO_CHANGE_READY) rc=" + std::to_string(rrc);
                LOG(WARNING) << "[MPP-DEC] " << lastError_;
                unsupportedFormat_ = true;
                return false;
            }
            continue;
        }

        const bool eos = mpp_frame_get_eos(frame) != 0;
        MppBuffer buf = mpp_frame_get_buffer(frame);
        if (!buf) {
            mpp_frame_deinit(&frame);
            if (eos) return false;
            continue;
        }

        const MppFrameFormat fmt = mpp_frame_get_fmt(frame);
        if (!isNv12(fmt)) {
            mpp_frame_deinit(&frame);
            unsupportedFormat_ = true;
            lastError_ = "rkvdec output format=" + std::to_string(static_cast<unsigned>(fmt)) +
                         " is not 8-bit NV12";
            LOG(WARNING) << "[MPP-DEC] " << lastError_ << " - downgrade to software decode";
            return false;
        }

        const uint8_t* ptr = static_cast<const uint8_t*>(mpp_buffer_get_ptr(buf));
        if (!ptr) {
            mpp_frame_deinit(&frame);
            lastError_ = "mpp_buffer_get_ptr returned NULL on a decoded frame";
            LOG(WARNING) << "[MPP-DEC] " << lastError_;
            unsupportedFormat_ = true;
            return false;
        }

        const int w = static_cast<int>(mpp_frame_get_width(frame));
        const int h = static_cast<int>(mpp_frame_get_height(frame));
        const int hs = static_cast<int>(mpp_frame_get_hor_stride(frame));
        const int vs = static_cast<int>(mpp_frame_get_ver_stride(frame));
        if (w <= 0 || h <= 0 || hs < w || vs < h) {
            mpp_frame_deinit(&frame);
            lastError_ = "nonsensical decoded geometry " + std::to_string(w) + "x" +
                         std::to_string(h) + " stride " + std::to_string(hs) + "x" +
                         std::to_string(vs);
            LOG(WARNING) << "[MPP-DEC] " << lastError_;
            continue;
        }

        out->planes[0] = ptr;
        out->planes[1] = ptr + static_cast<size_t>(hs) * vs;
        out->linesize[0] = hs;
        out->linesize[1] = hs;
        out->width = w;
        out->height = h;
        out->pts = static_cast<int64_t>(mpp_frame_get_pts(frame));

        width_ = w;
        height_ = h;
        horStride_ = hs;
        verStride_ = vs;
        current_ = frame;
        ++framesOut_;
        return true;
    }
    return false;
}

}  // namespace mpp
}  // namespace runtime
