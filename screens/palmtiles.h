// The palm menu's tiles (user, 2026-11-25: "completely customize the palm menu ... what the text on a
// action should be, and then what the command should run"): which tile each page of Quick Access shows,
// its words, and the input relay's action it runs, from ~/.config/frametop-palm.txt. Frametop Settings'
// Quick Settings page writes the file (conf/ft_palm_menu.py, the same parser in Python; its notes have
// the format), and the relay runs an action from here only if the file shows it.
//   With no file: kDefault, whose Quick Settings page shows those QUICK_SETTINGS in frametop.conf picks.
// Pure (screens/tests/palmtiles-test.cpp); vrquick.cpp reads the file and draws the card.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace palmtiles {

constexpr int kMax = 6;         // a page's tiles (quickaccess.h's kTiles)
constexpr int kLabelMax = 40;   // a label's characters (the rest is left out)

struct Entry {
    std::string action, label;   // (the relay's action, or ours: kReset)
};

struct Menu {
    std::vector<Entry> first, quick;   // what each page shows, in order, kMax at most
    Entry eyes;                        // the wide row while gaze mode is on
    bool eyesShown = false;
};

constexpr const char *kSteamMenu = "steam_menu";   // the palm's own pinch starts on this one
constexpr const char *kReset = "layout_reset";     // ft-screens' own (DoReset)

// conf/ft_palm_menu.py's DEFAULT (quicksettings/test checks it).
constexpr const char *kDefault =
    "menu\t1\tsteam_menu\tSteam Menu\n"
    "menu\t1\tscreens_toggle\tHide Screens\n"
    "menu\t1\tlayout_reset\tReset Layout…\n"
    "menu\t1\tspin_prev\tSpin Left\n"
    "menu\t1\tspin_next\tSpin Right\n"
    "menu\t1\tdock_all\tDock Windows\n"
    "quick\t1\tscreens_toggle\tScreens\n"
    "quick\t1\tkeyboard_toggle\tKeyboard\n"
    "quick\t1\tdesktopless_toggle\tDesktopless\n"
    "quick\t1\thands_toggle\tHand Tracking\n"
    "quick\t1\tpause_toggle\tGame Pause\n"
    "quick\t1\tdictate\tDictation\n"
    "eyes\t1\tgaze_quickcal\tQuick Eye Calibrate\n";

inline std::string Trim(const std::string &s) {
    const char *kSpace = " \t\r\n";
    const size_t a = s.find_first_not_of(kSpace);
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(kSpace) - a + 1);
}

// The first n characters of UTF-8 text.
inline std::string Head(const std::string &s, int n) {
    size_t i = 0;
    for (; i < s.size(); ++i)
        if ((s[i] & 0xc0) != 0x80 && n-- == 0) break;
    return s.substr(0, i);
}

// `item` is one of `list`'s, commas between.
inline bool Listed(const std::string &list, const std::string &item) {
    for (size_t at = 0; at <= list.size();) {
        const size_t end = std::min(list.find(',', at), list.size());
        if (Trim(list.substr(at, end - at)) == item) return true;
        at = end + 1;
    }
    return false;
}

// One line, "PAGE<TAB>1|0<TAB>ACTION<TAB>LABEL"; false: blank, a comment or not one.
inline bool ParseLine(const std::string &line, std::string *page, bool *on, Entry *e) {
    if (Trim(line).rfind("#", 0) == 0) return false;
    std::vector<std::string> parts;
    for (size_t at = 0; at <= line.size();) {
        const size_t end = std::min(line.find('\t', at), line.size());
        parts.push_back(Trim(line.substr(at, end - at)));
        at = end + 1;
    }
    if (parts.size() != 4 || (parts[1] != "0" && parts[1] != "1") || parts[2].empty() || parts[3].empty()) return false;
    *page = parts[0], *on = parts[1] == "1", *e = {parts[2], Trim(Head(parts[3], kLabelMax))};
    return true;
}

// A file's text as the card: bad lines left out, a page's first kMax shown, the first eyes row (the
// default one with none). `quick`: only these are shown on the Quick Settings page (null: all).
inline Menu Parse(const std::string &text, const char *quick = nullptr) {
    Menu m;
    bool haveEyes = false;
    for (size_t at = 0; at < text.size();) {
        const size_t end = std::min(text.find('\n', at), text.size());
        std::string page;
        bool on = false;
        Entry e;
        if (ParseLine(text.substr(at, end - at), &page, &on, &e)) {
            if (page == "eyes") {
                if (!haveEyes) m.eyes = e, m.eyesShown = on, haveEyes = true;
            } else if (page == "menu" || page == "quick") {
                std::vector<Entry> &to = page == "menu" ? m.first : m.quick;
                if (page == "quick" && quick) on = on && Listed(quick, e.action);
                if (on && int(to.size()) < kMax) to.push_back(e);
            }
        }
        at = end + 1;
    }
    if (!haveEyes) m.eyes = {"gaze_quickcal", "Quick Eye Calibrate"}, m.eyesShown = true;
    return m;
}

// The card with no file. `quick`: QUICK_SETTINGS's value (null: unset, all; empty: none).
inline Menu Defaults(const char *quick) { return Parse(kDefault, quick); }

// What still works while Frametop is paused for a VR game: the relay's PAUSED_ACTIONS
// (input/game_pause.py; quicksettings/test checks it) and commands.
constexpr const char *kPausedActions[] = {"pause_toggle", "steam_menu", "dictate", "dictate_hold", "release_keys"};
inline bool WorksPaused(const std::string &action) {
    for (const char *a : kPausedActions)
        if (action == a) return true;
    return action.rfind("run:", 0) == 0 || action.rfind("command:", 0) == 0;
}

// A screens toggle by its own name says which it would do, and needs no light for it.
inline bool SaysScreens(const Entry &e) {
    return e.action == "screens_toggle" && (e.label == "Hide Screens" || e.label == "Show Screens");
}

// A tile's words now.
inline std::string Words(const Entry &e, bool screensVisible) {
    return SaysScreens(e) ? screensVisible ? "Hide Screens" : "Show Screens" : e.label;
}

// Lit: a toggle that's on, where ft-screens knows (the rest show no state).
inline bool Lit(const Entry &e, bool screensVisible, bool paused) {
    if (e.action == "screens_toggle") return screensVisible && !SaysScreens(e);
    return e.action == "pause_toggle" && paused;
}

// The slot the palm's own pinch starts on, on the first page: Steam Menu's, wherever it is; on one
// without it, the first tile; on one with no tiles, kMax (the Quick Settings button: quickaccess.h's kButton).
inline int PinchSlot(const Menu &m) {
    for (size_t i = 0; i < m.first.size(); ++i)
        if (m.first[i].action == kSteamMenu) return int(i);
    return m.first.empty() ? kMax : 0;
}

}  // namespace palmtiles
