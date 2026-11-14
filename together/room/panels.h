// The panels shared in a room (docs/together.md, "What a guest sees"): whose a panel is, how
// many each person may share, and what happens to them when their owner goes. The owner's copy
// of the room holds them (proto::Room), and a guest's are put there on the guest's word, checked
// here: a panel is its sharer's own, by its id, and nobody shares another's.
//
// The rules only.
#pragma once

#include "../proto/messages.h"

#include <algorithm>

namespace room {

// The panels a Frame encodes itself (spike T1: three sessions at any size next to SteamVR). The
// numbers past them are their person's split compute PC's, which encodes them and hands them
// to its person's Frame to send (flow/pcpanels.h).
constexpr int kFramePanels = 3, kPcPanels = 4, kOwnPanels = kFramePanels + kPcPanels;
static_assert(kOwnPanels <= proto::kPanelNumbers && size_t(proto::kPeople * kOwnPanels) <= proto::kMostPanels, "a room holds all that its people may share");

enum class Shared { kNo, kSame, kChanged };

// Person `who` shares `panel`, or says where it is now. kNo when they may not: it isn't theirs,
// its number is past what a person shares, or they aren't in the room. (The room holds all
// that its people may share: there is no fill to run into.)
inline Shared SetPanel(proto::Room &room, int who, const proto::Panel &panel) {
    if (!room.person(who) || panel.owner() != who || panel.number() >= kOwnPanels) return Shared::kNo;
    for (proto::Panel &p : room.panels) {
        if (p.id != panel.id) continue;
        if (proto::SamePanel(p, panel)) return Shared::kSame;
        p = panel;
        return Shared::kChanged;
    }
    room.panels.push_back(panel);
    std::sort(room.panels.begin(), room.panels.end(), [](const proto::Panel &a, const proto::Panel &b) { return a.id < b.id; });
    return Shared::kChanged;
}

// Person `who` stops sharing their panel `id`. True when the room changed.
inline bool DropPanel(proto::Room &room, int who, uint8_t id) {
    const auto it = std::find_if(room.panels.begin(), room.panels.end(), [=](const proto::Panel &p) { return p.id == id && p.owner() == who; });
    if (it == room.panels.end()) return false;
    room.panels.erase(it);
    return true;
}

// Someone left: what they shared goes with them. True when the room changed.
inline bool DropPanelsOf(proto::Room &room, int who) {
    const size_t before = room.panels.size();
    room.panels.erase(std::remove_if(room.panels.begin(), room.panels.end(), [=](const proto::Panel &p) { return p.owner() == who; }), room.panels.end());
    return room.panels.size() != before;
}

}  // namespace room
