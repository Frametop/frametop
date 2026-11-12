// How a shared panel's video flows (together/flow, proto/framewire.h): who gets the next
// picture, the order a viewer decodes them in, the bitrate, how often it is encoded for those
// who see it, and a picture's bytes on its stream.
#include "../flow/attention.h"
#include "../flow/framegate.h"
#include "../flow/pace.h"
#include "../flow/sendgate.h"
#include "../proto/framewire.h"

#include <cstdio>
#include <string>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

using Links = std::vector<int>;

static void viewers() {
    flow::Viewers v;
    check("a panel nobody watches goes to nobody, and asks for no keyframe", !v.watched() && v.For(true).empty() && v.For(false).empty() && !v.WantsKey(0));

    v.Watch(1, 1, false);
    check("someone who begins to watch is waiting for a keyframe", v.watched() && v.waiting() == 1 && v.For(false).empty());
    check("...so one is asked for, once", v.WantsKey(1) && !v.WantsKey(1.01));
    check("the keyframe goes to them, and what follows it too", v.For(true) == Links{1} && v.For(false) == Links{1} && v.waiting() == 0);
    check("nobody waits: no keyframe is asked for", !v.WantsKey(5));

    v.Watch(2, 1, false);
    check("a second viewer waits while the first goes on", v.For(false) == Links{1} && v.waiting() == 1);
    check("their keyframe goes to both: the panel is encoded once", v.WantsKey(6) && v.For(true) == (Links{1, 2}) && v.For(false) == (Links{1, 2}));

    v.GaveUp(2);
    check("a picture given up on a link: nothing more goes there until a keyframe", v.For(false) == Links{1} && v.waiting() == 1);
    check("...which isn't asked for within half a second of the last one", !v.WantsKey(6.2) && !v.WantsKey(6.49) && v.WantsKey(6.5));
    check("...and puts them back in step", v.For(true) == (Links{1, 2}) && v.waiting() == 0);

    v.Watch(1, 1, true);
    check("a viewer whose decoder lost its place waits again", v.For(false) == Links{2} && v.waiting() == 1);
    int asked = 0;
    for (int i = 0; i < 1000; ++i) {
        v.Watch(1 + i % 2, 1, true);
        asked += v.WantsKey(8 + i * 0.001);
    }
    check("however often they ask, the encoder is asked twice a second", asked == 2);
    v.For(true);

    v.Watch(1, 1, false);
    check("saying again that you watch changes nothing", v.For(false) == (Links{1, 2}));
    check("the panel is seen as much as the one who sees most of it does", v.level() == 1);
    v.Watch(2, 2, false);
    check("...which is the one who has it focused", v.level() == 2 && v.For(false) == (Links{1, 2}));
    v.Watch(1, 0, false);
    check("one who looks away gets nothing", v.For(false) == Links{2} && v.For(true) == Links{2} && v.level() == 2);
    v.Watch(1, 1, false);
    check("...and waits for a keyframe when they look again", v.For(false) == Links{2} && v.waiting() == 1);
    v.Watch(2, 1, false);
    check("one who sees less of it than before stays in step", v.level() == 1 && v.For(false) == Links{2});
    v.Watch(1, 2, false);
    check("whichever of them sees most of it", v.level() == 2);
    v.Gone(1);
    check("...and what the panel is seen as falls back when the one who saw most goes", v.level() == 1);
    v.Gone(1), v.Gone(2), v.Gone(3);
    check("when the last viewer is gone the panel isn't watched", !v.watched() && !v.WantsKey(100) && v.level() == 0);
    v.GaveUp(7);
    check("a picture given up on a link that doesn't watch is nothing", !v.watched());
}

static void attention() {
    using flow::PicturesFor;
    const int hidden = proto::kHidden, view = proto::kInView, focused = proto::kFocused;
    check("a focused panel is encoded at its full rate, one in view at a third, one nobody sees not at all",
          PicturesFor(focused, 30) == 30 && PicturesFor(view, 30) == 10 && PicturesFor(hidden, 30) == 0 && PicturesFor(view, 60) == 20 && PicturesFor(focused, 60) == 60);
    check("...at five a second at the least, and never more than the full rate",
          PicturesFor(view, 15) == 5 && PicturesFor(view, 9) == 5 && PicturesFor(view, 14) == 5 && PicturesFor(view, 18) == 6 && PicturesFor(view, 3) == 3 && PicturesFor(focused, 3) == 3);

    flow::Seen seen;
    check("a panel that has just come counts as in view", seen.Told(0) == view);
    seen.Set(focused, 1);
    check("what its viewer has of it is told as it is", seen.Told(1) == focused);
    seen.Set(view, 2);
    check("...down as well as up", seen.Told(2) == view && seen.Told(500) == view);
    seen.Set(hidden, 10);
    check("out of view, it counts as in view for three seconds more", seen.Told(10) == view && seen.Told(12.99) == view);
    check("...and then as hidden", seen.Told(13) == hidden && seen.Told(1000) == hidden);
    seen.Set(hidden, 12.5);
    check("saying again that it is hidden starts nothing over", seen.Told(13) == hidden);
    seen.Set(focused, 14), seen.Set(hidden, 20);
    check("seen again and hidden again, the three seconds count from then", seen.Told(22.9) == view && seen.Told(23.1) == hidden);
    seen.Set(view, 21);
    check("one that comes back into view within them was never hidden", seen.Told(21) == view && seen.Told(30) == view);
    seen.Set(focused, 31), seen.Set(hidden, 32);
    check("a focused panel that goes out of view counts as in view meanwhile, not as focused", seen.Told(32.5) == view);
}

static flow::Order::Frame frame(uint32_t number, bool key = false) {
    flow::Order::Frame f;
    f.number = number, f.key = key;
    f.unit = {uint8_t(number), uint8_t(key)};
    return f;
}

// The numbers of the pictures that pass when `f` comes.
static std::vector<uint32_t> take(flow::Order &o, flow::Order::Frame f, double now) {
    std::vector<flow::Order::Frame> out;
    o.Take(std::move(f), now, out);
    std::vector<uint32_t> numbers;
    for (const flow::Order::Frame &p : out) {
        numbers.push_back(p.number);
        if (p.unit != std::vector<uint8_t>{uint8_t(p.number), uint8_t(p.key)}) numbers.push_back(0xdead);   // not the picture that came
    }
    return numbers;
}

using Numbers = std::vector<uint32_t>;

static void order() {
    flow::Order o;
    check("before a keyframe nothing passes", take(o, frame(11), 0).empty() && o.held() == 1);
    check("the keyframe passes, and the picture after it that came first", take(o, frame(10, true), 0.01) == (Numbers{10, 11}) && o.held() == 0);
    check("pictures in order pass as they come", take(o, frame(12), 0.04) == Numbers{12} && take(o, frame(13), 0.07) == Numbers{13});
    check("one that comes twice, or late, doesn't", take(o, frame(13), 0.08).empty() && take(o, frame(9, true), 0.08).empty() && take(o, frame(12), 0.08).empty());

    check("one that is early is held", take(o, frame(15), 0.1).empty() && take(o, frame(16), 0.11).empty() && o.held() == 2);
    check("...with no keyframe asked for yet: the one before it may only be late", !o.WantsKey(0.2));
    check("...and passes with those after it when the gap fills", take(o, frame(14), 0.25) == (Numbers{14, 15, 16}) && o.held() == 0);
    check("nothing is held: no keyframe is asked for, however long nothing comes", !o.WantsKey(60));

    check("a gap that doesn't fill", take(o, frame(18), 61).empty() && take(o, frame(19), 61.03).empty() && !o.WantsKey(61.2));
    check("...asks for a keyframe after a third of a second, and again every half", o.WantsKey(61.31) && !o.WantsKey(61.5) && o.WantsKey(61.82));
    check("a keyframe past the gap passes, and what was held before it is dropped", take(o, frame(20, true), 62) == Numbers{20} && o.held() == 0 && !o.WantsKey(63));
    check("a keyframe that jumps ahead passes with what follows it",
          take(o, frame(31), 63).empty() && take(o, frame(25), 63).empty() && take(o, frame(30, true), 63.1) == (Numbers{30, 31}) && o.held() == 0);

    for (uint32_t n = 40; n < 50; ++n) take(o, frame(n), 64);
    check("only a few early pictures are held, the latest of them", o.held() == flow::Order::kHeld);
    check("...so the earliest of them no longer pass when the gap fills", take(o, frame(32), 64.1) == Numbers{32} && o.held() == flow::Order::kHeld);
    check("...until a keyframe", take(o, frame(43, true), 64.2) == (Numbers{43, 44, 45, 46, 47, 48, 49}) && o.held() == 0);

    flow::Order round;
    check("the count goes round", take(round, frame(0xfffffffe, true), 0) == Numbers{0xfffffffe} && take(round, frame(0), 0).empty() &&
                                      take(round, frame(0xffffffff), 0) == (Numbers{0xffffffff, 0}) && take(round, frame(1), 0) == Numbers{1} &&
                                      take(round, frame(0xffffffff), 0).empty());

    o.Reset();
    check("a decoder that starts again takes only a keyframe", take(o, frame(50), 70).empty() && take(o, frame(52, true), 70) == Numbers{52} && o.held() == 0);
    o.Reset();
    check("...whatever its number, and the pictures after that one", take(o, frame(5, true), 71) == Numbers{5} && take(o, frame(6), 71) == Numbers{6});
}

static void pace() {
    using flow::Pace;
    Pace p(3'000'000);
    check("a panel starts at two megabits", p.bitrate() == Pace::kStart);
    check("one picture given up changes nothing", !p.GaveUp(1) && p.bitrate() == Pace::kStart);
    check("nor a second one long after", !p.GaveUp(2.5) && p.bitrate() == Pace::kStart);
    check("two within a second: slower by a third or so", p.GaveUp(3) && p.bitrate() == 1'400'000);
    check("...and the count starts again", !p.GaveUp(3.1) && p.GaveUp(3.2) && p.bitrate() == 980'000);

    check("no rise while pictures are still given up now and then", !p.Tick(5) && !p.Tick(8.1) && !p.GaveUp(8.15) && !p.Tick(13));
    check("five seconds without one: a little faster", p.Tick(13.2) && p.bitrate() == 1'127'000);
    check("...and again every five seconds", !p.Tick(15) && !p.Tick(18.1) && p.Tick(18.3) && p.bitrate() == 1'296'050);
    double now = 18.3;
    for (int i = 0; i < 20; ++i) p.Tick(now += 5);
    check("up to the owner's cap and no further", p.bitrate() == 3'000'000 && !p.Tick(now + 50));

    p.SetCap(1'000'000);
    check("a lower cap takes hold at once", p.bitrate() == 1'000'000 && !p.Tick(now + 100));
    p.SetCap(8'000'000);
    check("a higher one is climbed to", p.bitrate() == 1'000'000 && p.Tick(now + 200) && p.bitrate() == 1'150'000);

    for (int i = 0; i < 100; ++i) p.GaveUp(now + 300 + i * 0.01);
    check("a link that carries nothing brings it to the least, not to nothing", p.bitrate() == Pace::kLeast && !p.GaveUp(now + 302) && !p.GaveUp(now + 302.1));

    Pace low(100);
    check("no cap is below the least", low.bitrate() == Pace::kLeast);
    Pace fresh(8'000'000);
    check("the first five seconds count from the first moment told", !fresh.Tick(100) && !fresh.Tick(104.9) && fresh.Tick(105) && fresh.bitrate() == 2'300'000);
}

static void idle() {
    flow::Pace p(3'000'000);
    for (double now = 0; now < 60; now += 0.5) p.Idle(now);
    check("while nothing is sent the bitrate doesn't climb", !p.Tick(60.5) && !p.Tick(64) && p.bitrate() == flow::Pace::kStart);
    check("...and five seconds into sending it does", p.Tick(65) && p.bitrate() == flow::Pace::kStart * flow::Pace::kUp / 100);
}

static void framewire() {
    using namespace proto;
    const std::vector<uint8_t> unit = {0, 0, 0, 1, 0x40, 9, 8, 7};
    FrameHead h;
    h.panel = PanelId(2, 1), h.number = 0x01020304, h.key = true;
    const std::vector<uint8_t> sent = PackFrame(h, unit.data(), unit.size());
    check("a picture's stream is seven bytes and the picture", sent.size() == kFrameHead + unit.size() && sent[0] == kFrameStream && sent[1] == 17 && sent[6] == 1);

    FrameHead got;
    const uint8_t *at = nullptr;
    size_t size = 0;
    check("...and reads back", ReadFrame(sent.data(), sent.size(), got, at, size) && got.panel == 17 && got.number == 0x01020304 && got.key && size == unit.size() &&
                                 std::vector<uint8_t>(at, at + size) == unit);
    h.key = false;
    const std::vector<uint8_t> delta = PackFrame(h, unit.data(), unit.size());
    check("a picture that isn't a keyframe says so", ReadFrame(delta.data(), delta.size(), got, at, size) && !got.key);

    std::vector<uint8_t> bad = sent;
    bad[0] = 1;
    check("another kind of stream isn't a picture", !ReadFrame(bad.data(), bad.size(), got, at, size));
    bad = sent, bad[1] = 4 * kPanelNumbers;
    check("nor one for a panel no room has", !ReadFrame(bad.data(), bad.size(), got, at, size));
    bad = sent, bad[6] = 3;
    check("nor one with flags nobody set", !ReadFrame(bad.data(), bad.size(), got, at, size));
    check("nor a head with no picture, a part of one, or nothing", !ReadFrame(sent.data(), kFrameHead, got, at, size) && !ReadFrame(sent.data(), 3, got, at, size) &&
                                                                     !ReadFrame(nullptr, 0, got, at, size));

    std::vector<uint8_t> most(kMostFrame, 7), long1(kMostFrame + 1, 7);
    const std::vector<uint8_t> whole = PackFrame(h, most.data(), most.size());
    check("the longest picture is a megabyte", whole.size() == kFrameHead + kMostFrame && ReadFrame(whole.data(), whole.size(), got, at, size) && size == kMostFrame);
    check("a longer one isn't sent", PackFrame(h, long1.data(), long1.size()).empty() && PackFrame(h, unit.data(), 0).empty() && PackFrame(h, nullptr, 5).empty());
    std::vector<uint8_t> over = whole;
    over.push_back(7);
    check("...or taken", !ReadFrame(over.data(), over.size(), got, at, size));

    check("a later picture is later, round the count too", LaterFrame(5, 4) && !LaterFrame(4, 4) && !LaterFrame(3, 4) && LaterFrame(2, 0xfffffff0) && !LaterFrame(0xfffffff0, 2));
}

int main() {
    viewers();
    attention();
    order();
    pace();
    idle();
    framewire();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
