// Control of a shared panel, on a link's control stream (docs/together.md, "Control and
// moving"): asking its owner for it, being given it or not, and the input of someone who has it.
//
//   12 AskControl  u8 panel                         anyone in the room, to the panel's owner
//   13 Grant       u8 panel, u8 seconds             the panel's owner, to one person: the panel
//                                                   is yours to control for this long from now
//                                                   (said again while it stands), or with 0 it
//                                                   isn't: refused, or taken back
//   14 Act         u8 panel, u8 what, i32 a, i32 b  someone with a grant, to the panel's owner:
//                                                   their input on it
//
// What an Act is:
//
//   kActMove    the pointer is at a across and b down the panel's picture, each 0 to kActFar
//               (its far edge)
//   kActButton  button a (kLeft, kRight, kMiddle) went down (b 1) or up (b 0)
//   kActScroll  the wheel turned a to the right and b down, in 120ths of a notch
//   kActKey     key a (a Linux input key code) went down or up
//
// kActLetGo never crosses a link. It is what the owner's own ft-together tells the program that
// gives the input to the desktop: whatever anyone held down is let go.
//
// The bytes only. Everything read is bounded here; whether the sender may say it is the panel
// owner's to check (flow/control.h).
#pragma once

#include "messages.h"

namespace proto {

constexpr uint8_t kActLetGo = 0, kActMove = 1, kActButton = 2, kActScroll = 3, kActKey = 4;
constexpr int32_t kActFar = 65535;
constexpr int32_t kLeft = 1, kRight = 2, kMiddle = 3;
constexpr int32_t kMostScroll = 2400;   // twenty notches in one go
constexpr int32_t kMostKey = 255;       // the keys of a keyboard; what may be typed is the seat's rule (room/seat.h)
constexpr uint8_t kMostLease = 30;      // seconds a Grant may say

struct AskControl {
    uint8_t panel = 0;
};

struct Grant {
    uint8_t panel = 0;
    uint8_t seconds = 0;
};

struct Act {
    uint8_t panel = 0;
    uint8_t what = kActMove;
    int32_t a = 0, b = 0;
};

inline bool SameKey(const Act &x, const Act &y) { return x.what == y.what && x.a == y.a; }

// An Act's body, as it also goes to the program that gives it to the desktop (local/inputs.h).
inline std::vector<uint8_t> Body(const Act &m) {
    wire::Out out;
    out.U8(m.panel), out.U8(m.what), out.U32(uint32_t(m.a)), out.U32(uint32_t(m.b));
    return out.b;
}

inline std::vector<uint8_t> Pack(const AskControl &m) {
    wire::Out out;
    out.U8(m.panel);
    return Framed(kAskControl, out);
}

inline std::vector<uint8_t> Pack(const Grant &m) {
    wire::Out out;
    out.U8(m.panel), out.U8(m.seconds < kMostLease ? m.seconds : kMostLease);
    return Framed(kGrant, out);
}

inline std::vector<uint8_t> Pack(const Act &m) {
    wire::Out out;
    out.b = Body(m);
    return Framed(kAct, out);
}

inline bool Read(const std::vector<uint8_t> &body, AskControl &m) {
    wire::In in(body.data(), body.size());
    m.panel = in.U8();
    return in.done() && m.panel < kPeople * kPanelNumbers;
}

inline bool Read(const std::vector<uint8_t> &body, Grant &m) {
    wire::In in(body.data(), body.size());
    m.panel = in.U8(), m.seconds = in.U8();
    return in.done() && m.panel < kPeople * kPanelNumbers && m.seconds <= kMostLease;
}

// False when it isn't input someone could give: a place off the panel, a button or a key there
// isn't, something that is neither down nor up, a wheel turned further than one turns.
inline bool Read(const std::vector<uint8_t> &body, Act &m) {
    wire::In in(body.data(), body.size());
    m.panel = in.U8(), m.what = in.U8();
    m.a = int32_t(in.U32()), m.b = int32_t(in.U32());
    if (!in.done() || m.panel >= kPeople * kPanelNumbers) return false;
    switch (m.what) {
    case kActMove: return m.a >= 0 && m.a <= kActFar && m.b >= 0 && m.b <= kActFar;
    case kActButton: return m.a >= kLeft && m.a <= kMiddle && (m.b == 0 || m.b == 1);
    case kActScroll: return m.a >= -kMostScroll && m.a <= kMostScroll && m.b >= -kMostScroll && m.b <= kMostScroll && (m.a || m.b);
    case kActKey: return m.a >= 1 && m.a <= kMostKey && (m.b == 0 || m.b == 1);
    default: return false;
    }
}

}  // namespace proto
