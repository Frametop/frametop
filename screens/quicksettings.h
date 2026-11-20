// The palm menu's Quick Settings (user, 2026-11-19: "a quick settings menu that when clicked opens a small
// window that has toggle options ... ones that you can configure from inside of the FrameTop Settings
// application"): the toggles there are, and which the card shows, from QUICK_SETTINGS in frametop.conf
// (Frametop Settings' Quick Settings page, quicksettings/ft_quick_settings.py, writes it).
// conf/ft_input_actions.py's QUICK_SETTINGS_ACTIONS is the same list (quicksettings/test checks it); the input
// relay runs each from ft-screens under its rules. Pure (screens/tests/quickaccess-test.cpp); vrquick.cpp shows them.
#pragma once

#include <cstring>
#include <string>
#include <vector>

namespace quicksettings {

// Whose state a toggle shows (lit when on): ft-screens knows only some.
enum class State { None, Screens, Paused };

struct Toggle {
    const char *action;   // the relay's action, and its name in QUICK_SETTINGS
    const char *label;
    State state;
};

// Every one there is, in the order the card shows them.
constexpr Toggle kAll[] = {
    {"screens_toggle", "Screens", State::Screens},
    {"keyboard_toggle", "Keyboard", State::None},
    {"desktopless_toggle", "Desktopless", State::None},
    {"hands_toggle", "Hand Tracking", State::None},
    {"pause_toggle", "Game Pause", State::Paused},
    {"dictate", "Dictation", State::None},
};
constexpr int kCount = int(sizeof kAll / sizeof kAll[0]);
constexpr int kMax = 6;   // the card's grid
static_assert(kCount <= kMax, "every toggle fits the card: a picker can't pick more than it shows");

// QUICK_SETTINGS's value ("a,b,c"; unset: every one) as indices into kAll, in kAll's order: names it
// doesn't know are left out, and so are repeats. An empty value means none.
inline std::vector<int> Chosen(const char *value) {
    std::vector<int> out;
    for (int i = 0; i < kCount; ++i) {
        bool on = !value;
        for (const char *p = value; p && *p && !on;) {
            const char *end = std::strchr(p, ',');
            const size_t n = end ? size_t(end - p) : std::strlen(p);
            on = n == std::strlen(kAll[i].action) && !std::strncmp(p, kAll[i].action, n);
            p = end ? end + 1 : nullptr;
        }
        if (on) out.push_back(i);
    }
    return out;
}

// What still works while Frametop is paused for a VR game: the relay lets only pause_toggle through.
inline bool WorksPaused(int i) { return i >= 0 && i < kCount && !std::strcmp(kAll[i].action, "pause_toggle"); }

}  // namespace quicksettings
