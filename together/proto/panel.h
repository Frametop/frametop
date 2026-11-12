// A panel someone shares with the room (docs/together.md, "What a guest sees"): a screen or a
// floating window of theirs, where it stands in the room, how large it is there, and the size
// of the video its owner sends of it and its pictures a second at the most. It is part of the room's state (messages.h), so everyone
// puts it in the same place.
//
// A panel's id says whose it is: the owner's place in the room times kPanelNumbers, plus its
// number among theirs. So nobody can say a panel is someone else's, and who may send its video
// is read off the id.
//
// The bytes only. Everything read is bounded here, before the session sees it.
#pragma once

#include "bytes.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace proto {

constexpr int kPanelNumbers = 8;         // the numbers a person's panels may have
constexpr size_t kMostPanels = 28;       // in a room at once: all that its people may share (room/panels.h)
constexpr size_t kMostPanelName = 48;
constexpr int kLeastPanelSide = 128, kMostPanelSide = 4096;   // a stream's pixels a side, even
constexpr int kPanelPictures = 30, kMostPanelPictures = 60;   // a stream's pictures a second: unless its owner says, and at the most
constexpr float kLeastPanelMetres = 0.15f, kMostPanelMetres = 20;
constexpr float kLeastCurve = 0.5f, kMostCurve = 100;         // a curved panel's radius, metres; 0 is flat
constexpr float kPanelFar = 100;         // metres from the room's middle, as far as anything is (messages.h, kFar)

struct Panel {
    uint8_t id = 0;
    uint16_t width = 0, height = 0;   // the video's pixels
    uint8_t pictures = kPanelPictures;   // ...and the most it has in a second: what a decoder for it is to be good for
    float at[3] = {};                 // its middle, in the room
    float turn[4] = {1, 0, 0, 0};     // a unit quaternion (w, x, y, z): the panel faces its own +z
    float metres = 1;                 // how wide it is in the room; its height follows from the pixels
    float curve = 0;
    std::string name;                 // what its owner calls it ("Screen 2")

    int owner() const { return id / kPanelNumbers; }
    int number() const { return id % kPanelNumbers; }
};

inline uint8_t PanelId(int owner, int number) { return uint8_t(owner * kPanelNumbers + number); }

inline bool SamePanel(const Panel &a, const Panel &b) {
    bool same = a.id == b.id && a.width == b.width && a.height == b.height && a.pictures == b.pictures && a.metres == b.metres && a.curve == b.curve && a.name == b.name;
    for (int i = 0; i < 3; ++i) same = same && a.at[i] == b.at[i];
    for (int i = 0; i < 4; ++i) same = same && a.turn[i] == b.turn[i];
    return same;
}

// Whether it is a panel a room of `people` places could have: its id within them, a size a
// stream has, pictures a second a stream has, a place in the room, a turn that is one, a name
// that is text.
inline bool Fits(const Panel &p, int people) {
    if (p.id >= people * kPanelNumbers) return false;
    if (p.width < kLeastPanelSide || p.width > kMostPanelSide || p.height < kLeastPanelSide || p.height > kMostPanelSide || (p.width | p.height) & 1) return false;
    if (p.pictures < 1 || p.pictures > kMostPanelPictures) return false;
    double norm = 0;
    for (float v : p.turn) norm += double(v) * v;
    if (!(norm > 0.9 && norm < 1.1)) return false;
    for (float v : p.at)
        if (!(std::fabs(v) <= kPanelFar)) return false;
    if (!(p.metres >= kLeastPanelMetres && p.metres <= kMostPanelMetres)) return false;
    if (!(p.curve == 0 || (p.curve >= kLeastCurve && p.curve <= kMostCurve))) return false;
    return p.name.size() <= kMostPanelName && wire::IsText(p.name);
}

namespace panelwire {

inline void Put(wire::Out &out, const Panel &p) {
    out.U8(p.id);
    out.U16(p.width), out.U16(p.height), out.U8(p.pictures);
    for (float v : p.at) out.F32(v);
    for (float v : p.turn) out.Unit16(v);
    out.F32(p.metres), out.F32(p.curve);
    out.Text(p.name, kMostPanelName);
}

// False when it isn't a panel (Fits). A turn comes as 16 bits a part, and is made one again.
inline bool Take(wire::In &in, Panel &p, int people) {
    p.id = in.U8();
    p.width = in.U16(), p.height = in.U16(), p.pictures = in.U8();
    for (float &v : p.at) v = in.F32(-kPanelFar, kPanelFar);
    for (float &v : p.turn) v = in.Unit16();
    p.metres = in.F32(kLeastPanelMetres, kMostPanelMetres);
    p.curve = in.F32(0, kMostCurve);
    in.Text(p.name, kMostPanelName);
    if (!in.ok || !Fits(p, people)) return false;
    double norm = 0;
    for (float v : p.turn) norm += double(v) * v;
    for (float &v : p.turn) v = float(v / std::sqrt(norm));
    return true;
}

}  // namespace panelwire

}  // namespace proto
