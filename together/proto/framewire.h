// One picture of a shared panel's video, as it goes on a link (docs/together.md, "The
// protocol"): a one-way stream of its own, which carries
//
//   u8 what it is (kFrameStream), u8 the panel's id, u32 the picture's number, u8 flags
//   (1: a keyframe), then the encoded picture (an HEVC access unit) to the stream's end
//
// and nothing else. A lost packet is sent again by QUIC, so it costs that picture a round trip
// and not the video; a sender that gives a picture up resets its stream.
//
// The bytes only: whose panel it is and whether the picture is wanted is the session's to say.
#pragma once

#include "bytes.h"
#include "panel.h"

#include <cstdint>
#include <vector>

namespace proto {

constexpr uint8_t kFrameStream = 2;        // a one-way stream's first byte: what it carries
constexpr size_t kFrameHead = 7;
constexpr size_t kMostFrame = 1u << 20;    // an encoded picture's bytes: a longer one isn't sent, or taken

struct FrameHead {
    uint8_t panel = 0;
    uint32_t number = 0;   // counts a panel's pictures: the decoder needs them in order
    bool key = false;      // decodable with none before it
};

// Whether a picture numbered `number` came after one numbered `than`: the count wraps.
inline bool LaterFrame(uint32_t number, uint32_t than) { return int32_t(number - than) > 0; }

// The stream's bytes for one picture. Empty when the picture is none or too long to send.
inline std::vector<uint8_t> PackFrame(const FrameHead &h, const uint8_t *unit, size_t n) {
    if (!unit || n == 0 || n > kMostFrame) return {};
    wire::Out out;
    out.b.reserve(kFrameHead + n);
    out.U8(kFrameStream), out.U8(h.panel), out.U32(h.number), out.U8(h.key ? 1 : 0);
    out.Bytes(unit, n);
    return out.b;
}

// A whole stream's bytes read. `unit` points into `data`. False when it isn't a picture: another
// kind, a panel id no room has, flags nobody set, or no picture after the head.
inline bool ReadFrame(const uint8_t *data, size_t n, FrameHead &h, const uint8_t *&unit, size_t &size) {
    if (!data || n <= kFrameHead || n > kFrameHead + kMostFrame) return false;
    wire::In in(data, kFrameHead);
    const uint8_t kind = in.U8();
    h.panel = in.U8(), h.number = in.U32();
    const uint8_t flags = in.U8();
    if (!in.ok || kind != kFrameStream || h.panel >= 4 * kPanelNumbers || (flags & ~1)) return false;
    h.key = flags & 1;
    unit = data + kFrameHead, size = n - kFrameHead;
    return true;
}

}  // namespace proto
