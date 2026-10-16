// Small CPU-drawn textures on ft-screens' overlays (the controls, the glows, the carousel) without
// SetOverlayRaw. SteamVR never gives back the memory block each SetOverlayRaw call takes: the 201st
// call in a process fails ("Refusing to create memory block because 201 blocks are already
// outstanding"), and the overlay keeps its old texture at its new size and place. That was the
// glow bug of 2026-10-15: after about 40 minutes of hand use, a corner's glow showed as the
// fingertip dot's disc, as big as the corner's glow, and "grab that"'s halo kept the last panel's
// shape. Measured with a probe: hidden, shown or transparent, destroyed or not, 20 s apart or
// not, every call keeps its block.
//   Here each distinct image goes into a linear DMA-BUF SteamVR imports once (as the keyboard's
// buffers, keyboard.cpp), kept by its content, and the overlay switches to it with
// SetOverlayTexture, which has no such limit (600 switches, and 600 imports dropped again, fine).
// Images no overlay shows are dropped, the least recently used first, past kKeep of them.
#pragma once

#include <openvr.h>

#include <cstdint>

namespace overlaytex {

constexpr int kKeep = 64;

// w x h pixels, RGBA, straight alpha (as SetOverlayRaw took them), onto overlay o. Falls back to
// SetOverlayRaw if the buffers can't be made.
vr::EVROverlayError Set(vr::VROverlayHandle_t o, const uint8_t *px, int w, int h);
// The overlay is going (before DestroyOverlay): its image may be dropped.
void Forget(vr::VROverlayHandle_t o);
// Every image dropped (at shutdown, after the overlays are gone).
void Shutdown();

}  // namespace overlaytex
