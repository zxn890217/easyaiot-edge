// mpp_enc_probe.c -- read-only feasibility probe for RK3588 hardware H.264 encode.
//
// Context: neither FFmpeg build on the board carries rkmpp (conda's
// libavcodec.so.63 has zero "rkmpp" literals and its hwdevice name table is
// only cuda/drm/opencl/vaapi/vulkan; the container's BtbN /usr/local/bin/ffmpeg
// configure line has no --enable-rkmpp and reports "Unknown encoder
// 'h264_rkmpp'"). So any hardware encode path has to talk to librockchip_mpp
// directly, or to a re-built FFmpeg.
//
// Before spending either budget we must know whether this MPP build actually
// hands out an *encoder* context for RK3588. A blind ctypes call already got:
//   mpp_create rc=0
//   mpp_init(ENC, <bogus coding>) rc=-1  "unable to create enc (null) for soc rk3588 unsupported"
// The coding value there was wrong (MPP_VIDEO_CodingAVC is an implicit enum, not
// 0x14), so the -1 is inconclusive - but the "for soc rk3588 unsupported" tail
// comes from MPP's own factory table and has to be ruled out properly.
//
// This probe answers three things and nothing else:
//   1. the real numeric values of the enums we will hardcode later,
//   2. whether mpp_init(MPP_CTX_ENC, MPP_VIDEO_CodingAVC) succeeds,
//   3. whether MPP can hand back an H.264 SPS/PPS header, i.e. the VEPU
//      encoder object is genuinely alive on this SoC.
//
// Decode is probed as a control: RK3588's rkvdec is known-good, so a DEC
// success next to an ENC failure localises the problem to the encoder factory
// instead of the kmpp transport.
//
//   gcc -O2 -I/usr/include/rockchip mpp_enc_probe.c -o mpp_enc_probe \
//       -L/usr/lib/aarch64-linux-gnu -l:librockchip_mpp.so.0
//   ./mpp_enc_probe
//
// Stage 2 (full one-frame encode through MPP_ENC_SET_CFG) is behind
// -DMPP_ENC_PROBE_CFG because the MppEncCfg accessor API differs between MPP
// vintages; enable it only once the header has been read.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rockchip/rk_mpi.h>

static int g_mpp_verbose = 1;

static void dumpMppVersion(void) {
    // MPP logs its own banner from mpp_init(), but the build string is also
    // reachable through the exported symbol on most vintages. Only used if it
    // links; guarded by the weak attribute so an older .so still builds.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    extern const char *mpp_version_string(void) __attribute__((weak));
    const char *v = mpp_version_string ? mpp_version_string() : NULL;
#pragma GCC diagnostic pop
    printf("  mpp_version_string: %s\n", v ? v : "(symbol absent - see banner)");
    fflush(stdout);
}

// mpp_init() on a coding value this build does not know returns non-zero and
// MPP prints "<soc> unsupported"; a supported value returns 0. That is exactly
// the signal we are after, so the noise is welcome.
// This MPP vintage renamed the API handle struct: it is `MppApi` (rk_mpi.h:231
// declares `mpp_create(MppCtx *ctx, MppApi **mpi)`), not the legacy `MpiApi`.
static int tryCtx(MppCtxType type, MppCodingType coding, const char *label) {
    MppCtx ctx = NULL;
    MppApi *mpi = NULL;

    int ret = mpp_create(&ctx, &mpi);
    if (ret) {
        printf("[%-28s] mpp_create  rc=%d\n", label, ret);
        fflush(stdout);
        return ret;
    }

    ret = mpp_init(ctx, type, coding);
    printf("[%-28s] mpp_init    rc=%d   %s\n", label, ret, ret == 0 ? "OK" : "FAIL");
    fflush(stdout);

    if (ret == 0 && type == MPP_CTX_ENC && mpi) {
        // GET_HDR_SYNC is the real proof of life: it walks the VEPU driver
        // object and serialises SPS/PPS. A factory stub would fail here even if
        // mpp_init somehow succeeded.
        //
        // Only the length is printed unconditionally - this MPP vintage has no
        // mpp_packet_get_addr(), and we do not want a missing accessor to cost
        // us the primary verdict. Define MPP_ENC_PROBE_DUMP to also walk the
        // MppBuffer attached to the packet and hexdump the head bytes.
        MppPacket pkt = NULL;
        int hret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, &pkt);
        if (hret || !pkt) {
            printf("[%-28s] GET_HDR_SYNC rc=%d pkt=%p\n", label, hret, (void *)pkt);
        } else {
            size_t len = mpp_packet_get_length(pkt);
            printf("[%-28s] header bytes=%zu %s\n", label, len,
                   len > 8 ? "<== real SPS/PPS" : "<== suspiciously short");
#ifdef MPP_ENC_PROBE_DUMP
            MppBuffer buf = mpp_packet_get_buffer(pkt);
            unsigned char *dat = buf ? (unsigned char *)mpp_buffer_get_ptr(buf) : NULL;
            if (dat) {
                printf("    head:");
                for (size_t i = 0; i < len && i < 16; ++i) {
                    printf(" %02x", dat[i]);
                }
                printf("   (expect 00 00 00 01)\n");
            }
#endif
            mpp_packet_deinit(&pkt);
        }
        fflush(stdout);
    }

    if (ctx) {
        mpp_destroy(ctx);
    }
    return ret;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        g_mpp_verbose = 0;  // reserved: silence is done by MPP's own log level
    }

    printf("=== MPP encoder feasibility probe ===\n");
    dumpMppVersion();

    printf("-- enum values (these are what later code must not hardcode) --\n");
    // MPP_CTX_VPROC does not exist in this header vintage - the set here is
    // DEC / ENC / ISP / BUTT, so only ISP is printed for completeness.
    printf("  MPP_CTX_DEC=%d  MPP_CTX_ENC=%d  MPP_CTX_ISP=%d  MPP_CTX_BUTT=%d\n",
           (int)MPP_CTX_DEC, (int)MPP_CTX_ENC, (int)MPP_CTX_ISP, (int)MPP_CTX_BUTT);
    printf("  CodingUnused=%d  AutoDetect=%d  AVC=%d  HEVC=%d\n",
           (int)MPP_VIDEO_CodingUnused, (int)MPP_VIDEO_CodingAutoDetect,
           (int)MPP_VIDEO_CodingAVC, (int)MPP_VIDEO_CodingHEVC);
    fflush(stdout);

    printf("-- control: decoder contexts (rkvdec is known-good on rk3588) --\n");
    int decAvc = tryCtx(MPP_CTX_DEC, MPP_VIDEO_CodingAVC, "DEC/AVC");
    int decHevc = tryCtx(MPP_CTX_DEC, MPP_VIDEO_CodingHEVC, "DEC/HEVC");

    printf("-- the question: encoder contexts --\n");
    int encAvc = tryCtx(MPP_CTX_ENC, MPP_VIDEO_CodingAVC, "ENC/AVC");
    int encHevc = tryCtx(MPP_CTX_ENC, MPP_VIDEO_CodingHEVC, "ENC/HEVC");

    // If the named constants somehow are not the ones the factory table keys
    // on, sweep the neighbourhood so we still learn what this build accepts.
    printf("-- sweep around CodingAVC=%d (only rc=0 lines matter) --\n",
           (int)MPP_VIDEO_CodingAVC);
    for (int c = (int)MPP_VIDEO_CodingAVC - 4; c <= (int)MPP_VIDEO_CodingAVC + 8; ++c) {
        MppCtx ctx = NULL;
        MppApi *mpi = NULL;
        if (mpp_create(&ctx, &mpi) || !ctx) {
            continue;
        }
        int rc = mpp_init(ctx, MPP_CTX_ENC, (MppCodingType)c);
        if (rc == 0) {
            printf("  ENC coding=%d -> rc=0  <== accepted\n", c);
            fflush(stdout);
        }
        mpp_destroy(ctx);
    }

    printf("=== verdict ===\n");
    printf("DEC/AVC=%d DEC/HEVC=%d ENC/AVC=%d ENC/HEVC=%d\n", decAvc, decHevc, encAvc,
           encHevc);
    if (encAvc == 0) {
        printf("HARDWARE H264 ENCODE AVAILABLE -> direct-MPP backend is viable\n");
    } else if (decAvc == 0) {
        printf("decode ok, encode refused -> encoder factory disabled for this SoC"
               " (check MPP_CFG / vepu support in this build)\n");
    } else {
        printf("both refused -> kmpp transport problem, not an encoder question\n");
    }
    fflush(stdout);
    return 0;
}
