// Offline test of the gaze calibration panel's answers (pointer/helper/calpanel.h): which relay
// messages take the dot, close the panel, or do nothing while it's up; whose panel a claim is;
// and which index pinches take the dot. Needs no SteamVR.
//
//   pointer/test/calpanel-test.sh
#include "../helper/calpanel.h"

#include <cstdio>

static int failed = 0, total = 0;

static void check(bool ok, const char *what) {
    ++total;
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failed;
    }
}

int main() {
    struct Case {
        const char *msg;
        CalPanelAnswer want;
    } cases[] = {
        {"btn trigger 1", CalPanelAnswer::Accept},
        {"gazekey left 1", CalPanelAnswer::Accept},
        {"calkey 1", CalPanelAnswer::Accept},  // Enter, from the relay while the panel is up
        {"precision mouse 1", CalPanelAnswer::Accept},
        {"precision keyboard 1", CalPanelAnswer::Accept},
        {"gazedrag mouse 1", CalPanelAnswer::Accept},
        {"gazedrag keyboard 1", CalPanelAnswer::Accept},
        {"btn b 1", CalPanelAnswer::Quit},
        {"gazekey right 1", CalPanelAnswer::Quit},
        {"btn trigger 0", CalPanelAnswer::Ignore},
        {"btn b 0", CalPanelAnswer::Ignore},
        {"btn a 1", CalPanelAnswer::Ignore},  // the relay's laser claim
        {"btn x 1", CalPanelAnswer::Ignore},
        {"btn system 1", CalPanelAnswer::Ignore},
        {"gazekey left 0", CalPanelAnswer::Ignore},
        {"gazekey right 0", CalPanelAnswer::Ignore},
        {"calkey 0", CalPanelAnswer::Ignore},
        {"calkey 10", CalPanelAnswer::Ignore},
        {"precision mouse 0", CalPanelAnswer::Ignore},
        {"gazedrag mouse 0", CalPanelAnswer::Ignore},
        {"move 0.1000 -0.2000", CalPanelAnswer::None},
        {"show", CalPanelAnswer::None},
        {"recenter", CalPanelAnswer::None},
        {"scroll 0 1", CalPanelAnswer::None},
        {"typing", CalPanelAnswer::None},
    };
    const char *names[] = {"none", "accept", "quit", "ignore"};
    for (const Case &c : cases) {
        const CalPanelAnswer got = calPanelAnswer(c.msg);
        char what[96];
        std::snprintf(what, sizeof what, "\"%s\": %s, want %s", c.msg, names[int(got)], names[int(c.want)]);
        check(got == c.want, what);
    }

    // Claims: whose panel, and what isn't one.
    struct Claim {
        const char *msg;
        bool is, on;
        const char *owner;
    } claims[] = {
        {"calpanel 1", true, true, "ft_gazed"},
        {"calpanel 0", true, false, "ft_gazed"},
        {"calpanel 1 ft_gazebench.1234", true, true, "ft_gazebench.1234"},
        {"calpanel 0 ft_gazebench.1234", true, false, "ft_gazebench.1234"},
        {"calpanel 1  ft-bench", true, true, "ft-bench"},
        {"calpanel 2", false, false, ""},
        {"calpanel", false, false, ""},
        {"calpanel 1 bad/name", false, false, ""},
        {"calpanel 1 two words", false, false, ""},
        {"calpanel 1 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", false, false, ""},  // 65
        {"calpaneling 1", false, false, ""},
    };
    for (const Claim &c : claims) {
        bool on = false;
        std::string owner;
        const bool is = calPanelClaim(c.msg, on, owner);
        char what[160];
        std::snprintf(what, sizeof what, "claim \"%s\": %d %d %s", c.msg, is, on, owner.c_str());
        check(is == c.is && (!is || (on == c.on && owner == c.owner)), what);
    }

    // Pinches: only ones begun after Start, one answer a call, a new file or counters going back
    // start counting again.
    fh_gestures_t g{};
    CalPinch p;
    g.pinch[0].begins = 5, g.pinch[1].begins = 2;
    p.Start();
    check(p.Began(g, 1) == -1, "pinch: the first look only counts");
    check(p.Began(g, 1) == -1, "pinch: nothing new");
    g.pinch[1].begins = 3;
    check(p.Began(g, 1) == 1, "pinch: the right hand's");
    check(p.Began(g, 1) == -1, "pinch: taken once");
    g.grip[0].begins = 9, g.pinch2[0].begins = 4;
    check(p.Began(g, 1) == -1, "pinch: grips and middle pinches aren't");
    g.pinch[0].begins = 6, g.pinch[1].begins = 4;
    check(p.Began(g, 1) == 0, "pinch: both at once is one answer");
    check(p.Began(g, 1) == -1, "pinch: ...and both are counted");
    g.pinch[0].begins = 7;
    check(p.Began(g, 2) == -1, "pinch: a new file starts over");
    g.pinch[0].begins = 1;
    check(p.Began(g, 2) == -1, "pinch: counters that went back start over");
    g.pinch[0].begins = 2;
    check(p.Began(g, 2) == 0, "pinch: then counts again");
    p.Start();
    g.pinch[0].begins = 3;
    check(p.Began(g, 2) == -1, "pinch: one from before the panel opened doesn't count");

    std::printf("%s: %d of %d cases\n", failed ? "FAILED" : "ok", total - failed, total);
    return failed ? 1 : 0;
}
