// Who may control which of this Frame's shared panels (docs/together.md, "Control and moving",
// "Trust"). The Frame whose panels they are keeps this, and takes input for a panel only from
// someone it has a grant for.
//
// A grant is a lease. It is the wearer's to give, panel by panel and person by person, and it
// stands only while the wearer is known to be there: whoever knows says so every few seconds
// (ft-roomaway, with the headset on their head), and kLeaseS after the last word every grant is
// gone and has to be given again. Nobody controls the desktop of a headset that lies on a desk.
//
// Someone who would like a panel asks. An ask waits for the wearer's answer, and is forgotten
// when none comes.
//
// The rules only. Header only.
#pragma once

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace room {

// One of this Frame's panels, by its number among them, and someone in the room.
struct Granted {
    int number = 0, person = 0;
};

class Grants {
public:
    static constexpr double kLeaseS = 10;   // a grant stands this long after the wearer was last known to be there
    static constexpr double kAskS = 120;    // an ask waits this long for an answer

    // The wearer is there, as far as anyone can tell.
    void Here(double now) { here_ = now; }
    // How long the grants stand yet, in seconds: 0 when nobody says the wearer is there.
    double left(double now) const { return now - here_ < kLeaseS ? kLeaseS - (now - here_) : 0; }

    // Someone asks for a panel. False when it is theirs to control already.
    bool Ask(int number, int person, double now) {
        if (given_.count({number, person})) return false;
        asks_[{number, person}] = now;
        return true;
    }
    // The wearer gives it, asked or not. False while nobody says the wearer is there.
    bool Give(int number, int person, double now) {
        if (left(now) <= 0) return false;
        asks_.erase({number, person});
        given_.insert({number, person});
        return true;
    }
    // The wearer takes it back, or says no to an ask. False when there was neither.
    bool Take(int number, int person) { return asks_.erase({number, person}) + given_.erase({number, person}) > 0; }
    bool May(int number, int person, double now) const { return left(now) > 0 && given_.count({number, person}); }
    bool any(int person) const {
        for (const auto &g : given_)
            if (g.second == person) return true;
        return false;
    }

    // What no longer stands goes: every grant when the lease ran out, an ask that waited too
    // long, and whatever `keep(number, person)` has no panel or no person for any more. The
    // grants that ended come back, for their holders to be told.
    template <class Keep>
    std::vector<Granted> Sweep(double now, Keep keep) {
        std::vector<Granted> ended;
        for (auto it = given_.begin(); it != given_.end();)
            if (left(now) <= 0 || !keep(it->first, it->second)) ended.push_back({it->first, it->second}), it = given_.erase(it);
            else ++it;
        for (auto it = asks_.begin(); it != asks_.end();)
            if (now - it->second > kAskS || !keep(it->first.first, it->first.second)) it = asks_.erase(it);
            else ++it;
        return ended;
    }
    // The wearer went away, or the room was left: every grant ends (and with `asks`, the asks).
    std::vector<Granted> End(bool asks) {
        std::vector<Granted> ended;
        for (const auto &g : given_) ended.push_back({g.first, g.second});
        given_.clear();
        if (asks) asks_.clear();
        return ended;
    }

    std::vector<Granted> given() const {
        std::vector<Granted> out;
        for (const auto &g : given_) out.push_back({g.first, g.second});
        return out;
    }
    std::vector<Granted> asks() const {
        std::vector<Granted> out;
        for (const auto &a : asks_) out.push_back({a.first.first, a.first.second});
        return out;
    }

private:
    double here_ = -1e9;
    std::set<std::pair<int, int>> given_;            // (number, person)
    std::map<std::pair<int, int>, double> asks_;     // ...and when they asked
};

}  // namespace room
