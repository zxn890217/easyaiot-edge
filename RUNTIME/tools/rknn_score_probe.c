// rknn_score_probe.c -- read-only diagnostic for RKNN detections=0.
//
// Feeds a raw RGB24 image to a .rknn model through the exact same call path as
// RknnEngine::Run (UINT8 / NHWC / pass_through=0 / want_float=1) and prints the
// per-channel distribution of the output tensor, plus an input amplitude sweep,
// so we can tell apart:
//   (a) head parsed with the wrong layout          -> box/score split per channel
//   (b) quantization killed the score branch       -> scores ~0 at every amplitude
//   (c) input amplitude is wrong (255x vs 1x)      -> scores wake up as we divide
//
//   gcc -O2 -I. rknn_score_probe.c -o rknn_score_probe \
//       -L/opt/easyaiot/rknn-lib -lrknnrt -Wl,-rpath,/opt/easyaiot/rknn-lib
//   ./rknn_score_probe /app/data/models/15/model.rknn /tmp/p/scene.rgb 960 540 640

#include <rknn_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCH_MAX 16

static unsigned char *readFile(const char *path, size_t *lenOut) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "open %s failed\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)len);
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        *lenOut = (size_t)len;
    }
    return buf;
}

// Same geometry as yolocore::squarePad when center=0 (image at top-left, black fill);
// center=1 with fill=114 reproduces the ultralytics letterbox used at train time.
static void letterbox(const unsigned char *rgb, int iw, int ih, int side, int center,
                      int fill, unsigned char *out) {
    const int mx = iw > ih ? iw : ih;
    const int scale = side > mx ? side : mx;
    const int nw = (int)((double)iw * side / scale);
    const int nh = (int)((double)ih * side / scale);
    const int dx = center ? (side - nw) / 2 : 0;
    const int dy = center ? (side - nh) / 2 : 0;
    memset(out, (unsigned char)fill, (size_t)side * side * 3);
    for (int y = 0; y < nh; ++y) {
        int sy = (int)((double)y * ih / nh);
        if (sy >= ih) sy = ih - 1;
        for (int x = 0; x < nw; ++x) {
            int sx = (int)((double)x * iw / nw);
            if (sx >= iw) sx = iw - 1;
            const unsigned char *s = rgb + ((size_t)sy * iw + sx) * 3;
            unsigned char *d = out + (((size_t)(dy + y) * side) + (dx + x)) * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
}

typedef struct {
    int64_t nc;      // channel count from the output attr
    int64_t per;     // elems per channel, channel-major
    size_t bytes;    // bytes actually returned by rknn_outputs_get
    size_t n_elems;  // element count from the output attr
    float *f;        // the returned buffer, read as float (want_float=1)
    unsigned char *raw;
} OutBuf;

static int run_one(rknn_context ctx, const unsigned char *rgb, int iw, int ih, int side,
                   int center, int fill, int shift, const char *tag, OutBuf *outs, int nOut) {
    unsigned char *buf = (unsigned char *)malloc((size_t)side * side * 3);
    if (!buf) return -1;
    letterbox(rgb, iw, ih, side, center, fill, buf);
    if (shift > 0) {
        for (size_t i = 0; i < (size_t)side * side * 3; ++i) buf[i] = (unsigned char)(buf[i] >> shift);
    }

    rknn_input in;
    memset(&in, 0, sizeof(in));
    in.index = 0;
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;
    in.size = (uint32_t)((size_t)side * side * 3);
    in.pass_through = 0;
    in.buf = buf;
    int ret = rknn_inputs_set(ctx, 1, &in);
    if (ret < 0) {
        printf("%s: rknn_inputs_set ret=%d\n", tag, ret);
        free(buf);
        return -1;
    }
    ret = rknn_run(ctx, NULL);
    if (ret < 0) {
        printf("%s: rknn_run ret=%d\n", tag, ret);
        free(buf);
        return -1;
    }

    rknn_output oraw[8];
    const uint32_t nGet = nOut > 8 ? 8 : (uint32_t)nOut;
    memset(oraw, 0, sizeof(oraw));
    for (uint32_t i = 0; i < nGet; ++i) {
        oraw[i].index = i;
        oraw[i].want_float = 1;
        oraw[i].is_prealloc = 0;
    }
    ret = rknn_outputs_get(ctx, nGet, oraw, NULL);
    if (ret < 0) {
        printf("%s: rknn_outputs_get ret=%d\n", tag, ret);
        free(buf);
        return -1;
    }
    for (uint32_t i = 0; i < nGet; ++i) {
        outs[i].bytes = oraw[i].size;
        outs[i].raw = (unsigned char *)malloc(oraw[i].size ? oraw[i].size : 1);
        memcpy(outs[i].raw, oraw[i].buf, oraw[i].size);
        outs[i].f = (float *)malloc(oraw[i].size ? oraw[i].size : 1);
        memcpy(outs[i].f, oraw[i].buf, oraw[i].size);
    }
    rknn_outputs_release(ctx, nGet, oraw);
    free(buf);
    return 0;
}

static void stats(const OutBuf *o, const char *tag) {
    if (!o->f || !o->bytes) return;
    const size_t nf = o->n_elems;
    const size_t n = o->bytes / 4 < nf ? o->bytes / 4 : nf;

    size_t bNeg = 0, bTiny = 0, b01 = 0, b05 = 0, b2 = 0, bBig = 0;
    float gmin = 1e30f, gmax = -1e30f;
    size_t gmax_i = 0;
    for (size_t i = 0; i < n; ++i) {
        const float v = o->f[i];
        if (v < gmin) gmin = v;
        if (v > gmax) { gmax = v; gmax_i = i; }
        if (v < 0.0f) ++bNeg;
        else if (v <= 0.001f) ++bTiny;
        else if (v <= 0.1f) ++b01;
        else if (v <= 0.5f) ++b05;
        else if (v <= 2.0f) ++b2;
        else ++bBig;
    }
    printf("%-9s bytes=%zu n_elems=%zu B/elem=%.2f fp32[min=%.6f max=%.6f @%zu]\n", tag, o->bytes,
           nf, nf ? (double)o->bytes / (double)nf : 0.0, gmin, gmax, gmax_i);
    printf("          buckets <0:%zu [0,.001]:%zu (.001,.1]:%zu (.1,.5]:%zu (.5,2]:%zu >2:%zu\n",
           bNeg, bTiny, b01, b05, b2, bBig);
    printf("          head16 f32:");
    for (size_t i = 0; i < 16 && i < n; ++i) printf(" %.4g", o->f[i]);
    printf("\n");

    const int64_t nc = (o->nc > 0 && o->nc <= NCH_MAX) ? o->nc : 1;
    const int64_t per = o->per > 0 ? o->per : (int64_t)n;
    printf("          channel-major (stride=%lld):\n", (long long)per);
    for (int64_t c = 0; c < nc; ++c) {
        const size_t base = (size_t)(c * per);
        if (base >= n) break;
        size_t cnt = 0, gt05 = 0;
        float mn = 1e30f, mx = -1e30f;
        double sum = 0.0;
        for (size_t i = base; i < base + (size_t)per && i < n; ++i) {
            const float v = o->f[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            if (v > 0.05f) ++gt05;
            sum += v;
            ++cnt;
        }
        if (!cnt) continue;
        printf("          c[%lld] n=%zu min=%.4f max=%.4f mean=%.4f >0.05:%zu\n", (long long)c,
               cnt, mn, mx, (float)(sum / (double)cnt), gt05);
    }
    printf("          interleaved (stride=%lld):", (long long)nc);
    for (int64_t c = 0; c < nc; ++c) {
        float mn = 1e30f, mx = -1e30f;
        size_t cnt = 0;
        for (size_t i = (size_t)c; i < n; i += (size_t)nc) {
            const float v = o->f[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            ++cnt;
        }
        if (cnt) printf(" c%lld[%.3g,%.3g]", (long long)c, mn, mx);
    }
    printf("\n");
}

// One compact line per input amplitude: how strong is the best "score-like" value.
static void sweepLine(const OutBuf *o, const char *tag, int shift) {
    const size_t n = o->bytes / 4 < o->n_elems ? o->bytes / 4 : o->n_elems;
    size_t gt005 = 0, gt01 = 0;
    float sMax = -1e30f, gmax = -1e30f;
    for (size_t i = 0; i < n; ++i) {
        const float v = o->f[i];
        if (v > gmax) gmax = v;
        if (v >= 0.0f && v <= 1.5f) {
            if (v > sMax) sMax = v;
            if (v > 0.05f) ++gt005;
            if (v > 0.1f) ++gt01;
        }
    }
    printf("          %-8s shift=%d bestScoreLike=%.6f n>0.05:%zu n>0.1:%zu gmax=%.4f\n", tag, shift,
           sMax, gt005, gt01, gmax);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s model.rknn image.rgb width height [side]\n", argv[0]);
        return 2;
    }
    size_t mlen = 0;
    unsigned char *model = readFile(argv[1], &mlen);
    if (!model) return 2;
    const int iw = atoi(argv[3]);
    const int ih = atoi(argv[4]);
    const int side = argc > 5 ? atoi(argv[5]) : 640;
    size_t ilen = 0;
    unsigned char *img = readFile(argv[2], &ilen);
    if (!img) return 2;
    if (ilen < (size_t)iw * ih * 3) {
        fprintf(stderr, "image too small: %zu < %d*%d*3\n", ilen, iw, ih);
        return 2;
    }

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model, (uint32_t)mlen, 0, NULL);
    if (ret < 0) {
        fprintf(stderr, "rknn_init failed ret=%d\n", ret);
        return 1;
    }
    rknn_sdk_version v;
    memset(&v, 0, sizeof(v));
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &v, sizeof(v)) == 0)
        printf("sdk api=%s drv=%s\n", v.api_version, v.drv_version);
    rknn_set_core_mask(ctx, RKNN_NPU_CORE_AUTO);

    rknn_input_output_num io;
    memset(&io, 0, sizeof(io));
    if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) != 0) {
        fprintf(stderr, "rknn_query IN_OUT_NUM failed\n");
        return 1;
    }
    printf("n_inputs=%u n_outputs=%u\n", io.n_input, io.n_output);

    rknn_tensor_attr inAttr;
    memset(&inAttr, 0, sizeof(inAttr));
    inAttr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &inAttr, sizeof(inAttr));
    printf("input[0] dims=[%u,%u,%u,%u] fmt=%d type=%d size=%u n_elems=%u\n", inAttr.dims[0],
           inAttr.dims[1], inAttr.dims[2], inAttr.dims[3], inAttr.fmt, inAttr.type, inAttr.size,
           inAttr.n_elems);

    const int nOut = (int)io.n_output > 8 ? 8 : (int)io.n_output;
    OutBuf proto[8];
    memset(proto, 0, sizeof(proto));
    for (int i = 0; i < nOut; ++i) {
        rknn_tensor_attr oa;
        memset(&oa, 0, sizeof(oa));
        oa.index = (uint32_t)i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa));
        const int64_t d0 = oa.dims[0] ? oa.dims[0] : 1;
        const int64_t d1 = oa.dims[1] ? oa.dims[1] : 1;
        proto[i].nc = d0 * d1 > 0 ? d0 * d1 : 1;
        if (proto[i].nc > NCH_MAX) proto[i].nc = 8;
        proto[i].per = oa.n_elems ? (int64_t)oa.n_elems / proto[i].nc : 0;
        proto[i].n_elems = oa.n_elems;
        printf("output[%d] name=%s dims=[%u,%u,%u,%u] type=%d size=%u n_elems=%u zp=%d scale=%.8f\n",
               i, oa.name, oa.dims[0], oa.dims[1], oa.dims[2], oa.dims[3], oa.type, oa.size,
               oa.n_elems, (int)oa.zp, (double)oa.scale);
    }

    unsigned char *zero = (unsigned char *)calloc((size_t)side * side * 3, 1);
    unsigned char *rnd = (unsigned char *)malloc((size_t)side * side * 3);
    srand(20260907);
    for (size_t i = 0; i < (size_t)side * side * 3; ++i) rnd[i] = (unsigned char)(rand() & 0xFF);

    OutBuf o[8];
    printf("--- real scene, ultralytics letterbox (gray 114, centered) ---\n");
    memset(o, 0, sizeof(o));
    for (int i = 0; i < nOut; ++i) o[i] = proto[i];
    if (run_one(ctx, img, iw, ih, side, 1, 114, 0, "gray114", o, nOut) == 0) {
        for (int i = 0; i < nOut; ++i) stats(&o[i], "gray114");
        for (int i = 0; i < nOut; ++i) { free(o[i].f); free(o[i].raw); }
    }

    printf("--- all-black input ---\n");
    memset(o, 0, sizeof(o));
    for (int i = 0; i < nOut; ++i) o[i] = proto[i];
    if (zero && run_one(ctx, zero, side, side, side, 0, 0, 0, "black", o, nOut) == 0) {
        for (int i = 0; i < nOut; ++i) stats(&o[i], "black");
        for (int i = 0; i < nOut; ++i) { free(o[i].f); free(o[i].raw); }
    }

    printf("--- random noise input (false positive control) ---\n");
    memset(o, 0, sizeof(o));
    for (int i = 0; i < nOut; ++i) o[i] = proto[i];
    if (rnd && run_one(ctx, rnd, side, side, side, 0, 0, 0, "rand", o, nOut) == 0) {
        for (int i = 0; i < nOut; ++i) stats(&o[i], "rand");
        for (int i = 0; i < nOut; ++i) { free(o[i].f); free(o[i].raw); }
    }

    printf("--- input amplitude sweep (real scene, >> shift) ---\n");
    const int shifts[6] = {0, 1, 2, 3, 4, 6};
    for (int k = 0; k < 6; ++k) {
        memset(o, 0, sizeof(o));
        for (int i = 0; i < nOut; ++i) o[i] = proto[i];
        char tag[16];
        snprintf(tag, sizeof(tag), "sw%d", shifts[k]);
        if (run_one(ctx, img, iw, ih, side, 1, 114, shifts[k], tag, o, nOut) == 0) {
            for (int i = 0; i < nOut; ++i) sweepLine(&o[i], tag, shifts[k]);
            for (int i = 0; i < nOut; ++i) { free(o[i].f); free(o[i].raw); }
        }
    }

    free(rnd);
    free(zero);
    rknn_destroy(ctx);
    return 0;
}
