// KWin Window Stream's framing and key pin (stream/kwin-frame.h, issue 81): what the PC sends is
// read whole however it's split, a frame that's bad in any way ends the connection before any of
// it is kept, reading waits while frames aren't taken, and the pin is the one conf/ft_hostapi.py
// keeps for the same certificate. No network, no host.
#include "kwin-frame.h"

#include <openssl/pem.h>

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

static std::vector<uint8_t> Frame(char type, uint32_t declared, size_t actual) {
    std::vector<uint8_t> out{uint8_t(type)};
    const uint32_t size = htonl(declared);
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(&size), reinterpret_cast<const uint8_t *>(&size) + 4);
    for (size_t i = 0; i < actual; i++) out.push_back(uint8_t(i * 7));
    return out;
}

static bool Throws(KwinFramer &f, std::deque<KwinPacket> &packets) {
    try {
        f.Parse(packets);
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

// A self-signed certificate (no key kept) and its pin, from conf/ft_hostapi.py's spki_pin.
static const char kCert[] = R"(-----BEGIN CERTIFICATE-----
MIIBiTCCAS+gAwIBAgIUCcgJMkEzPC/0QlVt4Ip33kzjFGcwCgYIKoZIzj0EAwIw
GjEYMBYGA1UEAwwPa3dpbi1mcmFtZS10ZXN0MB4XDTI2MTEyMTAwMjgzN1oXDTM2
MTExODAwMjgzN1owGjEYMBYGA1UEAwwPa3dpbi1mcmFtZS10ZXN0MFkwEwYHKoZI
zj0CAQYIKoZIzj0DAQcDQgAElfBr4fZ7MLwdTu0SpnKfam/IfqH2JJ5rmb3TAkYi
zX4b/FSAkn1Jt7/JUy9fATCi3UOJoP7z90hkAwrELrI4FaNTMFEwHQYDVR0OBBYE
FAkhQG8FrE8aMmx2UeWwfn0YoSV5MB8GA1UdIwQYMBaAFAkhQG8FrE8aMmx2UeWw
fn0YoSV5MA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIhANQO/LU0
SGuywFKlWvwlZKj+QxLM8pPl1df3m2PAoUMWAiApYl9xokQ1GR0hjSsJ4+GBbDrD
4LeqQD21CFJZzFeWlQ==
-----END CERTIFICATE-----
)";
static const char kCertPin[] = "sha256//s6QBHMHEQaXs0u/lhGhm9vFKcsQn8FA+kqkG7YCVIWg=";

int main() {
    // Whole frames, however the bytes are split (every split point of two frames).
    std::vector<uint8_t> both = Frame('J', 3, 3), video = Frame('V', 600, 600);
    both.insert(both.end(), video.begin(), video.end());
    for (size_t cut = 0; cut <= both.size(); cut++) {
        KwinFramer f;
        std::deque<KwinPacket> packets;
        f.Append(both.data(), cut);
        while (f.Parse(packets)) {}
        f.Append(both.data() + cut, both.size() - cut);
        while (f.Parse(packets)) {}
        CHECK(packets.size() == 2);
        if (packets.size() == 2) {
            CHECK(packets[0].type == 'J' && packets[0].data.size() == 3);
            CHECK(packets[1].type == 'V' && packets[1].data.size() == 600 && packets[1].data[599] == uint8_t(599 * 7));
        }
        CHECK(f.incoming.empty());
    }

    // A frame announced but not all there yet: nothing until it is.
    {
        KwinFramer f;
        std::deque<KwinPacket> packets;
        const auto v = Frame('V', 100, 99);
        f.Append(v.data(), v.size());
        CHECK(!f.Parse(packets) && packets.empty());
    }

    // Bad frames end it, before their payload is waited for or kept.
    struct Bad { char media, type; uint32_t size; } bad[] = {
        {'V', 'A', 100},                         // sound on the picture's connection
        {'A', 'V', 100},                         // a picture on the sound's
        {'V', 'C', 4},                           // a command: only we send those
        {'V', 'V', 0},                           // empty
        {'V', 'V', uint32_t(kKwinMaxFrame + 1)}, // a picture bigger than any
        {'V', 'V', 0xffffffffu},                 // a length that would wrap size_t + 5 on 32 bits
        {'V', 'J', uint32_t(kKwinMaxJson + 1)},  // a list bigger than any
        {'A', 'A', 8},                           // sound with only its time
        {'A', 'A', uint32_t(kKwinMaxAudio + 1)}, // sound bigger than a packet
    };
    for (const Bad &b : bad) {
        KwinFramer f;
        f.mediaType = b.media;
        std::deque<KwinPacket> packets;
        const auto header = Frame(b.type, b.size, 0);
        f.Append(header.data(), header.size());
        CHECK(Throws(f, packets));
        CHECK(packets.empty());
    }
    {
        KwinFramer f;
        f.mediaType = 'A';
        std::deque<KwinPacket> packets;
        const auto a = Frame('A', 9, 9);
        f.Append(a.data(), a.size());
        CHECK(f.Parse(packets) && packets.size() == 1);
    }

    // Reading waits while kKwinQueued frames aren't taken, and what comes meanwhile is bounded.
    {
        KwinFramer f;
        std::deque<KwinPacket> packets;
        for (size_t i = 0; i <= kKwinQueued; i++) {
            const auto j = Frame('J', 2, 2);
            f.Append(j.data(), j.size());
        }
        while (f.Parse(packets)) {}
        CHECK(packets.size() == kKwinQueued && !f.incoming.empty());
        packets.pop_front();
        CHECK(f.Parse(packets) && f.incoming.empty());

        KwinFramer full;
        std::vector<uint8_t> chunk(kKwinReadChunk, 0);
        bool refused = false;
        try {
            for (int i = 0; i < 200; i++) full.Append(chunk.data(), chunk.size());
        } catch (const std::runtime_error &) {
            refused = true;
        }
        CHECK(refused && full.incoming.size() <= kKwinMaxFrame + kKwinReadChunk + 5);
        bool tooBig = false;
        try {
            KwinFramer f2;
            std::vector<uint8_t> big(kKwinReadChunk + 1, 0);
            f2.Append(big.data(), big.size());
        } catch (const std::runtime_error &) {
            tooBig = true;
        }
        CHECK(tooBig);
    }

    // What we send: framed, and nothing too long.
    {
        const std::string c = KwinFrame('C', "size 1280 800");
        CHECK(c.size() == 5 + 13 && c[0] == 'C' && c[4] == 13 && c.substr(5) == "size 1280 800");
        bool refused = false;
        try {
            KwinFrame('C', std::string(kKwinMaxSend + 1, 'x'));
        } catch (const std::runtime_error &) {
            refused = true;
        }
        CHECK(refused);
    }

    // The pin: the same as Remote Displays keeps for that certificate, and none for no certificate.
    {
        BIO *bio = BIO_new_mem_buf(kCert, -1);
        X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        CHECK(cert != nullptr);
        CHECK(KwinKeyPin(cert) == kCertPin);
        X509_free(cert);
        CHECK(KwinKeyPin(nullptr).empty());
    }

    if (failures) {
        std::fprintf(stderr, "kwin-frame-test: %d failed\n", failures);
        return 1;
    }
    std::puts("kwin-frame-test: ok");
    return 0;
}
