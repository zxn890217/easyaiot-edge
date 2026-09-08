#include "mpp/MppEnv.h"

#include <glob.h>

#include <cstdio>

#include <glog/logging.h>

namespace runtime {
namespace mpp {
namespace {

bool nodeExists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool anyRenderNode() {
    glob_t hits;
    const int rc = glob("/dev/dri/renderD*", 0, nullptr, &hits);
    const bool found = (rc == 0 && hits.gl_pathc > 0);
    if (rc == 0) globfree(&hits);
    return found;
}

}  // namespace

bool MppEnv::hostSupported(std::string* reason) {
    // RK3588 ships /dev/mpp_service; older vendor kernels call the same thing
    // /dev/vpu_service. Either one means the MPP userspace driver has something
    // to talk to.
    if (nodeExists("/dev/mpp_service") || nodeExists("/dev/vpu_service")) {
        if (!anyRenderNode() && reason) {
            // DRM buffers will then be unavailable; the group probe below falls
            // back to DMA_HEAP or NORMAL, so this is not a hard failure.
            LOG(WARNING) << "[MPP] no /dev/dri/renderD* present, buffer groups will "
                            "fall back to DMA_HEAP or normal heap";
        }
        return true;
    }
    if (reason) *reason = "/dev/mpp_service (or /dev/vpu_service) missing";
    return false;
}

MppBufferGroup MppEnv::openBufferGroup(const char* what, MppBufferType* typeOut) {
    MppBufferGroup grp = nullptr;
    static const MppBufferType order[] = {MPP_BUFFER_TYPE_DRM, MPP_BUFFER_TYPE_DMA_HEAP,
                                          MPP_BUFFER_TYPE_NORMAL};

    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        MppBufferGroup cand = nullptr;
        const int rc = mpp_buffer_group_get_internal(&cand, order[i]);
        if (rc == 0 && cand) {
            grp = cand;
            if (typeOut) *typeOut = order[i];
            LOG(INFO) << "[MPP] " << (what ? what : "ctx")
                      << " buffer_group=" << bufferTypeName(order[i]);
            return grp;
        }
        LOG(INFO) << "[MPP] " << (what ? what : "ctx") << " buffer_group "
                  << bufferTypeName(order[i]) << " unavailable (rc=" << rc << ")";
    }

    LOG(ERROR) << "[MPP] no buffer group available - check /dev/dri/renderD*, "
                  "/dev/dma_heap/* and /dev/mpp_service in the container";
    return nullptr;
}

const char* MppEnv::bufferTypeName(MppBufferType type) {
    // if-chain over stringified tokens instead of a switch: MPP has historically
    // aliased ION onto NORMAL, which a switch would reject as duplicate cases.
#define RUNTIME_CMP(x)              \
    if (static_cast<int>(type) == static_cast<int>(x)) return #x;
    RUNTIME_CMP(MPP_BUFFER_TYPE_NORMAL)
    RUNTIME_CMP(MPP_BUFFER_TYPE_DRM)
    RUNTIME_CMP(MPP_BUFFER_TYPE_ION)
    RUNTIME_CMP(MPP_BUFFER_TYPE_EXT_DMA)
    RUNTIME_CMP(MPP_BUFFER_TYPE_DMA_HEAP)
    RUNTIME_CMP(MPP_BUFFER_TYPE_BUTT)
#undef RUNTIME_CMP
    return "MPP_BUFFER_TYPE_UNKNOWN";
}

}  // namespace mpp
}  // namespace runtime
