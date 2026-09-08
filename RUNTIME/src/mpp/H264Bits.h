#ifndef RUNTIME_MPP_H264BITS_H
#define RUNTIME_MPP_H264BITS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace runtime {
namespace mpp {

/**
 * H.264 bitstream container helpers.
 *
 * Both directions are needed because the two hardware backends disagree with
 * what the RTMP/FLV muxer wants:
 *
 *   rkmpp (VEPU)  emits Annex-B  (start-code prefixed NAL units)
 *   FLV / RTMP    needs AVCC     (4-byte length prefixed NAL units) plus an
 *                 AVCDecoderConfigurationRecord in codecpar->extradata
 *
 * FFmpeg only ships the h264_mp4toannexb bitstream filter (AVCC -> Annex-B),
 * never the reverse, so the rewrite lives here.
 *
 * Annex-B -> AVCC is also what an AVCC-ingested source (rtmp://, http-flv)
 * needs before rkvdec will accept it, hence avccToAnnexB() as well.
 */

/** One NAL unit, pointing into the caller's buffer (no ownership). */
struct Nal {
    const uint8_t* data;  // first byte is the nal_unit_type header byte
    size_t size;
    uint8_t type;         // data[0] & 0x1f
};

/** NAL types we branch on. */
enum NalType {
    kNalSlice = 1,
    kNalSlicePartitionA = 2,
    kNalSEI = 6,
    kNalSPS = 7,
    kNalPPS = 8,
    kNalAUD = 9,
    kNalIDR = 5
};

/**
 * Split an Annex-B buffer into NAL units. Accepts 3- and 4-byte start codes and
 * tolerates leading/trailing junk. Zero-size NALs are dropped.
 */
void splitAnnexB(const uint8_t* buf, size_t len, std::vector<Nal>* out);

/** True when `buf` starts with an Annex-B start code (00 00 01 / 00 00 00 01). */
bool looksLikeAnnexB(const uint8_t* buf, size_t len);

/**
 * True when `buf` looks like a length-prefixed (AVCC/HVCC) access unit:
 * the first `lengthSize` bytes are zero with a non-zero tail and the value
 * matches the remaining buffer length. Used only to sniff the container of a
 * stream, never as a hard guarantee.
 */
bool looksLikeAvcc(const uint8_t* buf, size_t len, int lengthSize = 4);

/**
 * Build an AVCDecoderConfigurationRecord from raw SPS/PPS NAL units
 * (each including its 1-byte NAL header). Returns false when the SPS is too
 * short to carry profile/compatibility/level.
 */
bool buildAvcDecoderConfigRecord(const uint8_t* sps, size_t spsLen,
                                 const uint8_t* pps, size_t ppsLen,
                                 std::vector<uint8_t>* out);

/**
 * Rewrite an Annex-B access unit into 4-byte length prefixed NAL units.
 * SPS/PPS are kept in-band (matching what libx264 does with repeat headers),
 * so a player that ignores extradata still initialises.
 *
 * `keyOut` (optional) is set when the unit contains an IDR slice.
 */
bool annexBToAvcc(const uint8_t* buf, size_t len,
                  std::vector<uint8_t>* out, bool* keyOut);

/** Rewrite an AVCC access unit into Annex-B, appending to `out`. */
bool avccToAnnexB(const uint8_t* buf, size_t len, int lengthSize,
                  std::vector<uint8_t>* out);

}  // namespace mpp
}  // namespace runtime

#endif  // RUNTIME_MPP_H264BITS_H
