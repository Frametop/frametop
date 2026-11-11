// Control of shared panels, through ft-together (docs/together.md, "Control and moving",
// "Trust"; proto/control.h has the messages).
//
// On the Frame whose panels they are: someone in the room asks for a panel, the wearer gives
// it or doesn't, and from then on that person's input on it is passed to the program that
// gives it to the desktop. It is checked here, where it is enforced, and nowhere else: the
// input is for one of this Frame's own panels that is shared now, its sender holds a grant for
// that panel that still stands (room/grants.h: only while the wearer is known to be there), and
// nobody else has the desk (room/desk.h: one at a time, and what someone holds down is let go
// when they may hold it no longer). Input that fails any of these goes nowhere. A link that
// speaks of panels that aren't its to speak of, or faster than anyone's hands, is closed.
//
// On every other Frame: which panels the wearer was given, for as long as the last word on
// each says, and the wearer's own input on them sent to their owners.
//
// The rules only. The links are the session's and the desktop is handed in (local/inputs.h),
// so several of these can be run against each other in a test.
#pragma once

#include "../room/allowance.h"
#include "../room/desk.h"
#include "../room/grants.h"
#include "../session/session.h"

#include <map>
#include <string>
#include <vector>

namespace flow {

// This Frame's desktop, as it is given other people's input.
class Inputs {
public:
    virtual ~Inputs() = default;
    // Input from `person`, who may give it now. The act's panel is one of this Frame's.
    virtual void Input(int person, const proto::Act &act) = 0;
};

class Control {
public:
    // What one link may send: acts (a fast mouse says 125 a second), and asks and grants.
    static constexpr double kActsASecond = 240, kActsAtOnce = 480;
    static constexpr double kWordsASecond = 2, kWordsAtOnce = 8;
    // What the wearer's own programs may send to one owner, under what that owner takes.
    static constexpr double kMineASecond = 200, kMineAtOnce = 200;
    static constexpr double kMineWordsASecond = 1, kMineWordsAtOnce = 4;
    static constexpr double kTellS = 3;   // between the words that keep a grant standing at its holder's end

    Control(session::Session &session, Inputs &inputs) : session_(session), inputs_(inputs) {}

    // From the session: someone in the room said a message it doesn't read. False when it
    // wasn't theirs to say.
    bool Said(int link, uint16_t kind, const std::vector<uint8_t> &body, double now);
    void Down(int link) { link_.erase(link); }
    // Often (each look at the links is enough; ten times a second while busy()).
    void Tick(double now);
    bool busy() const { return desk_.holder() != room::Desk::kNobody; }

    // What this Frame's wearer does. Each answers with nothing, or with why not.
    void Here(double now);                                       // they are there, by whoever knows
    std::string Own(double now);                                 // their own hand is on their desktop
    std::string Give(int number, int person, double now);        // a panel of theirs, to someone
    std::string Take(int number, int person);                    // ...taken back, or an ask refused; -1: from everyone
    std::string Ask(int panel, double now);                      // someone else's panel, asked for
    std::string Act(const proto::Act &act, double now);          // their input on a panel they were given

    // {"there":B,"left":S,"asks":[{"number":N,"person":P}],"given":[...],"holder":P,
    //  "held":[{"panel":ID,"left":S}],"asked":[ID],"passed":N,"dropped":N}
    std::string State(double now) const;
    size_t passed() const { return passed_; }
    size_t dropped() const { return dropped_; }

private:
    struct Told {
        room::Allowance acts{kActsASecond, kActsAtOnce};
        room::Allowance words{kWordsASecond, kWordsAtOnce};
    };

    struct Mine {
        room::Allowance acts{kMineASecond, kMineAtOnce};
        room::Allowance words{kMineWordsASecond, kMineWordsAtOnce};
    };

    bool inRoom() const { return session_.mode() == session::Mode::kHosting || session_.mode() == session::Mode::kJoined; }
    std::string Who(int person) const;
    void Same(int person, double now);
    void Tell(const room::Granted &g, double seconds);
    void LetGo(int person, const std::vector<proto::Act> &let);
    void Ended(const std::vector<room::Granted> &ended);

    session::Session &session_;
    Inputs &inputs_;
    room::Grants grants_;
    room::Desk desk_;
    std::map<int, Told> link_;          // what each link may still say
    std::map<uint8_t, double> held_;    // the others' panels the wearer may control, and until when
    std::map<uint8_t, double> asked_;   // ...and the ones they asked for, and when
    std::map<int, Mine> mine_;          // what the wearer's own programs may still send, by the person it goes to
    std::string who_[proto::kPeople];   // who was in each place when its asks and grants were made
    double told_ = -1e9;
    size_t passed_ = 0, dropped_ = 0;
};

}  // namespace flow
