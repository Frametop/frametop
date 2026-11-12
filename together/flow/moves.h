// Moving shared panels, through ft-together (docs/together.md, "Control and moving";
// proto/move.h has the message).
//
// A panel stands where its owner's Frame has it, and its owner moves it there like any screen
// of theirs. Two others may move it: the room's owner, always, and whoever its owner gave it to,
// for as long as that grant stands (control.h: a grant covers clicking, typing and moving).
// They say where they put it, and the owner's Frame takes it there.
//
// On the Frame whose panels they are: a move is checked here, where it is enforced, and nowhere
// else. It is for one of this Frame's own panels that is shared now, and from someone who may
// move it. The newest place asked for each panel is kept until the program that places this
// Frame's screens asks for it (Taken), and no longer than kKeepS. A move from anyone else goes
// nowhere. A link that speaks of panels that aren't this Frame's, or faster than a hand moves
// one, is closed.
//
// On every other Frame: the wearer's own moves of someone else's panel, sent to its owner when
// the wearer may make them, and refused here when they may not, so that whoever asked can put
// the panel back.
//
// The rules only. Who holds which grant is asked through the two functions it is given.
#pragma once

#include "../proto/move.h"
#include "../room/allowance.h"
#include "../session/session.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace flow {

class Moves {
public:
    // What one link may send: a panel in someone's hand is told a few times a second.
    static constexpr double kMovesASecond = 30, kMovesAtOnce = 60;
    // What the wearer's own programs may send to one owner, under what that owner takes.
    static constexpr double kMineASecond = 20, kMineAtOnce = 40;
    static constexpr double kKeepS = 5;   // a place nobody took in this long is forgotten
    static constexpr const char *kMayNot = "you may not move that panel";

    // `granted(number, person, now)`: that person has this Frame's panel `number` now.
    // `holds(panel, now)`: the wearer has someone else's `panel` now.
    using Granted = std::function<bool(int number, int person, double now)>;
    using Holds = std::function<bool(uint8_t panel, double now)>;

    Moves(session::Session &session, Granted granted, Holds holds) : session_(session), granted_(std::move(granted)), holds_(std::move(holds)) {}

    // From the session: someone in the room moved a panel. False when it wasn't theirs to say.
    bool Said(int link, const std::vector<uint8_t> &body, double now);
    void Down(int link) { link_.erase(link); }
    // Often: what is of a room that was left, or of a panel no longer shared, is forgotten.
    void Tick(double now);

    // The wearer put someone else's panel there. Nothing, or why it wasn't sent.
    std::string Move(const proto::Move &move, double now);
    // Where the others put this Frame's panels since this was last asked, each panel's newest:
    // "COUNT NUMBER:PERSON:X:Y:Z:W:X:Y:Z ...", by the panels' numbers. They are forgotten with it.
    std::string Taken(double now);
    size_t dropped() const { return dropped_; }

private:
    struct Asked {
        int person = 0;
        proto::Move move;
        double when = 0;
    };

    bool inRoom() const { return session_.mode() == session::Mode::kHosting || session_.mode() == session::Mode::kJoined; }

    session::Session &session_;
    Granted granted_;
    Holds holds_;
    std::map<int, room::Allowance> link_;   // what each link may still say
    std::map<int, room::Allowance> mine_;   // what the wearer's own programs may still send, by the person it goes to
    std::map<int, Asked> asked_;            // this Frame's panels, by number: where someone put each
    size_t dropped_ = 0;
};

}  // namespace flow
