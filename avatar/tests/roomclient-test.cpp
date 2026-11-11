// A program's end of ft-together's local socket (avatar/room/client.h), against a stand-in for
// ft-together on a socket of the test's own: the room asked about, the others' poses watched,
// the floor laid, the avatar you have on told and the others' asked about, your own pose handed
// over, questions from other programs kept, and an ft-together that goes quiet. The clock is the test's: nothing here waits.
#include "../room/client.h"

#include "../../conf/ftsock.h"
#include "../../together/local/datagrams.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static std::string Name(const char *what) { return "ft_roomclient_test_" + std::to_string(getpid()) + "_" + what; }

// A datagram socket on a name of its own.
struct Sock {
    int fd = -1;
    std::string name;
    explicit Sock(const std::string &n) : name(n) {
        sockaddr_un addr;
        const socklen_t len = ft_sock_addr(n.c_str(), &addr);
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (bind(fd, reinterpret_cast<sockaddr *>(&addr), len) != 0) std::perror("bind"), std::exit(2);
    }
    ~Sock() { close(fd); }
    void To(const std::string &to, const void *data, size_t n) const { ft_sock_send(fd, to.c_str(), data, n, MSG_DONTWAIT); }
    void To(const std::string &to, const std::string &text) const { To(to, text.data(), text.size()); }
    // The next datagram waiting, and who sent it; false when none is.
    bool Next(std::string &out, sockaddr_un *from = nullptr, socklen_t *fromLen = nullptr) const {
        char buf[2048];
        sockaddr_un a;
        socklen_t al = sizeof a;
        const ssize_t n = recvfrom(fd, buf, sizeof buf, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&a), &al);
        if (n < 0) return false;
        out.assign(buf, size_t(n));
        if (from) *from = a, *fromLen = al;
        return true;
    }
};

// What stands in for ft-together: it answers what waits the way its script says.
struct Together {
    Sock sock;
    std::string state = "{\"mode\":\"idle\",\"note\":\"\",\"me\":-1,\"owner\":0,\"people\":[],\"asks\":[],\"codes\":0}";
    std::string floor = "ok none";   // what `floor` answers; one said is taken while this is "ok none"
    std::string looks = "error you aren't in a room";   // what `looks` answers
    std::vector<std::string> worn;   // the `wear` questions, whole
    bool answers = true;
    std::map<std::string, int> asked;   // each question's first word, counted
    std::vector<std::string> said;      // the floors said, whole
    std::vector<std::string> poses;     // the pose datagrams handed over
    sockaddr_un asker{};
    socklen_t askerLen = 0;

    Together() : sock(Name("together")) {}
    void Serve() {
        std::string q;
        while (sock.Next(q, &asker, &askerLen)) {
            if (!q.empty() && uint8_t(q[0]) == proto::kPoseDatagram) {
                poses.push_back(q);
                continue;
            }
            ++asked[q.substr(0, q.find(' '))];
            std::string reply = "error what?";
            if (q == "state") reply = "ok " + state;
            else if (q == "watch" || q == "listen") reply = "ok";
            else if (q == "floor") reply = floor;
            else if (q == "looks") reply = looks;
            else if (q == "wear" || q.rfind("wear ", 0) == 0) worn.push_back(q), reply = q == "wear secret" ? "error that avatar isn't marked as one the others may have" : "ok";
            else if (q.rfind("floor ", 0) == 0) {
                said.push_back(q);
                if (floor == "ok none") floor = "ok " + q.substr(6);
                reply = floor;
            }
            if (answers) sendto(sock.fd, reply.data(), reply.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&asker), askerLen);
        }
    }
    // To whoever asked last, as ft-together sends a watched pose.
    void Pose(int person, const proto::Pose &pose) {
        std::vector<uint8_t> out = {local::kPoseToWatcher, uint8_t(person)};
        const std::vector<uint8_t> bytes = proto::Pack(pose);
        out.insert(out.end(), bytes.begin(), bytes.end());
        sendto(sock.fd, out.data(), out.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&asker), askerLen);
    }
};

static const char *const kRoom =
    "{\"mode\":\"joined\",\"note\":\"\",\"me\":1,\"owner\":0,\"people\":["
    "{\"id\":0,\"name\":\"ada\",\"login\":\"ada@x\",\"away\":0,\"linked\":true,\"spot\":[1,0,2,0.5]},"
    "{\"id\":1,\"name\":\"ben\",\"login\":\"ben@x\",\"away\":0,\"linked\":true,\"spot\":[5,0,6,-1.2]}],\"asks\":[],\"codes\":0}";

static proto::Pose Standing(float x) {
    proto::Pose p;
    p.eyes[0] = x, p.eyes[1] = 1.6f, p.eyeHeight = 1.6f, p.head[0] = 1;
    return p;
}

int main() {
    using roomclient::Client;
    Together together;
    const std::string own = Name("own");
    std::string err;
    {
        Sock taken(own);
        Client c;
        check("a name that is taken isn't opened, and it says so", !c.Open(own, together.sock.name, true, err) && err.find("already running") != std::string::npos);
        check("...nor a name that isn't one", !c.Open("", together.sock.name, true, err) && !c.Open(own, std::string(200, 'x'), true, err));
        c.Tick(0, nullptr);   // closed: nothing happens
    }

    Client c;
    check("it opens on a name of its own", c.Open(own, together.sock.name, true, err) && !c.people().inRoom() && !c.laid());
    double now = 100;
    const auto pass = [&](double dt, const double *head = nullptr) {   // the loop goes round, and ft-together answers
        now += dt;
        c.Tick(now, head);
        together.Serve();
        c.Tick(now, head);
    };

    pass(0);   // (every step is a whole number of sixteenths of a second, so the sums are exact)
    pass(0.125);
    pass(0.125);
    check("outside a room it asks who is there, and nothing else", together.asked["state"] == 1 && together.asked.size() == 1 && !c.people().inRoom());
    pass(0.25);
    check("...every half second", together.asked["state"] == 2);

    together.state = kRoom;
    pass(0.5);
    pass(0.0625);
    check("in a room it knows who is there", c.people().inRoom() && c.people().me() == 1 && c.people().person(0).name == "ada");
    check("...asks to watch them, and asks where its floor lies", together.asked["watch"] == 1 && together.asked["floor"] == 1 && !c.laid());
    pass(0.625);
    check("with no floor laid and no head to lay it by, it asks again", together.asked["floor"] == 2 && together.said.empty() && !c.laid());

    // the wearer stands at (0.4, -1.1) of their own floor facing 2.0; the room's spot for them is (5, 6) facing -1.2
    const double head[3] = {0.4, -1.1, 2.0};
    pass(0.625, head);
    const roomfloor::Floor want = roomfloor::Lay(5, 6, -1.2, 0.4, -1.1, 2.0);
    check("with the head known, it lays the floor so that where they stand is their spot",
          together.said.size() == 1 && c.laid() && std::fabs(c.floor().x - want.x) < 1e-5 && std::fabs(c.floor().z - want.z) < 1e-5 && std::fabs(c.floor().yaw - want.yaw) < 1e-5);
    const int floors = together.asked["floor"], watches = together.asked["watch"], states = together.asked["state"];
    for (int i = 0; i < 32; ++i) pass(0.125, head);   // four seconds
    check("after that it asks about the floor now and then, the room twice a second, and to watch every second",
          together.said.size() == 1 && together.asked["floor"] - floors == 2 && together.asked["watch"] - watches == 4 && together.asked["state"] - states == 8);

    // ft-together's floor is the one that counts
    together.floor = "ok 7.000000 8.000000 0.250000";
    for (int i = 0; i < 17; ++i) pass(0.125, head);
    check("the floor ft-together tells is the one it has, whatever it said itself", c.laid() && c.floor().x == 7 && c.floor().z == 8 && c.floor().yaw == 0.25);
    for (const char *bad : {"ok 1 2", "ok 1 2 3 4", "ok nan 2 3", "ok inf 2 3", "ok one two three", "error you aren't in a room"}) {
        together.floor = bad;
        for (int i = 0; i < 17; ++i) pass(0.125, head);
    }
    check("...and an answer that isn't a floor changes nothing", c.laid() && c.floor().x == 7 && together.said.size() == 1);

    // poses
    check("nobody is drawn before their pose comes", !c.people().Drawn(0, now) && c.posesIn() == 0);
    together.Pose(0, Standing(2.5f));
    pass(0.0625, head);
    check("a watched pose is taken", c.posesIn() == 1 && c.people().Drawn(0, now) && c.people().person(0).pose.eyes[0] == 2.5f);
    together.Pose(3, Standing(1));   // nobody has that place
    const uint8_t rubbish[] = {local::kPoseToWatcher, 0, 1, 2, 3};
    sendto(together.sock.fd, rubbish, sizeof rubbish, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&together.asker), together.askerLen);
    pass(0.0625, head);
    check("...but not one of nobody, nor rubbish", c.posesIn() == 1);
    pass(1.25, head);
    check("someone whose poses stop isn't drawn any more", !c.people().Drawn(0, now));

    // voices
    std::vector<std::pair<int, std::string>> heard;
    const int listens = together.asked["listen"];
    const auto say = [&](int place) {
        const uint8_t voice[] = {local::kVoiceToListener, uint8_t(place), 2, 7, 0, 'o', 'p', 'u', 's'};
        sendto(together.sock.fd, voice, sizeof voice, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&together.asker), together.askerLen);
    };
    say(0);
    pass(0.0625, head);
    check("a program that doesn't listen isn't asked for, and a voice that comes all the same goes nowhere", together.asked["listen"] == listens && heard.empty());
    c.Listen([&heard](int person, const uint8_t *voice, size_t n) { heard.push_back({person, std::string(reinterpret_cast<const char *>(voice), n)}); });
    for (int i = 0; i < 16; ++i) pass(0.125, head);
    check("one that listens asks to every second", together.asked["listen"] - listens == 2);
    say(0);
    say(1);   // your own place
    say(3);   // nobody's
    say(200);
    pass(0.0625, head);
    check("a voice of someone else in the room is handed on as it came, with whose it is; your own and nobody's aren't",
          heard.size() == 1 && heard[0].first == 0 && heard[0].second == std::string("\x02\x07\x00opus", 7));
    check("the socket is there for a loop to sleep on", c.fd() >= 0);

    check("your own pose is handed to ft-together", c.Send(Standing(-0.5f)) && (together.Serve(), together.poses.size() == 1));
    proto::Pose mine;
    check("...as a pose", proto::Read(reinterpret_cast<const uint8_t *>(together.poses[0].data()), together.poses[0].size(), mine) && mine.eyes[0] == -0.5f);

    // avatars
    check("one that watches asks where the others' avatars are, every second", together.asked["looks"] >= 8 && together.asked["looks"] == together.asked["watch"]);
    check("...and until it is told which avatar its wearer has on, it says nothing of it", together.worn.empty() && c.people().person(0).avatar.empty());
    together.looks = "ok {\"wear\":\"\",\"sent\":false,\"why\":\"\",\"people\":[{\"id\":0,\"file\":\"/run/looks/aa.ftmodel\"}]}";
    for (int i = 0; i < 9; ++i) pass(0.125, head);
    check("where someone's avatar is kept is known from the answer", c.people().person(0).avatar == "/run/looks/aa.ftmodel");
    check("...which isn't taken for the room's state: the room is as it was", c.people().inRoom() && c.people().me() == 1 && c.people().person(0).name == "ada" && c.laid());
    together.looks = "ok {\"wear\":\"\",\"sent\":false,\"why\":\"\",\"people\":[{\"id\":0,\"file\":\"\"}]}";
    for (int i = 0; i < 9; ++i) pass(0.125, head);
    check("an avatar taken off is gone", c.people().person(0).avatar.empty() && c.people().inRoom());
    c.Wear("fox");
    pass(0.0625, head);
    check("the avatar the wearer has on is told at once", together.worn == std::vector<std::string>{"wear fox"});
    for (int i = 0; i < 32; ++i) pass(0.125, head);
    check("...and again every other second: ft-together may have started since", together.worn.size() == 3 && together.worn.back() == "wear fox");
    c.Wear("fox");
    pass(0.0625, head);
    check("the same one said again isn't told any sooner", together.worn.size() == 3);
    c.Wear("secret");
    pass(0.0625, head);
    check("another is told at once, and ft-together's no changes nothing here", together.worn.size() == 4 && together.worn.back() == "wear secret" && c.people().inRoom());
    c.Wear("");
    pass(0.0625, head);
    check("none is told as none", together.worn.size() == 5 && together.worn.back() == "wear");

    // other programs
    {
        Sock other(Name("other"));
        roomclient::Question q;
        check("nothing is asked of the program yet", !c.Next(q));
        other.To(own, "state\n");
        std::vector<uint8_t> forged = {local::kPoseToWatcher, 0};
        const std::vector<uint8_t> bytes = proto::Pack(Standing(9));
        forged.insert(forged.end(), bytes.begin(), bytes.end());
        other.To(own, forged.data(), forged.size());
        pass(0.0625, head);
        check("a question from another program is kept for the loop", c.Next(q) && q.text == "state");
        c.Reply(q, "ok here");
        std::string reply;
        check("...and its answer goes back to the asker", other.Next(reply) && reply == "ok here");
        check("a pose that didn't come from ft-together isn't one: it is a question like any other", c.posesIn() == 1 && !c.people().Drawn(0, now) && c.Next(q) && !c.Next(q));
        for (int i = 0; i < 40; ++i) other.To(own, "quit");
        pass(0.0625, head);
        int kept = 0;
        while (c.Next(q)) ++kept;
        check("only so many questions wait at once", kept == int(Client::kMostQuestions));
    }

    // the room is left, and another entered
    together.state = "{\"mode\":\"idle\",\"note\":\"You left the room.\",\"me\":-1,\"owner\":0,\"people\":[],\"asks\":[],\"codes\":0}";
    together.floor = "ok none";
    pass(0.5, head);
    pass(0.0625, head);
    check("out of the room, there is no floor", !c.people().inRoom() && !c.laid());
    const int looked = together.asked["looks"];
    const size_t wore = together.worn.size();
    for (int i = 0; i < 32; ++i) pass(0.125, head);
    check("...nobody's avatar is asked about, and the wearer's own is still told", together.asked["looks"] == looked && together.worn.size() == wore + 2);
    together.state = kRoom;
    pass(0.5, head);
    for (int i = 0; i < 12; ++i) pass(0.125, head);
    check("in the next room the floor is laid afresh", c.laid() && together.said.size() == 2 && std::fabs(c.floor().x - want.x) < 1e-5);

    // an ft-together that goes quiet, and one that is gone
    together.answers = false;
    for (int i = 0; i < 16; ++i) pass(0.125, head);
    check("a room isn't lost over two seconds of silence", c.people().inRoom());
    for (int i = 0; i < 8; ++i) pass(0.125, head);
    check("...but after that nobody is in it", !c.people().inRoom() && !c.laid());
    together.answers = true;
    for (int i = 0; i < 16; ++i) pass(0.125, head);
    check("and when it answers again, the room is back", c.people().inRoom() && c.laid());
    close(together.sock.fd);
    together.sock.fd = -1;
    check("a pose to an ft-together that isn't there doesn't go", !c.Send(Standing(0)));
    for (int i = 0; i < 24; ++i) now += 0.125, c.Tick(now, head);
    check("...and the room is gone", !c.people().inRoom());

    c.Close();
    Client again;
    check("closed, its name is free again", again.Open(own, Name("together"), false, err));
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
