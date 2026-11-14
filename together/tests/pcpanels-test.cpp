// The panels of someone's split compute PC (together/flow/pcpanels.h, proto/sender.h) and the
// link the session keeps apart for it (session.h, Config::device): a Frame with a PC and one
// that watches, in a room on the network made in the test (fakenet.h, pcseats.h). The PC is the
// test's own end of a link: what it says and sends is written there, and what it is told is read
// there.
#include "pcseats.h"

static const char *const kWindow = " 1920 1080 0 1.5 -2 1 0 0 0 2 0 Window";

static void bytes() {
    proto::Sender s, s2;
    s.code = "ABCDEFGH";
    std::vector<uint8_t> packed = proto::Pack(s);
    std::vector<uint8_t> body(packed.begin() + 6, packed.end());
    bool ok = packed[4] == proto::kSender && packed[5] == 0 && proto::Read(body, s2) && s2.version == proto::kVersion && s2.code == "ABCDEFGH";
    body.push_back(0);
    ok = ok && !proto::Read(body, s2);
    body.resize(body.size() - 2);
    check("a sender's first word comes back as it went, and one with more or less than that doesn't", ok && !proto::Read(body, s2) && !proto::Read({}, s2) && !proto::Read({1}, s2));
    s.version = 9, s.code = std::string(40, 'A');
    packed = proto::Pack(s), body.assign(packed.begin() + 6, packed.end());
    ok = proto::Read(body, s2) && s2.version == 9 && s2.code.empty() && body.size() == 2 + 1 + proto::kMostCode;
    check("one of another version is read as far as its version; a code is cut to what a code may be", ok && proto::Read({9, 0}, s2) && s2.version == 9);
    body = {uint8_t(proto::kVersion), 0, 2, 'A', 7};
    check("a code that isn't text is refused", !proto::Read(body, s2));
    body = {uint8_t(proto::kVersion), 0, uint8_t(proto::kMostCode + 1)};
    body.insert(body.end(), proto::kMostCode + 1, 'A');
    check("...and so is one longer than a code may be", !proto::Read(body, s2));

    ok = true;
    const struct {
        uint8_t number, word;
        uint32_t value;
        bool good;
    } words[] = {{3, 'R', 1, true},   {6, 'R', proto::kMostEncodeBits, true}, {3, 'R', 0, false},  {3, 'R', proto::kMostEncodeBits + 1, false}, {0, 'F', 1, true},  {7, 'F', 60, true},
                 {3, 'F', 0, false},  {3, 'F', 61, false},                     {3, 'P', 1, true},   {3, 'P', 60, true},                           {3, 'P', 0, false}, {3, 'P', 61, false},
                 {3, 'K', 0, true},   {3, 'K', 1, false},                      {3, 'W', 0, true},   {3, 'W', 1, true},                            {3, 'W', 2, false}, {8, 'K', 0, false},
                 {3, 'X', 0, false},  {3, 0, 0, false}};
    for (const auto &w : words) {
        proto::Encode e, e2;
        e.number = w.number, e.word = w.word, e.value = w.value;
        packed = proto::Pack(e), body.assign(packed.begin() + 6, packed.end());
        const bool read = proto::Read(body, e2);
        ok = ok && packed[4] == proto::kEncode && body.size() == 6 && read == w.good && (!read || (e2.number == w.number && e2.word == w.word && e2.value == w.value));
        body.push_back(0);
        ok = ok && !proto::Read(body, e2);
        body.resize(5);
        ok = ok && !proto::Read(body, e2);
    }
    check("what to encode at comes back as it went, and a number, a word or a value there isn't is refused", ok);
}

static void calling() {
    Home h;
    Desk &d = h.desk;
    if (!check("two people sit in a room", h.sit())) return;
    const std::string code = d.pc.Invite(h.net.now);
    check("a code for the PC is an invite's: eight characters", code.size() == room::Invites::kCodeLength && !d.pc.linked());

    Pc lost(h.net, d, "100.64.0.99");
    bool ok = !lost.calls(code) && lost.refused() == int(proto::Why::kBad) && !d.pc.linked();
    check("a PC the tailnet doesn't know is refused, whatever it shows", ok);
    Pc old(h.net, d);
    check("...and one of another version, told to update", !old.calls(code, proto::kVersion + 1) && old.refused() == int(proto::Why::kVersion) && !d.pc.linked());
    Pc wrong(h.net, d);
    check("...and one with a wrong code", !wrong.calls("WRNGCODE") && wrong.refused() == int(proto::Why::kCode) && !d.pc.linked());
    Pc mute(h.net, d);
    mute.say({5, 0, 0, 0, uint8_t(proto::kSender), 0, 1, 0, 3, 'A', 'B'});
    check("a first word that can't be read ends the link, with nothing said", mute.shut() && mute.refused() == 0 && h.net.heard[mute.end.link].empty());
    Pc other(h.net, d);
    proto::Encode e;
    e.number = 3;
    other.say(proto::Pack(e));
    check("a stranger who says what only a Frame says to a PC is no PC", other.shut() && !d.pc.linked());
    Pc hello(h.net, d);
    proto::Hello hi;
    std::memcpy(hi.room, h.a.s->room().room, proto::kRoomId);
    hello.say(proto::Pack(hi));
    ok = hello.shut();   // (a hello without an invite: refused as any stranger is)
    check("...nor is someone who came as a Frame", ok && hello.refused() == int(proto::Why::kCode));
    Pc asker(h.net, d);
    hi.code = h.a.ask("invite").substr(3), hi.name = "Cy", hi.port = 47193;
    asker.say(proto::Pack(hi));
    ok = !asker.shut() && h.a.asking() != 0;
    check("...nor one who asked to be let into the room, and has the PC's code too", ok && !asker.calls(code) && !d.pc.linked() && !h.a.asking());

    // a Frame this one called, and a knock at a guest's Frame
    {
        Home in;
        ok = in.b.ask("open") == "ok" && join(in.b, in.a);
        Pc knock(in.net, in.desk);
        proto::Hello k;
        std::memcpy(k.room, in.a.s->room().room, proto::kRoomId);
        k.name = "Cy", k.port = 47193;
        knock.say(proto::Pack(k));
        ok = ok && !knock.shut();   // (it waits for the owner's word of who is in the room)
        check("...nor one who knocked at a guest's Frame, as someone of its room would", ok && !knock.calls(in.desk.pc.Invite(in.net.now)) && !in.desk.pc.linked());
        Home out;
        const std::string mine = out.desk.pc.Invite(out.net.now);
        bool swapped = false;
        out.net.tamper = [&](Net::Event &e) {
            if (e.what != Net::Event::kBytes || e.to.frame != &out.a || swapped) return;
            proto::Sender s;
            s.code = mine;
            e.bytes = proto::Pack(s), swapped = true;
        };
        ok = out.b.ask("open") == "ok" && out.a.ask("join " + where(out.b) + " " + out.b.ask("invite").substr(3)) == "ok";
        if (!swapped) out.b.ask("accept " + std::to_string(out.b.asking()));
        check("a Frame this one called, which answers as a PC would, is no PC", ok && swapped && !out.desk.pc.linked() && out.a.s->mode() == session::Mode::kIdle);
    }

    // a Frame with nobody to hear a PC, and a PC before any room
    {
        session::Config plain;
        plain.name = "Plain", plain.port = 47193, plain.dice = roll;
        session::Session bare(h.b, h.b, plain);
        bare.Up(9000, kPcAt, h.net.now);
        const std::vector<uint8_t> word = proto::Pack(proto::Sender{});
        bare.Bytes(9000, word.data(), word.size(), h.net.now);
        check("a Frame with nobody to hear a PC ends its link", h.net.shut.count(9000) == 1);
        Home early;
        early.net.nextLink = 0;
        Pc zero(early.net, early.desk);
        ok = zero.end.link == 0 && zero.calls(early.desk.pc.Invite(early.net.now)) && early.desk.pc.linked();
        check("a PC may come before any room, on whatever link it comes", ok && zero.heard() == early.every("R2000000 F30 P10 W0"));
        const std::vector<uint8_t> first = proto::Pack([&] {
            proto::Sender s;
            s.code = early.desk.pc.Invite(early.net.now);
            return s;
        }());
        ok = !early.desk.pc.Said(77, kPcAt, proto::kHello, {first.begin() + 6, first.end()}, early.desk.video, early.net.now);
        check("the first word is a Sender, whatever else reads as one", ok && early.desk.pc.linked() && !zero.shut());
    }

    Pc pc(h.net, d);
    ok = pc.calls(code) && d.pc.linked() && pc.refused() == 0;
    check("the PC with the code is taken, and hears what each of its four panels is to be encoded at", ok && pc.heard() == h.every("R2000000 F30 P10 W0"));
    check("...and none of that went to the Frame's own programs", d.own.words.empty());
    check("it is nobody in the room", h.a.s->PersonOn(pc.end.link) == -1 && h.a.s->MemberLinks().size() == 1 && h.a.has("\"people\":[{\"id\":0") && !h.a.asking());
    Pc again(h.net, d);
    check("a code works once", !again.calls(code) && again.refused() == int(proto::Why::kCode) && d.pc.linked() && !pc.shut());
    h.net.wait(300);
    check("the link isn't timed as a stranger's or an asker's is", d.pc.linked() && !pc.shut());

    ok = h.a.ask("leave") == "ok" && h.a.s->mode() == session::Mode::kIdle;
    check("the room closes: the PC's link is not the room's, and stays", ok && d.pc.linked() && !pc.shut() && h.b.s->mode() == session::Mode::kIdle);
    h.a.s->Hang(999);
    ok = h.sit();
    h.a.s->Hang(h.a.s->MemberLinks()[0]);
    check("only a device's link can be hung up from outside", ok && h.a.s->MemberLinks().size() == 1 && h.b.s->mode() == session::Mode::kJoined && d.pc.linked());

    const std::string second = d.pc.Invite(h.net.now);
    Pc next(h.net, d);
    pc.heard();
    ok = next.calls(second) && pc.shut() && d.pc.linked() && next.heard() == h.every("R2000000 F30 P10 W0");
    d.video.SetQuality(flow::kHigh);
    check("another PC with a new code takes the first one's place, and is the one told from then on", ok && pc.heard().empty() && next.heard() == h.every("R2000000 F60 P20"));
    check("...and the first one's pictures are no longer the PC's", (pc.unit(3, true), !pc.taken) && (next.unit(3, true), next.taken));
    d.video.SetQuality(flow::kBalanced);
    next.heard();
    next.say(proto::Pack(proto::Sender{}));
    ok = next.shut() && !d.pc.linked();
    d.video.SetQuality(flow::kLow);
    check("a PC that goes on talking is let go, and nothing more is said to it", ok && next.heard().empty());

    Pc last(h.net, d);
    ok = last.calls(d.pc.Invite(h.net.now)) && d.pc.linked() && last.heard() == h.every("R1000000 F15 P5 W0");
    d.pc.Down(last.end.link + 1);
    ok = ok && d.pc.linked();
    d.pc.Down(last.end.link);
    d.video.SetQuality(flow::kBalanced);
    check("a link that went is the PC gone, if it was the PC's", ok && !d.pc.linked() && last.heard().empty());
    check("...and with no PC, what there is to encode at is said to nobody", h.net.heard.count(-1) == 0);

    // an old code, and guesses
    h.net.devices.insert("100.64.0.52");
    const std::string stale = d.pc.Invite(h.net.now);
    h.net.wait(room::Invites::kGoodForS + 1);
    Pc slow(h.net, d, "100.64.0.52");
    check("a code is good for ten minutes", !slow.calls(stale) && slow.refused() == int(proto::Why::kCode) && !d.pc.linked());
    const std::string good = d.pc.Invite(h.net.now);
    ok = true;
    for (int i = 0; i < room::Invites::kMostWrong; ++i) {
        Pc guess(h.net, d);
        ok = ok && !guess.calls("GUESS" + std::to_string(i) + "AB") && guess.refused() == int(proto::Why::kCode);
    }
    Pc late(h.net, d);
    check("five wrong codes from an address, and the next from there isn't looked at", ok && !late.calls(good) && !d.pc.linked());
    h.net.devices.insert("100.64.0.51");
    Pc elsewhere(h.net, d, "100.64.0.51");
    check("...which another address doesn't pay for", elsewhere.calls(good) && d.pc.linked());
}

static void pictures() {
    Home h;
    Desk &d = h.desk;
    Seat &sb = h.sb;
    if (!check("two people sit in a room", h.sit())) return;
    Pc pc(h.net, d);
    if (!check("the PC calls", pc.calls(d.pc.Invite(h.net.now)))) return;
    pc.heard();
    const uint8_t p3 = proto::PanelId(h.a.s->me(), 3), p0 = proto::PanelId(h.a.s->me(), 0);

    pc.unit(3, true);
    check("a picture of a panel that isn't shared goes nowhere", pc.taken && d.sent == 0 && d.pc.dropped() == 0);
    bool ok = h.a.ask(std::string("share 3") + kWindow) == "ok" && h.a.ask(std::string("share 0") + kScreen) == "ok" && h.b.s->room().panel(p3) && h.b.s->room().panel(p3)->number() == 3;
    pc.unit(3, true);
    check("...nor one of a shared panel nobody watches", ok && d.sent == 0 && pc.heard().empty());

    sb.video.SinkCame(p3, h.net.now);
    h.net.pump();
    check("someone begins to watch the PC's panel: the PC hears that, and is asked for a keyframe", pc.heard() == "3W1 3K0" && d.video.viewers(3) == 1 && d.own.words.empty());
    const Unit k = pc.unit(3, true), d1 = pc.unit(3, false), d2 = pc.unit(3, false);
    check("its pictures reach them through the PC's own Frame, byte for byte, as that Frame's panel", sb.since(0, p3) == (std::vector<Unit>{k, d1, d2}) && sb.shown[0].key && !sb.shown[1].key && d.sent == 3);
    sb.video.Attention(p3, proto::kFocused, false, h.net.now);
    h.net.pump();
    check("focused, it is to be encoded at its full rate", pc.heard() == "3P30");

    // the Frame's own panels go on as they did
    sb.video.SinkCame(p0, h.net.now);
    h.net.pump();
    ok = d.own.watched[0] && d.own.words[0] == 2 && pc.heard().empty();
    const Unit own(300, 7);
    d.video.Unit(0, true, own.data(), own.size(), h.net.now);
    h.net.pump();
    check("what is said about a panel of the Frame's own goes to the Frame's programs, and its pictures go on as before", ok && sb.shown.back().panel == p0 && sb.shown.back().unit == own);
    d.video.SinkCame(8, h.net.now);
    d.video.Attention(8, proto::kInView, false, h.net.now);
    check("...and so does what is said to a program that shows someone else's panel", !d.own.decode.empty() && d.own.decode.back() == std::make_pair(uint8_t(8), true));
    d.pc.Show(8, true, own);
    check("...and a picture for it", d.own.shown.size() == 1 && d.own.shown[0].panel == 8 && d.own.shown[0].key && d.own.shown[0].unit == own);
    d.pc.Decode(9, false);
    check("...and that one is not to decode", d.own.decode.back() == std::make_pair(uint8_t(9), false));

    // order, on the way from the PC
    size_t n = sb.shown.size();
    const uint32_t at = pc.count[3];
    const Unit e2 = pc.unit(3, false, at + 2), e3 = pc.unit(3, false, at + 3);
    ok = sb.shown.size() == n;
    const Unit e1 = pc.unit(3, false, at + 1);
    check("pictures that overtook one wait for it, and go on in order", ok && sb.since(n, p3) == (std::vector<Unit>{e1, e2, e3}));
    pc.unit(3, false, at + 3);
    pc.unit(3, false, at + 1);
    check("one that comes twice, or late, goes on once", sb.shown.size() == n + 3);
    pc.unit(3, false, at + 5);
    h.tick(0.2);
    ok = pc.heard().empty();
    h.tick(0.2);
    ok = ok && pc.heard() == "3K0";
    h.tick(0.3);
    ok = ok && pc.heard().empty();
    h.tick(0.3);
    check("a gap that doesn't fill: the PC is asked for a keyframe, and again every half second until one comes", ok && pc.heard() == "3K0" && sb.shown.size() == n + 3);
    const Unit k2 = pc.unit(3, true, at + 9);
    h.tick(1);
    check("...which is where the panel goes on", sb.since(n + 3, p3) == std::vector<Unit>{k2} && pc.heard().empty());

    // what the PC may not send
    n = sb.shown.size();
    for (int number : {0, 2, 7}) pc.unit(number, true);
    ok = pc.taken && d.pc.dropped() == 3 && sb.shown.size() == n;
    check("a picture of a panel that isn't a PC's is dropped", ok);
    const uint8_t junk[] = {proto::kFrameStream, 3, 1, 0, 0, 0, 2, 9}, other[] = {9, 3, 1, 0, 0, 0, 1, 9};
    ok = d.pc.Frame(pc.end.link, junk, sizeof junk, d.video, h.net.now) && d.pc.Frame(pc.end.link, other, sizeof other, d.video, h.net.now) && d.pc.Frame(pc.end.link, junk, 7, d.video, h.net.now);
    check("...and so is one that isn't a picture", ok && d.pc.dropped() == 6 && sb.shown.size() == n);
    const int toB = h.a.s->MemberLinks()[0];
    ok = !d.pc.Frame(toB, junk, sizeof junk, d.video, h.net.now) && !d.pc.Frame(-1, junk, sizeof junk, d.video, h.net.now) && d.pc.dropped() == 6;
    check("a picture on someone else's link isn't the PC's to drop", ok);
    Home quiet;
    const uint8_t first[] = {proto::kFrameStream, 3, 1, 0, 0, 0, 1, 9};
    check("...nor is any picture while there is no PC", quiet.sit() && !quiet.desk.pc.Frame(-1, first, sizeof first, quiet.desk.video, 0) && !quiet.desk.pc.Frame(0, first, sizeof first, quiet.desk.video, 0));

    h.tick(1);
    n = sb.shown.size();
    size_t dropped = d.pc.dropped();
    pc.unit(3, true);
    for (int i = 0; i < 200; ++i) pc.unit(3, false);
    const size_t took = sb.shown.size() - n;
    ok = took >= size_t(flow::PcPanels::kPicturesAtOnce) && took <= size_t(flow::PcPanels::kPicturesAtOnce) + 1 && d.pc.dropped() - dropped == 201 - took;
    check("a flood of pictures: a burst is taken, the rest dropped", ok);
    dropped = d.pc.dropped();
    ok = h.a.ask(std::string("share 4") + kWindow) == "ok";
    sb.video.SinkCame(proto::PanelId(h.a.s->me(), 4), h.net.now);
    h.net.pump();
    n = sb.shown.size();
    const Unit k4 = pc.unit(4, true);
    check("...of that panel: another of the PC's has its own allowance, and its own count", ok && d.pc.dropped() == dropped && sb.shown.size() == n + 1 && sb.shown.back().unit == k4 && sb.shown.back().panel == p3 + 1);

    // the link to a viewer
    h.tick(2);
    pc.heard();
    const int rate = d.video.bitrate(3);
    d.video.Lost(toB, 3, h.net.now);
    d.video.Lost(toB, 3, h.net.now + 0.1);
    ok = d.video.bitrate(3) == rate * flow::Pace::kDown / 100 && pc.heard() == "3R" + std::to_string(d.video.bitrate(3));
    d.video.Lost(toB, 6, h.net.now + 2);
    d.video.Lost(toB, 6, h.net.now + 2.1);
    ok = ok && d.video.bitrate(6) == rate * flow::Pace::kDown / 100;
    d.video.Lost(toB, 7, h.net.now + 2.2);
    check("pictures given up on the way to a viewer bring the PC's bitrate down, as a Frame's", ok && d.own.words[0] == 2);

    ok = h.a.ask("unshare 3") == "ok" && !h.b.s->room().panel(p3);
    h.tick(1);
    check("taken back, the panel is watched by nobody", ok && d.video.viewers(3) == 0 && pc.heard().find("3W0") != std::string::npos);
    ok = h.a.ask("unshare 6") == "ok" && h.a.ask("unshare 7") == "error no such panel" && h.a.ask(std::string("share 7") + kWindow).rfind("error a panel's number is 0 to 6", 0) == 0;
    check("a person's panels are numbered 0 to 6", ok && h.a.ask(std::string("share 6") + kWindow) == "ok" && h.b.s->room().panel(proto::PanelId(h.a.s->me(), 6)));

    // a gap in another panel's pictures, and another PC
    const uint8_t p4 = p3 + 1;
    h.tick(2);
    pc.heard();
    n = sb.shown.size();
    const uint32_t at4 = pc.count[4];
    const Unit f1 = pc.unit(4, false), f2 = pc.unit(4, false);
    pc.unit(4, false, at4 + 4);
    h.tick(0.2);
    h.tick(0.2);
    const std::string asked = pc.heard();
    ok = sb.since(n, p4) == (std::vector<Unit>{f1, f2}) && asked.find("4K0") != std::string::npos && asked.find("3K0") == std::string::npos;
    check("a gap in another panel's pictures: the keyframe is asked for that panel", ok);
    Pc next(h.net, d);
    ok = next.calls(d.pc.Invite(h.net.now)) && pc.shut();
    n = sb.shown.size();
    const Unit n1 = next.unit(4, true), n2 = next.unit(4, false);
    check("another PC takes this one's place: its pictures are counted from its own first, and go on to who watches", ok && next.taken && sb.since(n, p4) == (std::vector<Unit>{n1, n2}));
}

static void guests() {
    Home h;
    if (!check("two people sit in a room", h.sit())) return;
    // the guest's PC: its Frame's video rules are a Seat's here, so only the room is looked at
    bool ok = h.b.ask(std::string("share 5") + kWindow) == "ok";
    const uint8_t p5 = proto::PanelId(h.b.s->me(), 5);
    check("a guest's PC panel is in the room on the guest's word, as their other panels are", ok && h.a.s->room().panel(p5) && h.b.s->room().panel(p5) && h.a.s->room().panel(p5)->owner() == h.b.s->me());
    proto::Panel forged = *h.a.s->room().panel(p5);
    forged.id = proto::PanelId(h.b.s->me(), 7);
    h.b.Send(h.b.s->MemberLinks()[0], proto::Pack(forged));
    h.net.pump();
    check("...and one numbered past them gets its sender taken out of the room", h.a.s->room().people.size() == 1 && h.b.s->mode() == session::Mode::kIdle);
}

int main() {
    bytes();
    calling();
    pictures();
    guests();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
