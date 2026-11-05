#pragma once

#include "openvr.h"
#include "panel.hpp"
#include "vk_texture.h"
#include <bit>

namespace framekeyboard {
// Full images still alternate between the two Vulkan textures. Damage tracking
// belongs to the renderer; partial uploads would need damage history per texture.
class PanelTexture {
  public:
    bool create(VulkanContext& context, vr::VROverlayHandle_t overlay, std::string& error,
                bool prefer_native = true) {
        context_ = &context;
        overlay_ = overlay;
        native_ = false;
        VkImageFormatProperties properties{};
        const bool supported = std::endian::native == std::endian::little &&
                               vkGetPhysicalDeviceImageFormatProperties(
                                   context.physicalDevice(), VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D,
                                   VK_IMAGE_TILING_OPTIMAL,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT,
                                   0, &properties) == VK_SUCCESS;
        if (prefer_native && supported &&
            vr::VROverlay()->SetOverlayFlag(overlay_, vr::VROverlayFlags_IsPremultiplied, true) ==
                vr::VROverlayError_None) {
            premultiplied_enabled_ = true;
            if (native_texture_.create(context, panel_width, texture_height, error,
                                       VK_FORMAT_B8G8R8A8_UNORM)) {
                native_ = true;
                return true;
            }
        }
        return create_rgba(error);
    }
    bool native_pixels() const { return native_; }
    void destroy() {
        native_texture_.destroy();
        rgba_texture_.destroy();
    }
    bool update(const PanelRenderer& renderer, std::string& error) {
        if (native_) {
            // Cairo ARGB32 is premultiplied BGRA bytes on little-endian Frame.
            // Its fixed-size image surface is tightly packed; check rather than
            // silently assuming that a future renderer keeps the same stride.
            auto* surface = renderer.surface();
            if (cairo_image_surface_get_stride(surface) == panel_width * 4 &&
                native_texture_.update(overlay_, cairo_image_surface_get_data(surface), error)) {
                return true;
            }
            // A runtime may accept the flag/format but reject the submitted
            // texture. Switch safely while the old images are still alive.
            const auto cleared = vr::VROverlay()->ClearOverlayTexture(overlay_);
            if (cleared != vr::VROverlayError_None) {
                error = "Cannot clear rejected native overlay texture";
                return false;
            }
            if (!create_rgba(error)) {
                return false;
            }
        }
        const auto rgba = renderer.rgba();
        return rgba_texture_.update(overlay_, rgba.data(), error);
    }

  private:
    bool create_rgba(std::string& error) {
        native_ = false;
        if (premultiplied_enabled_ &&
            vr::VROverlay()->SetOverlayFlag(overlay_, vr::VROverlayFlags_IsPremultiplied, false) !=
                vr::VROverlayError_None) {
            error = "Cannot configure straight-alpha overlay fallback";
            return false;
        }
        premultiplied_enabled_ = false;
        error.clear();
        return rgba_texture_.create(*context_, panel_width, texture_height, error);
    }
    VulkanContext* context_{};
    vr::VROverlayHandle_t overlay_{};
    // Retain rejected native images until VR shutdown, just like accepted ones.
    OverlayTexture native_texture_, rgba_texture_;
    bool native_{}, premultiplied_enabled_{};
};
} // namespace framekeyboard
