// One person at a time on a Frame's desktop (docs/together.md, "Control and moving"). A
// desktop has one pointer and one keyboard, whichever of its panels they are on (KWin has one
// seat), so of the people who may control a panel one gives it input at a time: whoever acts
// while nobody else does holds the desk, and it frees kHoldS after their last input, or when
// what they hold down is let go. The wearer's own hand takes it from anyone.
//
// The desk also knows what its holder holds down, so that it can be let go for them: when they
// lose the desk, their grant or their link, or hold it for longer than anyone does. A button or
// a key somebody else left down would otherwise stay down on the wearer's desktop.
//
// And it knows which keys someone else may type there: the keys of text and of moving about in
// it. Not the Meta keys, which are the desktop's own, nor the power, sleep and volume keys.
//
// The rules only. Header only.
#pragma once

#include "../proto/control.h"

#include <utility>
#include <vector>

namespace room {

// Whether a guest may press a key (a Linux input key code): Escape to Delete without SysRq.
inline bool Typed(int key) { return key >= 1 && key <= 111 && key != 99; }

class Desk {
public:
    static constexpr int kNobody = -1;
    static constexpr double kHoldS = 1.5;       // the desk frees this long after its holder's last input
    static constexpr double kMostDownS = 60;    // nothing stays down longer
    static constexpr size_t kMostDown = 8;      // buttons and keys one person holds down at once

    // Whether `person`'s input at `now` is passed on: nobody else holds the desk, and it is
    // something they can do (a key that may be typed, a button going down that isn't down, one
    // going up that they hold).
    bool Take(int person, const proto::Act &act, double now) {
        Lapse(now);
        if (holder_ != kNobody && holder_ != person) return false;
        if (act.what == proto::kActKey && !Typed(act.a)) return false;
        if (act.what == proto::kActButton || act.what == proto::kActKey) {
            size_t at = 0;
            while (at < down_.size() && !proto::SameKey(down_[at].first, act)) ++at;
            if (act.b ? at < down_.size() || down_.size() >= kMostDown : at == down_.size()) return false;
            if (act.b) down_.push_back({act, now});
            else down_.erase(down_.begin() + long(at));
        }
        holder_ = person, last_ = now;
        return true;
    }
    // The wearer's own hand is on their desktop: the desk is theirs. `let` gets what someone
    // else held down, as the acts that let it go.
    void Own(int wearer, double now, std::vector<proto::Act> &let) {
        if (holder_ != wearer) LetGo(let);
        holder_ = wearer, last_ = now;
    }
    // Someone may give no more input: what they hold is let go, and the desk is free.
    void Leave(int person, std::vector<proto::Act> &let) {
        if (holder_ != person) return;
        LetGo(let);
        holder_ = kNobody;
    }
    // Now and then, often while someone holds the desk: what was down too long is let go.
    void Tick(double now, std::vector<proto::Act> &let) {
        if (!down_.empty() && now - down_.front().second > kMostDownS) LetGo(let), last_ = now;
        Lapse(now);
    }
    int holder() const { return holder_; }
    size_t down() const { return down_.size(); }

private:
    void Lapse(double now) {
        if (holder_ != kNobody && down_.empty() && now - last_ > kHoldS) holder_ = kNobody;
    }
    void LetGo(std::vector<proto::Act> &let) {
        for (auto it = down_.rbegin(); it != down_.rend(); ++it) {
            proto::Act up = it->first;
            up.b = 0;
            let.push_back(up);
        }
        down_.clear();
    }

    int holder_ = kNobody;
    double last_ = 0;
    std::vector<std::pair<proto::Act, double>> down_;   // what the holder holds down, and since when
};

}  // namespace room
