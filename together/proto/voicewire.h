// A person's voice as a datagram on a Frametop Together link (docs/together.md, "The
// protocol"): 20 ms of it, as one Opus packet.
//
//   u8  kind    kVoiceDatagram
//   u16 count   which 20 ms it is: one more for each the sender's microphone gave, also for
//               those it didn't send (silence isn't sent), so a gap in the counts is that long
//               a gap in the voice
//   ...         the Opus packet, 1 to kMostVoice bytes
//
// The packet isn't looked into here. Whoever plays it hands it to the Opus decoder, which
// takes any bytes, and does so in a program of its own (ft-roomvoice).
//
// Header only; the bytes only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace proto {

constexpr uint8_t kVoiceDatagram = 2;   // a datagram's first byte: what it is
constexpr size_t kMostVoice = 400;      // an Opus packet's bytes: 20 ms at the bitrates voice has is under 200

struct Voice {
    uint16_t count = 0;
    const uint8_t *opus = nullptr;   // into the datagram it was read from
    size_t size = 0;
};

inline std::vector<uint8_t> PackVoice(uint16_t count, const uint8_t *opus, size_t size) {
    std::vector<uint8_t> out;
    if (!size || size > kMostVoice) return out;
    out.reserve(size + 3);
    out.push_back(kVoiceDatagram), out.push_back(uint8_t(count)), out.push_back(uint8_t(count >> 8));
    out.insert(out.end(), opus, opus + size);
    return out;
}

// A voice datagram read. False when it isn't one.
inline bool ReadVoice(const uint8_t *data, size_t n, Voice &out) {
    if (n < 4 || n > kMostVoice + 3 || data[0] != kVoiceDatagram) return false;
    out.count = uint16_t(data[1] | data[2] << 8);
    out.opus = data + 3, out.size = n - 3;
    return true;
}

}  // namespace proto
