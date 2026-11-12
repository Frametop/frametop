// What a PC's streaming host and ft-pcsend say to each other (docs/together.md, "A PC's
// panels"). The host runs ft-pcsend with `--feed` and hands it, on its standard input, each
// picture it encodes of a window that is one of the PC's panels: one message after another, in
// the framing a Frame's media programs use (local/mediawire.h),
//
//   u32 its length after these four bytes (little-endian), 'N', u8 the panel's number,
//   u8 flags (1: a keyframe), the encoded picture (an HEVC access unit)
//
// On its standard output ft-pcsend says, a line each, what the host is to encode:
//
//   panel N R BITS     the bits a second to encode panel N at
//   panel N F COUNT    the pictures a second its owner lets it have
//   panel N P COUNT    the pictures a second to encode, at most
//   panel N K 0        the next picture is to be a keyframe
//   panel N W 1|0      whether anyone watches it: if not, nothing need be encoded
//
// and what became of the call ("called ADDRESS", "refused: WHY", "the link to the headset
// ended"). The host's end of this is its own code: the two meet only in these bytes and lines.
//
// The bytes, and a descriptor read without waiting. Header only.
#pragma once

#include "../local/mediawire.h"
#include "../proto/panel.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace pcsend {

constexpr uint8_t kFeedUnit = 'N';
constexpr size_t kMostFeed = 3 + local::kMostMediaUnit;   // a message after its length
constexpr size_t kMostAPass = 4u << 20;                    // what is read before the link is looked at again

struct Unit {
    int number = 0;
    bool key = false;
    std::vector<uint8_t> picture;
};

// One message's body as a picture. False when it isn't one: another kind, a number no panel
// has, flags nobody set, or no picture.
inline bool ReadUnit(uint8_t kind, const std::vector<uint8_t> &body, Unit &unit) {
    if (kind != kFeedUnit || body.size() < 3 || body[0] >= proto::kPanelNumbers || (body[1] & ~1)) return false;
    unit.number = body[0], unit.key = body[1] & 1;
    unit.picture.assign(body.begin() + 2, body.end());
    return true;
}

// The host's pictures, as they come on a descriptor.
class Feed {
public:
    enum State { kOpen, kEnded, kBroken };

    explicit Feed(int fd) : fd_(fd), reader_(kMostFeed) { fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK); }

    int fd() const { return fd_; }
    State state() const { return state_; }

    // Reads what waits, at most kMostAPass of it, and adds the whole pictures in it to `units`.
    // Ended: the host closed it. Broken: what came isn't a picture, and nothing after it is read.
    void Read(std::vector<Unit> &units) {
        uint8_t buf[1 << 16], kind = 0;
        std::vector<uint8_t> body;
        for (size_t taken = 0; state_ == kOpen && taken < kMostAPass;) {
            const ssize_t n = read(fd_, buf, sizeof buf);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
            if (n <= 0) {
                state_ = reader_.held() ? kBroken : kEnded;   // (ended inside a picture: torn)
                return;
            }
            taken += size_t(n);
            reader_.Feed(buf, size_t(n));
            while (state_ == kOpen && reader_.Next(kind, body)) {
                Unit unit;
                if (ReadUnit(kind, body, unit)) units.push_back(std::move(unit));
                else state_ = kBroken;
            }
            if (reader_.broken()) state_ = kBroken;
        }
    }

private:
    int fd_;
    local::MediaReader reader_;
    State state_ = kOpen;
};

}  // namespace pcsend
