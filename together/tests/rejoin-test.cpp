// A headset that comes into a room again while the owner still holds its old link
// (together/session/wearer.cpp, Answer): the owner hears of a lost link up to ten seconds after
// the guest does, and a guest let in within that time must not be in the room twice. Sessions
// run against each other on a network made in the test (fakenet.h). No sockets, no clock.
#include "fakenet.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static bool idle(Frame &f) { return f.s->mode() == session::Mode::kIdle; }

// How often the room lists a headset.
static int listed(const proto::Room &room, const Frame &f) {
    int n = 0;
    for (const proto::Person &p : room.people) n += p.address == f.address && p.port == f.port;
    return n;
}

static bool sameRoom(Frame &a, Frame &b) { return proto::Pack(a.s->room()) == proto::Pack(b.s->room()); }

int main() {
    Net net;
    Frame ann(net, "100.64.0.1", 47193, "Ann"), bo(net, "100.64.0.2", 47193, "Bo"), cy(net, "100.64.0.3", 47193, "Cy");
    bool ok = ann.ask("open") == "ok" && join(ann, bo) && join(ann, cy) && bo.s->me() == 1 && cy.s->me() == 2;
    check("two guests are in, with a link between them", ok && bo.s->Linked(2) && cy.s->Linked(1) && ann.s->MemberLinks().size() == 2);

    net.lose(bo, ann);
    ok = idle(bo) && bo.has("the link to its owner was lost") && listed(ann.s->room(), bo) == 1 && ann.s->MemberLinks().size() == 2 && !cy.s->Linked(1);
    check("a guest loses the link to the owner: the owner hasn't heard yet, and still lists them", ok);

    ok = bo.ask("join " + where(ann) + " " + ann.ask("invite").substr(3)) == "ok" && ann.ask("decline " + std::to_string(ann.asking())) == "ok";
    check("asking again and being told no leaves the room as it was", ok && idle(bo) && listed(ann.s->room(), bo) == 1 && ann.s->room().people.size() == 3);

    const uint32_t turn = ann.s->room().turn;
    const int old = ann.s->LinkTo(1);
    ok = join(ann, bo) && listed(ann.s->room(), bo) == 1 && ann.s->room().people.size() == 3 && bo.s->me() == 1;
    check("let in again, they are in the room once, in the place they had", ok);
    check("...the others were told they left and then that they came", ok && ann.s->room().turn == turn + 2 && sameRoom(ann, cy) && sameRoom(ann, bo));
    check("...the owner closed the old link, and has one to each guest", net.shut.count(old) && ann.s->MemberLinks().size() == 2 && ann.s->LinkTo(1) >= 0 && ann.s->LinkTo(1) != old);
    check("...and the guests have a link again", bo.s->Linked(2) && cy.s->Linked(1) && bo.s->MemberLinks().size() == 2 && cy.s->MemberLinks().size() == 2);
    ok = cy.ask("away off") == "ok" && bo.s->room().person(2)->away == proto::kAwayOff && bo.ask("away game") == "ok" && cy.s->room().person(1)->away == proto::kAwayGame;
    check("...and the room goes on for all three", ok);

    // an ask the guest gave up, which the owner hasn't heard the end of, and their next one
    const std::string first = ann.ask("invite").substr(3), second = ann.ask("invite").substr(3);
    ok = cy.ask("leave") == "ok" && ann.s->room().people.size() == 2 && cy.ask("join " + where(ann) + " " + first) == "ok" && ann.asking();
    const int older = ann.asking();
    net.lose(cy, ann);
    ok = ok && idle(cy) && cy.ask("join " + where(ann) + " " + second) == "ok" && ann.s->asks().size() == 2;
    check("a guest asks, loses the link and asks again: the owner has two asks from them", ok);
    ok = ann.ask("accept " + std::to_string(older)) == "ok" && listed(ann.s->room(), cy) == 1 && cy.s->mode() == session::Mode::kCalling;
    for (const session::Ask &a : ann.s->asks()) ok = ok && ann.ask("accept " + std::to_string(a.id)) == "ok";
    check("both let in, they are in the room once and hold it", ok && listed(ann.s->room(), cy) == 1 && ann.s->room().people.size() == 3 && cy.s->mode() == session::Mode::kJoined);
    check("...with one link to each guest at the owner, and the room the same for all", ann.s->MemberLinks().size() == 2 && sameRoom(ann, cy) && sameRoom(ann, bo) && bo.s->Linked(cy.s->me()) && cy.s->Linked(1));

    // a second program on the same headset is another person (the test rigs run two there)
    Frame two(net, "100.64.0.2", 47194, "Bo too");
    ok = join(ann, two) && ann.s->room().people.size() == 4 && listed(ann.s->room(), bo) == 1 && listed(ann.s->room(), two) == 1 && bo.s->mode() == session::Mode::kJoined;
    check("another port at the same address is someone else, and takes nobody's place", ok && bo.s->Linked(3) && two.s->Linked(1) && two.s->Linked(2));

    ok = bo.ask("leave") == "ok" && idle(bo) && ann.s->room().people.size() == 3 && join(ann, bo) && ann.s->room().people.size() == 4 && listed(ann.s->room(), bo) == 1;
    check("someone who left and comes back is let in as anyone is", ok && sameRoom(ann, cy) && sameRoom(ann, two));

    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
