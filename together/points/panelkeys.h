// Which of ft-screens' panels shows each of the room's shared panels on this Frame, as
// ft-roompanels says it (`panels` on its socket):
//
//   ok COUNT ID:SCREEN:WxH ...
//
// ID is the room's id of a shared panel, SCREEN the number of the screen that shows it here
// (one of this desktop's own that its wearer shares, or the remote screen a peer's panel is
// decoded onto), and WxH its picture's pixels. A screen's panel in SteamVR is the overlay
// ft-screens made for it, found by its key (screens/vr.cpp: frametop.screen.N, and
// frametop.remote.N from screens/remote.h's first on).
//
// An answer is taken whole or not at all. The reading only: no sockets. Header only.
#pragma once

#include "../proto/panel.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace points {

constexpr int kPanels = proto::kPeople * proto::kPanelNumbers;
constexpr int kRemoteFirst = 101, kRemoteLast = 116;   // screens/remote.h

struct PanelKey {
    bool known = false;
    std::string key;      // the overlay's
    int width = 0, height = 0;
};

class PanelKeys {
public:
    // ft-roompanels' answer. False, and nothing changed, when it isn't one.
    bool Read(const std::string &answer) {
        std::istringstream in(answer);
        std::string word;
        int count = -1;
        if (!(in >> word) || word != "ok" || !(in >> count) || count < 0 || count > int(proto::kMostPanels)) return false;
        PanelKey got[kPanels];
        int read = 0;
        while (in >> word) {
            int id = -1, screen = -1, w = 0, h = 0;
            char more = 0;
            if (std::sscanf(word.c_str(), "%d:%d:%dx%d%c", &id, &screen, &w, &h, &more) != 4) return false;
            const bool own = screen >= 1 && screen < kRemoteFirst, remote = screen >= kRemoteFirst && screen <= kRemoteLast;
            if (id < 0 || id >= kPanels || got[id].known || (!own && !remote) || w < 64 || h < 64 || w > 8192 || h > 8192) return false;
            got[id].known = true, got[id].width = w, got[id].height = h;
            got[id].key = (own ? "frametop.screen." : "frametop.remote.") + std::to_string(screen);
            ++read;
        }
        if (read != count) return false;
        for (int i = 0; i < kPanels; ++i) key_[i] = got[i];
        return true;
    }

    void Forget() {
        for (PanelKey &k : key_) k = PanelKey();
    }

    // The panel with the room's id `id`, known or not.
    const PanelKey &of(int id) const { return id >= 0 && id < kPanels ? key_[id] : none_; }

private:
    PanelKey key_[kPanels], none_;
};

}  // namespace points
