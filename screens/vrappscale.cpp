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

// A spin of the wheel is one change on the PC: the last step, once it rests this long.
constexpr int64_t kRestMs = 220;

struct App {
    double scale = 1;       // what the menu says: the PC's, or the one asked for
    appscale::Wheel wheel;
    int64_t askAt = -1;     // ms: ask for it then (-1: nothing to ask)
};
std::map<int, App> g_apps;

App *AppAt(int i) {
    const auto it = g_apps.find(i);
    const auto s = g_screens.find(i);
    return it != g_apps.end() && s != g_screens.end() && IsWindowPanel(s->second) ? &it->second : nullptr;
}

void Ask(int i, App &a) {
    a.askAt = -1;
    char scale[16];
    std::snprintf(scale, sizeof scale, "%.4f", a.scale);
    RunLayout({"window", "scale", std::to_string(i + 1), scale});
}

}  // namespace

double AppScale(int i) {
    const App *a = AppAt(i);
    return a ? a->scale : 0;
}

void SetAppScale(int i, double scale, const char *by) {
    App *a = AppAt(i);
    if (!a || appscale::Same(a->scale, scale)) return;
    std::printf("screen %d: scale %d%% (%s)\n", i + 1, appscale::Percent(scale), by);
    a->scale = scale;
    Ask(i, *a);
}

void UpdateAppScale() {
    const int64_t now = NowMs();
    for (auto &[i, a] : g_apps)
        if (a.askAt >= 0 && now >= a.askAt) {
            if (!AppAt(i)) { a.askAt = -1; continue; }   // (its panel went meanwhile)
            std::printf("screen %d: scale %d%% (Meta+scroll)\n", i + 1, appscale::Percent(a.scale));
            Ask(i, a);
        }
}

}  // namespace ftvr

using namespace ftvr;

extern "C" {

void ft_vr_remote_scale(int index, double scale) {
    if (scale > 0) g_apps[index] = App{scale};
    else g_apps.erase(index);
}

bool ft_vr_app_scale_wheel(int index, double dy) {
    App *a = g_meta && dy != 0 ? AppAt(index) : nullptr;
    if (!a) return false;
    SendRelay("notap");   // (Meta was held for this: its release opens no menu)
    const double to = appscale::Step(a->scale, a->wheel.Add(-dy));
    if (!appscale::Same(to, a->scale)) a->scale = to, a->askAt = NowMs() + kRestMs;
    return true;
}

}  // extern "C"
