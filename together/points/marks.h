// Where the others in the room point (docs/together.md, "Control and moving": everyone sees
// everyone's pointer, in its owner's colour): the last word each of them sent, and how long it
// stands.
//
// A pointer rests on a panel with a word a second (proto/pointwire.h), so one that hasn't been
// heard of for kSilentS is gone: its person left, their link dropped, or their program ended
// with the pointer on a panel. Whether a datagram is newer than the last is ft-together's to
// see before it passes one on (local/pointgate.h); whether the panel it names is shown here is
// the drawer's.
//
// Where they look, while they share that (docs/together.md, "Gaze"), is kept the same way:
// ft-together tells it as a pointer's word with another first byte (local/glances.h), ten times
// a second, so one not heard of for kGlanceSilentS is gone.
//
// The rules only. Header only.
#pragma once

#include "../local/datagrams.h"
#include "../proto/pointwire.h"

namespace points {

struct Mark {
    bool shown = false;
    uint8_t panel = proto::kNoPanel;
    double x = 0, y = 0;    // across and down the panel's picture, 0 to 1
    bool pressed = false;   // they hold a button down there
};

class Marks {
public:
    static constexpr double kSilentS = 2.5;
    static constexpr double kGlanceSilentS = 1.0;

    // `kind`: the first byte of the words it keeps (kPointToWatcher: where they point;
    // kGlanceToWatcher: where they look). `silent`: how long one stands.
    explicit Marks(uint8_t kind = local::kPointToWatcher, double silent = kSilentS) : kind_(kind), silent_(silent) {}

    // A datagram ft-together sent to one who asked `points`: the kind, whose it is, then a
    // pointer datagram. False when it isn't one of this kind.
    bool Take(const uint8_t *data, size_t n, double now) {
        proto::Point point;
        if (n < 2 || data[0] != kind_ || data[1] >= proto::kPeople || !proto::Read(data + 2, n - 2, point)) return false;
        Said &s = said_[data[1]];
        s.point = point, s.at = now, s.any = true;
        return true;
    }

    // Where the person at `place` points now, if anywhere.
    Mark At(int place, double now) const {
        Mark m;
        if (place < 0 || place >= proto::kPeople) return m;
        const Said &s = said_[place];
        if (!s.any || now - s.at > silent_ || s.point.panel == proto::kNoPanel) return m;
        m.shown = true, m.panel = s.point.panel;
        m.x = s.point.x / 65535.0, m.y = s.point.y / 65535.0;
        m.pressed = (s.point.flags & proto::kPointPressed) != 0;
        return m;
    }

    // Out of the room: nobody points anywhere.
    void Forget() {
        for (Said &s : said_) s = Said();
    }

private:
    struct Said {
        proto::Point point;
        double at = 0;
        bool any = false;
    };
    const uint8_t kind_;
    const double silent_;
    Said said_[proto::kPeople];
};

}  // namespace points
