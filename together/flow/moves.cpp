// Moving shared panels, through ft-together (moves.h).
#include "moves.h"

#include <cstdio>
#include <iterator>

namespace flow {

bool Moves::Said(int link, const std::vector<uint8_t> &body, double now) {
    const int person = session_.PersonOn(link), me = session_.me();
    if (person < 0) return false;
    proto::Move move;
    room::Allowance &told = link_.try_emplace(link, kMovesASecond, kMovesAtOnce).first->second;
    if (!proto::Read(body, move) || !told.Spend(now) || move.panel / proto::kPanelNumbers != me) return false;
    const proto::Room &room = session_.room();
    const int number = move.panel % proto::kPanelNumbers;
    // a late move, after a grant ended or the panel went, is nothing worse than late
    if (!room.panel(move.panel) || !(person == room.owner || granted_(number, person, now))) return ++dropped_, true;
    asked_[number] = {person, move, now};
    return true;
}

void Moves::Tick(double now) {
    if (!inRoom()) {
        asked_.clear(), mine_.clear();
        return;
    }
    const proto::Room &room = session_.room();
    const int me = session_.me();
    for (auto it = asked_.begin(); it != asked_.end();)
        it = now - it->second.when > kKeepS || !room.panel(proto::PanelId(me, it->first)) ? asked_.erase(it) : std::next(it);
}

std::string Moves::Move(const proto::Move &move, double now) {
    if (!inRoom()) return "you aren't in a room";
    const proto::Room &room = session_.room();
    const proto::Panel *p = room.panel(move.panel);
    if (!p) return "no such panel";
    if (p->owner() == session_.me()) return "that panel is yours";   // it moves with its screen here
    if (session_.me() != room.owner && !holds_(move.panel, now)) return kMayNot;
    proto::Move read;
    if (!proto::Read(proto::Body(move), read)) return "that isn't a place in the room";
    if (!mine_.try_emplace(p->owner(), kMineASecond, kMineAtOnce).first->second.Spend(now)) return "too fast";
    return session_.Tell(p->owner(), proto::Pack(read));
}

std::string Moves::Taken(double now) {
    Tick(now);
    std::string out = std::to_string(asked_.size());
    for (const auto &[number, asked] : asked_) {
        char text[160];
        const proto::Move &m = asked.move;
        std::snprintf(text, sizeof text, " %d:%d:%.4f:%.4f:%.4f:%.5f:%.5f:%.5f:%.5f", number, asked.person, double(m.at[0]), double(m.at[1]), double(m.at[2]), double(m.turn[0]),
                      double(m.turn[1]), double(m.turn[2]), double(m.turn[3]));
        out += text;
    }
    asked_.clear();
    return out;
}

}  // namespace flow
