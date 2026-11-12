// A shared panel's new place, on a link's control stream (docs/together.md, "Control and
// moving"):
//
//   15 Move  u8 panel, f32 at[3], i16 turn[4]   someone who may move the panel, to its owner:
//                                               where in the room they put it
//
// A panel stands where its owner's Frame has it: its screen there is the panel. So that Frame
// moves it, and says where it is now as it does when its own person moves it (a Panel,
// panel.h). The room has one place for a panel, and nobody a copy of their own.
//
// The bytes only. Everything read is bounded here; whether the sender may move the panel is
// its owner's to check (flow/moves.h).
#pragma once

#include "messages.h"

namespace proto {

struct Move {
    uint8_t panel = 0;
    float at[3] = {};                 // its middle, in the room
    float turn[4] = {1, 0, 0, 0};     // a unit quaternion (w, x, y, z), as a Panel's
};

// A Move's body.
inline std::vector<uint8_t> Body(const Move &m) {
    wire::Out out;
    out.U8(m.panel);
    for (float v : m.at) out.F32(v);
    for (float v : m.turn) out.Unit16(v);
    return out.b;
}

inline std::vector<uint8_t> Pack(const Move &m) {
    wire::Out out;
    out.b = Body(m);
    return Framed(kMove, out);
}

// False when it isn't a place a panel could have: further out than the room goes, or a turn
// that is none. A turn comes as 16 bits a part, and is made one again.
inline bool Read(const std::vector<uint8_t> &body, Move &m) {
    wire::In in(body.data(), body.size());
    m.panel = in.U8();
    for (float &v : m.at) v = in.F32(-kPanelFar, kPanelFar);
    for (float &v : m.turn) v = in.Unit16();
    if (!in.done() || m.panel >= kPeople * kPanelNumbers) return false;
    double norm = 0;
    for (float v : m.turn) norm += double(v) * v;
    if (!(norm > 0.9 && norm < 1.1)) return false;
    for (float &v : m.turn) v = float(v / std::sqrt(norm));
    return true;
}

}  // namespace proto
