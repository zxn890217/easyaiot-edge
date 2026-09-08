#include "mpp/H264Bits.h"

#include <cstring>

namespace runtime {
namespace mpp {
namespace {

constexpr int kMinSpsHeaderBytes = 4;  // nal header + profile + compat + level

// Returns the length of the start code at `p` (0 when there is none).
// `end` bounds the scan so we never read past the caller's buffer.
size_t startCodeLen(const uint8_t* p, const uint8_t* end) {
    if (end - p < 3) return 0;
    if (p[0] != 0 || p[1] != 0) return 0;
    if (p[2] == 1) return 3;
    if (end - p >= 4 && p[2] == 0 && p[3] == 1) return 4;
    return 0;
}

void putBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void putBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

uint32_t getBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint32_t getBe16(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]);
}

}  // namespace

void splitAnnexB(const uint8_t* buf, size_t len, std::vector<Nal>* out) {
    out->clear();
    if (!buf || len == 0) return;

    const uint8_t* begin = buf;
    const uint8_t* end = buf + len;

    // Locate every start code, then take the bytes between two of them (or up
    // to the end of the buffer) as one NAL unit.
    std::vector<std::pair<const uint8_t*, size_t>> scodes;  // (nal start, sc len)
    for (const uint8_t* p = begin; p + 2 < end;) {
        const size_t scl = startCodeLen(p, end);
        if (scl > 0) {
            const uint8_t* nalStart = p + scl;
            if (nalStart < end) scodes.emplace_back(nalStart, scl);
            p += scl;
        } else {
            ++p;
        }
    }

    for (size_t i = 0; i < scodes.size(); ++i) {
        const uint8_t* nalStart = scodes[i].first;
        size_t body;
        if (i + 1 < scodes.size()) {
            // The next entry points just past its own start code, so step back
            // over those bytes to find where this NAL ends.
            const uint8_t* nextStart = scodes[i + 1].first - scodes[i + 1].second;
            body = static_cast<size_t>(nextStart - nalStart);
        } else {
            body = static_cast<size_t>(end - nalStart);
        }
        // Trailing zero padding is legal Annex-B noise; VEPU does not emit it but
        // decoders and muxers dislike it, so trim from the back.
        while (body > 0 && nalStart[body - 1] == 0) --body;
        if (body == 0) continue;

        Nal nal;
        nal.data = nalStart;
        nal.size = body;
        nal.type = static_cast<uint8_t>(nalStart[0] & 0x1f);
        out->push_back(nal);
    }
}

bool looksLikeAnnexB(const uint8_t* buf, size_t len) {
    if (!buf || len < 4) return false;
    // A 4-byte start code starts with 00 00 00 01, a 3-byte one with 00 00 01.
    if (buf[0] == 0 && buf[1] == 0) {
        if (buf[2] == 1) return true;
        if (len >= 4 && buf[2] == 0 && buf[3] == 1) return true;
    }
    return false;
}

bool looksLikeAvcc(const uint8_t* buf, size_t len, int lengthSize) {
    if (!buf || lengthSize <= 0 || lengthSize > 4) return false;
    if (static_cast<size_t>(lengthSize) >= len) return false;

    // The length prefix of a 720p frame is a few hundred kilobytes, so the top
    // three bytes are always zero. Combined with a plausible total this is a
    // good enough container sniff.
    for (int i = 0; i < lengthSize - 1; ++i) {
        if (buf[i] != 0) return false;
    }
    const uint32_t first = (lengthSize == 4) ? getBe32(buf) : getBe16(buf);
    if (first == 0) return false;
    return static_cast<size_t>(first) <= len - static_cast<size_t>(lengthSize);
}

bool buildAvcDecoderConfigRecord(const uint8_t* sps, size_t spsLen,
                                 const uint8_t* pps, size_t ppsLen,
                                 std::vector<uint8_t>* out) {
    // sps[1..3] are the profile/compatibility/level bytes, so the SPS needs at
    // least a header byte plus those three.
    if (!sps || spsLen < kMinSpsHeaderBytes || !pps || ppsLen < 2 ||
        spsLen > 0xffff || ppsLen > 0xffff) {
        return false;
    }

    out->clear();
    out->reserve(11 + spsLen + 3 + ppsLen);

    const uint8_t* spsBody = sps + 1;  // skip the nal_unit_type byte
    out->push_back(0x01);              // configurationVersion
    out->push_back(spsBody[0]);         // AVCProfileIndication
    out->push_back(spsBody[1]);         // profile_compatibility
    out->push_back(spsBody[2]);         // AVCLevelIndication
    out->push_back(static_cast<uint8_t>(0xfc | 0x03));  // 6 reserved bits + lengthSizeMinusOne=3
    out->push_back(static_cast<uint8_t>(0xe0 | 0x01));  // 3 reserved bits + numOfSPS=1
    out->push_back(static_cast<uint8_t>(spsLen >> 8));
    out->push_back(static_cast<uint8_t>(spsLen & 0xff));
    out->insert(out->end(), sps, sps + spsLen);
    out->push_back(0x01);  // numOfPPS
    out->push_back(static_cast<uint8_t>(ppsLen >> 8));
    out->push_back(static_cast<uint8_t>(ppsLen & 0xff));
    out->insert(out->end(), pps, pps + ppsLen);
    return true;
}

bool annexBToAvcc(const uint8_t* buf, size_t len, std::vector<uint8_t>* out, bool* keyOut) {
    if (!buf || len == 0 || !out) return false;

    std::vector<Nal> nals;
    splitAnnexB(buf, len, &nals);
    if (nals.empty()) return false;

    if (keyOut) *keyOut = false;
    out->clear();
    size_t total = 0;
    for (const Nal& nal : nals) {
        if (nal.type == kNalIDR && keyOut) *keyOut = true;
        total += 4 + nal.size;
    }
    out->resize(total);
    uint8_t* w = out->data();
    for (const Nal& nal : nals) {
        putBe32(w, static_cast<uint32_t>(nal.size));
        w += 4;
        std::memcpy(w, nal.data, nal.size);
        w += nal.size;
    }
    return true;
}

bool avccToAnnexB(const uint8_t* buf, size_t len, int lengthSize, std::vector<uint8_t>* out) {
    if (!buf || len == 0 || !out || lengthSize <= 0 || lengthSize > 4) return false;
    if (static_cast<size_t>(lengthSize) >= len) return false;

    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    size_t emitted = 0;

    while (static_cast<size_t>(end - p) >= static_cast<size_t>(lengthSize)) {
        const uint32_t nalLen =
            (lengthSize == 4) ? getBe32(p) : (lengthSize == 3 ? (static_cast<uint32_t>(p[0]) << 16) |
                                                                     (static_cast<uint32_t>(p[1]) << 8) |
                                                                     static_cast<uint32_t>(p[2])
                                                               : getBe16(p));
        p += lengthSize;
        if (nalLen == 0 || static_cast<size_t>(nalLen) > static_cast<size_t>(end - p)) return false;

        static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
        out->insert(out->end(), kStartCode, kStartCode + 4);
        out->insert(out->end(), p, p + nalLen);
        p += nalLen;
        ++emitted;
    }
    return emitted > 0;
}

}  // namespace mpp
}  // namespace runtime
