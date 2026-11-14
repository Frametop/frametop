// A split compute PC's panels, on the link from the PC to its own person's Frame
// (docs/together.md, "A PC's panels"). The PC calls the Frame as another Frame would, and says
// first, on the control stream:
//
//   16 Sender  u16 version, text code           the one-time code the Frame made for it
//
// A Frame that takes it says, whenever one changes, what the PC is to encode each of its
// panels at (a refused PC gets a Refuse, messages.h, and the link ends):
//
//   17 Encode  u8 number, u8 word, u32 value    the panel's number among its person's (3 to 6),
//                                               and one of the words below
//
//   'R'  the bits a second to encode at
//   'F'  the pictures a second its encoder is to be set for: what the panel's owner lets it have
//   'P'  the pictures a second to encode, at most (fewer than 'F' says while nobody has the
//        panel focused)
//   'K'  the next picture is to be a keyframe (the value is 0)
//   'W'  whether anyone watches the panel (1) or not (0): if not, nothing need be encoded
//
// They are the words a Frame's own encoding program is told (local/mediawire.h). The PC sends
// each picture as a Frame does (framewire.h), with the panel's number where a Frame says the
// panel's id, and counts each panel's pictures from a number of its own.
//
// The bytes only. Nothing of the Frame's is in it.
#pragma once

#include "messages.h"

namespace proto {

constexpr uint8_t kEncodeRate = 'R', kEncodeFull = 'F', kEncodePictures = 'P', kEncodeKey = 'K', kEncodeWatched = 'W';
constexpr uint32_t kMostEncodeBits = 100'000'000;

struct Sender {
    uint16_t version = kVersion;
    std::string code;
};

struct Encode {
    uint8_t number = 0;
    uint8_t word = kEncodeKey;
    uint32_t value = 0;
};

inline std::vector<uint8_t> Pack(const Sender &m) {
    wire::Out out;
    out.U16(m.version);
    out.Text(m.code, kMostCode);
    return Framed(kSender, out);
}

inline std::vector<uint8_t> Pack(const Encode &m) {
    wire::Out out;
    out.U8(m.number), out.U8(m.word), out.U32(m.value);
    return Framed(kEncode, out);
}

// One of another version reads as far as its version and is true, as a Hello does: the rest of
// it is not ours to understand, and its sender can still be told to update.
inline bool Read(const std::vector<uint8_t> &body, Sender &m) {
    wire::In in(body.data(), body.size());
    m.version = in.U16();
    if (in.ok && m.version != kVersion) return m.code.clear(), true;
    in.Text(m.code, kMostCode);
    return in.done();
}

// False when it isn't something to encode at: a number no panel has, a word there isn't, or a
// value its word can't have.
inline bool Read(const std::vector<uint8_t> &body, Encode &m) {
    wire::In in(body.data(), body.size());
    m.number = in.U8(), m.word = in.U8(), m.value = in.U32();
    if (!in.done() || m.number >= kPanelNumbers) return false;
    switch (m.word) {
    case kEncodeRate: return m.value >= 1 && m.value <= kMostEncodeBits;
    case kEncodeFull:
    case kEncodePictures: return m.value >= 1 && m.value <= uint32_t(kMostPanelPictures);
    case kEncodeKey: return m.value == 0;
    case kEncodeWatched: return m.value <= 1;
    default: return false;
    }
}

}  // namespace proto
