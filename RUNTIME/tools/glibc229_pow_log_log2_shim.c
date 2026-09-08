/*
 * glibc229_pow_log_log2_shim.c -- let a glibc 2.28 container consume the board's
 * prebuilt librockchip_mpp.so.
 *
 * Status: diagnostic-grade candidate. Nothing here is wired into the build yet.
 * The experiment it unblocks is RUNTIME/tools/mpp_enc_probe.c running inside
 * video-service.
 *
 * The problem, measured on the board
 * ----------------------------------
 * video-service is CentOS/RHEL 8 based (el8, /usr/lib64 layout, glibc 2.28).
 * The host is Ubuntu 20.04 (glibc 2.31) and its /usr/lib/aarch64-linux-gnu/
 * librockchip_mpp.so.0 therefore carries versioned undefined references:
 *
 *     pow@GLIBC_2.29   log@GLIBC_2.29   log2@GLIBC_2.29
 *
 * and nothing else from a newer glibc - readelf -V reports 2.17 / 2.27 / 2.28 /
 * 2.29 as the complete set of required nodes.
 *
 * Two things were measured and both rule out the cheap fixes:
 *   - librockchip_mpp.so.0 already has DT_NEEDED libm.so.6, so the gap is not a
 *     missing library; adding -lm ahead of it changes nothing and reproduces the
 *     same three undefined references verbatim.
 *   - the container's /lib64/libm.so.6 exports pow/log/log2 only at
 *     GLIBC_2.17. `nm -D` finds no 2.29 variant and `readelf -V` shows no
 *     GLIBC_2.29 version node at all.
 *
 * A versioned reference never binds to a differently-versioned definition, which
 * is why a perfectly good pow sitting in libm cannot satisfy the link.
 *
 * Why a version script is mandatory, not just .symver
 * ---------------------------------------------------
 * Naming a symbol GLIBC_2.29 is not enough: ld also requires that the version
 * node GLIBC_2.29 exists in the shared objects on its link map before it will
 * accept a reference qualified with it. The exported-version map in the .map file
 * is what creates that node. Without it the link keeps failing with the identical
 * message, which is easy to misread as "the shim is not being picked up".
 *
 * Why this is not a reimplementation of math
 * ------------------------------------------
 * Each function forwards to glibc's own *_finite entry point. Those are real
 * exported symbols in the container's libm at GLIBC_2.17 (verified with nm -D),
 * are part of the long-standing ABI (they are what older gcc's -ffast-math
 * called), and are the same implementation with the special-value pre-dispatch
 * folded away. So this is a version-node relabel plus one call, and it introduces
 * no numerics of its own.
 *
 *   pow  -> __pow_finite     log  -> __log_finite     log2 -> __log2_finite
 *
 * Deployment notes
 * ----------------
 * SUPERSEDED -- this shim does not work, in either naming scheme, and the reason
 * is structural rather than a matter of getting the invocation right.
 *
 * It does clear the static link (with the .map file supplying the GLIBC_2.29
 * node, SHIM_OK and COMPILE_OK both printed). It then fails at execution:
 *
 *     ./mpp_probe_in: /lib64/libm.so.6: version `GLIBC_2.29' not found
 *     (required by /tmp/librockchip_mpp.so.1)
 *
 * ld.so's startup version audit never searches symbols. It takes the vn_file
 * string recorded in DT_VERNEED -- here the literal "libm.so.6" -- resolves that
 * single name to an already-loaded object, and looks for the version node only
 * inside it. So:
 *   - named libmppshim.so.6 and preloaded: never consulted, because the audit
 *     only ever asks the object called libm.so.6. This is what was measured.
 *   - named libm.so.6, as an earlier revision of this comment proposed: it would
 *     satisfy the audit but shadows the real libm, and then sin/cos/sqrt/exp are
 *     missing. That proposal was wrong, not merely untried.
 *
 * Correct conclusion: the constraint lives inside librockchip_mpp.so's own
 * .gnu.version_r, so that is what has to change. See RUNTIME/tools/
 * mpp_glibc_patch.c, which retargets the requirement at GLIBC_2.17 -- the version
 * the container's libm really does export -- and needs no shim, no LD_PRELOAD and
 * no base image swap.
 *
 * Build (inside the container, alongside the .map file):
 *
 *     gcc -O2 -fPIC -shared -Wall glibc229_pow_log_log2_shim.c \
 *         -o libmppshim.so.6 -Wl,--version-script=glibc229_shim.map -lm
 *
 * Checks that must pass before this is trusted in the streaming path
 * -----------------------------------------------------------------
 *  1. readelf -V shows GLIBC_2.29 as a DEFINED version providing exactly
 *     pow/log/log2, and GLIBC_2.17 as the only needed version.
 *  2. readelf -d shows SONAME libm.so.6.
 *  3. ldd -r on the final binary reports no unresolved versioned symbols.
 *  4. Numerical equivalence is NOT assumed. Encode the same synthetic NV12 frame
 *     through the shim on glibc 2.28 and natively on the host's glibc 2.31, then
 *     compare the H.264 streams byte for byte. Rockchip's rate controller is what
 *     consumes these functions, so a divergence would present as bitrate drift or
 *     quality wobble rather than a crash - the worst thing to discover only after
 *     deployment.
 */

#include <math.h>

/* glibc 2.28 exports these from libm at GLIBC_2.17. Named exactly so that the
 * shim itself cannot end up requiring a newer glibc than it fixes. */
extern double __pow_finite(double, double);
extern double __log_finite(double);
extern double __log2_finite(double);

/* .symver, not __asm__("pow@@GLIBC_2.29"): the latter makes gcc emit a raw label
 * named pow@@GLIBC_2.29, and aarch64 as does not give '@' the x86 meaning, so it
 * fails with "unknown mnemonic `pow'". */
__asm__(".symver shim_pow,pow@@GLIBC_2.29");
__asm__(".symver shim_log,log@@GLIBC_2.29");
__asm__(".symver shim_log2,log2@@GLIBC_2.29");

double shim_pow(double x, double y) {
    return __pow_finite(x, y);
}

double shim_log(double x) {
    return __log_finite(x);
}

double shim_log2(double x) {
    return __log2_finite(x);
}
