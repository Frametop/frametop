// What the room is told of this Frame's own wearer. This Frame's programs hand ft-together
// datagrams that aren't questions (datagrams.h): where the wearer is, their voice, where they
// point, how their mouth is. Here each becomes what goes to everyone in the room, or nothing:
//
//  - A pose goes with ft-together's own count, the mouth as it was last told (mouth.h), and
//    where the wearer looks only while they share that (gaze.h).
//  - A voice goes as it came, when it is one.
//  - A pointer goes with ft-together's count, unless it would say where the wearer looks
//    without their having said it may (hush.h). Their acts on a panel pass the same rule.
//  - A mouth is kept for the poses.
//
// The rules only: no sockets here. Header only.
#pragma once

#include "../proto/voicewire.h"
#include "datagrams.h"
#include "gaze.h"
#include "hush.h"
#include "mouth.h"
#include "pointgate.h"
#include "posegate.h"

#include <cstdint>
#include <vector>

namespace local {

class Own {
public:
    // A datagram from one of this Frame's programs at `now` (seconds). False when it is none of
    // the wearer's: a question. `out`: what goes to everyone in the room for it, empty when
    // nothing does.
    bool Take(const uint8_t *data, size_t n, double now, std::vector<uint8_t> &out) {
        out.clear();
        if (n == 0) return false;
        if (data[0] == proto::kPoseDatagram) {
            poses_.Stamp(data, n, mouth_, gaze_, now, out);
        } else if (data[0] == proto::kVoiceDatagram) {
            proto::Voice voice;
            if (proto::ReadVoice(data, n, voice)) out.assign(data, data + n);
        } else if (data[0] == proto::kPointDatagram) {
            proto::Point point;
            if (proto::Read(data, n, point) && hush_.Point(point, gaze_.quiet(now), now)) out = points_.Stamp(point);
        } else if (data[0] == kMouthToTogether) {
            mouth_.Take(data, n, now);
        } else {
            return false;
        }
        return true;
    }

    // The wearer's act on a panel someone gave them, at `now`: what goes to its owner for it,
    // in order, is added to `out`.
    void Act(const proto::Act &act, double now, std::vector<proto::Act> &out) { hush_.Act(act, gaze_.quiet(now), now, out); }

    Gaze &gaze() { return gaze_; }
    const Gaze &gaze() const { return gaze_; }

private:
    PoseCount poses_;
    PointCount points_;
    Mouth mouth_;
    Gaze gaze_;
    Hush hush_;
};

}  // namespace local
