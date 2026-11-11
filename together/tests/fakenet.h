// A network made in a test (session-test.cpp, video-test.cpp): several Frames' sessions run
// against each other with no sockets and no clock. A link is two queues, and the test hands the
// bytes over when it pumps.
#pragma once

#include "../session/commands.h"

#include <deque>
#include <functional>
#include <memory>
#include <set>

inline uint64_t g_seed = 99;
inline uint64_t roll() { return g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull, g_seed >> 11; }

struct Net;

// One Frame: its session, and its end of every link.
struct Frame : session::Links, session::Tailnet {
    Net &net;
    std::string address, login;
    uint16_t port;
    bool onTailnet = true;
    std::unique_ptr<session::Session> s;
    std::vector<std::string> called;   // every host it called
    std::vector<std::pair<int, proto::Watch>> watches;   // what the room's people said they have of its panels, and on which link
    std::function<void(int link, const proto::Watch &watch, double now)> onWatch;   // ...told here too, when set

    Frame(Net &n, const std::string &address, uint16_t port, const std::string &name);
    int Call(const std::string &host, uint16_t port) override;
    void Send(int link, const std::vector<uint8_t> &message) override;
    void Close(int link) override;
    bool Self(session::Who &who, std::string &at) override { return who.login = login, who.name = "frame-" + login, at = address, onTailnet; }
    bool WhoIs(const std::string &at, session::Who &who) override;
    bool MayCall(const std::string &at) override { return at.rfind("100.", 0) == 0; }

    std::string ask(const std::string &text);
    std::string state() { return session::State(*s); }
    bool has(const std::string &text) { return state().find(text) != std::string::npos; }
    // The number of the first ask waiting for this owner's answer, or 0.
    int asking() {
        const std::string st = state();
        const size_t at = st.find("\"asks\":[{\"id\":");
        return at == std::string::npos ? 0 : std::atoi(st.c_str() + at + 14);
    }
};

// The network between them. A link has a number at each end; what one end does reaches the
// other when the test pumps.
struct Net {
    struct End {
        Frame *frame = nullptr;
        int link = -1;
    };
    struct Event {
        End to;
        enum { kUp, kBytes, kDown } what;
        std::string address;
        std::vector<uint8_t> bytes;
    };
    std::vector<Frame *> frames;
    std::map<std::pair<Frame *, int>, End> far;   // each end's other end
    std::deque<Event> queue;
    std::map<int, std::vector<uint8_t>> heard;    // what was sent on a link whose other end is the test's
    std::set<int> shut;                           // ...and the ones of those that were closed
    std::function<void(Event &)> tamper;          // rewrites what is on its way
    int nextLink = 1;
    double now = 1000;
    size_t piece = 0;   // bytes arrive in pieces this long (0: whole)

    void pump() {
        while (!queue.empty()) {
            Event e = queue.front();
            queue.pop_front();
            if (tamper) tamper(e);
            session::Session &s = *e.to.frame->s;
            if (e.what == Event::kUp) s.Up(e.to.link, e.address, now);
            else if (e.what == Event::kDown) s.Down(e.to.link, now);
            else if (!piece) s.Bytes(e.to.link, e.bytes.data(), e.bytes.size(), now);
            else
                for (size_t at = 0; at < e.bytes.size(); at += piece) s.Bytes(e.to.link, e.bytes.data() + at, std::min(piece, e.bytes.size() - at), now);
        }
    }
    void wait(double seconds) {
        for (double end = now + seconds; now < end;) {
            now += 1;
            for (Frame *f : frames) f->s->Tick(now);
            pump();
        }
    }
    // The link between two Frames breaks under both.
    void cut(Frame &a, Frame &b) {
        for (auto it = far.begin(); it != far.end();) {
            if (it->first.first != &a || it->second.frame != &b) {
                ++it;
                continue;
            }
            queue.push_back({{&a, it->first.second}, Event::kDown, "", {}});
            queue.push_back({it->second, Event::kDown, "", {}});
            far.erase({it->second.frame, it->second.link});
            it = far.erase(it);
        }
        pump();
    }
    // A new link to `to` whose other end is the test's: what comes in on it is the test's to write.
    End stranger(Frame &to, const std::string &from) {
        const End theirs = {&to, nextLink++};
        queue.push_back({theirs, Event::kUp, from, {}});
        pump();
        return theirs;
    }
    void say(const End &to, const std::vector<uint8_t> &bytes) {
        queue.push_back({to, Event::kBytes, "", bytes});
        pump();
    }
    // Why the test's end of a link was refused, or 0 when it wasn't.
    int refused(const End &theirs) {
        const std::vector<uint8_t> &bytes = heard[theirs.link];
        proto::Parser parser;
        parser.Feed(bytes.data(), bytes.size());
        uint16_t kind = 0;
        std::vector<uint8_t> body;
        proto::Refuse r;
        while (parser.Next(kind, body))
            if (kind == proto::kRefuse && proto::Read(body, r)) return int(r.why);
        return 0;
    }
};

inline Frame::Frame(Net &n, const std::string &at, uint16_t p, const std::string &name) : net(n), address(at), login(name + "@example.com"), port(p) {
    session::Config c;
    c.name = name, c.port = p, c.dice = roll;
    c.watched = [this](int link, const proto::Watch &w, double now) {
        watches.push_back({link, w});
        if (onWatch) onWatch(link, w, now);
    };
    s = std::make_unique<session::Session>(*this, *this, c);
    net.frames.push_back(this);
}

inline int Frame::Call(const std::string &host, uint16_t p) {
    called.push_back(host);
    const int mine = net.nextLink++;
    Frame *callee = nullptr;
    for (Frame *f : net.frames)
        if ((f->address == host || "frame-" + f->login == host) && f->port == p) callee = f;
    if (!callee) return net.queue.push_back({{this, mine}, Net::Event::kDown, "", {}}), mine;
    const int theirs = net.nextLink++;
    net.far[{this, mine}] = {callee, theirs};
    net.far[{callee, theirs}] = {this, mine};
    net.queue.push_back({{callee, theirs}, Net::Event::kUp, address, {}});
    net.queue.push_back({{this, mine}, Net::Event::kUp, callee->address, {}});
    return mine;
}

inline void Frame::Send(int link, const std::vector<uint8_t> &message) {
    const auto it = net.far.find({this, link});
    if (it != net.far.end()) net.queue.push_back({it->second, Net::Event::kBytes, "", message});
    else net.heard[link].insert(net.heard[link].end(), message.begin(), message.end());
}

inline void Frame::Close(int link) {
    const auto it = net.far.find({this, link});
    if (it == net.far.end()) return void(net.shut.insert(link));
    net.queue.push_back({it->second, Net::Event::kDown, "", {}});
    net.far.erase({it->second.frame, it->second.link});
    net.far.erase({this, link});
}

inline bool Frame::WhoIs(const std::string &at, session::Who &who) {
    for (Frame *f : net.frames)
        if (f->address == at && f->onTailnet) return who.login = f->login, who.name = "frame-" + f->login, true;
    return false;
}

inline std::string Frame::ask(const std::string &text) {
    const std::string reply = session::Command(*s, text, net.now);
    net.pump();
    return reply;
}

inline std::string where(const Frame &f) { return f.address + " " + std::to_string(f.port); }

// `guest` joins `owner`'s room with a fresh invite and is let in.
inline bool join(Frame &owner, Frame &guest) {
    const std::string invite = owner.ask("invite");
    if (invite.rfind("ok ", 0) != 0 || guest.ask("join " + where(owner) + " " + invite.substr(3)) != "ok") return false;
    return owner.ask("accept " + std::to_string(owner.asking())) == "ok" && guest.s->mode() == session::Mode::kJoined;
}
