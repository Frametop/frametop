// A flat picture ft-avatars shows in the room, drawn afresh many times a second: a SteamVR
// overlay showing both eyes' views side by side, and the three buffers it is drawn through
// (vroutput.h): the one shown, the one shown before it (SteamVR may still be reading it), and
// one being drawn. They share a depth buffer: one is drawn at a time. The mirror is one, and so
// is your own body below you.
#pragma once

#include "mirror.h"
#include "vroutput.h"

namespace sheet {

class Sheet {
public:
    // An overlay `key` (SteamVR's name for it) called `name`, 2 x `w` by `h` pixels, sorted at
    // `order` among the overlays. False with a line on stderr when it can't be made.
    bool Make(const vroutput::Maker &maker, const char *key, const char *name, int order, int w, int h);
    void Free(const vroutput::Maker &maker);

    // Where it is in the room from now on.
    void Stand(const mirror::Glass &at);
    // The buffer to draw next, bound: neither the one shown nor the one before it. `at`: where
    // the sheet is to be once this picture is up (for one that moves with what it shows).
    vroutput::Buffer &Next(const mirror::Glass *at = nullptr);
    // A buffer the GPU has finished goes up, and the sheet is shown if it wasn't. True when one did.
    bool Show(const vroutput::Maker &maker);
    // Out of sight until the next picture is up.
    void Hide();

    bool drawing() const { return drawing_ >= 0; }   // a picture is with the GPU: Next waits for Show
    bool visible() const { return visible_; }

private:
    vr::VROverlayHandle_t overlay_ = vr::k_ulOverlayHandleInvalid;
    vroutput::Buffer out_[3];
    mirror::Glass at_[3];
    bool moves_[3] = {};
    GLuint depth_ = 0;
    int shown_ = -1, before_ = -1, drawing_ = -1;
    bool visible_ = false;
};

}  // namespace sheet
