// Control of shared panels, through ft-together (control.h).
#include "control.h"

#include <cmath>
#include <iterator>
#include <sstream>

namespace flow {

// Who is in a place, for telling them from the next one in it: a headset and its login.
std::string Control::Who(int person) const {
    const proto::Person *p = session_.room().person(person);
    return p ? p->address + " " + std::to_string(p->port) + " " + p->login : "";
}

// A place's asks and grants are the person's who was in it when they were made. Someone else
// in it starts with none, however soon after the last one they came.
void Control::Same(int person, double now) {
    if (person < 0 || person >= proto::kPeople || Who(person) == who_[person]) return;
    who_[person] = Who(person);
    Ended(grants_.Sweep(now, [person](int, int theirs) { return theirs != person; }));
}

void Control::Tell(const room::Granted &g, double seconds) {
    proto::Grant grant;
    grant.panel = proto::PanelId(session_.me(), g.number);
    grant.seconds = uint8_t(std::ceil(std::fmin(seconds, double(proto::kMostLease))));
    session_.Tell(g.person, proto::Pack(grant));   // with no link to them yet, the next word in kTellS reaches them
}

void Control::LetGo(int person, const std::vector<proto::Act> &let) {
    for (const proto::Act &act : let) inputs_.Input(person, act);
}

// Grants that stand no longer: what their holders held down is let go, and they are told.
void Control::Ended(const std::vector<room::Granted> &ended) {
    for (const room::Granted &g : ended) {
        std::vector<proto::Act> let;
        desk_.Leave(g.person, let);
        LetGo(g.person, let);
        if (inRoom()) Tell(g, 0);
    }
}

bool Control::Said(int link, uint16_t kind, const std::vector<uint8_t> &body, double now) {
    const int person = session_.PersonOn(link), me = session_.me();
    if (person < 0) return false;
    if (kind != proto::kAskControl && kind != proto::kGrant && kind != proto::kAct) return true;   // a later version's
    const proto::Room &room = session_.room();
    Told &told = link_[link];
    Same(person, now);
    if (kind == proto::kAskControl) {
        proto::AskControl ask;
        if (!proto::Read(body, ask) || !told.words.Spend(now) || ask.panel / proto::kPanelNumbers != me) return false;
        const room::Granted g = {ask.panel % proto::kPanelNumbers, person};
        if (!room.panel(ask.panel)) return true;   // one that just went
        if (!grants_.Ask(g.number, person, now)) Tell(g, grants_.left(now));   // theirs already: they hear it again
        return true;
    }
    if (kind == proto::kGrant) {   // only a panel's owner gives it
        proto::Grant grant;
        if (!proto::Read(body, grant) || !told.words.Spend(now) || grant.panel / proto::kPanelNumbers != person) return false;
        asked_.erase(grant.panel);
        if (grant.seconds && room.panel(grant.panel)) held_[grant.panel] = now + grant.seconds;
        else held_.erase(grant.panel);
        return true;
    }
    proto::Act act;
    if (!proto::Read(body, act) || !told.acts.Spend(now) || act.panel / proto::kPanelNumbers != me) return false;
    // late input, after a grant ended or while someone else has the desk, is nothing worse than late
    if (!room.panel(act.panel) || !grants_.May(act.panel % proto::kPanelNumbers, person, now) || !desk_.Take(person, act, now)) return ++dropped_, true;
    ++passed_;
    inputs_.Input(person, act);
    return true;
}

void Control::Tick(double now) {
    const int holder = desk_.holder();
    std::vector<proto::Act> let;
    if (!inRoom()) {   // the room was left: nothing of it stands
        desk_.Leave(holder, let);
        LetGo(holder, let);
        grants_.End(true);
        held_.clear(), asked_.clear(), mine_.clear();
        for (std::string &who : who_) who.clear();
        return;
    }
    const proto::Room &room = session_.room();
    const int me = session_.me();
    if (session_.away()) Ended(grants_.End(false));   // the wearer went: what they gave is taken back at once
    Ended(grants_.Sweep(now, [&](int number, int person) { return room.panel(proto::PanelId(me, number)) && room.person(person) && Who(person) == who_[person]; }));
    desk_.Tick(now, let);
    LetGo(holder, let);
    if (now - told_ >= kTellS) {
        told_ = now;
        for (const room::Granted &g : grants_.given()) Tell(g, grants_.left(now));
    }
    for (auto it = held_.begin(); it != held_.end();) it = it->second <= now || !room.panel(it->first) ? held_.erase(it) : std::next(it);
    for (auto it = asked_.begin(); it != asked_.end();)
        it = now - it->second > room::Grants::kAskS || !room.panel(it->first) ? asked_.erase(it) : std::next(it);
}

void Control::Here(double now) {
    Ended(grants_.Sweep(now, [](int, int) { return true; }));   // what ran out before this word stays ended
    grants_.Here(now);
}

std::string Control::Own(double now) {
    if (!inRoom()) return "";
    const int holder = desk_.holder();
    std::vector<proto::Act> let;
    desk_.Own(session_.me(), now, let);
    LetGo(holder, let);
    return "";
}

std::string Control::Give(int number, int person, double now) {
    if (!inRoom()) return "you aren't in a room";
    const proto::Room &room = session_.room();
    if (number < 0 || number >= proto::kPanelNumbers || !room.panel(proto::PanelId(session_.me(), number))) return "you don't share that panel";
    if (person == session_.me() || !room.person(person)) return "nobody is in that place";
    if (session_.away()) return "you are away";
    Same(person, now);
    if (!grants_.Give(number, person, now)) return "nobody says you have the headset on, and control is given only while you are there";
    Tell({number, person}, grants_.left(now));
    return "";
}

std::string Control::Take(int number, int person) {
    if (!inRoom()) return "you aren't in a room";
    bool any = false;
    for (int p = 0; p < proto::kPeople; ++p)
        if ((person < 0 || p == person) && grants_.Take(number, p)) any = true, Ended({{number, p}});
    return any ? "" : "nobody has that panel or asks for it";
}

std::string Control::Ask(int panel, double now) {
    if (!inRoom()) return "you aren't in a room";
    const proto::Panel *p = panel >= 0 && panel < proto::kPeople * proto::kPanelNumbers ? session_.room().panel(panel) : nullptr;
    if (!p) return "no such panel";
    if (p->owner() == session_.me()) return "that panel is yours";
    if (!mine_.try_emplace(p->owner()).first->second.words.Spend(now)) return "you asked too often";
    proto::AskControl ask;
    ask.panel = uint8_t(panel);
    const std::string whyNot = session_.Tell(p->owner(), proto::Pack(ask));
    if (whyNot.empty()) asked_[ask.panel] = now;
    return whyNot;
}

std::string Control::Act(const proto::Act &act, double now) {
    if (!inRoom()) return "you aren't in a room";
    const proto::Panel *p = session_.room().panel(act.panel);
    if (!p) return "no such panel";
    if (p->owner() == session_.me()) return "that panel is yours";
    if (!Holds(act.panel, now)) return "you have no control of that panel";
    proto::Act read;
    if (!proto::Read(proto::Body(act), read) || (act.what == proto::kActKey && !room::Typed(act.a))) return "that isn't input a panel takes";
    if (!mine_.try_emplace(p->owner()).first->second.acts.Spend(now)) return "too fast";
    return session_.Tell(p->owner(), proto::Pack(act));
}

bool Control::Granted(int number, int person, double now) {
    Same(person, now);
    return grants_.May(number, person, now);
}

bool Control::Holds(uint8_t panel, double now) const {
    const auto held = held_.find(panel);
    return held != held_.end() && held->second > now;
}

std::string Control::State(double now) const {
    std::ostringstream out;
    const auto pairs = [&out](const std::vector<room::Granted> &list) {
        for (size_t i = 0; i < list.size(); ++i) out << (i ? "," : "") << "{\"number\":" << list[i].number << ",\"person\":" << list[i].person << "}";
    };
    out << "{\"there\":" << (grants_.left(now) > 0 ? "true" : "false") << ",\"left\":" << grants_.left(now) << ",\"asks\":[";
    pairs(grants_.asks());
    out << "],\"given\":[";
    pairs(grants_.given());
    out << "],\"holder\":" << desk_.holder() << ",\"held\":[";
    bool first = true;
    for (const auto &[panel, until] : held_) {
        if (until <= now) continue;
        out << (first ? "" : ",") << "{\"panel\":" << int(panel) << ",\"left\":" << until - now << "}";
        first = false;
    }
    out << "],\"asked\":[";
    first = true;
    for (const auto &asked : asked_) out << (first ? "" : ",") << int(asked.first), first = false;
    out << "],\"passed\":" << passed_ << ",\"dropped\":" << dropped_ << "}";
    return out.str();
}

}  // namespace flow
