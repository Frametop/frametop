// The edge each panel's grab bar is on (barside.h's geometry; vr.cpp places the controls by
// Screen::barSide), picked from the bar's menu (vrbarmenu.cpp) or `barside N EDGE`.
//   Each panel's edge is remembered by its key (TileKnown's: "screen:2", "app:ID", "remote:HOST/APP")
// in ~/.config/frametop-bars.txt, a line each, "KEY<TAB>EDGE"; a panel with no key yet keeps its edge
// for the session.
#include "vrpanel.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace ftvr {
namespace {

dashtiles::Memory g_barMemory;          // key -> edge name (barside::Name)
std::map<int, std::string> g_barKey;    // panel -> its key (TileKnown's)

std::string BarsPath() {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/frametop-bars.txt";
}
// (a pick is a rare, deliberate act: written at once)
void WriteBars() {
    const std::string path = BarsPath(), tmp = path + ".tmp";
    std::FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return (void)std::fprintf(stderr, "grab bar edges: can't write %s\n", tmp.c_str());
    const std::string text = g_barMemory.Text();
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0)
        std::fprintf(stderr, "grab bar edges: can't write %s\n", path.c_str());
}

// Moved to an edge: its controls go there now.
void Apply(Screen &s, barside::Side side) {
    if (s.barSide == side) return;
    s.barSide = side;
    PlaceChrome(s);
}

}  // namespace

void LoadBarSides() {
    std::FILE *f = std::fopen(BarsPath().c_str(), "r");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (1 << 20)) text.append(buf, n);
    std::fclose(f);
    g_barMemory.Load(text);
    std::printf("grab bar edges: %zu remembered (%s)\n", g_barMemory.Size(), BarsPath().c_str());
}

void BarSideKnown(int i, const std::string &key) {
    g_barKey[i] = key;
    const auto it = g_screens.find(i);
    if (it == g_screens.end()) return;
    barside::Side side = barside::Side::Bottom;
    if (!barside::Parse(g_barMemory.Get(key), &side)) {
        // nothing remembered: an edge picked before its key came is its key's now
        if (it->second.barSide != barside::Side::Bottom && g_barMemory.Set(key, barside::Name(it->second.barSide))) WriteBars();
        return;
    }
    g_barMemory.Used(key);
    if (side != it->second.barSide)
        std::printf("screen %d: its grab bar on the %s (remembered for %s)\n", i + 1, barside::Name(side), key.c_str());
    Apply(it->second, side);
}

// Its app or display went: the next one on the panel starts at the bottom (or its own remembered edge).
void BarSideGone(int i) {
    if (BarMenuOn(i)) CloseBarMenu("its panel went");
    g_barKey.erase(i);
    const auto it = g_screens.find(i);
    if (it != g_screens.end()) it->second.barSide = barside::Side::Bottom;
}

bool SetBarSide(int i, barside::Side side, const char *by) {
    const auto it = g_screens.find(i);
    if (it == g_screens.end()) return false;
    const auto key = g_barKey.find(i);
    if (key != g_barKey.end() && g_barMemory.Set(key->second, barside::Name(side))) WriteBars();
    std::printf("screen %d: its grab bar on the %s (%s)%s\n", i + 1, barside::Name(side), by,
                key == g_barKey.end() ? "; not remembered: it has no key yet" : "");
    std::fflush(stdout);
    Apply(it->second, side);
    return true;
}

}  // namespace ftvr
