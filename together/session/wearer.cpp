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
    p.spot = spot, p.away = away_;
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
        const int place = room::FreeId(room_);
        if (!accept) return Refuse(id, proto::Why::kDeclined, "The room's owner said no."), "";
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
    if (number < 0 || number >= room::kFramePanels) return "a headset shares " + std::to_string(room::kFramePanels) + " panels at most";
    panel.id = proto::PanelId(me_, number);
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

std::string Session::Unshare(int number) {
    if (mode_ != Mode::kHosting && mode_ != Mode::kJoined) return "you aren't in a room";
    if (number < 0 || number >= room::kFramePanels) return "no such panel";
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

std::vector<Ask> Session::asks() const {
    std::vector<Ask> out;
    for (const auto &[id, l] : link_)
        if (l.state == Link::kAsking) out.push_back({l.ask, l.hello.name, l.who.login, l.address});
    return out;
}

}  // namespace session
