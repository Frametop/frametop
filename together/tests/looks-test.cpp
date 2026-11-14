// The avatars the room's people have on (together/flow/looks.h): several Frames' sessions and
// their looks' rules run against each other on a network made in the test (fakenet.h). Whose
// file is asked for and of whom, what is sent and to whom, what is kept, and what isn't: a file
// nobody asked for, one that isn't what the room said, a flood of asks. No sockets, no clock,
// no files.
#include "../flow/looks.h"
#include "fakenet.h"

#include <cstdio>
#include <cstring>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static flow::FileBytes file(uint8_t seed, size_t n) {
    std::vector<uint8_t> bytes(n);
    for (size_t i = 0; i < n; ++i) bytes[i] = uint8_t(seed + i * 31 + (i >> 8));
    return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}
static proto::Look look(const flow::FileBytes &f) { return flow::LookOf(f->data(), f->size()); }

struct Rig;
static std::map<Frame *, Rig *> g_rigs;

// One Frame's avatars: what its wearer has on, the files it keeps, and its end of the links.
struct Rig : flow::FileWires, flow::Closet {
    Frame &f;
    flow::Looks looks;
    flow::FileBytes worn;                           // the file on this Frame's disk, as the wearer has it on
    std::vector<std::pair<proto::Look, std::vector<uint8_t>>> kept;
    std::map<int, size_t> awaited;
    size_t sent = 0;
    std::function<flow::FileBytes(flow::FileBytes)> swap;   // what goes out instead, when set

    explicit Rig(Frame &frame) : f(frame), looks(*frame.s, *this, *this) {
        g_rigs[&f] = this;
        f.onWant = [this](int link, const proto::Want &want, double now) { looks.Wanted(link, want, now); };
    }
    void Await(int link, size_t bytes) override { awaited[link] = bytes; }
    // As the links have it: the far end takes one file, of no more than it awaits.
    bool File(int link, const flow::FileBytes &file) override {
        ++sent;
        const flow::FileBytes out = swap ? swap(file) : file;
        const auto far = f.net.far.find({&f, link});
        if (far == f.net.far.end() || !out) return true;
        Rig &to = *g_rigs.at(far->second.frame);
        size_t &most = to.awaited[far->second.link];
        if (out->size() <= most) most = 0, to.looks.File(far->second.link, *out, f.net.now);
        return true;
    }
    flow::FileBytes Worn(const proto::Look &l) override { return worn && proto::SameLook(look(worn), l) ? worn : nullptr; }
    bool Has(const proto::Look &l) const override {
        for (const auto &k : kept)
            if (proto::SameLook(k.first, l)) return true;
        return false;
    }
    void Keep(const proto::Look &l, const std::vector<uint8_t> &bytes) override { kept.push_back({l, bytes}); }
    void Only(const std::vector<proto::Look> &still) override {
        for (size_t i = kept.size(); i-- > 0;) {
            bool worn = false;
            for (const proto::Look &l : still) worn = worn || proto::SameLook(l, kept[i].first);
            if (!worn) kept.erase(kept.begin() + long(i));
        }
    }
    // The wearer puts a file on (none: takes it off).
    void wear(const flow::FileBytes &file) {
        worn = file;
        f.s->Wear(file ? look(file) : proto::Look{}, f.net.now);
        f.net.pump();
    }
    bool has(const flow::FileBytes &file) const {
        for (const auto &k : kept)
            if (proto::SameLook(k.first, look(file)) && k.second == *file) return true;
        return false;
    }
};

// Time passes: every Frame's session and looks tick each second.
static void wait(Net &net, double seconds) {
    for (double end = net.now + seconds; net.now < end;) {
        net.now += 1;
        for (Frame *f : net.frames) f->s->Tick(net.now), g_rigs.at(f)->looks.Tick(net.now);
        net.pump();
    }
}

static void hashes() {
    const uint8_t abc[] = {'a', 'b', 'c'};
    const uint8_t want[4] = {0xba, 0x78, 0x16, 0xbf};
    const proto::Look l = flow::LookOf(abc, 3);
    check("a file's look is its size and SHA-256", l.size == 3 && !std::memcmp(l.hash, want, 4) && l.hash[31] == 0xad);
    const std::vector<uint8_t> big(proto::kMostLook + 1);
    check("no bytes, or more than an avatar may be, is no look", flow::LookOf(abc, 0).none() && flow::LookOf(nullptr, 3).none() && flow::LookOf(big.data(), big.size()).none() && !flow::LookOf(big.data(), proto::kMostLook).none());
}

static void sharing() {
    Net net;
    Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo"), c(net, "100.64.0.3", 47193, "Cy");
    Rig ra(a), rb(b), rc(c);
    const flow::FileBytes fox = file(1, 300000), owl = file(2, 5000), cat = file(3, 70000);
    ra.wear(fox), rb.wear(owl);
    bool ok = a.ask("open") == "ok" && join(a, b);
    wait(net, 2);
    check("each asks the other for the avatar they have on, and keeps the file that comes", ok && ra.has(owl) && rb.has(fox) && ra.kept.size() == 1 && rb.kept.size() == 1);
    size_t asks = a.wants.size() + b.wants.size(), sent = ra.sent + rb.sent;
    wait(net, 120);
    check("...once: a file that is here isn't asked for again", asks == 2 && a.wants.size() + b.wants.size() == asks && ra.sent + rb.sent == sent && sent == 2);
    ok = join(a, c);
    wait(net, session::Session::kMeshRetryS + 3);
    check("someone with no avatar to send gets the others', from the ones who have them on, and is asked for nothing", ok && rc.has(fox) && rc.has(owl) && c.wants.empty() && ra.sent == 2 && rb.sent == 2 && ra.kept.size() == 1);
    rb.wear(cat);
    wait(net, 3);
    check("a change of avatar: the new file is fetched, and the old one let go", ra.has(cat) && rc.has(cat) && !ra.has(owl) && !rc.has(owl) && rc.kept.size() == 2);
    rb.wear(nullptr);
    wait(net, 3);
    check("an avatar taken off: nothing is asked for, and its file is let go", !ra.has(cat) && rc.kept.size() == 1 && ra.kept.empty());
    rc.wear(fox);   // the same file as the owner has on
    wait(net, 3);
    check("two people with the same avatar: one file does for both", rb.kept.size() == 1 && rb.has(fox) && ra.kept.size() == 1 && ra.has(fox));
    ok = c.ask("leave") == "ok" && b.ask("leave") == "ok";
    wait(net, 2);
    check("out of the room, the files that were kept are gone; what the wearer has on stays theirs", ok && rb.kept.empty() && rc.kept.empty() && ra.kept.empty() && rc.worn == fox);
}

// What a peer may try.
static void strangers() {
    Net net;
    Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo"), c(net, "100.64.0.3", 47193, "Cy");
    Rig ra(a), rb(b), rc(c);
    const flow::FileBytes fox = file(1, 300000), owl = file(2, 5000), cat = file(3, 70000);
    const auto linkTo = [](Frame &from, int person) { return from.s->LinkTo(person); };
    ra.wear(fox);
    bool ok = a.ask("open") == "ok" && join(a, b) && join(a, c);
    wait(net, session::Session::kMeshRetryS + 3);
    ok = ok && rb.has(fox) && rc.has(fox) && ra.kept.empty();

    // a file nobody asked for
    ra.looks.File(linkTo(a, 1), *owl, net.now);
    check("a file from someone who has no avatar on isn't kept", ok && ra.kept.empty() && ra.looks.refused() == 1);
    rb.worn = owl;
    b.s->Wear(look(owl), net.now), net.pump();   // the room knows; nobody has ticked, so nobody asked
    rc.looks.File(linkTo(c, 1), *owl, net.now);
    check("...nor one that wasn't asked for, right as it is", rc.kept.size() == 1 && rc.looks.refused() == 1);
    wait(net, 3);
    ok = ra.has(owl) && rc.has(owl);
    rc.looks.File(linkTo(c, 0), *owl, net.now);   // the owner sends Bo's
    check("...nor someone's avatar from anyone but them", ok && rc.looks.refused() == 2 && rc.kept.size() == 2);

    // a file that isn't what the room said
    rb.swap = [&](flow::FileBytes) { return file(9, cat->size()); };
    rb.wear(cat);
    wait(net, 3);
    ok = !ra.has(cat) && ra.looks.refused() == 2 && ra.kept.size() == 0 && rb.sent == 4;
    check("a file whose bytes aren't the avatar the room said isn't kept", ok);
    wait(net, 20);
    ok = rb.sent == 4;
    wait(net, flow::Looks::kAskAgainS);
    ok = ok && rb.sent == 6 && !ra.has(cat);
    rb.swap = nullptr;
    wait(net, flow::Looks::kAskAgainS + 3);
    check("...and is asked for again, later, until the right one comes", ok && ra.has(cat) && rc.has(cat) && rb.sent == 8);
    rb.swap = [&](flow::FileBytes f) { return std::make_shared<const std::vector<uint8_t>>(f->begin(), f->end() - 1); };
    rb.wear(owl);
    wait(net, 3);
    ok = !ra.has(owl) && !rc.has(owl);
    rb.swap = [&](flow::FileBytes) { return fox; };   // longer than was awaited: the links don't take it
    const size_t refused = ra.looks.refused();
    wait(net, flow::Looks::kAskAgainS + 3);
    check("...as is one cut short, or one longer than the avatar the room said", ok && !ra.has(owl) && ra.looks.refused() == refused);
    rb.swap = nullptr;

    // someone who changes avatars all the time
    net.now += 600;
    const size_t before = rb.sent;
    for (int i = 0; i < 30; ++i) {
        rb.wear(i % 2 ? cat : owl);
        wait(net, 3);
    }
    check("someone who changes avatars all the time is followed only so often", rb.sent - before >= 2 * 3 && rb.sent - before <= 2 * (3 + 4));

    // asks that aren't for what the wearer has on, and too many
    net.now += 600;
    wait(net, 3);
    const size_t had = ra.sent;
    proto::Want w;
    std::memcpy(w.hash, look(owl).hash, proto::kLookHash);
    ra.looks.Wanted(linkTo(a, 1), w, net.now);
    std::memcpy(w.hash, look(fox).hash, proto::kLookHash);
    ra.looks.Wanted(9999, w, net.now);
    check("the wearer's file goes only to someone in the room who asks for the avatar they have on", ra.sent == had);
    for (int i = 0; i < 20; ++i) ra.looks.Wanted(linkTo(a, 1), w, net.now);
    ok = ra.sent - had == size_t(flow::Looks::kSendsAtOnce);
    ra.looks.Wanted(linkTo(a, 2), w, net.now);
    check("...and only so often to one link, whatever it asks; another link's asks are its own", ok && ra.sent - had == size_t(flow::Looks::kSendsAtOnce) + 1);
    ra.worn = cat;   // the file changed on disk, and the room wasn't told yet
    net.now += 600;
    ra.looks.Wanted(linkTo(a, 2), w, net.now);
    check("a file that isn't the one the room was told of any more isn't sent", ra.sent - had == size_t(flow::Looks::kSendsAtOnce) + 1);

    // a link that comes back
    ra.wear(fox);
    wait(net, 3);
    ok = rb.has(fox) && rc.has(fox);
    rb.wear(owl);
    rb.swap = [](flow::FileBytes) { return flow::FileBytes(); };   // nothing comes
    wait(net, 3);
    ok = ok && !rc.has(owl);
    rb.swap = nullptr;
    net.cut(b, c), net.cut(c, b);
    wait(net, session::Session::kMeshRetryS + 3);
    check("a file that was on its way when its link broke is asked for again on the new link, without the wait", ok && rc.has(owl));
}

int main() {
    hashes();
    sharing();
    strangers();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
