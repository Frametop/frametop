// The panels of the wearer's split compute PC (docs/together.md, "A PC's panels"): the numbers
// past the Frame's own (room/panels.h). The PC encodes them and hands the pictures to this
// Frame, which sends them on as it sends its own (video.h): the others in the room get them on
// the links they have to this Frame, and nothing of theirs ever reaches the PC.
//
// The PC calls this Frame, and is taken on the word of a one-time code that this Frame made
// for it (room/invites.h; whoever asked for the code hands it to the PC) from an address the
// tailnet knows. One PC at a time: the next one with a good code takes the last one's place. It
// is told what a Frame's own encoding program is told, panel by panel (proto/sender.h), and its
// pictures are put back in order before they go on (framegate.h): a gap is answered with the
// word for a keyframe.
//
// To the video's rules it is the programs of this Frame (flow::Programs): what is said to the
// one that encodes a PC's panel goes to the PC, and everything else to the Frame's own.
//
// The rules only. The links, the tailnet, the clock and the dice are handed in.
#pragma once

#include "../proto/sender.h"
#include "../room/invites.h"
#include "framegate.h"
#include "video.h"

namespace flow {

class PcPanels : public Programs {
public:
    static constexpr double kPicturesASecond = Video::kPicturesASecond, kPicturesAtOnce = Video::kPicturesAtOnce;   // taken of one panel

    PcPanels(session::Session &session, session::Links &links, session::Tailnet &tailnet, Programs &frames, std::function<uint64_t()> dice)
        : session_(session), links_(links), tailnet_(tailnet), frames_(frames), dice_(std::move(dice)) {}

    // A code for the wearer's PC to call with: good once, for ten minutes.
    std::string Invite(double now) { return invites_.Make(now, dice_); }
    // What a link that isn't a Frame's says (session::Config::device). False: it is closed.
    bool Said(int link, const std::string &address, uint16_t kind, const std::vector<uint8_t> &body, Video &video, double now);
    // A picture from a link. False when the link isn't the PC's: the picture is someone else's.
    bool Frame(int link, const uint8_t *data, size_t n, Video &video, double now);
    void Down(int link);
    // Often (every loop pass will do).
    void Tick(double now);

    bool linked() const { return link_ >= 0; }
    size_t dropped() const { return dropped_; }   // pictures from the PC that weren't taken

    void Rate(int number, int bitrate) override;
    void Full(int number, int aSecond) override;
    void Pictures(int number, int aSecond) override;
    void Key(int number) override;
    void Watched(int number, bool watched) override;
    void Show(uint8_t panel, bool key, const std::vector<uint8_t> &unit) override { frames_.Show(panel, key, unit); }
    void Decode(uint8_t panel, bool decode) override { frames_.Decode(panel, decode); }

private:
    struct Panel {
        Order order;
        room::Allowance allowance{kPicturesASecond, kPicturesAtOnce};
    };

    static bool Pcs(int number) { return number >= room::kFramePanels && number < room::kOwnPanels; }
    bool Refuse(int link, proto::Why why, const char *text);
    void Say(int number, uint8_t word, uint32_t value);

    session::Session &session_;
    session::Links &links_;
    session::Tailnet &tailnet_;
    Programs &frames_;
    std::function<uint64_t()> dice_;
    room::Invites invites_;
    int link_ = -1;
    Panel panels_[room::kPcPanels];
    size_t dropped_ = 0;
};

}  // namespace flow
