// See overlaytex.h.
#include "overlaytex.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

namespace overlaytex {
namespace {

struct Image {
    gbm_bo *bo = nullptr;
    int fd = -1;
    vr::SharedTextureHandle_t handle = 0;
    int users = 0;         // overlays showing it
    uint64_t used = 0;     // when last put on one (a counter)
};
using Key = std::tuple<int, int, uint64_t>;   // w, h, the pixels' hash

int g_drm = -1;
gbm_device *g_gbm = nullptr;
int g_state = 0;   // 0: not tried yet, 1: buffers, -1: SetOverlayRaw
std::map<Key, Image> g_images;
std::map<vr::VROverlayHandle_t, Key> g_on;   // what each overlay shows
uint64_t g_clock = 0;

uint64_t Hash(const uint8_t *px, size_t n) {   // FNV-1a, 8 bytes at a time
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, px + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < n; ++i) h = (h ^ px[i]) * 1099511628211ull;
    return h;
}

void Drop(Image &im) {
    if (im.handle) vr::VRIPCResourceManager()->UnrefResource(im.handle);
    if (im.fd >= 0) close(im.fd);
    if (im.bo) gbm_bo_destroy(im.bo);
    im = Image{};
}

bool Ready() {
    if (g_state) return g_state > 0;
    g_drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (g_drm >= 0) g_gbm = gbm_create_device(g_drm);
    g_state = g_gbm ? 1 : -1;
    if (g_state < 0) std::printf("overlay textures: no render node; using SetOverlayRaw (at most 200 changes)\n");
    return g_state > 0;
}

bool Make(Image &im, const uint8_t *px, int w, int h) {
    im.bo = gbm_bo_create(g_gbm, uint32_t(w), uint32_t(h), GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!im.bo || (im.fd = gbm_bo_get_fd(im.bo)) < 0) return false;
    uint32_t stride = 0;
    void *mapping = nullptr;
    auto *dst = static_cast<uint8_t *>(gbm_bo_map(im.bo, 0, 0, uint32_t(w), uint32_t(h), GBM_BO_TRANSFER_WRITE, &stride, &mapping));
    if (!dst) return false;
    for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * stride, px + size_t(y) * w * 4, size_t(w) * 4);   // (ABGR8888: R, G, B, A in memory)
    gbm_bo_unmap(im.bo, mapping);
    vr::DmabufAttributes_t a{};
    a.unWidth = uint32_t(w), a.unHeight = uint32_t(h);
    a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
    a.unFormat = DRM_FORMAT_ABGR8888;
    a.ulModifier = DRM_FORMAT_MOD_LINEAR;
    a.unPlaneCount = 1;
    a.plane[0].unOffset = gbm_bo_get_offset(im.bo, 0);
    a.plane[0].unStride = gbm_bo_get_stride(im.bo);
    a.plane[0].nFd = im.fd;
    return vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &im.handle) && im.handle;
}

void Release(vr::VROverlayHandle_t o) {
    auto on = g_on.find(o);
    if (on == g_on.end()) return;
    auto it = g_images.find(on->second);
    if (it != g_images.end() && it->second.users > 0) --it->second.users;
    g_on.erase(on);
}

// Past kKeep images: drop the least recently used no overlay shows.
void Trim() {
    while (int(g_images.size()) > kKeep) {
        auto old = g_images.end();
        for (auto it = g_images.begin(); it != g_images.end(); ++it)
            if (!it->second.users && (old == g_images.end() || it->second.used < old->second.used)) old = it;
        if (old == g_images.end()) return;
        Drop(old->second);
        g_images.erase(old);
    }
}

}  // namespace

vr::EVROverlayError Set(vr::VROverlayHandle_t o, const uint8_t *px, int w, int h) {
    if (o == vr::k_ulOverlayHandleInvalid) return vr::VROverlayError_UnknownOverlay;
    if (!Ready()) return vr::VROverlay()->SetOverlayRaw(o, const_cast<uint8_t *>(px), uint32_t(w), uint32_t(h), 4);
    const Key key{w, h, Hash(px, size_t(w) * h * 4)};
    auto it = g_images.find(key);
    if (it == g_images.end()) {
        Image im;
        if (!Make(im, px, w, h)) {
            std::fprintf(stderr, "overlay textures: a %dx%d buffer failed; SetOverlayRaw this once\n", w, h);
            Drop(im);
            Release(o);
            return vr::VROverlay()->SetOverlayRaw(o, const_cast<uint8_t *>(px), uint32_t(w), uint32_t(h), 4);
        }
        it = g_images.emplace(key, im).first;
    }
    const auto on = g_on.find(o);
    if (on == g_on.end() || on->second != key) {
        Release(o);
        g_on[o] = key, ++it->second.users;
    }
    it->second.used = ++g_clock;
    vr::Texture_t tex = {&it->second.handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    const vr::EVROverlayError err = vr::VROverlay()->SetOverlayTexture(o, &tex);
    Trim();
    return err;
}

void Forget(vr::VROverlayHandle_t o) { Release(o); }

void Shutdown() {
    for (auto &[k, im] : g_images) Drop(im);
    g_images.clear(), g_on.clear();
    if (g_gbm) gbm_device_destroy(g_gbm);
    if (g_drm >= 0) close(g_drm);
    g_gbm = nullptr, g_drm = -1, g_state = 0;
}

}  // namespace overlaytex
