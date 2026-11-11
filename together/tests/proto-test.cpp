// Frametop Together's bytes (together/proto): text a peer may show us, the control stream's
// messages there and back and cut short at every length, the parser on a stream that comes in
// pieces, the pose datagram, and all of them on noise. No sockets.
#include "../proto/messages.h"
#include "../proto/posewire.h"

#include <cstdio>
#include <limits>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static uint32_t g_seed = 12345;
static uint32_t roll() { return g_seed = g_seed * 1664525u + 1013904223u, g_seed >> 8; }

// A framed message's body.
static std::vector<uint8_t> body(const std::vector<uint8_t> &framed) { return {framed.begin() + 6, framed.end()}; }

static proto::Room room3() {
    proto::Room r;
    for (size_t i = 0; i < proto::kRoomId; ++i) r.room[i] = uint8_t(i * 7 + 1);
    r.turn = 41, r.owner = 0;
    const char *names[3] = {"Daenon", "Zoë \"Z\"", "ゲスト"};
    for (int i = 0; i < 3; ++i) {
        proto::Person p;
        p.id = uint8_t(i == 2 ? 3 : i), p.name = names[i], p.login = std::string("user") + char('a' + i) + "@example.com";
        p.address = "100.64.0." + std::to_string(i + 1), p.port = uint16_t(47193 + i), p.away = uint8_t(i == 1 ? proto::kAwayGame : 0);
        p.spot.at[0] = 0.9f * float(i), p.spot.at[2] = -0.25f, p.spot.yaw = 0.5f;
        r.people.push_back(p);
    }
    return r;
}

static bool same(const proto::Room &a, const proto::Room &b) {
    bool ok = !std::memcmp(a.room, b.room, proto::kRoomId) && a.turn == b.turn && a.owner == b.owner && a.people.size() == b.people.size();
    for (size_t i = 0; ok && i < a.people.size(); ++i) {
        const proto::Person &p = a.people[i], &q = b.people[i];
        ok = p.id == q.id && p.name == q.name && p.login == q.login && p.address == q.address && p.port == q.port && p.away == q.away &&
             !std::memcmp(p.spot.at, q.spot.at, sizeof p.spot.at) && p.spot.yaw == q.spot.yaw;
    }
    return ok;
}

static void texts() {
    bool ok = wire::IsText("") && wire::IsText("Daenon's Frame") && wire::IsText("Zoë ゲスト 🙂") && wire::IsText("a\"b\\c");
    check("names in any script are text", ok);
    ok = true;
    for (const char *bad : {"a\nb", "tab\there", "\x1b[31mred", "del\x7f", "\xc2\x85", "\xe2\x80\xae" "evil", "\xe2\x81\xa6x", "\xe2\x80\xa8"})
        ok = ok && !wire::IsText(bad);
    check("control characters and the ones that turn text around are not", ok && !wire::IsText(std::string("nul\0here", 8)));
    ok = true;
    for (const char *bad : {"\xff", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf8\x88\x80\x80\x80", "cut \xe3\x82", "\x80"}) ok = ok && !wire::IsText(bad);
    check("nor is anything that isn't well-formed UTF-8", ok);
    ok = wire::Clean("line\none\x1b", 99) == "lineone" && wire::Clean("ゲスト", 7) == "ゲス" && wire::Clean("\xff\xfe", 9).empty() && wire::Clean("abc", 0).empty();
    check("our own text is cleaned and cut at a character's boundary before it is sent", ok);
    for (int i = 0; i < 20000; ++i) {
        std::string s;
        for (uint32_t n = roll() % 12; n > 0; --n) s += char(roll());
        const std::string c = wire::Clean(s, 8);
        ok = ok && wire::IsText(c) && c.size() <= 8;
    }
    check("...whatever it was", ok);
}

static void messages() {
    proto::Hello h, h2;
    h.code = "ABCD2345", h.name = "Daenon", h.port = 47193;
    h.room[3] = 9;
    bool ok = proto::Read(body(proto::Pack(h)), h2) && h2.version == proto::kVersion && h2.code == h.code && h2.name == h.name && h2.port == h.port && h2.room[3] == 9;
    check("a hello comes back as it went", ok);
    h.version = 7;
    std::vector<uint8_t> other = body(proto::Pack(h));
    other.resize(2);   // a later version's hello may be anything after its version
    check("a hello of another version is read as far as its version", proto::Read(other, h2) && h2.version == 7);

    const proto::Room r = room3();
    proto::Room r2;
    check("a room comes back as it went, names in any script", proto::Read(body(proto::Pack(r)), r2) && same(r, r2));
    proto::Welcome w, w2;
    w.you = 3, w.room = r;
    check("a welcome too", proto::Read(body(proto::Pack(w)), w2) && w2.you == 3 && same(r, w2.room));
    w.you = 2;
    ok = !proto::Read(body(proto::Pack(w)), w2);
    w.you = 0;
    check("a welcome to someone who isn't in its room, or to its owner, is refused", ok && !proto::Read(body(proto::Pack(w)), w2));

    proto::Refuse f, f2;
    f.why = proto::Why::kCode, f.text = "That invite code isn't good.";
    ok = proto::Read(body(proto::Pack(f)), f2) && f2.why == proto::Why::kCode && f2.text == f.text;
    std::vector<uint8_t> longer = body(proto::Pack(f));
    longer.push_back(1), longer[0] = 99;
    check("a refusal comes back, and one a later version wrote is still a refusal", ok && proto::Read(longer, f2) && f2.why == proto::Why::kBad && f2.text == f.text);
    proto::Away a, a2;
    a.away = proto::kAwayOff;
    ok = proto::Read(body(proto::Pack(a)), a2) && a2.away == proto::kAwayOff;
    a.away = 8;
    check("away comes back, and away in a way nobody can be is refused", ok && !proto::Read(body(proto::Pack(a)), a2));

    // what a room may not say
    const auto refused = [&](const char *what, void (*spoil)(proto::Room &)) {
        proto::Room bad = room3();
        spoil(bad);
        check(what, !proto::Read(body(proto::Pack(bad)), r2));
    };
    refused("a room with nobody in it is refused", [](proto::Room &x) { x.people.clear(); });
    refused("...with more people than a room holds", [](proto::Room &x) { x.people.resize(5); for (size_t i = 0; i < 5; ++i) x.people[i].id = uint8_t(i); });
    refused("...with two people in one place", [](proto::Room &x) { x.people[1].id = 0; });
    refused("...with someone in a place there isn't", [](proto::Room &x) { x.people[1].id = 4; });
    refused("...whose owner isn't in it", [](proto::Room &x) { x.owner = 2; });
    refused("...with someone standing a mile away", [](proto::Room &x) { x.people[1].spot.at[0] = 1609; });
    refused("...or nowhere", [](proto::Room &x) { x.people[1].spot.yaw = std::numeric_limits<float>::quiet_NaN(); });
    refused("...or at infinity", [](proto::Room &x) { x.people[2].spot.at[1] = -std::numeric_limits<float>::infinity(); });
    // text is cleaned on the way out, so a name that isn't text has to be written by hand
    std::vector<uint8_t> raw = body(proto::Pack(r));
    const size_t name = proto::kRoomId + 4 + 1 + 1 + 1 + 1;   // the first person's name, after its length
    raw[name] = '\n';
    check("...with a name that isn't text", !proto::Read(raw, r2));

    // cut short at every length, and with more after it
    w.you = 3;
    h.version = proto::kVersion;
    const std::vector<uint8_t> whole[4] = {body(proto::Pack(r)), body(proto::Pack(w)), body(proto::Pack(h)), body(proto::Pack(proto::Away{}))};
    ok = true;
    for (int which = 0; which < 4; ++which) {
        const auto reads = [&](const std::vector<uint8_t> &b) {
            return which == 0 ? proto::Read(b, r2) : which == 1 ? proto::Read(b, w2) : which == 2 ? proto::Read(b, h2) && h2.version == proto::kVersion : proto::Read(b, a2);
        };
        ok = ok && reads(whole[which]);
        for (size_t n = 0; n < whole[which].size(); ++n) ok = ok && !reads(std::vector<uint8_t>(whole[which].begin(), whole[which].begin() + long(n)));
        std::vector<uint8_t> more = whole[which];
        more.push_back(0);
        ok = ok && !reads(more);
    }
    check("a message cut short anywhere, or with more after it, is refused", ok);

    // noise
    ok = true;
    for (int i = 0; i < 60000; ++i) {
        std::vector<uint8_t> noise(roll() % 200);
        for (uint8_t &b : noise) b = uint8_t(roll());
        if (i % 3 == 0 && noise.size() > 24) noise[proto::kRoomId + 5] = uint8_t(1 + roll() % 4);   // a count that could be one
        if (proto::Read(noise, r2)) ok = ok && r2.people.size() >= 1 && r2.people.size() <= size_t(proto::kPeople) && r2.person(r2.owner);
        if (proto::Read(noise, w2)) ok = ok && w2.room.person(w2.you);
        if (proto::Read(noise, h2) && h2.version == proto::kVersion) ok = ok && wire::IsText(h2.name) && h2.name.size() <= proto::kMostName;
        proto::Read(noise, f2), proto::Read(noise, a2);
    }
    check("noise is refused or read as something within every bound", ok);
}

static void parser() {
    const proto::Room r = room3();
    proto::Hello h;
    h.name = "x";
    std::vector<uint8_t> stream;
    for (const std::vector<uint8_t> &m : {proto::Pack(h), proto::Pack(r), proto::PackLeave(), proto::Pack(r)}) stream.insert(stream.end(), m.begin(), m.end());
    bool ok = true;
    for (size_t piece : {size_t(1), size_t(5), size_t(7), size_t(64), stream.size()}) {
        proto::Parser p;
        std::vector<uint16_t> kinds;
        uint16_t kind;
        std::vector<uint8_t> b;
        proto::Room got;
        for (size_t at = 0; at < stream.size(); at += piece) {
            ok = ok && p.Feed(stream.data() + at, std::min(piece, stream.size() - at));
            while (p.Next(kind, b)) {
                kinds.push_back(kind);
                if (kind == proto::kRoom) ok = ok && proto::Read(b, got) && same(got, r);
                if (kind == proto::kLeave) ok = ok && b.empty();
            }
        }
        ok = ok && kinds == std::vector<uint16_t>{proto::kHello, proto::kRoom, proto::kLeave, proto::kRoom} && !p.Next(kind, b);
    }
    check("a stream in pieces of any size comes out as its messages, in order", ok);

    proto::Parser p;
    wire::Out big;
    big.U32(uint32_t(proto::kMostMessage + 1)), big.U16(proto::kRoom);
    uint16_t kind;
    std::vector<uint8_t> b;
    ok = p.Feed(proto::PackLeave().data(), 6) && !p.Feed(big.b.data(), big.b.size()) && !p.Next(kind, b) && !p.Feed(stream.data(), 1);
    check("a message longer than any may be breaks the stream for good, before its body is waited for", ok);

    // a long stream doesn't pile up
    proto::Parser l;
    const std::vector<uint8_t> one = proto::Pack(r);
    size_t n = 0;
    ok = true;
    for (int i = 0; i < 5000; ++i) {
        ok = ok && l.Feed(one.data(), one.size());
        while (l.Next(kind, b)) ++n;
    }
    check("five thousand messages in, five thousand out", ok && n == 5000);
    ok = true;
    for (int i = 0; i < 3000; ++i) {
        proto::Parser f;
        for (int k = 0; k < 6; ++k) {
            std::vector<uint8_t> noise(4 + roll() % 40);
            for (uint8_t &x : noise) x = uint8_t(roll());
            if (k == 0 && i % 2) noise[2] = noise[3] = 0;   // a length that could be one
            if (!f.Feed(noise.data(), noise.size())) break;
            while (f.Next(kind, b)) ok = ok && b.size() <= proto::kMostMessage;
        }
    }
    check("noise never gets a body past the limit out of it", ok);
}

static void poses() {
    proto::Pose p, q;
    p.turn = 65535, p.eyes[0] = 0.3f, p.eyes[1] = 1.62f, p.eyes[2] = -2.5f;
    const float turn[4] = {0.7071068f, 0, 0.7071068f, 0};
    std::memcpy(p.head, turn, sizeof turn);
    p.eyeHeight = 1.71f;
    p.mouth[0] = 255, p.mouth[4] = 17;
    proto::HandPose &right = p.hand[1];
    right.tracked = right.fingers = right.hasElbow = true;
    right.wrist[0] = 0.4f, right.wrist[1] = 1.1f, right.wrist[2] = -2.8f, right.elbow[1] = 1.0f;
    std::memcpy(right.turn, turn, sizeof turn);
    for (int f = 0; f < proto::kFingerTurns; ++f) right.finger[f][0] = std::cos(0.05f * float(f)), right.finger[f][1] = std::sin(0.05f * float(f));
    const std::vector<uint8_t> sent = proto::Pack(p);
    bool ok = proto::Read(sent.data(), sent.size(), q) && q.turn == 65535 && !q.hand[0].tracked && q.hand[1].tracked && q.hand[1].fingers && q.hand[1].hasElbow;
    ok = ok && std::fabs(q.eyes[2] + 2.5f) < 1e-6 && std::fabs(q.eyeHeight - 1.71f) < 1e-3 && q.mouth[0] == 255 && q.mouth[4] == 17 && q.hand[1].elbow[1] == 1.0f;
    for (int i = 0; i < 4; ++i) ok = ok && std::fabs(q.head[i] - turn[i]) < 1e-4 && std::fabs(q.hand[1].turn[i] - turn[i]) < 1e-4;
    for (int f = 0; f < proto::kFingerTurns; ++f) ok = ok && std::fabs(q.hand[1].finger[f][0] - right.finger[f][0]) < 0.01 && std::fabs(q.hand[1].finger[f][1] - right.finger[f][1]) < 0.01;
    check("a pose comes back as it went: the head, a hand with its fingers and elbow, the mouth", ok);
    p.hand[0] = right;
    check("a whole pose fits a datagram four times over", proto::Pack(p).size() == 215 && sent.size() == 123);
    proto::Pose bare;
    const std::vector<uint8_t> small = proto::Pack(bare);
    check("with no hands tracked it is 31 bytes", small.size() == 31 && proto::Read(small.data(), small.size(), q) && !q.hand[0].tracked && !q.hand[1].tracked);
    check("a newer pose is told from an older one across the count's wrap", proto::Newer(3, 65530) && proto::Newer(101, 100) && !proto::Newer(100, 100) && !proto::Newer(65530, 3));

    const auto refused = [&](const char *what, void (*spoil)(proto::Pose &)) {
        proto::Pose bad = p;
        spoil(bad);
        const std::vector<uint8_t> b = proto::Pack(bad);
        check(what, !proto::Read(b.data(), b.size(), q));
    };
    refused("a head a mile away is refused", [](proto::Pose &x) { x.eyes[0] = 1609; });
    refused("...a hand at no place", [](proto::Pose &x) { x.hand[1].wrist[2] = std::numeric_limits<float>::quiet_NaN(); });
    refused("...a turn that isn't one", [](proto::Pose &x) { x.head[0] = x.head[2] = 0; });
    refused("...a finger's too", [](proto::Pose &x) { x.hand[0].finger[14][0] = x.hand[0].finger[14][1] = 0; });
    refused("...and a giant", [](proto::Pose &x) { x.eyeHeight = 2.9f; });
    ok = true;
    for (size_t n = 0; n < sent.size(); ++n) ok = ok && !proto::Read(sent.data(), n, q);
    std::vector<uint8_t> more = sent;
    more.push_back(0);
    ok = ok && !proto::Read(more.data(), more.size(), q);
    more = sent, more[0] = 2;
    ok = ok && !proto::Read(more.data(), more.size(), q);
    more = small, more[25] = 0x02;   // fingers of a hand that isn't there
    check("a pose cut short, with more after it, of another kind, or with fingers and no hand is refused", ok && !proto::Read(more.data(), more.size(), q));
    ok = true;
    for (int i = 0; i < 200000; ++i) {
        std::vector<uint8_t> noise = i % 2 ? proto::Pack(p) : std::vector<uint8_t>(31 + roll() % 190);
        for (uint32_t k = i % 2 ? 1 + roll() % 3 : uint32_t(noise.size()); k > 0; --k) noise[roll() % noise.size()] = uint8_t(roll());
        if (i % 2 == 0) noise[0] = proto::kPoseDatagram;
        if (!proto::Read(noise.data(), noise.size(), q)) continue;
        ok = ok && q.eyeHeight >= 0.5f && q.eyeHeight <= 2.5f;
        for (int k = 0; k < 3; ++k) ok = ok && std::fabs(q.eyes[k]) <= proto::kFar && std::fabs(q.hand[0].wrist[k]) <= proto::kFar && std::fabs(q.hand[1].elbow[k]) <= proto::kFar;
        double norm = 0;
        for (float v : q.head) norm += double(v) * v;
        ok = ok && std::fabs(norm - 1) < 1e-3;
    }
    check("noise and damaged poses are refused or read as a pose within every bound", ok);
}

int main() {
    texts();
    messages();
    parser();
    poses();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
