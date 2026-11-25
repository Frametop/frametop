// The edge each panel's grab bar is on, and whether it sits inside that edge or past it (barside.h's
// geometry; vr.cpp places the controls by Screen::barSide and barInset), picked from the bar's menu
// (vrbarmenu.cpp) or `barside N EDGE|inside|outside|default`.
//   Each panel's choices are remembered by its key (TileKnown's: "screen:2", "app:ID", "remote:HOST/APP")
// in ~/.config/frametop-bars.txt, a line each, "KEY<TAB>EDGE" or "KEY<TAB>EDGE inside|outside"
// (barside::Text); a panel with no key yet keeps them for the session. One with no inside or outside
// of its own follows the setting (GRAB_BAR_INSIDE).
#include "vrpanel.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace ftvr {
namespace {

dashtiles::Memory g_barMemory;          // key -> its edge and inset (barside::Text)
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

// Moved to an edge, inside it or past it: its controls go there now.
void Apply(Screen &s, barside::Side side, barside::Inset inset) {
    if (s.barSide == side && s.barInset == inset) return;
    s.barSide = side, s.barInset = inset;
    PlaceChrome(s);
}

// Its choices kept under its key. False: it has no key yet.
bool Remember(int i, barside::Side side, barside::Inset inset) {
    const auto key = g_barKey.find(i);
    if (key == g_barKey.end()) return false;
    if (g_barMemory.Set(key->second, barside::Text(side, inset))) WriteBars();
    return true;
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
    Screen &s = it->second;
    barside::Side side = barside::Side::Bottom;
    barside::Inset inset = barside::Inset::Default;
    if (!barside::Parse(g_barMemory.Get(key), &side, &inset)) {
        // nothing remembered: choices made before its key came are its key's now
        if (s.barSide != barside::Side::Bottom || s.barInset != barside::Inset::Default) Remember(i, s.barSide, s.barInset);
        return;
    }
    g_barMemory.Used(key);
    if (side != s.barSide || inset != s.barInset)
        std::printf("screen %d: its grab bar on the %s (remembered for %s)\n", i + 1, barside::Text(side, inset).c_str(), key.c_str());
    Apply(s, side, inset);
}

// Its app or display went: the next one on the panel starts at the bottom, inside or outside as the
// setting says (or with its own remembered choices).
void BarSideGone(int i) {
    if (BarMenuOn(i)) CloseBarMenu("its panel went");
    g_barKey.erase(i);
    const auto it = g_screens.find(i);
    if (it != g_screens.end()) it->second.barSide = barside::Side::Bottom, it->second.barInset = barside::Inset::Default;
}

bool SetBarSide(int i, barside::Side side, const char *by) {
    const auto it = g_screens.find(i);
    if (it == g_screens.end()) return false;
    const bool kept = Remember(i, side, it->second.barInset);
    std::printf("screen %d: its grab bar on the %s (%s)%s\n", i + 1, barside::Name(side), by,
                kept ? "" : "; not remembered: it has no key yet");
    std::fflush(stdout);
    Apply(it->second, side, it->second.barInset);
    return true;
}

bool SetBarInset(int i, barside::Inset inset, const char *by) {
    const auto it = g_screens.find(i);
    if (it == g_screens.end()) return false;
    const bool kept = Remember(i, it->second.barSide, inset);
    std::printf("screen %d: its grab bar %s (%s)%s\n", i + 1,
                inset == barside::Inset::Default ? "where the setting puts it" : inset == barside::Inset::Inside ? "inside its edge" : "past its edge",
                by, kept ? "" : "; not remembered: it has no key yet");
    std::fflush(stdout);
    Apply(it->second, it->second.barSide, inset);
    return true;
}

}  // namespace ftvr
