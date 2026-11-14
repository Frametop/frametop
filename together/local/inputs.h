// Other people's input on its way to this Frame's desktop (docs/together.md, "Control and
// moving"). ft-together decides whose input is taken (flow/control.h) and gives none of it to
// the desktop itself: the program that does asks `inputs` on ft-together's socket about once a
// second, and from then on gets each act as a datagram,
//
//   kInputToTaker (0x85), whose it is (their place in the room, a byte), then the act's body
//   (proto/control.h: the panel's id, what it is, a, b)
//
// like a watcher its poses (watchers.h). Nobody is waited for, so an act can be missed by a
// program whose queue is full, and a missed "up" would leave a button down on the wearer's
// desktop. So the first act after one was missed is preceded by proto::kActLetGo from nobody
// (place 255): whatever anyone held down is let go.
//
// One program takes the input at a time: two would each give the desktop every click. Another
// is told no until the first has stopped asking.
#pragma once

#include "../flow/control.h"
#include "watchers.h"

namespace local {

class Inputs : public flow::Inputs {
public:
    static constexpr int kNobody = 255;

    // `sock`: ft-together's local socket, which the acts are sent from.
    explicit Inputs(int sock) : sock_(sock) {}
    // The program at `from` gives the desktop its input from `now` on (Watchers::Add). False
    // while another does.
    bool Add(const sockaddr_un &from, socklen_t len, double now) { return now_ = now, takers_.Add(from, len, now); }
    // The time, for knowing who still asks.
    void At(double now) { now_ = now; }
    void Input(int person, const proto::Act &act) override;
    size_t missed() const { return missed_; }

private:
    const int sock_;
    Watchers takers_{kInputToTaker, 1};
    double now_ = 0;
    bool behind_ = false;   // an act was missed: the next is preceded by a let-go
    size_t missed_ = 0;
};

}  // namespace local
