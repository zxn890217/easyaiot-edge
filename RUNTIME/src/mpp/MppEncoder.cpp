#include "mpp/MppEncoder.h"

#include <cstring>

#include <glog/logging.h>

namespace runtime {
namespace mpp {
namespace {

constexpr int kHorStrideAlign = 64;
constexpr int kVerStrideAlign = 16;
constexpr size_t kHeaderBytes = 1024;
// Safety net for the drain loop: with in-order IPPP output we should never see
// more packets than we have frames in flight.
constexpr int kDrainBurst = 8;

void setIfOk(MppEncCfg cfg, const char* key, RK_S32 value) {
    mpp_enc_cfg_set_s32(cfg, key, value);
}

}  // namespace

MppEncoder::MppEncoder() = default;

MppEncoder::~MppEncoder() { close(); }

bool MppEncoder::hostSupported(std::string* reason) { return MppEnv::hostSupported(reason); }

bool MppEncoder::open(int width, int height, int fps, int bitRate, int gop) {
    close();
    lastError_.clear();

    if (width <= 0 || height <= 0) {
        lastError_ = "invalid picture size";
        return false;
    }
    std::string reason;
    if (!MppEnv::hostSupported(&reason)) {
        lastError_ = "host has no rockchip codec device (" + reason + ")";
        LOG(WARNING) << "[MPP-ENC] " << lastError_;
        return false;
    }

    width_ = width;
    height_ = height;
    horStride_ = alignUp(width, kHorStrideAlign);
    verStride_ = alignUp(height, kVerStrideAlign);
    frameSize_ = static_cast<size_t>(horStride_) * verStride_ * 3 / 2;
    fps_ = fps > 0 ? fps : 25;
    bitRate_ = bitRate > 0 ? bitRate : 4000000;
    gop_ = gop > 0 ? gop : fps_ * 2;

    if (mpp_create(&ctx_, &mpi_) || !mpi_) {
        lastError_ = "mpp_create failed";
        LOG(ERROR) << "[MPP-ENC] " << lastError_;
        close();
        return false;
    }
    const int rc = mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (rc) {
        lastError_ = "mpp_init(MPP_CTX_ENC, AVC) rc=" + std::to_string(rc);
        LOG(ERROR) << "[MPP-ENC] " << lastError_ << " - VEPU unavailable";
        close();
        return false;
    }

    group_ = MppEnv::openBufferGroup("enc");
    if (!group_) {
        lastError_ = "no MPP buffer group";
        close();
        return false;
    }

    if (!configure() || !extractHeader() || !allocSlots()) {
        close();
        return false;
    }

    LOG(INFO) << "[MPP-ENC] VEPU H.264 ready " << width_ << "x" << height_
              << " stride=" << horStride_ << "x" << verStride_ << " fps=" << fps_
              << " bps=" << bitRate_ << " gop=" << gop_;
    return true;
}

bool MppEncoder::configure() {
    if (mpp_enc_cfg_init(&cfg_) || !cfg_) {
        lastError_ = "mpp_enc_cfg_init failed";
        LOG(ERROR) << "[MPP-ENC] " << lastError_;
        return false;
    }

    // GET -> mutate -> SET is the only shape this MPP vintage accepts: the
    // legacy SET_PREP_CFG / SET_RC_CFG commands are deprecated and the raw
    // mandatory-key set differs between releases.
    int rc = mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg_);
    if (rc) {
        lastError_ = "control(MPP_ENC_GET_CFG) rc=" + std::to_string(rc);
        LOG(ERROR) << "[MPP-ENC] " << lastError_;
        return false;
    }

    setIfOk(cfg_, "prep:width", width_);
    setIfOk(cfg_, "prep:height", height_);
    setIfOk(cfg_, "prep:hor_stride", horStride_);
    setIfOk(cfg_, "prep:ver_stride", verStride_);
    setIfOk(cfg_, "prep:format", MPP_FMT_YUV420SP);  // NV12

    setIfOk(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR);
    setIfOk(cfg_, "rc:bps_target", bitRate_);
    setIfOk(cfg_, "rc:bps_max", bitRate_ * 17 / 16);
    setIfOk(cfg_, "rc:bps_min", bitRate_ * 15 / 16);
    setIfOk(cfg_, "rc:fps_in_flex", 0);
    setIfOk(cfg_, "rc:fps_in_num", fps_);
    setIfOk(cfg_, "rc:fps_in_denom", 1);
    setIfOk(cfg_, "rc:fps_out_flex", 0);
    setIfOk(cfg_, "rc:fps_out_num", fps_);
    setIfOk(cfg_, "rc:fps_out_denom", 1);
    setIfOk(cfg_, "rc:gop", gop_);

    setIfOk(cfg_, "codec:type", MPP_VIDEO_CodingAVC);
    setIfOk(cfg_, "h264:profile", 100);  // High
    setIfOk(cfg_, "h264:level", 50);     // 5.0
    setIfOk(cfg_, "h264:cabac_en", 1);
    setIfOk(cfg_, "h264:cabac_idc", 0);
    setIfOk(cfg_, "h264:trans8x8", 1);

    rc = mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_);
    if (rc) {
        lastError_ = "control(MPP_ENC_SET_CFG) rc=" + std::to_string(rc) +
                     " - cfg keys differ in this MPP build";
        LOG(ERROR) << "[MPP-ENC] " << lastError_;
        return false;
    }

    // Prove the config actually stuck instead of trusting rc == 0.
    RK_S32 gotW = 0, gotH = 0;
    mpp_enc_cfg_get_s32(cfg_, "prep:width", &gotW);
    mpp_enc_cfg_get_s32(cfg_, "prep:height", &gotH);
    if (gotW != width_ || gotH != height_) {
        lastError_ = "cfg round trip mismatch prep:width=" + std::to_string(gotW) +
                     " prep:height=" + std::to_string(gotH);
        LOG(ERROR) << "[MPP-ENC] " << lastError_;
        return false;
    }
    return true;
}

bool MppEncoder::extractHeader() {
    // MPP copies the header *into* the packet, so it has to be initialised over
    // real memory first. Passing &MppPacket instead yields
    // "mpp_packet_copy invalid input" while still returning rc = 0.
    std::vector<uint8_t> hdr(kHeaderBytes, 0);
    MppPacket packet = nullptr;
    if (mpp_packet_init(&packet, hdr.data(), hdr.size()) || !packet) {
        lastError_ = "mpp_packet_init for header failed";
        LOG(ERROR) << "[MPP-ENC] " << lastError_;
        return false;
    }
    const int rc = mpi_->control(ctx_, MPP_ENC_GET_HDR_SYNC, packet);
    const size_t len = mpp_packet_get_length(packet);
    const uint8_t* data = static_cast<const uint8_t*>(mpp_packet_get_data(packet));

    bool ok = false;
    if (rc == 0 && len > 4 && data) {
        std::vector<Nal> nals;
        splitAnnexB(data, len, &nals);
        const Nal* sps = nullptr;
        const Nal* pps = nullptr;
        for (const Nal& nal : nals) {
            if (nal.type == kNalSPS && !sps) sps = &nal;
            if (nal.type == kNalPPS && !pps) pps = &nal;
        }
        if (sps && pps) {
            ok = buildAvcDecoderConfigRecord(sps->data, sps->size, pps->data, pps->size,
                                             &extradata_);
        } else if (data[0] == 0x01) {
            // Some builds hand back a ready AVCDecoderConfigurationRecord.
            extradata_.assign(data, data + len);
            ok = true;
        }
    }
    mpp_packet_deinit(&packet);

    if (!ok || extradata_.empty()) {
        lastError_ = "MPP_ENC_GET_HDR_SYNC returned no usable SPS/PPS (rc=" +
                     std::to_string(rc) + " len=" + std::to_string(len) + ")";
        LOG(ERROR) << "[MPP-ENC] " << lastError_ << " - cannot mux without extradata";
        return false;
    }
    LOG(INFO) << "[MPP-ENC] extradata(AVCC) " << extradata_.size() << " bytes";
    return true;
}

bool MppEncoder::allocSlots() {
    slots_.resize(kInputSlots);
    for (int i = 0; i < kInputSlots; ++i) {
        Slot& slot = slots_[i];
        if (mpp_buffer_get(group_, &slot.buf, frameSize_) || !slot.buf) {
            lastError_ = "mpp_buffer_get(" + std::to_string(frameSize_) + ") failed";
            LOG(ERROR) << "[MPP-ENC] " << lastError_;
            return false;
        }
        slot.ptr = static_cast<uint8_t*>(mpp_buffer_get_ptr(slot.buf));
        if (!slot.ptr) {
            lastError_ = "mpp_buffer_get_ptr returned NULL (buffer type unsupported)";
            LOG(ERROR) << "[MPP-ENC] " << lastError_;
            return false;
        }
        if (mpp_frame_init(&slot.frame) || !slot.frame) {
            lastError_ = "mpp_frame_init failed";
            LOG(ERROR) << "[MPP-ENC] " << lastError_;
            return false;
        }
        mpp_frame_set_width(slot.frame, width_);
        mpp_frame_set_height(slot.frame, height_);
        mpp_frame_set_hor_stride(slot.frame, horStride_);
        mpp_frame_set_ver_stride(slot.frame, verStride_);
        mpp_frame_set_fmt(slot.frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(slot.frame, slot.buf);
        mpp_frame_set_eos(slot.frame, 0);
        freeSlots_.push_back(static_cast<size_t>(i));
    }
    LOG(INFO) << "[MPP-ENC] " << kInputSlots << " NV12 input slots of " << frameSize_
              << " bytes, fd0=" << mpp_buffer_get_fd(slots_[0].buf);
    return true;
}

void MppEncoder::freeSlots() {
    for (Slot& slot : slots_) {
        if (slot.frame) {
            mpp_frame_deinit(&slot.frame);
            slot.frame = nullptr;
        }
        if (slot.buf) {
            mpp_buffer_put(slot.buf);
            slot.buf = nullptr;
        }
        slot.ptr = nullptr;
    }
    slots_.clear();
    freeSlots_.clear();
    pending_.clear();
}

void MppEncoder::close() {
    freeSlots();
    if (cfg_) {
        mpp_enc_cfg_deinit(cfg_);
        cfg_ = nullptr;
    }
    if (group_) {
        mpp_buffer_group_put(group_);
        group_ = nullptr;
    }
    if (ctx_) {
        mpp_destroy(ctx_);
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
    extradata_.clear();
    nextPts_ = 0;
    eosSent_ = false;
}

uint8_t* MppEncoder::acquireInput() {
    if (freeSlots_.empty() || ctx_ == nullptr) return nullptr;
    return slots_[freeSlots_.front()].ptr;
}

uint8_t* MppEncoder::acquireInputChroma() {
    if (freeSlots_.empty() || ctx_ == nullptr) return nullptr;
    return slots_[freeSlots_.front()].ptr + static_cast<size_t>(horStride_) * verStride_;
}

bool MppEncoder::submit() {
    if (!ctx_ || freeSlots_.empty()) return false;
    const size_t index = freeSlots_.front();
    freeSlots_.pop_front();

    Slot& slot = slots_[index];
    mpp_frame_set_pts(slot.frame, static_cast<RK_S64>(nextPts_));
    mpp_frame_set_eos(slot.frame, 0);

    const int rc = mpi_->encode_put_frame(ctx_, slot.frame);
    if (rc) {
        lastError_ = "encode_put_frame rc=" + std::to_string(rc);
        LOG(WARNING) << "[MPP-ENC] " << lastError_ << ", dropping pts=" << nextPts_;
        freeSlots_.push_back(index);
        return false;
    }
    pending_.push_back(InFlight{index, nextPts_});
    ++nextPts_;
    return true;
}

bool MppEncoder::toAvcc(const uint8_t* annexb, size_t len, MppEncPacket* pkt) {
    std::vector<uint8_t> converted;
    bool key = false;
    if (annexBToAvcc(annexb, len, &converted, &key)) {
        pkt->data.swap(converted);
        pkt->key = key;
        return true;
    }
    if (avccFailures_++ == 0) {
        LOG(WARNING) << "[MPP-ENC] Annex-B -> AVCC rewrite failed, passing the raw "
                        "bitstream through (expect muxer trouble)";
    }
    pkt->data.assign(annexb, annexb + len);
    bool foundIdr = false;
    std::vector<Nal> nals;
    splitAnnexB(annexb, len, &nals);
    for (const Nal& nal : nals) {
        if (nal.type == kNalIDR) foundIdr = true;
    }
    pkt->key = foundIdr;
    return false;
}

void MppEncoder::drain(std::vector<MppEncPacket>* out) {
    if (!ctx_ || !out) return;

    for (int guard = 0; guard < kDrainBurst && !pending_.empty(); ++guard) {
        MppPacket packet = nullptr;
        const int rc = mpi_->encode_get_packet(ctx_, &packet);
        if (rc) {
            lastError_ = "encode_get_packet rc=" + std::to_string(rc);
            LOG(WARNING) << "[MPP-ENC] " << lastError_;
            return;
        }
        if (!packet) return;  // still encoding; the rest arrives with later frames

        MppEncPacket pkt;
        const size_t len = mpp_packet_get_length(packet);
        const uint8_t* data = static_cast<const uint8_t*>(mpp_packet_get_data(packet));
        if (len > 0 && data) toAvcc(data, len, &pkt);
        mpp_packet_deinit(&packet);

        // Output is in-order (no B frames), so the oldest submitted frame owns
        // this access unit; its slot can go back to the free list.
        pkt.pts = pending_.front().pts;
        freeSlots_.push_back(pending_.front().slot);
        pending_.pop_front();

        ++packetsOut_;
        if (len > 0) out->push_back(std::move(pkt));
    }
}

bool MppEncoder::flush(std::vector<MppEncPacket>* out) {
    if (!ctx_) return true;
    if (!eosSent_) {
        // EOS travels as a NULL frame on the put port, not as a real frame with
        // the eos bit set.
        MppFrame eos = nullptr;
        const int rc = mpi_->encode_put_frame(ctx_, eos);
        if (rc) LOG(WARNING) << "[MPP-ENC] put EOS rc=" << rc;
        eosSent_ = true;
    }
    drain(out);
    return pending_.empty();
}

}  // namespace mpp
}  // namespace runtime
