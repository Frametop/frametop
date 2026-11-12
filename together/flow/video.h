// The shared panels' video through ft-together (docs/together.md, "The protocol"): from the
// program that encodes one of this Frame's panels to the links of those who watch it, and from
// a link to the program that shows someone else's panel here.
//
// Out: a picture goes to the viewers the panel's sendgate names, on a stream each; one that
// had to be given up puts its viewer out of step and counts against the panel's bitrate
// (sendgate.h, pace.h). In: a picture is taken only on the link of the panel's owner, for a
// panel that is in the room and that a program here shows, within what a panel may send in a
// second; and it is handed on in order (framegate.h). What a viewer here sees of each panel is
// told to the panel's owner, again every second, so a link that came late or came back learns it.
// A panel is encoded for the one who sees most of it, and a viewer who stops seeing it goes on
// getting it for a few seconds (attention.h). Of the panels shown here, only as many decode as
// the Frame's decoder has room for, the ones the wearer sees most of first; the others are
// stills, and their owners hear that nobody sees them here (budget.h).
//
// The rules only. The links and this Frame's programs are handed in (net/, local/media.h), so
// two of these can be run against each other in a test.
#pragma once

#include "../room/allowance.h"
#include "../room/panels.h"
#include "../session/session.h"
#include "attention.h"
#include "budget.h"
#include "framegate.h"
#include "pace.h"
#include "sendgate.h"

#include <map>
#include <vector>

namespace flow {

// The links, as the video uses them.
class Wires {
public:
    virtual ~Wires() = default;
    // One picture's stream to a link's far end. `tag` comes back with Lost if it had to be
    // given up. False when it can't be sent at all.
    virtual bool Frame(int link, std::vector<uint8_t> bytes, uint32_t tag) = 0;
};

// This Frame's programs that make the pictures and show them.
class Programs {
public:
    virtual ~Programs() = default;
    // To the one that encodes this Frame's panel `number`:
    virtual void Rate(int number, int bitrate) = 0;       // bits a second from now on
    virtual void Pictures(int number, int aSecond) = 0;   // pictures a second from now on, at most
    virtual void Key(int number) = 0;                     // the next picture is to be a keyframe
    virtual void Watched(int number, bool watched) = 0;   // whether anyone watches: if not, nothing need be encoded
    // To the one that shows panel `panel`: the next picture to decode,
    virtual void Show(uint8_t panel, bool key, const std::vector<uint8_t> &unit) = 0;
    // ...and whether it is to decode at all, or give its decoder up and keep its last picture.
    virtual void Decode(uint8_t panel, bool decode) = 0;
};

class Video {
public:
    static constexpr int kBalanced = 3'000'000;   // the cap until the owner picks another (bits a second)
    static constexpr int kBalancedPictures = 30;  // ...and its pictures a second, for a panel someone has focused
    static constexpr double kTellEvery = 1;       // seconds between a viewer's words to a panel's owner
    static constexpr double kPicturesASecond = 120, kPicturesAtOnce = 30;   // taken of one panel

    Video(session::Session &session, Wires &wires, Programs &programs) : session_(session), wires_(wires), programs_(programs) {}

    // From this Frame's programs. A source encodes the wearer's panel `number`; a sink shows
    // the panel `panel` of someone else.
    void SourceCame(int number);
    void Unit(int number, bool key, const uint8_t *unit, size_t n, double now);
    void SinkCame(uint8_t panel, double now);
    void SinkWent(uint8_t panel);
    // What the sink's panel has of its viewer: proto::kHidden, kInView or kFocused, and whether
    // its decoder needs a keyframe to go on. Hidden holds a few seconds later (attention.h).
    void Attention(uint8_t panel, int level, bool fresh, double now);
    // The sink says it has a decoder now or none any more, or that the hardware gave it none.
    void SinkDecodes(uint8_t panel, bool has, double now);
    void SinkRefused(uint8_t panel, double now);

    // From the links. Watch is the session's word that a member watches a panel of ours.
    void Watch(int link, const proto::Watch &watch, double now);
    void Frame(int link, const uint8_t *data, size_t n, double now);
    void Lost(int link, uint32_t tag, double now);
    // Often (every loop pass will do).
    void Tick(double now);

    size_t dropped() const { return dropped_; }   // pictures from a link that weren't taken
    // Of the wearer's panel `number`: what its source is asked to encode at, and how many watch.
    int bitrate(int number) const { return shared_[number].pace.bitrate(); }
    int pictures(int number) const { return shared_[number].pictures; }
    size_t viewers(int number) const { return shared_[number].viewers.count(); }
    // The panels shown here that are stills: their programs aren't to decode.
    std::vector<uint8_t> stills() const;

private:
    struct Shared {
        Viewers viewers;
        Pace pace{kBalanced};
        uint32_t count = 0;
        bool watched = false;   // as the source was last told
        int pictures = PicturesFor(proto::kInView, kBalancedPictures);   // ...and its pictures a second
    };
    struct Watching {
        Order order;
        room::Allowance allowance{kPicturesASecond, kPicturesAtOnce};
        Seen seen;
        int said = -1;          // the level the owner was last told
        bool fresh = true;      // the owner hasn't been told yet that a keyframe is needed
        double told = -1e9;
    };

    bool Mine(int number) const;
    void Settle(int number, double now);
    void Decoders(double now);
    int Told(uint8_t panel, const Watching &w, double now) const;
    void Tell(uint8_t panel, Watching &w, double now);

    session::Session &session_;
    Wires &wires_;
    Programs &programs_;
    Shared shared_[room::kFramePanels];
    std::map<uint8_t, Watching> watching_;
    Budget budget_;
    size_t dropped_ = 0;
};

}  // namespace flow
