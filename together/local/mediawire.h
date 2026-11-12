// What ft-together and this Frame's media programs say to each other (media.h): on a stream
// socket, "@ft_together_media", one message after another, each
//
//   u32 its length after these four bytes (little-endian), u8 what it is, then its body
//
// A program that encodes one of the wearer's panels (a source, ft-sharepanel) says first which:
//   'S' u8 the panel's number (0 to 2)
// and then hands over each picture it encodes:
//   'U' u8 flags (1: a keyframe), the encoded picture
// It is told, whenever one changes:
//   'R' u32 the bits a second to encode at
//   'P' u8  the pictures a second to encode, at most (fewer while nobody has the panel focused)
//   'K'     the next picture is to be a keyframe
//   'W' u8  whether anyone watches the panel (1) or not (0): if not, nothing need be encoded
//
// A program that shows someone else's panel (a sink, ft-peerpanel) says first which:
//   'V' u8 the panel's id in the room
// and then, whenever it changes, what the panel has of its viewer:
//   'A' u8 level (0 hidden, 1 in view, 2 focused), u8 1 when its decoder needs a keyframe to go on
// and what became of its decoder:
//   'D' u8 0 it has none any more, 1 it has one, 2 the hardware gave it none
// It is told whether to decode (it begins without a decoder, a still):
//   'D' u8 1 decode the panel, 0 give the decoder up and keep the last picture
// and handed the panel's pictures, in order, none missing since the last keyframe:
//   'U' u8 flags (1: a keyframe), the encoded picture
//
// The bytes only. Header only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace local {

constexpr uint8_t kMediaSource = 'S', kMediaSink = 'V', kMediaUnit = 'U', kMediaRate = 'R', kMediaPictures = 'P', kMediaKey = 'K', kMediaWatched = 'W', kMediaAttention = 'A', kMediaDecoder = 'D';
constexpr uint8_t kDecoderNone = 0, kDecoderHas = 1, kDecoderRefused = 2;   // what a sink says of its decoder
constexpr size_t kMostMediaUnit = 1u << 20;              // an encoded picture (proto::kMostFrame)
constexpr size_t kMostMedia = 2 + kMostMediaUnit;        // a message after its length: what it is, flags, a picture

// One message added to `out`.
inline void PutMedia(std::vector<uint8_t> &out, uint8_t kind, const uint8_t *body, size_t n) {
    const uint32_t length = uint32_t(1 + n);
    const uint8_t head[5] = {uint8_t(length), uint8_t(length >> 8), uint8_t(length >> 16), uint8_t(length >> 24), kind};
    out.insert(out.end(), head, head + 5);
    if (n) out.insert(out.end(), body, body + n);
}
inline void PutMedia(std::vector<uint8_t> &out, uint8_t kind) { PutMedia(out, kind, nullptr, 0); }
inline void PutMedia(std::vector<uint8_t> &out, uint8_t kind, uint8_t a) { PutMedia(out, kind, &a, 1); }
inline void PutMedia(std::vector<uint8_t> &out, uint8_t kind, uint8_t a, uint8_t b) {
    const uint8_t body[2] = {a, b};
    PutMedia(out, kind, body, 2);
}
inline void PutMediaRate(std::vector<uint8_t> &out, uint32_t bitrate) {
    const uint8_t body[4] = {uint8_t(bitrate), uint8_t(bitrate >> 8), uint8_t(bitrate >> 16), uint8_t(bitrate >> 24)};
    PutMedia(out, kMediaRate, body, 4);
}
// A picture: 'U', flags, the unit.
inline void PutMediaUnit(std::vector<uint8_t> &out, bool key, const uint8_t *unit, size_t n) {
    const uint32_t length = uint32_t(2 + n);
    const uint8_t head[6] = {uint8_t(length), uint8_t(length >> 8), uint8_t(length >> 16), uint8_t(length >> 24), kMediaUnit, uint8_t(key ? 1 : 0)};
    out.insert(out.end(), head, head + 6);
    out.insert(out.end(), unit, unit + n);
}
inline uint32_t MediaU32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The messages out of the bytes a socket brings, however they are cut up. A length of none, or
// longer than `most`, breaks it for good; whoever feeds it takes the whole messages out after
// each Feed, so it never holds much more than one.
class MediaReader {
public:
    explicit MediaReader(size_t most = kMostMedia) : most_(most) {}

    // False once what came can't be a message: the socket is to be closed.
    bool Feed(const uint8_t *data, size_t n) {
        if (broken_) return false;
        in_.insert(in_.end(), data, data + n);
        return Check();
    }
    // The next whole message, if one is here.
    bool Next(uint8_t &kind, std::vector<uint8_t> &body) {
        if (broken_ || in_.size() - at_ < 4) return false;
        const size_t length = MediaU32(in_.data() + at_);
        if (in_.size() - at_ - 4 < length) return false;
        kind = in_[at_ + 4];
        body.assign(in_.begin() + long(at_ + 5), in_.begin() + long(at_ + 4 + length));
        at_ += 4 + length;
        if (at_ == in_.size()) in_.clear(), at_ = 0;
        else if (at_ > (1u << 16)) in_.erase(in_.begin(), in_.begin() + long(at_)), at_ = 0;
        Check();
        return true;
    }
    bool broken() const { return broken_; }
    // Bytes held that aren't a whole message yet, and whole ones nobody took.
    size_t held() const { return in_.size() - at_; }

private:
    // Whether the message at the front may be one. With more than a message waiting, only the
    // front one is checked: the rest when it is their turn.
    bool Check() {
        if (in_.size() - at_ < 4) return true;
        const size_t length = MediaU32(in_.data() + at_);
        if (length == 0 || length > most_) broken_ = true, in_.clear(), at_ = 0;
        return !broken_;
    }

    size_t most_, at_ = 0;
    std::vector<uint8_t> in_;
    bool broken_ = false;
};

}  // namespace local
