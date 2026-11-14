// A Frametop Together session, its rules (docs/together.md): opening a room, inviting, asking
// to come in and being let in or not, who is in the room, the panels they share, and the links
// between its people. The owner's copy of the room is the room; a guest holds the last one the
// owner sent, and links up with the other guests it lists (the one with the lower address
// calls). A guest's panel is in the room once the owner has put it there on the guest's word,
// and so is the avatar a guest has on.
//
// A link whose first word is a Sender's isn't a Frame's: it is a split compute PC with panels
// to send. The session only keeps such a link apart from the room's and passes on what it
// says (Config::device); who it is and what it may say is flow/pcpanels.h's to check.
//
// Everything a link brings is untrusted input, also after its sender was let in: the messages
// are bounded by their readers (proto/messages.h), and here every message is checked against
// who sent it and what they may say. A link that says anything else is dropped.
//
// The rules only. The network, the tailnet, the clock and the dice are handed in, so two of
// these can be run against each other in a test, and ft-together and the fake peer are the same
// code on different links. commands.h puts it behind a line of text. session.cpp has what the
// links bring, wearer.cpp what this Frame's wearer does.
#pragma once

#include "../proto/messages.h"
#include "../room/allowance.h"
#include "../room/invites.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace session {

// Who tailscaled says is at an address.
struct Who {
    std::string login, name;
};

// The links to other people's Frames, as the session uses them (net/ has msquic's).
class Links {
public:
    virtual ~Links() = default;
    // Calls someone: the new link's number (never negative), or -1. Up or Down follows.
    virtual int Call(const std::string &host, uint16_t port) = 0;
    // A message on the link's control stream: they arrive in the order they were sent.
    virtual void Send(int link, const std::vector<uint8_t> &message) = 0;
    // Ends a link once what was sent on it has gone. Nothing more comes from it, Down neither.
    virtual void Close(int link) = 0;
};

// The tailnet (net/ asks tailscaled).
class Tailnet {
public:
    virtual ~Tailnet() = default;
    virtual bool Self(Who &who, std::string &address) = 0;           // false: Tailscale isn't up
    virtual bool WhoIs(const std::string &address, Who &who) = 0;    // false: nobody the tailnet knows
    virtual bool MayCall(const std::string &address) = 0;            // an address on the tailnet
};

struct Config {
    std::string name;              // what the others call this person
    uint16_t port = 0;             // where this end listens
    std::function<uint64_t()> dice;
    // Someone in the room says what they have of a panel of ours (flow/video.h sends by it).
    std::function<void(int link, const proto::Watch &watch, double now)> watched;
    // Someone in the room asks for the file of an avatar (flow/looks.h sends ours, if it is that).
    std::function<void(int link, const proto::Want &want, double now)> wanted;
    // Someone in the room says a message of a kind the session doesn't read itself (control's:
    // flow/control.h). False when it wasn't theirs to say: the link is closed.
    std::function<bool(int link, uint16_t kind, const std::vector<uint8_t> &body, double now)> said;
    // A link that isn't a Frame's says something, its first word (a Sender) and all after it.
    // `address` is its far end's. False when it wasn't theirs to say: the link is closed.
    std::function<bool(int link, const std::string &address, uint16_t kind, const std::vector<uint8_t> &body, double now)> device;
};

enum class Mode { kIdle, kHosting, kCalling, kJoined };

// Someone with a good invite, waiting for the owner's answer.
struct Ask {
    int id;
    std::string name, login, address;   // the name is what they call themselves; the login is Tailscale's
};

class Session {
public:
    static constexpr double kHelloWaitS = 10;   // a link gets this long to say who it is, and a call to be let in or asked about
    static constexpr double kAskWaitS = 120;    // ...and the owner this long to answer an ask
    static constexpr double kMeshRetryS = 3;    // between calls to a guest that didn't answer
    static constexpr size_t kMostStrangers = 8; // links that haven't said who they are, at once
    // What one link may say about panels (shared, moved, taken back, watched): a second, at once.
    static constexpr double kPanelWordsASecond = 40, kPanelWordsAtOnce = 40;
    // ...and about avatars (what they have on, a file asked for). A guest says what it has on
    // every kLookEveryS at most, until the room has it.
    static constexpr double kLookWordsASecond = 1, kLookWordsAtOnce = 6, kLookEveryS = 2;

    Session(Links &links, Tailnet &tailnet, Config config);

    // From the links. `address` is the far end's, as the network saw it.
    void Up(int link, const std::string &address, double now);
    void Bytes(int link, const uint8_t *data, size_t n, double now);
    void Down(int link, double now);
    // Now and then (a second apart is enough): what was waiting too long ends.
    void Tick(double now);

    // What this Frame's wearer does. Each answers with nothing, or with why not.
    std::string Open(const proto::Spot &spot);               // a room, with you its owner standing there
    std::string Invite(double now, std::string &code);       // a one-time code for your room
    // Calls a room's owner. How it went shows in mode() and note().
    std::string Join(const std::string &host, uint16_t port, const std::string &code, double now);
    std::string Answer(int ask, bool accept);                // one of asks()
    // proto::kAwayGame, kAwayOff, or 0 for back. It is the wearer's own, in a room or not: the
    // next room has it from the start, so nobody is there for a moment who isn't.
    std::string SetAway(uint8_t away);
    std::string Leave();                                     // its owner leaving ends the room for everyone
    // The avatar the wearer has on, for the others to ask for (none: they see the abstract
    // figure). Like away it is the wearer's own, in a room or not.
    std::string Wear(const proto::Look &look, double now);
    // Asks someone in the room for the file of the avatar they have on.
    std::string Want(int person, const proto::Look &look);
    // Shares the wearer's panel `number` (0 to room::kOwnPanels - 1: the Frame's own, then
    // their PC's) with the room, or says where and how large it is now. The panel's id is set
    // here; a guest's is in room() once the owner has taken it.
    std::string Share(int number, proto::Panel panel);
    std::string Unshare(int number);
    // The pictures a second the wearer's panels have at the most, from now on: the room hears
    // it with each of them. Like away it is the wearer's own, in a room or not.
    void PanelPictures(int aSecond);
    // Tells a panel's owner what the wearer has of it.
    std::string Watch(const proto::Watch &watch);
    // A framed message to one of the room's people, on the link to them: for what the session
    // carries without reading it (Config::said at the other end).
    std::string Tell(int person, const std::vector<uint8_t> &message);
    // Ends a link that isn't a Frame's (Config::device), for whoever it was handed to.
    void Hang(int link);

    Mode mode() const { return mode_; }
    int me() const { return me_; }
    uint8_t away() const { return away_; }                   // as the wearer was last said to be
    const proto::Look &look() const { return look_; }        // what the wearer was last said to have on
    const proto::Room &room() const { return room_; }
    const std::string &note() const { return note_; }        // the last thing worth telling the wearer
    std::vector<Ask> asks() const;                           // who waits for the owner's answer
    size_t codes() const { return invites_.waiting(); }      // invites nobody used yet
    // The person a link belongs to once they are in the room with us, or -1.
    int PersonOn(int link) const;
    // The links to the room's people, for what goes to everyone (poses, voice).
    std::vector<int> MemberLinks() const;
    // The link to one of the room's people once they are in the room with us, or -1.
    int LinkTo(int person) const { return PersonOn(LinkOf(person)) == person ? LinkOf(person) : -1; }
    // Whether one of the room's people is this Frame's wearer, or someone it has a link to.
    bool Linked(int person) const { return person == me_ || LinkTo(person) >= 0; }

private:
    struct Link {
        enum State { kStranger, kAsking, kMember, kCalledOwner, kCalledPeer, kDevice };
        State state = kStranger;
        std::string address;
        proto::Parser parser;
        double since = 0;
        bool helloed = false;   // their Hello came
        proto::Hello hello;
        Who who;
        int person = -1;        // kMember, kCalledPeer
        int ask = 0;            // kAsking
        room::Allowance panelWords{kPanelWordsASecond, kPanelWordsAtOnce};
        room::Allowance lookWords{kLookWordsASecond, kLookWordsAtOnce};
    };

    void Handle(int link, uint16_t kind, const std::vector<uint8_t> &body, double now);
    void OnHello(int link, Link &l, const std::vector<uint8_t> &body, double now);
    void Asked(int link, Link &l, double now);
    bool Knocked(int link, Link &l);
    void OnRoom(const proto::Room &room, double now);
    void Mesh(double now);
    void SayLook(double now);
    void Changed();
    void Remove(int person);
    void Refuse(int link, proto::Why why, const std::string &text);
    void Drop(int link);
    void End(const std::string &note);
    proto::Hello MyHello(const std::string &code) const;
    int LinkOf(int person) const;
    // The guest the room lists at a headset's address and port, or -1.
    int GuestAt(const std::string &address, uint16_t port) const;

    Links &links_;
    Tailnet &tailnet_;
    Config config_;
    Mode mode_ = Mode::kIdle;
    int me_ = -1;
    uint8_t away_ = 0;
    proto::Look look_;
    uint8_t panelPictures_ = proto::kPanelPictures;
    double lookSaid_ = -1e9;   // when a guest last told the owner what it has on
    proto::Room room_;
    room::Invites invites_;
    std::map<int, Link> link_;
    std::string code_, note_;   // the invite a call carries; the last thing worth telling
    std::map<uint8_t, proto::Panel> asked_;   // a guest's panels, as the owner was last told of them
    int nextAsk_ = 0;
    double meshTried_[proto::kPeople] = {};
};

}  // namespace session
