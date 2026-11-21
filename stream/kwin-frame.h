// KWin Window Stream's framing and the PC's key pin, apart from the connection (kwin-window.h)
// so they can be tested on their own (stream/test/kwin-frame-test.cpp). From utrack's receiver
// (github.com/utrack/frametop, branch custom-window-streamer, stream/window-stream.h; MIT).
//
// A frame is a type byte and a big-endian uint32 length, then the payload. Everything here comes
// from the PC, so a frame with a type the connection doesn't carry, no payload, or more than its
// type may have, ends the connection before any of it is kept.
#pragma once

#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

constexpr size_t kKwinMaxFrame = 4u << 20;  // one HEVC access unit
constexpr size_t kKwinMaxJson = 1u << 20;
constexpr size_t kKwinMaxAudio = 4096;       // a uint64 time and one Opus packet
constexpr size_t kKwinMaxSend = 4096;
constexpr size_t kKwinReadChunk = 32768;
constexpr size_t kKwinQueued = 4;            // frames parsed and not taken yet: then reading waits

// "sha256//BASE64" of a certificate's public key: curl's pin, as kept in hosts/ADDRESS.pin
// (conf/ft_hostapi.py's spki_pin). "" when it can't be worked out.
inline std::string KwinKeyPin(X509 *certificate) {
    unsigned char *encoded = nullptr;
    const int size = certificate ? i2d_X509_PUBKEY(X509_get_X509_PUBKEY(certificate), &encoded) : -1;
    unsigned char digest[EVP_MAX_MD_SIZE], base64[EVP_MAX_MD_SIZE * 2];
    unsigned digestSize = 0;
    const bool hashed = size > 0 && EVP_Digest(encoded, size_t(size), digest, &digestSize, EVP_sha256(), nullptr);
    OPENSSL_free(encoded);
    if (!hashed) return "";
    const int count = EVP_EncodeBlock(base64, digest, int(digestSize));
    return "sha256//" + std::string(reinterpret_cast<char *>(base64), size_t(count));
}

struct KwinPacket {
    char type;
    std::vector<uint8_t> data;
};

// One frame to send: a C command or J message of at most kKwinMaxSend bytes.
inline std::string KwinFrame(char type, const std::string &payload) {
    if (payload.size() > kKwinMaxSend) throw std::runtime_error("a message too long to send");
    const uint32_t size = htonl(uint32_t(payload.size()));
    std::string message(1, type);
    message.append(reinterpret_cast<const char *>(&size), sizeof size);
    return message + payload;
}

// The frames in what came in. mediaType is what else besides J this connection carries: V (the
// picture's) or A (the sound's).
struct KwinFramer {
    char mediaType = 'V';
    std::vector<uint8_t> incoming;

    // Bytes read: at most kKwinReadChunk at a time, on top of at most one frame not taken yet.
    void Append(const uint8_t *bytes, size_t n) {
        if (n > kKwinReadChunk || incoming.size() + n > kKwinMaxFrame + kKwinReadChunk + 5)
            throw std::runtime_error("the host sent more than a frame's worth");
        incoming.insert(incoming.end(), bytes, bytes + n);
    }

    // The next whole frame onto packets; false when there isn't one yet, or kKwinQueued wait there.
    bool Parse(std::deque<KwinPacket> &packets) {
        if (incoming.size() < 5 || packets.size() >= kKwinQueued) return false;
        uint32_t size;
        std::memcpy(&size, incoming.data() + 1, sizeof size);
        size = ntohl(size);
        const char type = char(incoming[0]);
        if ((type != 'J' && type != mediaType) || size == 0 || size > kKwinMaxFrame ||
            (type == 'J' && size > kKwinMaxJson) || (type == 'A' && (size <= 8 || size > kKwinMaxAudio)))
            throw std::runtime_error("the host sent a malformed frame");
        if (incoming.size() < size_t(size) + 5) return false;
        packets.push_back({type, {incoming.begin() + 5, incoming.begin() + 5 + size}});
        incoming.erase(incoming.begin(), incoming.begin() + 5 + size);
        return true;
    }
};
