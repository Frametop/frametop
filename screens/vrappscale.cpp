// The scale of a PC app window's panel (docs/remote-displays.md, "Scale"): how big its app draws in the
// same pixels. ft-layout says it ("remote N scale 1.25", with the stream's start) for a window whose PC
// takes scales; the bar's menu has a Scale page for it (vrbarmenu.cpp), and Meta+scroll over the panel
// steps it, as it does a floating window's. Either goes to the PC through `ft-layout window scale`, which
// keeps it and answers with the scale the panel has then (the old one, when the PC didn't take it).
#include "vrpanel.h"
#include "appscale.h"

#include <cstdio>
#include <map>
#include <string>

namespace ftvr {
namespace {

std::map<int, appscale::Asking> g_apps;

appscale::Asking *AppAt(int i) {
    const auto it = g_apps.find(i);
    const auto s = g_screens.find(i);
    return it != g_apps.end() && s != g_screens.end() && IsWindowPanel(s->second) ? &it->second : nullptr;
}

// The size that takes its scale: a corner dragged to one a few pixels off waits for a picture that size,
// not the one dragged to, or for none if it's the one it shows.
void TakeSize(int i, int w, int h) {
    const auto it = g_screens.find(i);
    if (it == g_screens.end() || !IsWindowPanel(it->second)) return;
    Screen &s = it->second;
    if (s.stretchW <= 0 || s.drag == Drag::Resize || FarStretching(s)) return;   // (not resized, or still being)
    if (!appscale::Near(w, h, s.stretchW, s.stretchH)) return;   // (said of an earlier size)
    if (w == s.width && h == s.height) Stretch(s, 0, 0);
    else if (w != s.stretchW || h != s.stretchH) Stretch(s, w, h);
    else return;
    if (TileAnchored(i)) RetileDash();
}

}  // namespace

double AppScale(int i) {
    const auto *a = AppAt(i);
    return a ? a->scale : 0;
}

void SetAppScale(int i, double scale) {
    if (auto *a = AppAt(i)) a->Pick(scale, NowMs());
}

void ForgetAppScale(int i) { g_apps.erase(i); }

void UpdateAppScale() {
    const int64_t now = NowMs();
    for (auto &[i, a] : g_apps) {
        if (!AppAt(i) || !a.Due(now)) continue;
        std::printf("screen %d: scale %d%%\n", i + 1, appscale::Percent(a.scale));
        char scale[16];
        std::snprintf(scale, sizeof scale, "%.4f", a.scale);
        RunLayout({"window", "scale", std::to_string(i + 1), scale});
    }
}

}  // namespace ftvr

using namespace ftvr;

extern "C" {

void ft_vr_remote_scale(int index, double scale, int width, int height) {
    if (scale <= 0) return ForgetAppScale(index);
    g_apps[index].Said(scale);
    if (width > 0) TakeSize(index, width, height);
}

bool ft_vr_app_scale_wheel(int index, double dy) {
    auto *a = g_meta && dy != 0 ? AppAt(index) : nullptr;
    if (!a) return false;
    SendRelay("notap");   // (Meta was held for this: its release opens no menu)
    a->Scroll(-dy, NowMs());
    return true;
}

}  // extern "C"
