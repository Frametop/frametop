// The Frame's decoder among other people's panels (together/flow/budget.h): who decodes and who
// is a still, by how much the wearer sees of each, and what happens when the hardware has none.
#include "../flow/budget.h"

#include <cstdio>
#include <string>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

using flow::Budget;
constexpr int kHidden = proto::kHidden, kInView = proto::kInView, kFocused = proto::kFocused;
constexpr int kSmall = 1000;   // a load that never counts

// What Settle says, as "+3 -5": panel 3 is to decode, panel 5 to give its decoder up.
static std::string said(Budget &b, double now) {
    std::string out;
    for (const Budget::Word &w : b.Settle(now)) out += (out.empty() ? "" : " ") + std::string(w.decode ? "+" : "-") + std::to_string(int(w.panel));
    return out;
}
// ...and the programs do as they were told, until nothing more is said.
static std::string done(Budget &b, double now) {
    std::string out;
    for (std::vector<Budget::Word> words = b.Settle(now); !words.empty(); words = b.Settle(now)) {
        for (const Budget::Word &w : words) {
            out += (out.empty() ? "" : " ") + std::string(w.decode ? "+" : "-") + std::to_string(int(w.panel));
            b.Has(w.panel, w.decode);
        }
    }
    return out;
}

static void seen() {
    Budget b;
    check("no panels: nothing is said", said(b, 0).empty() && !b.decodes(3));
    b.Came(3, kSmall);
    check("a panel the wearer doesn't see has no decoder", said(b, 0).empty() && !b.decodes(3));
    b.Sees(3, kInView, 1);
    check("one they see is told to decode, once", said(b, 1) == "+3" && b.decodes(3) && said(b, 1.1).empty());
    b.Has(3, true);
    b.Sees(3, kFocused, 2);
    check("...and goes on when they focus it", said(b, 2).empty() && b.decodes(3));
    b.Sees(3, kHidden, 3);
    check("one they see no longer gives its decoder up", said(b, 3) == "-3" && !b.decodes(3) && said(b, 3.1).empty());
    b.Has(3, false);
    b.Sees(3, kInView, 4);
    check("...and has one again when they do", said(b, 4) == "+3");
    b.Went(3);
    check("a panel whose program went is forgotten", said(b, 5).empty() && !b.decodes(3));
    b.Sees(3, kFocused, 6), b.Has(3, true), b.Refused(3, 6);
    check("...and words about it mean nothing", said(b, 6).empty());

    Budget again;
    again.Came(3, kSmall), again.Sees(3, kInView, 0);
    done(again, 0);
    again.Came(3, kSmall), again.Sees(3, kInView, 1);
    check("a program that comes for a panel another showed begins anew: it is told to decode", !again.decodes(3) && said(again, 1) == "+3");

    Budget full;
    for (int id = 0; id < 6; ++id) full.Came(uint8_t(id), kSmall), full.Sees(uint8_t(id), kInView, 0);
    done(full, 0);
    full.Sees(5, kHidden, 1);
    const std::string gave = said(full, 1);
    full.Sees(5, kInView, 1.1);
    check("one seen again before it gave its decoder up keeps it, with every place taken", gave == "-5" && said(full, 1.1) == "+5");

    Budget opening;
    for (int id = 0; id < 6; ++id) opening.Came(uint8_t(id), kSmall), opening.Sees(uint8_t(id), kInView, 0);
    bool ok = said(opening, 0) == "+0 +1 +2 +3 +4 +5";
    for (int id = 0; id < 5; ++id) opening.Has(uint8_t(id), true);
    opening.Sees(4, kHidden, 1);
    opening.Came(6, kSmall), opening.Sees(6, kInView, 1);
    check("a decoder being opened counts like one that is open, and so does one being given up", ok && said(opening, 1) == "-4" && said(opening, 1.1).empty());
    opening.Has(4, false);
    check("...until it is", said(opening, 1.2) == "+6");

    check("a picture's load is its macroblocks a second, a part of one counted whole",
          Budget::Load(1920, 1080, 30) == 120 * 68 * 30 && Budget::Load(16, 16, 1) == 1 && Budget::Load(17, 16, 2) == 4 && Budget::Load(640, 360, 30) == 40 * 23 * 30);
}

static void ranks() {
    Budget b;
    for (int id = 0; id < 8; ++id) b.Came(uint8_t(id), kSmall), b.Sees(uint8_t(id), kInView, 0);
    check("of more panels than may decode at once, six do", Budget::kMostDecoders == 6 && done(b, 0) == "+0 +1 +2 +3 +4 +5" && !b.decodes(6) && !b.decodes(7));
    check("...and the others stay stills", said(b, 100).empty());

    b.Sees(7, kFocused, 101);
    check("one the wearer focuses takes the place of one only in view: that one gives its decoder up first", said(b, 101) == "-5" && !b.decodes(7));
    check("...and until it has, nothing more is said", said(b, 101.5).empty());
    b.Has(5, false);
    check("...then the focused one decodes", done(b, 102) == "+7" && b.decodes(7) && !b.decodes(5));

    b.Sees(6, kFocused, 103);
    check("a second one focused takes the next one's", done(b, 103) == "-4 +6");
    b.Sees(7, kInView, 104), b.Sees(6, kInView, 105);
    check("no longer focused, they keep their decoders: nobody ranks above them", said(b, 105).empty());
    b.Sees(3, kHidden, 106);
    check("a decoder given up goes to a still: of two never focused, the one with the lower id", done(b, 106) == "-3 +4");
    b.Sees(0, kHidden, 107);
    check("...and the next to the other", done(b, 107) == "-0 +5");

    Budget c;
    for (int id = 0; id < 8; ++id) c.Came(uint8_t(id), kSmall), c.Sees(uint8_t(id), kInView, 0);
    done(c, 0);
    c.Sees(6, kFocused, 1), c.Sees(6, kInView, 2);
    c.Sees(7, kFocused, 3), c.Sees(7, kInView, 4);
    check("of two stills focused once, each takes a place...", done(c, 4) == "-4 -5 +7 +6");
    c.Sees(4, kFocused, 5), c.Sees(4, kInView, 6);
    check("...from one never focused, while there is one", done(c, 6) == "-3 +4");
    c.Sees(3, kFocused, 7);
    const std::string first = done(c, 7);
    c.Sees(3, kInView, 9);
    check("......and so do the next", first == "-2 +3" && done(c, 9).empty());
    c.Sees(5, kFocused, 10), c.Sees(5, kInView, 11);
    c.Sees(2, kFocused, 12), c.Sees(2, kInView, 13);
    check("......until all that decode were focused once", done(c, 13) == "-0 -1 +2 +5");
    c.Sees(1, kFocused, 14), c.Sees(1, kInView, 15);
    check("then the one focused longest ago loses its decoder to one focused since", done(c, 15) == "-6 +1");
    c.Sees(0, kFocused, 16);
    check("...by when it stopped being focused, not when it began", done(c, 16) == "-7 +0");
    c.Went(4);
    check("a program that went leaves its place to the still focused last", done(c, 17) == "+7");

    Budget d;
    for (int id = 0; id < 8; ++id) d.Came(uint8_t(id), kSmall), d.Sees(uint8_t(id), kInView, 0);
    done(d, 0);
    d.Sees(4, kHidden, 1), d.Sees(5, kHidden, 1);
    check("two give their decoders up at once", said(d, 1) == "-4 -5");
    d.Has(4, false);
    check("...and a still is told for each place as it comes free", said(d, 1.1) == "+6" && said(d, 1.15).empty());
    d.Has(5, false);
    check("......the next for the next", said(d, 1.2) == "+7");

    // two panels of which only one may decode at a time
    Budget e;
    e.Came(0, Budget::kMostLoad), e.Came(1, Budget::kMostLoad);
    e.Sees(0, kFocused, 0), e.Sees(1, kInView, 0);
    bool ok = done(e, 0) == "+0";
    e.Sees(1, kFocused, 10);
    check("of two focused at once, the one focused later decodes", ok && done(e, 10) == "-0 +1");
    e.Sees(1, kInView, 20);
    e.Sees(0, kInView, 50);
    check("of two no longer focused, the one that was until later", done(e, 50) == "-1 +0");
    e.Sees(0, kFocused, 60);
    e.Sees(0, kInView, 70), e.Sees(1, kFocused, 70);
    check("one focused at the moment another stops being comes first", done(e, 70) == "-0 +1");
}

static void load() {
    const int big = Budget::kMostLoad / 2 - 100;
    Budget b;
    for (int id = 0; id < 3; ++id) b.Came(uint8_t(id), big), b.Sees(uint8_t(id), kInView, 0);
    b.Came(3, 150), b.Sees(3, kInView, 0);
    check("panels decode while their load fits, and a small one after a large one that doesn't", done(b, 0) == "+0 +1 +3" && !b.decodes(2));
    b.Sees(2, kFocused, 1);
    check("a large one focused takes what it needs from the ones below it", said(b, 1) == "-1" && !b.decodes(2));
    b.Has(1, false);
    check("...once that is free", done(b, 2) == "+2");

    Budget c;
    c.Came(0, Budget::kMostLoad * 2), c.Came(1, kSmall);
    c.Sees(0, kFocused, 0), c.Sees(1, kInView, 0);
    check("one larger than everything allowed still decodes when it is the one looked at, alone", done(c, 0) == "+0" && !c.decodes(1));
    c.Sees(0, kHidden, 1);
    check("...and the others when it isn't", done(c, 1) == "-0 +1");
    Budget d;
    d.Came(0, Budget::kMostLoad), d.Came(1, 1), d.Came(2, 1);
    for (int id = 0; id < 3; ++id) d.Sees(uint8_t(id), kInView, 0);
    check("the load allowed is allowed whole, and nothing past it", done(d, 0) == "+0");
    Budget e;
    e.Came(0, Budget::kMostLoad - 5), e.Came(1, 5), e.Came(2, 1);
    for (int id = 0; id < 3; ++id) e.Sees(uint8_t(id), kInView, 0);
    check("...also when two make it up", done(e, 0) == "+0 +1");

    Budget f;
    for (int id = 0; id < 4; ++id) f.Came(uint8_t(id), big);
    f.Sees(0, kInView, 0), f.Sees(1, kInView, 0);
    done(f, 0);
    for (int id = 0; id < 4; ++id) f.Sees(uint8_t(id), id < 2 ? kHidden : kInView, 1);
    check("two large ones give their decoders up for two the wearer sees", said(f, 1) == "-0 -1");
    f.Has(0, false);
    check("...and each of those is told when the load it needs is free", said(f, 1.1) == "+2");
    f.Has(1, false);
    check("......", said(f, 1.2) == "+3");
}

static void refused() {
    Budget b;
    for (int id = 0; id < 4; ++id) b.Came(uint8_t(id), kSmall), b.Sees(uint8_t(id), kInView, 0);
    check("four panels in view are told to decode", said(b, 0) == "+0 +1 +2 +3");
    b.Has(0, true), b.Has(1, true);
    b.Refused(2, 1);
    check("the hardware has none for the third, which outranks nobody: it is a still, and the others go on", said(b, 1).empty() && !b.decodes(2) && b.decodes(3));
    b.Refused(3, 1.1);
    check("...and so is the fourth", said(b, 1.1).empty() && !b.decodes(3));
    check("no more are tried for ten seconds", said(b, 11).empty());
    b.Came(4, kSmall), b.Sees(4, kInView, 11);
    check("...then one more: the next in rank that isn't set aside", said(b, 11.2) == "+4");
    b.Has(4, true);
    check("...and after ten more, another, but the ones the hardware refused wait thirty", said(b, 21.3).empty() && said(b, 30.9).empty());
    check("...and then try again", said(b, 31.05) == "+2" && said(b, 31.07).empty());
    b.Has(2, true);
    check("......each when its thirty seconds are over", said(b, 31.2) == "+3");
    b.Refused(3, 31.3);
    check("refused again, it waits again, and what decodes is what the hardware gave", said(b, 41.4).empty() && said(b, 61.2).empty() && said(b, 61.3) == "+3");

    Budget c;
    for (int id = 0; id < 6; ++id) c.Came(uint8_t(id), kSmall), c.Sees(uint8_t(id), kInView, 0);
    done(c, 0);
    c.Refused(0, 1);
    c.Came(6, kSmall), c.Sees(6, kInView, 2);
    check("one that decoded and is refused later (the hardware has no room for its pictures) leaves its place: ten seconds on a still is tried there",
          !c.decodes(0) && said(c, 2).empty() && said(c, 11.2) == "+6");

    Budget d;
    d.Came(0, kSmall), d.Came(1, kSmall);
    d.Sees(0, kFocused, 0), d.Sees(1, kInView, 0);
    said(d, 0);
    d.Refused(1, 0), d.Refused(0, 0.1);
    check("the hardware has none at all: the focused one doesn't wait for the place of a still", said(d, 0.1).empty() && said(d, 10.2).empty());
    check("...and each tries again when its thirty seconds are over", said(d, 30.05) == "+1" && said(d, 30.2) == "+0");
}

static void swapped() {
    Budget b;
    for (int id = 0; id < 3; ++id) b.Came(uint8_t(id), kSmall), b.Sees(uint8_t(id), kInView, 0);
    done(b, 0);
    b.Came(9, kSmall), b.Sees(9, kFocused, 1);
    check("a panel that comes focused is told to decode with the rest", said(b, 1) == "+9");
    b.Refused(9, 1.1);
    check("the hardware has none: the one that ranks last gives its decoder up for it", said(b, 1.1) == "-2" && !b.decodes(9));
    b.Has(2, false);
    check("...and then it tries again", said(b, 1.2) == "+9");
    b.Has(9, true);
    check("with one, the other stays a still", said(b, 5).empty() && b.decodes(9) && !b.decodes(2));
    check("...until one more is tried", said(b, 11.0).empty() && said(b, 11.2) == "+2");
    b.Refused(2, 11.3);
    check("......which the hardware may refuse: the wearer sees no other less, so it waits", said(b, 11.3).empty() && said(b, 41.2).empty() && said(b, 41.4) == "+2");
    b.Has(2, true);
    b.Came(5, kSmall), b.Sees(5, kInView, 50);
    check("a panel that is seen as much as the others takes nobody's place", said(b, 50) == "+5");
    b.Refused(5, 50.1);
    check("...when the hardware has none for it", said(b, 50.1).empty() && said(b, 60).empty());

    // the same, and the hardware has none for it in the other's place either
    Budget c;
    for (int id = 0; id < 3; ++id) c.Came(uint8_t(id), kSmall), c.Sees(uint8_t(id), kInView, 0);
    done(c, 0);
    c.Came(9, kSmall), c.Sees(9, kFocused, 1);
    said(c, 1), c.Refused(9, 1.1);
    check("a panel is refused, and takes another's place", said(c, 1.1) == "-2");
    c.Has(2, false);
    said(c, 1.2), c.Refused(9, 1.3);
    check("refused there too, it is left a still and the other has its decoder back at once", said(c, 1.3) == "+2" && !c.decodes(9));
    c.Has(2, true);
    check("...for thirty seconds", said(c, 31.2).empty() && said(c, 31.4) == "+9");
    c.Refused(9, 31.5);
    check("then it may take the other's place again, once", said(c, 31.5) == "-2");
    c.Has(2, false);
    c.Sees(9, kHidden, 31.6);
    check("a panel that is looked away from while another makes room leaves it its decoder", said(c, 31.6) == "+2");
    c.Has(2, true);
    c.Sees(9, kFocused, 40);
    check("looked at again while the hardware is short, it takes the other's place without asking the hardware first", said(c, 40) == "-2");
    c.Has(2, false);
    check("...and tries there", said(c, 40.1) == "+9");
    c.Refused(9, 40.2);
    check("......once: refused, it waits, and the other has its decoder back", said(c, 40.2) == "+2" && said(c, 55).empty());
    c.Has(2, true);
    c.Came(4, kSmall), c.Sees(4, kInView, 56);
    check("what the hardware gave before isn't lowered by that: a place past it is tried as before", said(c, 56) == "+4");

    // refused while others are still opening theirs
    Budget d;
    for (int id = 0; id < 5; ++id) d.Came(uint8_t(id), kSmall), d.Sees(uint8_t(id), kInView, 0);
    said(d, 0);
    d.Has(0, true), d.Has(1, true);
    d.Refused(4, 0.1);
    check("one refused while others still open theirs: those go on", said(d, 0.1).empty() && d.decodes(2) && d.decodes(3) && !d.decodes(4));
    d.Refused(3, 0.2);
    d.Has(2, true);
    check("...and each of them the hardware refuses lowers what is tried by one", said(d, 0.3).empty() && said(d, 10.1).empty());
    d.Went(2);
    check("a place that comes free within what the hardware gave is taken once the refused may try", said(d, 12).empty() && said(d, 30.15) == "+4");

    // refused while one that gave its decoder up still has it
    Budget e;
    e.Came(0, kSmall), e.Came(1, kSmall);
    e.Sees(0, kInView, 0), e.Sees(1, kInView, 0);
    done(e, 0);
    e.Sees(1, kHidden, 1), e.Came(2, kSmall), e.Sees(2, kInView, 1);
    bool ok = said(e, 1) == "-1 +2";
    e.Refused(2, 1.1);
    check("one refused while a panel the wearer stopped seeing still has its decoder waits for that", ok && said(e, 1.1).empty());
    e.Has(1, false);
    check("...and tries again: the hardware gave two", said(e, 1.2) == "+2");

    // refused later, with a decoder
    Budget g;
    for (int id = 0; id < 3; ++id) g.Came(uint8_t(id), kSmall), g.Sees(uint8_t(id), kInView, 0);
    done(g, 0);
    g.Came(9, kSmall), g.Sees(9, kFocused, 1);
    said(g, 1), g.Refused(9, 1.1);
    said(g, 1.1), g.Has(2, false);
    ok = said(g, 1.2) == "+9";
    g.Has(9, true);
    g.Refused(0, 2);
    check("one that went on decoding while another made room, refused later: a still, and what is tried is what the hardware gives now",
          ok && said(g, 2).empty() && !g.decodes(0) && !g.decodes(2));
    g.Refused(9, 3);
    check("one that decoded in another's place, refused later: it takes the next one's", said(g, 3) == "-1");

    // looked away from while another makes room, and looked at again later
    Budget h;
    for (int id = 0; id < 3; ++id) h.Came(uint8_t(id), kSmall), h.Sees(uint8_t(id), kInView, 0);
    done(h, 0);
    h.Came(9, kSmall), h.Sees(9, kFocused, 1);
    said(h, 1), h.Refused(9, 1.1);
    said(h, 1.1), h.Has(2, false);
    h.Sees(9, kHidden, 1.2);
    said(h, 1.2), h.Has(2, true);
    h.Sees(9, kFocused, 20);
    check("a panel looked at again when one more may be tried is tried", said(h, 20) == "+9");
    h.Refused(9, 20.1);
    check("...and refused, takes the place of one seen less, as if for the first time", said(h, 20.1) == "-2");

    Budget i;
    i.Came(0, kSmall), i.Came(1, kSmall);
    i.Sees(0, kInView, 0), i.Sees(1, kInView, 0);
    done(i, 0);
    i.Came(9, kSmall), i.Sees(9, kFocused, 1), i.Sees(9, kInView, 2);
    ok = said(i, 2) == "+9";
    i.Refused(9, 2.1);
    check("one only in view takes the place of one in view that was focused longer ago, or never", ok && said(i, 2.1) == "-1");
}

int main() {
    seen();
    ranks();
    load();
    refused();
    swapped();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
