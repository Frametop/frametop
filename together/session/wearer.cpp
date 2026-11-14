// A Frametop Together session: what this Frame's wearer does (session.h). What the links bring
// is session.cpp's.
#include "session.h"

#include "../room/panels.h"
#include "../room/seats.h"

#include <cmath>
#include <cstring>

namespace session {

std::string Session::Open(const proto::Spot &spot) {
    if (mode_ != Mode::kIdle) return "you are in a room already";
    for (float v : {spot.at[0], spot.at[1], spot.at[2]})
        if (!(std::fabs(v) <= proto::kFar)) return "that spot is too far away";
    if (!(std::fabs(spot.yaw) <= 7)) return "that isn't a direction";
    Who who;
    proto::Person p;
    if (!tailnet_.Self(who, p.address)) return "Tailscale isn't running on this headset";
    p.spot = spot, p.away = away_, p.look = look_;
    p.name = config_.name.empty() ? who.name : config_.name, p.login = who.login, p.port = config_.port;
    room_ = proto::Room{};
    for (size_t i = 0; i < proto::kRoomId; i += 8) {
        const uint64_t roll = config_.dice();
        std::memcpy(room_.room + i, &roll, 8);
    }
    room_.people.push_back(p);
    mode_ = Mode::kHosting, me_ = 0, note_ = "Your room is open.";
    return "";
}

std::string Session::Invite(double now, std::string &code) {
    if (mode_ != Mode::kHosting) return "open a room first";
    code = invites_.Make(now, config_.dice);
    return "";
}

std::string Session::Join(const std::string &host, uint16_t port, const std::string &code, double now) {
    if (mode_ != Mode::kIdle) return "you are in a room already";
    if (host.empty() || host.size() > 253 || !port || code.size() > proto::kMostCode) return "that isn't a headset and an invite code";
    const int link = links_.Call(host, port);
    if (link < 0) return "couldn't call " + host;
    Link &l = link_[link];
    l.state = Link::kCalledOwner, l.address = host, l.since = now;
    mode_ = Mode::kCalling, code_ = code, note_.clear();
    return "";
}

std::string Session::Answer(int ask, bool accept) {
    for (auto &[id, l] : link_) {
        if (mode_ != Mode::kHosting || l.state != Link::kAsking || l.ask != ask) continue;
        if (!accept) return Refuse(id, proto::Why::kDeclined, "The room's owner said no."), "";
        // A headset comes in once. One the room lists already lost its link, and its end of that
        // was heard there first: the old link and its place go before the new one is given.
        if (const int was = GuestAt(l.address, l.hello.port); was >= 0) LinkOf(was) >= 0 ? Drop(LinkOf(was)) : Remove(was);
        const int place = room::FreeId(room_);
        if (place < 0) return Refuse(id, proto::Why::kFull, "The room is full."), "the room is full";
        proto::Person p;
        p.id = uint8_t(place), p.name = l.hello.name.empty() ? l.who.name : l.hello.name, p.login = l.who.login;
        p.address = l.address, p.port = l.hello.port, p.spot = room::SpotBeside(room_.person(room_.owner)->spot, place);
        note_ = p.name + " came in.";
        room_.people.push_back(p);
        Changed();   // to those already in: the newcomer's copy is their welcome
        l.state = Link::kMember, l.person = place;
        proto::Welcome w;
        w.you = p.id, w.room = room_;
        links_.Send(id, proto::Pack(w));
        return "";
    }
    return "no such ask";
}

std::string Session::SetAway(uint8_t away) {
    if (away & ~(proto::kAwayGame | proto::kAwayOff)) return "that isn't a way to be away";
    away_ = away;
    if (mode_ == Mode::kHosting) {
        if (room_.people[0].away != away) room_.people[0].away = away, Changed();
    } else if (mode_ == Mode::kJoined) {
        proto::Away a;
        a.away = away;
        links_.Send(LinkOf(room_.owner), proto::Pack(a));
    }
    return "";
}

std::string Session::Wear(const proto::Look &look, double now) {
    if (look.size > proto::kMostLook) return "that avatar is too large to send";
    look_ = look.none() ? proto::Look{} : look;
    SayLook(now);
    return "";
}

// The room hears what the wearer has on. An owner's is in the room at once. A guest's goes to
// the owner, and again from Tick until the owner's room has it.
void Session::SayLook(double now) {
    if (mode_ == Mode::kHosting) {
        if (!proto::SameLook(room_.people[0].look, look_)) room_.people[0].look = look_, Changed();
    } else if (mode_ == Mode::kJoined && !proto::SameLook(room_.person(me_)->look, look_) && now - lookSaid_ >= kLookEveryS) {
        lookSaid_ = now;
        links_.Send(LinkOf(room_.owner), proto::Pack(look_));
    }
}

std::string Session::Want(int person, const proto::Look &look) {
    const int link = person == me_ ? -1 : LinkTo(person);
    if (link < 0) return "no link to them yet";
    proto::Want w;
    std::memcpy(w.hash, look.hash, proto::kLookHash);
    links_.Send(link, proto::Pack(w));
    return "";
}

std::string Session::Leave() {
    if (mode_ == Mode::kIdle) return "you aren't in a room";
    const char *const note = mode_ == Mode::kHosting ? "You closed your room." : mode_ == Mode::kCalling ? "You stopped calling." : "You left the room.";
    for (int link : MemberLinks()) links_.Send(link, proto::PackLeave());
    std::vector<int> asking;
    for (const auto &[id, l] : link_)
        if (l.state == Link::kAsking) asking.push_back(id);
    for (int id : asking) Refuse(id, proto::Why::kNotOpen, "The room closed.");
    End(note);
    return "";
}

// The owner puts its own panel in the room; a guest asks the owner to, and says so again only
// when the panel changed.
std::string Session::Share(int number, proto::Panel panel) {
    if (mode_ != Mode::kHosting && mode_ != Mode::kJoined) return "you aren't in a room";
    if (number < 0 || number >= room::kOwnPanels) return "a panel's number is 0 to " + std::to_string(room::kOwnPanels - 1);
    panel.id = proto::PanelId(me_, number), panel.pictures = panelPictures_;
    panel.name = wire::Clean(panel.name, proto::kMostPanelName);
    if (!proto::Fits(panel, proto::kPeople)) return "that isn't a panel: its size, its place or its turn is off";
    if (mode_ == Mode::kHosting) {
        if (room::SetPanel(room_, me_, panel) == room::Shared::kChanged) Changed();
        return "";
    }
    const auto asked = asked_.find(panel.id);
    if (asked != asked_.end() && proto::SamePanel(asked->second, panel)) return "";
    asked_[panel.id] = panel;
    links_.Send(LinkOf(room_.owner), proto::Pack(panel));
    return "";
}

void Session::PanelPictures(int aSecond) {
    if (aSecond < 1 || aSecond > proto::kMostPanelPictures || aSecond == panelPictures_) return;
    panelPictures_ = uint8_t(aSecond);
    if (mode_ == Mode::kHosting) {   // the owner's are in the room at once
        bool changed = false;
        for (proto::Panel &p : room_.panels)
            if (p.owner() == me_) p.pictures = panelPictures_, changed = true;
        if (changed) Changed();
    }
    // (a guest's: the owner is told of each again)
    for (auto &[id, panel] : asked_) panel.pictures = panelPictures_, links_.Send(LinkOf(room_.owner), proto::Pack(panel));
}

std::string Session::Unshare(int number) {
    if (mode_ != Mode::kHosting && mode_ != Mode::kJoined) return "you aren't in a room";
    if (number < 0 || number >= room::kOwnPanels) return "no such panel";
    proto::Unpanel u;
    u.panel = proto::PanelId(me_, number);
    if (mode_ == Mode::kHosting) {
        if (room::DropPanel(room_, me_, u.panel)) Changed();
    } else if (asked_.erase(u.panel)) {
        links_.Send(LinkOf(room_.owner), proto::Pack(u));
    }
    return "";
}

std::string Session::Watch(const proto::Watch &watch) {
    const proto::Panel *panel = mode_ == Mode::kHosting || mode_ == Mode::kJoined ? room_.panel(watch.panel) : nullptr;
    if (!panel) return "no such panel";
    if (panel->owner() == me_) return "that panel is yours";
    const int link = LinkOf(panel->owner());
    if (PersonOn(link) != panel->owner()) return "no link to that panel's owner yet";
    links_.Send(link, proto::Pack(watch));
    return "";
}

std::string Session::Tell(int person, const std::vector<uint8_t> &message) {
    const int link = person == me_ ? -1 : LinkTo(person);
    if (link < 0) return "no link to them yet";
    links_.Send(link, message);
    return "";
}

std::vector<Ask> Session::asks() const {
    std::vector<Ask> out;
    for (const auto &[id, l] : link_)
        if (l.state == Link::kAsking) out.push_back({l.ask, l.hello.name, l.who.login, l.address});
    return out;
}

}  // namespace session
