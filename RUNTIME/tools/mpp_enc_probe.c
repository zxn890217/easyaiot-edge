// mpp_enc_probe.c -- RK3588 hardware H.264 encode feasibility probe (stage 1 + 2).
//
//   gcc -O2 -I/usr/include/rockchip mpp_enc_probe.c -o mpp_enc_probe \
//       -L/usr/lib/aarch64-linux-gnu -l:librockchip_mpp.so.0
//   ./mpp_enc_probe          # stage 1: context capability
//   ./mpp_enc_probe encode   # stage 2: real one-frame encode through VEPU
//   ./mpp_enc_probe encode /tmp/mpp/out.264
//
// Why this exists
// ---------------
// Neither FFmpeg build on the board carries rkmpp, so a hardware encode path has
// to drive librockchip_mpp directly (see RUNTIME plan notes). Before committing
// to that we must separate two very different claims:
//
//   stage 1 - "MPP hands out an encoder context for rk3588"   [proven: yes]
//   stage 2 - "VEPU actually emits an H.264 bitstream"        [this file]
//
// Stage 1 already produced a capability table matching the real silicon
// (coding 7/8/9 = AVC/MJPEG/VP8 accepted; h263/mpeg4/wmv/rv/vp9 refused), which
// a stubbed build would not reproduce. But mpp_init() alone never touches the
// encoder core, so it is not sufficient.
//
// What stage 2 proves, in order of value:
//
//   1. Which MppBufferGroup allocation path works: DRM, DMA_HEAP or NORMAL.
//      This is the container compatibility question - MPP_BUFFER_TYPE_DRM needs
//      a DRM render node, DMA_HEAP needs /dev/dma_heap/*, and only NORMAL is
//      fd-free. Whichever one wins dictates what has to be mounted into
//      video-service, so it is probed first and reported loudly.
//   2. That the MppEncCfg string-key API (MPP_ENC_GET_CFG -> mutate ->
//      MPP_ENC_SET_CFG) is accepted by this MPP vintage. Note this build's
//      MppEncCfg is `void*` with set_s32/get_s32 accessors, not the newer
//      directly-addressable struct, and the legacy SET_PREP_CFG/SET_RC_CFG
//      commands are marked deprecated - so the GET-then-SET round trip is the
//      only forward-compatible shape.
//   3. That SPS/PPS can be extracted into a caller-owned packet. MPP copies the
//      header into a pre-allocated MppPacket; passing &MppPacket instead yields
//      "mpp_packet_copy invalid input" while still returning rc=0, which is a
//      trap we already hit once. rc=0 from control() is never taken as evidence.
//   4. That a synthetic NV12 frame goes in and a real bitstream comes out, and
//      that the output NAL types are what an H.264 IDR stream should contain.
//
// The bitstream, if any, is written to the path given in argv[3] (default
// /tmp/mpp/out.264) so it can be inspected off-box.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>

#define DEF_OUT "/tmp/mpp/out.264"

// ---------------------------------------------------------------- helpers ---

// Stringify rather than switch: the grep on this board lists DRM / DMA_HEAP /
// ION / EXT_DMA / DMA_HEAP / NORMAL / MASK / BUTT as distinct *tokens*, but
// MPP has historically aliased ION onto NORMAL, which would blow up a switch as
// duplicate case values. #ifying the macro sidesteps the question.
static const char *bufTypeName(MppBufferType t) {
#define CMP(x) if ((int)t == (int)(x)) return #x;
    CMP(MPP_BUFFER_TYPE_NORMAL)
    CMP(MPP_BUFFER_TYPE_DRM)
    CMP(MPP_BUFFER_TYPE_ION)
    CMP(MPP_BUFFER_TYPE_EXT_DMA)
    CMP(MPP_BUFFER_TYPE_DMA_HEAP)
    CMP(MPP_BUFFER_TYPE_BUTT)
#undef CMP
    return "?";
}

// Walks an Annex-B stream and prints the NAL types found. FLV/RTMP will later
// need this same stream rewritten to length-prefixed AVCC, so the start-code
// layout is worth seeing in raw form now.
static void dumpNals(const unsigned char *p, size_t n, const char *tag) {
    size_t i = 0;
    int shown = 0;
    printf("    %s NALs:", tag);
    while (i + 4 < n && shown < 8) {
        size_t sc = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            sc = 4;
        } else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            sc = 3;
        }
        if (!sc) {
            ++i;
            continue;
        }
        int type = p[i + sc] & 0x1f;
        const char *name = (type == 5)   ? "IDR"
                           : (type == 7) ? "SPS"
                           : (type == 8) ? "PPS"
                           : (type == 6) ? "SEI"
                           : (type == 1) ? "SLICE"
                                         : "other";
        printf(" %s(%d)", name, type);
        ++shown;
        i += sc;
    }
    if (!shown) {
        printf(" none-found(head=%02x %02x %02x %02x)", p[0], p[1], p[2], p[3]);
    }
    printf("\n");
    fflush(stdout);
}

// ------------------------------------------------------- stage 1 (contexts) --

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

    if (ctx) {
        mpp_destroy(ctx);
    }
    return ret;
}

static void stage1(void) {
    printf("-- enum values (later code must not hardcode these) --\n");
    printf("  MPP_CTX_DEC=%d ENC=%d ISP=%d BUTT=%d\n", (int)MPP_CTX_DEC, (int)MPP_CTX_ENC,
           (int)MPP_CTX_ISP, (int)MPP_CTX_BUTT);
    printf("  CodingUnused=%d AutoDetect=%d AVC=%d HEVC=%d\n",
           (int)MPP_VIDEO_CodingUnused, (int)MPP_VIDEO_CodingAutoDetect,
           (int)MPP_VIDEO_CodingAVC, (int)MPP_VIDEO_CodingHEVC);
    fflush(stdout);

    printf("-- control: decoder contexts (rkvdec known-good on rk3588) --\n");
    tryCtx(MPP_CTX_DEC, MPP_VIDEO_CodingAVC, "DEC/AVC");
    tryCtx(MPP_CTX_DEC, MPP_VIDEO_CodingHEVC, "DEC/HEVC");

    printf("-- the question: encoder contexts --\n");
    tryCtx(MPP_CTX_ENC, MPP_VIDEO_CodingAVC, "ENC/AVC");
    tryCtx(MPP_CTX_ENC, MPP_VIDEO_CodingHEVC, "ENC/HEVC");

    // Enumerate the encoder capability table by brute force. The shape of this
    // table (which codings are refused and with which message) is the strongest
    // evidence available that VEPU support is real rather than stubbed.
    printf("-- encoder capability table --\n");
    static const struct {
        int v;
        const char *n;
    } tbl[] = {{2, "mpeg2"}, {3, "h263"},   {4, "mpeg4"}, {5, "wmv"},
               {6, "rv"},    {7, "avc"},    {8, "mjpeg"}, {9, "vp8"},
               {10, "vp9"},  {0x01000004, "hevc"}};
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); ++i) {
        MppCtx ctx = NULL;
        MppApi *mpi = NULL;
        if (mpp_create(&ctx, &mpi) || !ctx) {
            printf("  create failed at coding=%d (%s)\n", tbl[i].v, tbl[i].n);
            continue;
        }
        int rc = mpp_init(ctx, MPP_CTX_ENC, (MppCodingType)tbl[i].v);
        printf("  coding=%-10d %-6s enc %s\n", tbl[i].v, tbl[i].n,
               rc == 0 ? "ACCEPTED" : "refused");
        fflush(stdout);
        mpp_destroy(ctx);
    }
}

// ---------------------------------------------------- stage 2 (real encode) --

#define ENC_W   1280
#define ENC_H   720
#define ALIGN_UP(v, a) (((v) + (a) - 1) / (a) * (a))

static int stage2(const char *outPath) {
    const int horStride = ALIGN_UP(ENC_W, 64);
    const int verStride = ALIGN_UP(ENC_H, 16);
    const size_t frameSize = (size_t)horStride * verStride * 3 / 2;
    const int bitRate = 2000000;
    const int fps = 25;
    const int gop = 25;
    int rcFail = 0;

    printf("=== stage 2: real encode %dx%d hor_stride=%d ver_stride=%d bps=%d ===\n",
           ENC_W, ENC_H, horStride, verStride, bitRate);

    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    int ret = mpp_create(&ctx, &mpi);
    if (ret || !mpi) {
        printf("mpp_create rc=%d\n", ret);
        return 1;
    }
    ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    printf("mpp_init(ENC,AVC) rc=%d\n", ret);
    if (ret) {
        rcFail = 1;
        goto out_ctx;
    }

    // ---- 1. buffer group: pick the allocation path that this board/container
    //         can actually service. Printed first because it dictates mounts.
    MppBufferGroup grp = NULL;
    MppBufferType pick = MPP_BUFFER_TYPE_BUTT;
    static const MppBufferType order[] = {MPP_BUFFER_TYPE_DRM,
                                          MPP_BUFFER_TYPE_DMA_HEAP,
                                          MPP_BUFFER_TYPE_NORMAL};
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]) && !grp; ++i) {
        MppBufferGroup cand = NULL;
        int r = mpp_buffer_group_get_internal(&cand, order[i]);
        printf("buffer_group_get_internal(%-9s) rc=%d %s\n", bufTypeName(order[i]), r,
               r == 0 && cand ? "<== usable" : "");
        fflush(stdout);
        if (!r && cand) {
            grp = cand;
            pick = order[i];
        }
    }
    if (!grp) {
        printf("NO buffer group available -> VEPU cannot be fed. "
               "Check /dev/dri/renderD*, /dev/dma_heap/*, /dev/mpp_service\n");
        rcFail = 1;
        goto out_ctx;
    }
    printf("CHOSEN buffer type = %s  (container must expose whatever this needs)\n",
           bufTypeName(pick));

    // ---- 2. encoder config, GET-then-mutate-then-SET so we never have to know
    //         the complete mandatory key set for this MPP vintage.
    MppEncCfg cfg = NULL;
    if (mpp_enc_cfg_init(&cfg)) {
        printf("mpp_enc_cfg_init failed\n");
        rcFail = 1;
        goto out_grp;
    }
    ret = mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
    printf("control(MPP_ENC_GET_CFG) rc=%d\n", ret);
    if (ret) {
        rcFail = 1;
        goto out_cfg;
    }

    mpp_enc_cfg_set_s32(cfg, "prep:width", ENC_W);
    mpp_enc_cfg_set_s32(cfg, "prep:height", ENC_H);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", horStride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", verStride);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitRate);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bitRate * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bitRate * 15 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", gop);

    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);   // High
    mpp_enc_cfg_set_s32(cfg, "h264:level", 50);      // 5.0
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);

    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    printf("control(MPP_ENC_SET_CFG) rc=%d %s\n", ret,
           ret == 0 ? "(cfg accepted)" : "(cfg REJECTED - keys differ in this build)");
    fflush(stdout);
    if (ret) {
        rcFail = 1;
        goto out_cfg;
    }

    // Round trip a couple of keys to prove the cfg actually stuck, rather than
    // being silently ignored.
    {
        RK_S32 gotW = 0, gotMode = -1;
        mpp_enc_cfg_get_s32(cfg, "prep:width", &gotW);
        mpp_enc_cfg_get_s32(cfg, "rc:mode", &gotMode);
        printf("cfg round-trip: prep:width=%d rc:mode=%d (expect %d / %d)\n", gotW,
               gotMode, ENC_W, (int)MPP_ENC_RC_MODE_CBR);
    }

    // ---- 3. SPS/PPS into a caller-owned packet. MPP copies *into* this packet,
    //         so it must be initialised over real memory first.
    {
        static unsigned char hdr[512];
        MppPacket hp = NULL;
        mpp_packet_init(&hp, hdr, sizeof(hdr));
        ret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, hp);
        size_t hlen = hp ? mpp_packet_get_length(hp) : 0;
        printf("GET_HDR_SYNC rc=%d len=%zu %s\n", ret, hlen,
               (ret == 0 && hlen > 4) ? "<== real header" : "<== NO header bytes");
        if (ret == 0 && hlen > 4) {
            unsigned char *hd = (unsigned char *)mpp_packet_get_data(hp);
            printf("    head:");
            for (size_t i = 0; i < hlen && i < 16; ++i) {
                printf(" %02x", hd[i]);
            }
            printf("\n");
            dumpNals(hd, hlen, "header");
        }
        mpp_packet_deinit(&hp);
        fflush(stdout);
    }

    // ---- 4. one synthetic NV12 frame in, bitstream out.
    {
        MppBuffer inBuf = NULL;
        if (mpp_buffer_get(grp, frameSize, &inBuf) || !inBuf) {
            printf("mpp_buffer_get(%zu) failed\n", frameSize);
            rcFail = 1;
            goto out_cfg;
        }
        unsigned char *px = (unsigned char *)mpp_buffer_get_ptr(inBuf);
        if (!px) {
            printf("mpp_buffer_get_ptr returned NULL\n");
            rcFail = 1;
            goto out_cfg;
        }
        // Gradient luma + neutral chroma; moves horizontally per frame so the
        // rate controller sees real inter-frame change.
        memset(px, 128, frameSize);
        for (int y = 0; y < verStride; ++y) {
            for (int x = 0; x < horStride; ++x) {
                px[(size_t)y * horStride + x] = (unsigned char)((x + y) & 0xff);
            }
        }
        memset(px + (size_t)horStride * verStride, 128,
               (size_t)horStride * verStride / 2);

        MppFrame frame = NULL;
        if (mpp_frame_init(&frame) || !frame) {
            printf("mpp_frame_init failed\n");
            rcFail = 1;
            goto out_cfg;
        }
        mpp_frame_set_width(frame, ENC_W);
        mpp_frame_set_height(frame, ENC_H);
        mpp_frame_set_hor_stride(frame, horStride);
        mpp_frame_set_ver_stride(frame, verStride);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame, inBuf);
        mpp_frame_set_eos(frame, 0);

        // VEPU needs a pipeline fill; push frames until packets start coming
        // back, then keep going so we collect an actual sequence.
        FILE *of = fopen(outPath, "wb");
        printf("output file: %s (%s)\n", outPath, of ? "open ok" : strerror(errno));
        size_t total = 0;
        int framesIn = 0, pkts = 0;
        for (int attempt = 0; attempt < 12 && pkts < 6; ++attempt) {
            mpp_frame_set_pts(frame, (MppPts)framesIn);
            ret = mpi->encode(ctx, frame, NULL);
            if (ret) {
                printf("  encode_put #%d rc=%d\n", attempt, ret);
                fflush(stdout);
                break;
            }
            ++framesIn;
            MppPacket op = NULL;
            if (mpi->encode_get_packet(ctx, &op)) {
                printf("  encode_get_packet #%d failed\n", attempt);
                break;
            }
            if (op) {
                size_t len = mpp_packet_get_length(op);
                unsigned char *dat = (unsigned char *)mpp_packet_get_data(op);
                total += len;
                ++pkts;
                printf("  pkt#%d len=%zu %s\n", pkts, len,
                       (len > 4) ? "<== BITSTREAM" : "(empty)");
                if (len > 4 && dat) {
                    dumpNals(dat, len, "pkt");
                    if (of) {
                        fwrite(dat, 1, len, of);
                    }
                }
                if (pkts == 1) {
                    fflush(stdout);
                }
                mpp_packet_deinit(&op);
            }
        }
        if (of) {
            fclose(of);
        }
        mpp_frame_deinit(&frame);
        mpp_buffer_put(inBuf);

        printf("=== stage 2 verdict: frames_in=%d packets=%d total_bytes=%zu ===\n",
               framesIn, pkts, total);
        if (pkts > 0 && total > 100) {
            printf("VEPU HARDWARE H.264 ENCODER PRODUCES A REAL BITSTREAM\n");
        } else {
            printf("no bitstream produced -> direct-MPP backend is NOT viable\n");
            rcFail = 1;
        }
        fflush(stdout);
    }

out_cfg:
    mpp_enc_cfg_deinit(cfg);
out_grp:
    if (grp) {
        mpp_buffer_group_put(grp);
    }
out_ctx:
    mpp_destroy(ctx);
    return rcFail;
}

int main(int argc, char **argv) {
    stage1();
    if (argc > 1 && strcmp(argv[1], "encode") == 0) {
        return stage2(argc > 2 ? argv[2] : DEF_OUT);
    }
    printf("\n(re-run with `encode` for the stage-2 bitstream test)\n");
    return 0;
}
