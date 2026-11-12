// What someone lets the video of their panels have (together/flow/quality.h), through the
// video's rules (together/flow/video.h) on the Frames of videoseats.h: what the programs that
// encode the panels are told when their owner picks, what the room hears of it, and what that
// does to the decoders of whoever shows the panels.
#include "videoseats.h"

static const char *const kLarge = " 3840 2160 0 1.5 -2 1 0 0 0 2 0 Screen";

static void names() {
    check("the three have their names", flow::QualityNamed("low") == &flow::kLow && flow::QualityNamed("balanced") == &flow::kBalanced && flow::QualityNamed("high") == &flow::kHigh);
    check("...and nothing else is one", !flow::QualityNamed("") && !flow::QualityNamed("Low") && !flow::QualityNamed("higher") && !flow::QualityNamed("hig"));
    bool ok = true;
    for (const flow::Quality *q : {&flow::kLow, &flow::kBalanced, &flow::kHigh})
        ok = ok && q->bits >= flow::Pace::kLeast && q->pictures >= flow::kLeastPictures && q->pictures <= proto::kMostPanelPictures;
    check("each is a bitrate a panel may have and pictures a second a panel may say", ok);
    check("Low has 1 Mbit/s and 15 pictures a second, Balanced 3 and 30, High 8 and 60",
          flow::kLow.bits == 1000000 && flow::kLow.pictures == 15 && flow::kBalanced.bits == 3000000 && flow::kBalanced.pictures == 30 && flow::kHigh.bits == 8000000 && flow::kHigh.pictures == 60);
    check("more of both from one to the next", flow::kLow.bits < flow::kBalanced.bits && flow::kBalanced.bits < flow::kHigh.bits && flow::kLow.pictures < flow::kBalanced.pictures &&
                                                 flow::kBalanced.pictures < flow::kHigh.pictures);
    check("a panel nobody picked for says what Balanced has", proto::Panel{}.pictures == flow::kBalanced.pictures);
}

// What the programs that encode the wearer's panels are told.
static void sources() {
    Table t;
    Seat &sa = t.sa, &sb = t.sb;
    if (!check("three people sit in a room, and one shares a panel", t.sit() && t.a.ask(std::string("share 0") + kScreen) == "ok")) return;
    const uint8_t pa = proto::PanelId(t.a.s->me(), 0);
    const auto level = [&](int l) {
        sb.video.Attention(pa, l, false, t.net.now);
        t.net.pump();
    };
    sa.video.SourceCame(0);
    check("until its owner picks, a panel has Balanced", &sa.video.quality() == &flow::kBalanced && sa.full[0] == 30 && sa.fullWords[0] == 1 && sa.pictures[0] == 10 && sa.rate[0] == flow::Pace::kStart);

    sa.video.SetQuality(flow::kLow);
    check("Low: the bitrate comes down to its cap at once", &sa.video.quality() == &flow::kLow && sa.rate[0] == flow::kLow.bits && sa.rateWords[0] == 2 && sa.video.bitrate(0) == flow::kLow.bits);
    check("...the encoder is set for its pictures a second, and a panel in view has a third, but five at the least",
          sa.full[0] == 15 && sa.fullWords[0] == 2 && sa.pictures[0] == 5 && sa.pictureWords[0] == 2 && sa.video.pictures(0) == 5);
    check("...and so are the panels with no program yet", sa.video.bitrate(2) == flow::kLow.bits && sa.video.pictures(2) == 5);
    sa.video.SourceCame(2);
    check("...whose program hears it when it comes", sa.rate[2] == flow::kLow.bits && sa.full[2] == 15 && sa.pictures[2] == 5);

    sb.video.SinkCame(pa, t.net.now);
    t.net.pump();
    level(proto::kFocused);
    sa.unit(0, true);
    check("focused by someone, it has all of them", sa.pictures[0] == 15 && sa.watched[0]);
    for (int i = 0; i < 6; ++i) t.tick(5.1);
    check("...and the bitrate stays at the cap", sa.rate[0] == flow::kLow.bits && sa.rateWords[0] == 2);

    sa.video.SetQuality(flow::kHigh);
    check("High while someone has it focused: the encoder is set for more, and has it at once", sa.full[0] == 60 && sa.pictures[0] == 60 && sa.video.pictures(0) == 60);
    check("...but the bitrate only may climb now: it is where it was", sa.rate[0] == flow::kLow.bits && sa.video.bitrate(0) == flow::kLow.bits);
    t.tick(5.1);
    bool ok = sa.rate[0] == flow::kLow.bits * flow::Pace::kUp / 100;
    for (int i = 0; i < 20; ++i) t.tick(5.1);
    check("...and climbs, as far as the new cap", ok && sa.rate[0] == flow::kHigh.bits && sa.video.bitrate(0) == flow::kHigh.bits);
    level(proto::kInView);
    check("in view, a third of them", sa.pictures[0] == 20);

    const int words = sa.pictureWords[0], fulls = sa.fullWords[0];
    sa.video.SetQuality(flow::kBalanced);
    check("Balanced again: bitrate, encoder and pictures at once, a word each",
          sa.rate[0] == flow::kBalanced.bits && sa.full[0] == 30 && sa.fullWords[0] == fulls + 1 && sa.pictures[0] == 10 && sa.pictureWords[0] == words + 1);
    level(proto::kFocused);
    level(proto::kHidden);
    for (int i = 0; i < 4; ++i) t.tick(1.01);
    check("nobody watches it any more: what it had when someone last did stands", !sa.watched[0] && sa.pictures[0] == 10);
    sa.video.SetQuality(flow::kHigh);
    check("a panel nobody watches is set as one in view, for when someone does", sa.full[0] == 60 && sa.pictures[0] == 20 && sa.video.pictures(0) == 20);
    level(proto::kFocused);
    check("...and has all of them once it is focused", sa.watched[0] && sa.pictures[0] == 60);
}

// What the room hears: the pictures a second each panel has at the most.
static void heard() {
    Table t;
    Seat &sa = t.sa, &sb = t.sb;
    const auto pictures = [](Headset &at, uint8_t panel) {
        const proto::Panel *p = at.s->room().panel(panel);
        return p ? int(p->pictures) : -1;
    };
    sb.video.SetQuality(flow::kLow);
    if (!check("three people sit in a room, and two share a panel", t.sit() && t.a.ask(std::string("share 0") + kScreen) == "ok" && t.b.ask(std::string("share 0") + kScreen) == "ok")) return;
    t.net.pump();
    const uint8_t pa = proto::PanelId(t.a.s->me(), 0), pb = proto::PanelId(t.b.s->me(), 0);
    check("a panel says what its owner lets it have", pictures(t.a, pa) == 30 && pictures(t.c, pa) == 30 && t.a.has("\"pictures\":30"));
    check("...also when they picked before they came into the room", pictures(t.a, pb) == 15 && pictures(t.c, pb) == 15 && pictures(t.b, pb) == 15);

    sa.video.SetQuality(flow::kHigh);
    t.net.pump();
    check("the owner picks: everyone has it of their panel at once, and of nobody else's", pictures(t.b, pa) == 60 && pictures(t.c, pa) == 60 && pictures(t.b, pb) == 15);
    check("...which is what their programs are told of the room", t.c.has("\"pictures\":60") && t.c.has("\"pictures\":15") && !t.c.has("\"pictures\":30"));
    sb.video.SetQuality(flow::kBalanced);
    t.net.pump();
    check("a guest picks: the owner puts it in the room for everyone", pictures(t.a, pb) == 30 && pictures(t.c, pb) == 30 && pictures(t.b, pb) == 30 && pictures(t.c, pa) == 60);
    bool ok = t.b.ask(std::string("share 0") + kScreen) == "ok" && t.a.ask(std::string("share 1") + kScreen) == "ok";
    t.net.pump();
    check("shared again, or another one: the same", ok && pictures(t.c, pb) == 30 && pictures(t.c, proto::PanelId(t.a.s->me(), 1)) == 60);
    const std::string was = t.c.state();
    const size_t sent = t.a.messages + t.b.messages;
    sa.video.SetQuality(flow::kHigh), sb.video.SetQuality(flow::kBalanced);
    t.net.pump();
    check("picking what they have already changes nothing, and nobody is told", t.c.state() == was && t.a.messages + t.b.messages == sent);
    t.a.s->PanelPictures(0), t.a.s->PanelPictures(proto::kMostPanelPictures + 1), t.b.s->PanelPictures(0), t.b.s->PanelPictures(proto::kMostPanelPictures + 1);
    t.net.pump();
    check("...nor of pictures a second no panel may say", t.c.state() == was && t.a.messages + t.b.messages == sent);
    t.a.s->PanelPictures(1), t.b.s->PanelPictures(1);
    t.net.pump();
    bool least = pictures(t.c, pa) == 1 && pictures(t.c, pb) == 1;
    t.a.s->PanelPictures(proto::kMostPanelPictures), t.b.s->PanelPictures(proto::kMostPanelPictures);
    t.net.pump();
    check("...but of the least and the most one may", least && pictures(t.c, pa) == proto::kMostPanelPictures && pictures(t.c, pb) == proto::kMostPanelPictures);
    sb.video.SetQuality(flow::kLow), sb.video.SetQuality(flow::kBalanced);
    t.net.pump();
    check("...and picked again, it is what was picked", pictures(t.c, pb) == 30 && pictures(t.c, pa) == 60);

    ok = t.b.ask("leave") == "ok";
    t.net.pump();
    sb.video.SetQuality(flow::kHigh);
    ok = ok && join(t.a, t.b) && t.b.ask(std::string("share 2") + kScreen) == "ok";
    t.net.pump();
    check("picked between two rooms: the next one has it", ok && pictures(t.c, proto::PanelId(t.b.s->me(), 2)) == 60);
}

// What it costs whoever shows the panels: a decoder is to be good for the pictures a second
// the panel says (flow/budget.h).
static void decoders() {
    Table t;
    Seat &sa = t.sa, &sb = t.sb;
    if (!check("three people sit in a room, and one shares two large panels",
               t.sit() && t.a.ask(std::string("share 0") + kLarge) == "ok" && t.a.ask(std::string("share 1") + kLarge) == "ok"))
        return;
    using Words = std::vector<std::pair<uint8_t, bool>>;
    const uint8_t p0 = proto::PanelId(t.a.s->me(), 0), p1 = proto::PanelId(t.a.s->me(), 1);
    const int one = flow::Budget::Load(3840, 2160, 30);
    check("two of them are within what the decoder is asked for, at Balanced", 2 * one <= flow::Budget::kMostLoad && 3 * one > flow::Budget::kMostLoad);
    sb.video.SinkCame(p0, t.net.now), sb.video.SinkCame(p1, t.net.now);
    sb.video.Attention(p0, proto::kFocused, false, t.net.now);
    t.net.pump();
    sb.video.SinkDecodes(p0, true, t.net.now), sb.video.SinkDecodes(p1, true, t.net.now);
    check("both decode", sb.decode == (Words{{p0, true}, {p1, true}}) && sb.video.stills().empty());

    sa.video.SetQuality(flow::kHigh);
    t.net.pump();
    t.tick(0.01);
    check("their owner picks High: they cost twice as much, and the one the wearer sees less of is a still",
          sb.decode.size() == 3 && sb.decode.back() == std::make_pair(p1, false) && sb.video.stills() == std::vector<uint8_t>{p1});
    sb.video.SinkDecodes(p1, false, t.net.now);
    for (int i = 0; i < 5; ++i) t.tick(10.1);
    check("...for as long as they do", sb.decode.size() == 3 && sb.video.stills() == std::vector<uint8_t>{p1});
    sa.video.SetQuality(flow::kBalanced);
    t.net.pump();
    t.tick(0.01);
    check("Balanced again: both decode", sb.decode.size() == 4 && sb.decode.back() == std::make_pair(p1, true) && sb.video.stills().empty());

    bool ok = t.a.ask(std::string("share 1") + kScreen) == "ok";
    t.net.pump();
    sb.video.SinkDecodes(p1, true, t.net.now);
    sa.video.SetQuality(flow::kHigh);
    t.net.pump();
    t.tick(0.01);
    check("a panel made smaller costs less from then on: High has room for both", ok && sb.decode.size() == 4 && sb.video.stills().empty());
}

int main() {
    names();
    sources();
    heard();
    decoders();
    std::printf(failed ? "FAILED\n" : "all ok\n");
    return failed ? 1 : 0;
}
