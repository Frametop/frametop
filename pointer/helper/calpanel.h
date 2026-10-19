// The gaze calibration panel's answers (see ft-pointer.cpp's top): while a panel is up, a press
// from the relay answers it instead of clicking. Here so it can be tested without SteamVR
// (pointer/test/calpanel-test.sh).
//
// Accept (take this dot): the left button's press ("btn trigger 1", whatever mouse button,
// controller button or key combination is mapped to the left action), Enter on a keyboard (the
// relay's "calkey 1", sent only while the panel is up), Meta+J ("gazekey left 1"), a gaze
// precision or gaze drag press ("precision|gazedrag <source> 1"; those are the left button for
// someone who mapped it to one, and were dropped until 2026-10-09, so the panel never took a dot
// from them), and an index pinch with hand tracking (CalPinch, below). Quit: the right button
// ("btn b 1") and Meta+K ("gazekey right 1"). Their releases, and the other buttons, do nothing
// while the panel is up.
//
// Whose panel: "calpanel 1" is the gaze service's (@ft_gazed); "calpanel 1 NAME" is another
// program's, answered on @NAME (the gaze bench). The last claim wins.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "../../hands/include/fh_gestures.h"
}

enum class CalPanelAnswer { None, Accept, Quit, Ignore };

inline CalPanelAnswer calPanelAnswer(const char *msg) {
    char source[16];
    int value;
    if (!std::strncmp(msg, "btn trigger 1", 13) || !std::strncmp(msg, "gazekey left 1", 14) ||
        !std::strcmp(msg, "calkey 1"))
        return CalPanelAnswer::Accept;
    if ((std::sscanf(msg, "precision %15s %d", source, &value) == 2 ||
         std::sscanf(msg, "gazedrag %15s %d", source, &value) == 2) && value == 1)
        return CalPanelAnswer::Accept;
    if (!std::strncmp(msg, "btn b 1", 7) || !std::strncmp(msg, "gazekey right 1", 15))
        return CalPanelAnswer::Quit;
    if (!std::strncmp(msg, "btn ", 4) || !std::strncmp(msg, "gazekey ", 8) ||
        !std::strncmp(msg, "precision ", 10) || !std::strncmp(msg, "gazedrag ", 9) ||
        !std::strncmp(msg, "calkey ", 7))
        return CalPanelAnswer::Ignore;
    return CalPanelAnswer::None;
}

// "calpanel 1 [NAME]" or "calpanel 0": false if msg isn't one. on: the panel is up; owner: the
// abstract socket that gets the answers (ft_gazed without a NAME). A NAME is 1 to 64 letters,
// digits, '_', '-' or '.'; another one makes the message not a claim.
inline bool calPanelClaim(const char *msg, bool &on, std::string &owner) {
    int v;
    int used = 0;
    if (std::sscanf(msg, "calpanel %d%n", &v, &used) != 1 || (v != 0 && v != 1)) return false;
    const char *rest = msg + used;
    while (*rest == ' ') ++rest;
    std::string name = *rest ? rest : "ft_gazed";
    if (name.size() > 64) return false;
    for (const char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    on = v == 1, owner = name;
    return true;
}

// Index pinches for the panel: a pinch that began since the last look (FH_PINCH begins counted
// per side) is a tap that takes the dot, whether it was let go yet or not, so a held pinch takes
// one. Start() at the panel's opening, so pinches from before don't count; a new gestures file
// (opens changed) or counters that went back (ft-hands started over) start counting again.
class CalPinch {
public:
    void Start() { based_ = false; }
    // The side (0 left, 1 right) of a pinch begun since the last call, or -1. One per call: a
    // pinch by each hand at once is one answer.
    int Began(const fh_gestures_t &g, int opens) {
        if (!based_ || opens != opens_ || g.pinch[0].begins < seen_[0] || g.pinch[1].begins < seen_[1]) {
            seen_[0] = g.pinch[0].begins, seen_[1] = g.pinch[1].begins;
            opens_ = opens, based_ = true;
            return -1;
        }
        int side = -1;
        for (int s = 0; s < 2; ++s)
            if (g.pinch[s].begins != seen_[s]) {
                seen_[s] = g.pinch[s].begins;
                if (side < 0) side = s;
            }
        return side;
    }

private:
    bool based_ = false;
    int opens_ = -1;
    uint32_t seen_[2] = {0, 0};
};
