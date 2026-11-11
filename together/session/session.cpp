// A Frametop Together session's rules: what the links bring (session.h). What the wearer does
// is wearer.cpp's.
#include "session.h"

#include "../room/panels.h"

#include <cstring>
#include <tuple>

namespace session {
namespace {

const char kUpdate[] = "The other headset runs another version of Frametop Together. Update Frametop on both.";

bool SameRoom(const uint8_t a[proto::kRoomId], const uint8_t b[proto::kRoomId]) { return !std::memcmp(a, b, proto::kRoomId); }

}  // namespace

Session::Session(Links &links, Tailnet &tailnet, Config config) : links_(links), tailnet_(tailnet), config_(std::move(config)) {
    for (double &t : meshTried_) t = -1e9;
}

proto::Hello Session::MyHello(const std::string &code) const {
    proto::Hello h;
    if (mode_ == Mode::kHosting || mode_ == Mode::kJoined) std::memcpy(h.room, room_.room, proto::kRoomId);
    h.code = code, h.name = config_.name, h.port = config_.port;
    return h;
}

int Session::LinkOf(int person) const {
    for (const auto &[id, l] : link_)
        if ((l.state == Link::kMember || l.state == Link::kCalledPeer) && l.person == person) return id;
    return -1;
}

int Session::GuestAt(const std::string &address, uint16_t port) const {
    for (const proto::Person &p : room_.people)
        if (p.id != room_.owner && p.address == address && p.port == port) return p.id;
    return -1;
}

int Session::PersonOn(int link) const {
    const auto it = link_.find(link);
    return it != link_.end() && it->second.state == Link::kMember ? it->second.person : -1;
}

std::vector<int> Session::MemberLinks() const {
    std::vector<int> out;
    for (const auto &[id, l] : link_)
        if (l.state == Link::kMember) out.push_back(id);
    return out;
}

void Session::Up(int link, const std::string &address, double now) {
    const auto it = link_.find(link);
    if (it == link_.end()) {   // someone calls
        size_t strangers = 0;
        for (const auto &[id, l] : link_) strangers += l.state == Link::kStranger;
        if (strangers >= kMostStrangers) return links_.Close(link);
        Link &l = link_[link];
        l.address = address, l.since = now;
        return;
    }
    Link &l = it->second;   // one we called
    // a name may have led anywhere, and a guest's address is only what the room's owner said
    if (!tailnet_.MayCall(address) || (l.state == Link::kCalledPeer && address != l.address)) {
        if (l.state == Link::kCalledOwner) note_ = "That address isn't on your tailnet.";
        return Drop(link);
    }
    l.address = address, l.since = now;
    links_.Send(link, proto::Pack(MyHello(l.state == Link::kCalledOwner ? code_ : "")));
}

void Session::Bytes(int link, const uint8_t *data, size_t n, double now) {
    auto it = link_.find(link);
    if (it == link_.end()) return;
    if (!it->second.parser.Feed(data, n)) return Drop(link);
    uint16_t kind = 0;
    std::vector<uint8_t> body;
    // a message may end its link, or every link
    while ((it = link_.find(link)) != link_.end() && it->second.parser.Next(kind, body)) Handle(link, kind, body, now);
}

void Session::Down(int link, double) {
    const auto it = link_.find(link);
    if (it == link_.end()) return;
    const Link l = std::move(it->second);
    link_.erase(it);
    if (l.state == Link::kCalledOwner) {
        if (note_.empty()) note_ = "Couldn't reach the room.";
        End(note_);
    } else if (l.state == Link::kMember && mode_ == Mode::kHosting) {
        Remove(l.person);
    } else if (l.state == Link::kMember && mode_ == Mode::kJoined && l.person == room_.owner) {
        End("The room ended: the link to its owner was lost.");
    }
}

void Session::Drop(int link) {
    if (!link_.count(link)) return;
    links_.Close(link);
    Down(link, 0);
}

void Session::Refuse(int link, proto::Why why, const std::string &text) {
    proto::Refuse r;
    r.why = why, r.text = text;
    links_.Send(link, proto::Pack(r));
    Drop(link);
}

void Session::End(const std::string &note) {
    for (const auto &[id, l] : link_) links_.Close(id);
    link_.clear();
    invites_.Clear();
    mode_ = Mode::kIdle, me_ = -1, room_ = proto::Room{}, code_.clear(), asked_.clear(), lookSaid_ = -1e9;
    for (double &t : meshTried_) t = -1e9;
    note_ = note;
}

void Session::Changed() {
    ++room_.turn;
    const std::vector<uint8_t> message = proto::Pack(room_);
    for (int link : MemberLinks()) links_.Send(link, message);
}

void Session::Remove(int person) {
    for (size_t i = 0; i < room_.people.size(); ++i)
        if (room_.people[i].id == person && person != room_.owner) {
            note_ = room_.people[i].name + " left.";
            room_.people.erase(room_.people.begin() + long(i));
            room::DropPanelsOf(room_, person);   // what they shared goes with them
            return Changed();
        }
}

void Session::Handle(int link, uint16_t kind, const std::vector<uint8_t> &body, double now) {
    Link &l = link_.at(link);
    const bool fromOwner = mode_ == Mode::kJoined && l.state == Link::kMember && l.person == room_.owner;
    switch (kind) {
    case proto::kHello:
        return OnHello(link, l, body, now);
    case proto::kRefuse: {
        proto::Refuse r;
        if (l.state != Link::kCalledOwner || !proto::Read(body, r)) return Drop(link);
        note_ = "Not let in: " + (r.text.empty() ? std::string("no reason given.") : r.text);
        return Drop(link);
    }
    case proto::kWelcome: {
        proto::Welcome w;
        if (l.state != Link::kCalledOwner || !l.helloed || !proto::Read(body, w)) return Drop(link);
        mode_ = Mode::kJoined, me_ = w.you, room_ = w.room;
        l.state = Link::kMember, l.person = room_.owner;
        note_ = "You are in " + room_.person(room_.owner)->name + "'s room.";
        if (room_.person(me_)->away != away_) {   // away before coming in: the room hears it first
            proto::Away a;
            a.away = away_;
            links_.Send(link, proto::Pack(a));
        }
        SayLook(now);   // and what they have on
        return Mesh(now);
    }
    case proto::kRoom: {
        proto::Room r;
        if (!fromOwner || !proto::Read(body, r) || !SameRoom(r.room, room_.room) || r.owner != room_.owner) return Drop(link);
        return OnRoom(r, now);
    }
    case proto::kAway: {
        proto::Away a;
        if (mode_ != Mode::kHosting || l.state != Link::kMember || !proto::Read(body, a)) return Drop(link);
        for (proto::Person &p : room_.people)
            if (p.id == l.person && p.away != a.away) p.away = a.away, Changed();
        return;
    }
    case proto::kPanel: {   // a guest shares a panel of theirs, or says where it is now
        proto::Panel p;
        if (mode_ != Mode::kHosting || l.state != Link::kMember || !proto::Read(body, p) || !l.panelWords.Spend(now)) return Drop(link);
        const room::Shared shared = room::SetPanel(room_, l.person, p);
        if (shared == room::Shared::kNo) return Drop(link);
        if (shared == room::Shared::kChanged) Changed();
        return;
    }
    case proto::kUnpanel: {
        proto::Unpanel u;
        if (mode_ != Mode::kHosting || l.state != Link::kMember || !proto::Read(body, u) || !l.panelWords.Spend(now)) return Drop(link);
        if (u.panel / proto::kPanelNumbers != l.person) return Drop(link);
        if (room::DropPanel(room_, l.person, u.panel)) Changed();
        return;
    }
    case proto::kWatch: {   // what they have of a panel of ours; one that just went is nothing
        proto::Watch w;
        if (l.state != Link::kMember || !proto::Read(body, w) || !l.panelWords.Spend(now)) return Drop(link);
        if (w.panel / proto::kPanelNumbers != me_) return Drop(link);
        if (room_.panel(w.panel) && config_.watched) config_.watched(link, w, now);
        return;
    }
    case proto::kLook: {   // what a guest has on
        proto::Look look;
        if (mode_ != Mode::kHosting || l.state != Link::kMember || !proto::Read(body, look) || !l.lookWords.Spend(now)) return Drop(link);
        for (proto::Person &p : room_.people)
            if (p.id == l.person && !proto::SameLook(p.look, look)) p.look = look, Changed();
        return;
    }
    case proto::kWant: {   // the file of an avatar: whether it is ours to send is flow/looks.h's to say
        proto::Want w;
        if (l.state != Link::kMember || !proto::Read(body, w) || !l.lookWords.Spend(now)) return Drop(link);
        if (config_.wanted) config_.wanted(link, w, now);
        return;
    }
    case proto::kLeave:
        if (fromOwner) return End("The room ended: its owner closed it.");
        return Drop(link);   // an owner's Drop takes them out of the room
    default:   // a kind the session doesn't read: a member's goes to whoever does, or is one of a later version
        if (l.state != Link::kMember || (config_.said && !config_.said(link, kind, body, now))) Drop(link);
    }
}

void Session::OnHello(int link, Link &l, const std::vector<uint8_t> &body, double now) {
    proto::Hello h;
    if (l.helloed || !proto::Read(body, h)) return Drop(link);
    if (h.version != proto::kVersion) {
        if (l.state == Link::kStranger) return Refuse(link, proto::Why::kVersion, kUpdate);
        if (l.state == Link::kCalledOwner) note_ = kUpdate;
        return Drop(link);
    }
    l.helloed = true, l.hello = h;
    switch (l.state) {
    case Link::kCalledOwner:   // their Welcome or their Refuse follows, once the owner has answered
        return;
    case Link::kCalledPeer:
        if (!SameRoom(h.room, room_.room)) return Drop(link);
        l.state = Link::kMember;
        return;
    case Link::kStranger:
        if (mode_ == Mode::kHosting) return Asked(link, l, now);
        if (mode_ != Mode::kJoined) return Refuse(link, proto::Why::kNotOpen, "No room is open on that headset.");
        if (!SameRoom(h.room, room_.room)) return Refuse(link, proto::Why::kNotInRoom, "That headset is in another room.");
        Knocked(link, l);   // or they wait until the owner has told us of them
        return;
    default:
        return Drop(link);
    }
}

// The owner's side: a stranger with an invite.
void Session::Asked(int link, Link &l, double now) {
    Who who;
    if (!tailnet_.WhoIs(l.address, who)) return Refuse(link, proto::Why::kBad, "Your headset isn't on this room's tailnet.");
    size_t asking = 0;
    for (const auto &[id, other] : link_) asking += other.state == Link::kAsking;
    // a headset the room lists already asks for the place it has (wearer.cpp, Answer)
    const size_t others = room_.people.size() - (GuestAt(l.address, l.hello.port) >= 0);
    if (others + asking >= size_t(proto::kPeople)) return Refuse(link, proto::Why::kFull, "The room is full.");
    if (invites_.Use(l.hello.code, l.address, now) != room::Invites::Answer::kGood)
        return Refuse(link, proto::Why::kCode, "That invite code isn't good. A code works once, for ten minutes.");
    l.state = Link::kAsking, l.who = who, l.ask = ++nextAsk_, l.since = now;
    links_.Send(link, proto::Pack(MyHello("")));
    note_ = (who.login.empty() ? l.address : who.login) + " asks to come in.";
}

// A guest's side: another guest calls. True once they are who the owner said is in the room.
bool Session::Knocked(int link, Link &l) {
    for (const proto::Person &p : room_.people) {
        if (p.id == me_ || p.id == room_.owner || p.address != l.address || p.port != l.hello.port || LinkOf(p.id) >= 0) continue;
        Who who;
        if (!tailnet_.WhoIs(l.address, who)) break;
        l.state = Link::kMember, l.person = p.id, l.who = who;
        links_.Send(link, proto::Pack(MyHello("")));
        return true;
    }
    return false;
}

// A guest's side: the room as its owner has it now.
void Session::OnRoom(const proto::Room &r, double now) {
    if (r.turn <= room_.turn) return;
    if (!r.person(me_)) return End("You were taken out of the room.");
    for (int id = 0; id < proto::kPeople; ++id) {   // a place's next person is called without waiting for the last one's turn
        const proto::Person *was = room_.person(id), *is = r.person(id);
        if (!was || !is || was->address != is->address || was->port != is->port) meshTried_[id] = -1e9;
    }
    room_ = r;
    std::vector<int> gone;
    for (const auto &[id, l] : link_) {
        if (l.state != Link::kMember && l.state != Link::kCalledPeer) continue;
        const proto::Person *p = room_.person(l.person);
        if (!p || (l.person != room_.owner && p->address != l.address)) gone.push_back(id);
    }
    for (int id : gone) Drop(id);
    for (auto &[id, l] : link_)
        if (l.state == Link::kStranger && l.helloed) Knocked(id, l);
    Mesh(now);
}

// A guest's side: the other guests are called, by the one of each two whose address is lower.
void Session::Mesh(double now) {
    if (mode_ != Mode::kJoined) return;
    const proto::Person mine = *room_.person(me_);
    for (const proto::Person &p : room_.people) {
        if (p.id == me_ || p.id == room_.owner || LinkOf(p.id) >= 0) continue;
        if (std::tie(mine.address, mine.port) >= std::tie(p.address, p.port)) continue;
        if (now - meshTried_[p.id] < kMeshRetryS || !tailnet_.MayCall(p.address)) continue;
        meshTried_[p.id] = now;
        const int link = links_.Call(p.address, p.port);
        if (link < 0) continue;
        Link &l = link_[link];
        l.state = Link::kCalledPeer, l.person = p.id, l.address = p.address, l.since = now;
    }
}

void Session::Tick(double now) {
    std::vector<int> late, unanswered;
    for (const auto &[id, l] : link_) {
        const double waited = now - l.since;
        if (l.state == Link::kAsking && waited > kAskWaitS) unanswered.push_back(id);
        else if ((l.state == Link::kStranger || l.state == Link::kCalledPeer) && waited > kHelloWaitS) late.push_back(id);
        else if (l.state == Link::kCalledOwner && waited > kHelloWaitS + (l.helloed ? kAskWaitS : 0)) late.push_back(id);
    }
    for (int id : unanswered) Refuse(id, proto::Why::kDeclined, "The room's owner didn't answer.");
    for (int id : late) {
        const auto it = link_.find(id);
        if (it == link_.end()) continue;
        if (it->second.state == Link::kCalledOwner) note_ = "No answer from the room.";
        Drop(id);
    }
    Mesh(now);
    SayLook(now);
}

}  // namespace session
