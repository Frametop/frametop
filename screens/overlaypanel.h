// A standalone program's CPU-drawn overlay panel shown without SetOverlayRaw's flicker: three
// linear DMA-BUFs SteamVR imports once, each picture copied (premultiplied) into the next and the
// overlay switched to it with SetOverlayTexture. ft-spotpanel, ft-gazepanel and ft-handpanel had a
// copy each. Each program keeps its own Present (its locks, its SetOverlayRaw fallback while ok is
// false, its texture bounds) around Fill and Texture here, and opens its own device on the render
// node: they're separate processes, so there's nothing to share as ft-screens' rendernode does.
// ft-screens' own overlay textures (overlaytex.cpp) keep their images by content instead, so
// they aren't this.
//   OVERLAYPANEL_PURE leaves out everything but the copy (screens/tests/overlaypanel-test.cpp,
// which has no OpenVR or GBM headers).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#ifndef OVERLAYPANEL_PURE
#include "rendernode.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <openvr.h>
#include <unistd.h>

#include <cstdio>
#endif

namespace overlaypanel {

struct Area {
    int x, y, w, h;
};

// The part of a pw x ph picture to copy: area, inside the picture, if the buffer already has
// the whole picture area changed from (fresh); otherwise all of it.
inline Area ToCopy(const Area *area, bool fresh, int pw, int ph) {
    Area all{0, 0, pw, ph}, a = area && fresh ? *area : all;
    a.x = std::clamp(a.x, 0, pw), a.y = std::clamp(a.y, 0, ph);
    a.w = std::clamp(a.w, 0, pw - a.x), a.h = std::clamp(a.h, 0, ph - a.y);
    return a;
}

// Area a of px (pw wide, RGBA, straight alpha) into dst (stride bytes a row, the same place),
// premultiplied.
inline void CopyPremultiplied(uint8_t *dst, uint32_t stride, const uint8_t *px, int pw, Area a) {
    for (int y = a.y; y < a.y + a.h; ++y) {
        const uint8_t *src = &px[(size_t(y) * pw + a.x) * 4];
        uint8_t *row = dst + size_t(y) * stride + size_t(a.x) * 4;
        for (int i = 0; i < a.w * 4; i += 4) {
            const unsigned al = src[i + 3];
            row[i] = uint8_t(src[i] * al / 255), row[i + 1] = uint8_t(src[i + 1] * al / 255);
            row[i + 2] = uint8_t(src[i + 2] * al / 255), row[i + 3] = uint8_t(al);
        }
    }
}

#ifndef OVERLAYPANEL_PURE

struct Buffer {
    gbm_bo *bo = nullptr;
    int fd = -1;
    vr::SharedTextureHandle_t handle = 0;
    uint64_t whole = 0;  // the whole picture it last had (Ring::whole), 0: none
};

struct Ring {
    const char *who;  // the program, for its lines on stderr
    int w = 0, h = 0;  // the biggest picture they take
    int drm = -1;
    gbm_device *gbm = nullptr;
    Buffer b[3];
    int next = 0;
    bool ok = false;
    uint64_t whole = 0;  // counts the pictures filled whole

    explicit Ring(const char *who, int w = 0, int h = 0) : who(who), w(w), h(h) {}

    // The three buffers, width x height; false (and a line on stderr) if they can't be made, and
    // then nothing is kept but the size.
    bool Make(int width, int height) {
        w = width, h = height;
        drm = open(FT_RENDER_NODE, O_RDWR | O_CLOEXEC);
        if (drm >= 0) gbm = gbm_create_device(drm);
        for (Buffer &x : b) {
            // ABGR8888 is R, G, B, A in memory, like the programs' pictures.
            if (gbm) x.bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
            if (!x.bo || (x.fd = gbm_bo_get_fd(x.bo)) < 0) break;
            vr::DmabufAttributes_t a{};
            a.unWidth = w, a.unHeight = h;
            a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
            a.unFormat = DRM_FORMAT_ABGR8888;
            a.ulModifier = DRM_FORMAT_MOD_LINEAR;
            a.unPlaneCount = 1;
            a.plane[0].unOffset = gbm_bo_get_offset(x.bo, 0);
            a.plane[0].unStride = gbm_bo_get_stride(x.bo);
            a.plane[0].nFd = x.fd;
            if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &x.handle)) x.handle = 0;
            if (!x.handle) break;
        }
        ok = b[2].handle != 0;
        if (!ok) {
            std::fprintf(stderr, "%s: no shared buffers; falling back to SetOverlayRaw (it flickers)\n", who);
            Drop();
        }
        return ok;
    }

    // Everything but the size.
    void Drop() {
        for (Buffer &x : b) {
            if (x.handle) vr::VRIPCResourceManager()->UnrefResource(x.handle);
            if (x.fd >= 0) close(x.fd);
            if (x.bo) gbm_bo_destroy(x.bo);
            x = Buffer{};
        }
        if (gbm) gbm_device_destroy(gbm);
        if (drm >= 0) close(drm);
        gbm = nullptr, drm = -1, ok = false;
    }

    // The picture (pw x ph, at most w x h, RGBA, straight alpha) into the next buffer,
    // premultiplied (the overlay's flag says so); that buffer, or nullptr (and a line on stderr)
    // if it can't be mapped. With area: only that changed since the last whole picture, so a
    // buffer that has that picture gets only the area. Only while ok.
    Buffer *Fill(const uint8_t *px, int pw, int ph, const Area *area = nullptr) {
        Buffer &x = b[next];
        next = (next + 1) % 3;
        if (!area) ++whole;
        const Area a = ToCopy(area, x.whole == whole, pw, ph);
        x.whole = whole;
        uint32_t stride = 0;
        void *mapping = nullptr;
        auto *dst = static_cast<uint8_t *>(gbm_bo_map(x.bo, 0, 0, pw, ph, GBM_BO_TRANSFER_WRITE, &stride, &mapping));
        if (!dst) {
            std::fprintf(stderr, "%s: can't map a buffer\n", who);
            return nullptr;
        }
        CopyPremultiplied(dst, stride, px, pw, a);
        gbm_bo_unmap(x.bo, mapping);
        return &x;
    }

    // The part of the buffers a pw x ph picture fills, for SetOverlayTextureBounds.
    vr::VRTextureBounds_t Bounds(int pw, int ph) const { return {0, 0, float(pw) / w, float(ph) / h}; }

    // A line on stderr if the picture didn't go to SteamVR.
    void Check(vr::IVROverlay *ov, vr::EVROverlayError e) const {
        if (e != vr::VROverlayError_None)
            std::fprintf(stderr, "%s: the picture didn't go to SteamVR: %s\n", who, ov->GetOverlayErrorNameFromEnum(e));
    }
};

// The texture for SetOverlayTexture (x must outlive the call).
inline vr::Texture_t Texture(Buffer &x) {
    return {&x.handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
}

#endif  // OVERLAYPANEL_PURE

}  // namespace overlaypanel
