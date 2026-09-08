#ifndef RUNTIME_MPP_MPPENV_H
#define RUNTIME_MPP_MPPENV_H

#include <string>

#include <rockchip/rk_mpi.h>

namespace runtime {
namespace mpp {

/** VEPU/rkvdec want the strides on a grid, see mpp_enc_probe.c. */
inline int alignUp(int v, int n) { return (v + n - 1) / n * n; }

/**
 * Board-level MPP environment probes shared by the encoder and the decoder.
 *
 * Everything here exists because the runtime runs inside a container that is
 * not the image the Rockchip BSP was built for: the usable buffer allocation
 * path (DRM vs DMA_HEAP vs plain heap) and even whether /dev/mpp_service was
 * mounted can only be discovered at runtime. mpp_enc_probe.c proved the probe
 * order on this exact board, this just moves it into the process.
 */
class MppEnv {
public:
    /**
     * Cheap, allocation-free check that this host has a Rockchip video codec
     * device. `reason` receives a human-readable explanation on false so the
     * caller can log why it fell back to software.
     */
    static bool hostSupported(std::string* reason = nullptr);

    /**
     * Create an internal buffer group using the first allocation path that the
     * board services (DRM -> DMA_HEAP -> NORMAL). Returns NULL when none works,
     * which means hardware codec is unusable regardless of device nodes.
     *
     * `what` only labels the log line ("enc" / "dec").
     * Caller owns the group and must mpp_buffer_group_put() it.
     */
    static MppBufferGroup openBufferGroup(const char* what, MppBufferType* typeOut = nullptr);

    /** Log-friendly name for a buffer type token. */
    static const char* bufferTypeName(MppBufferType type);
};

}  // namespace mpp
}  // namespace runtime

#endif  // RUNTIME_MPP_MPPENV_H
