// Where someone points, as a datagram on a Frametop Together link (docs/together.md, "Control
// and moving"): everyone in the room sees everyone's pointer, in its owner's colour.
//
//   u8  kind    kPointDatagram
//   u16 count   a newer one from an older (posewire.h, Newer)
//   u8  panel   the shared panel they point at, or kNoPanel: at none
//   u16 x, y    where on it: across and down its picture, each 0 to 65535 (its far edge)
//   u8  flags   kPointPressed: they hold a button down there
//
// About thirty a second while the pointer moves, and a late one is worth nothing. It says
// nothing of whether they may click there: that is a grant's (control.h).
//
// Header only; the bytes only.
#pragma once

#include "messages.h"

namespace proto {

constexpr uint8_t kPointDatagram = 3;   // a datagram's first byte: what it is
constexpr uint8_t kNoPanel = 255;
constexpr uint8_t kPointPressed = 1;
constexpr size_t kPointBytes = 9;

struct Point {
    uint16_t count = 0;
    uint8_t panel = kNoPanel;
    uint16_t x = 0, y = 0;
    uint8_t flags = 0;
};

inline std::vector<uint8_t> Pack(const Point &m) {
    wire::Out out;
    out.U8(kPointDatagram), out.U16(m.count), out.U8(m.panel), out.U16(m.x), out.U16(m.y), out.U8(m.flags);
    return out.b;
}

// A pointer datagram read. False when it isn't one.
inline bool Read(const uint8_t *data, size_t n, Point &m) {
    wire::In in(data, n);
    if (in.U8() != kPointDatagram) return false;
    m.count = in.U16(), m.panel = in.U8(), m.x = in.U16(), m.y = in.U16(), m.flags = in.U8();
    return in.done() && (m.panel < kPeople * kPanelNumbers || m.panel == kNoPanel) && !(m.flags & ~kPointPressed);
}

}  // namespace proto
