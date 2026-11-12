// Input from the other people of a Together room (screens/peerinput.h), against a desk that
// writes down what it was asked to do: where their pointer goes, what they hold and let go,
// what isn't a screen that takes it, a window of the wearer's PC with a seat of its own, whose
// turn the pointer is, and when the wearer's own hand is told.
#include <assert.h>
#include "../peerinput.h"

static char g_log[4096];
static int g_typing = 1;   // the screen typing goes to (from 0); -1: not the desktop's

static bool size(void *ctx, int screen, int *width, int *height) {
    (void)ctx;
    if (screen == 0) return *width = 1920, *height = 1080, true;
    if (screen == 1) return *width = 3440, *height = 1440, true;
    if (screen == 100) return *width = 1280, *height = 720, true;    // two windows of the wearer's PC, each on a stream
    if (screen == 101) return *width = 1920, *height = 1080, true;
    return false;   // the rest: a spare, a room's panel, nothing
}

static int seat(void *ctx, int screen) {
    (void)ctx;
    return screen >= 100 ? screen + 1 : 0;
}

static void pointer(void *ctx, const struct ft_event *e) {
    (void)ctx;
    char line[96];
    if (e->type == FT_MOTION) snprintf(line, sizeof line, "m%d:%.0f,%.0f ", e->screen, e->x, e->y);
    else if (e->type == FT_BUTTON) snprintf(line, sizeof line, "b%d:%.0f,%.0f:%u%s ", e->screen, e->x, e->y, e->button, e->pressed ? "+" : "-");
    else snprintf(line, sizeof line, "s%d:%.2f,%.2f ", e->screen, e->dx, e->dy);
    strcat(g_log, line);
}

static bool key(void *ctx, int screen, uint32_t code, bool pressed) {
    (void)ctx;
    if (pressed && screen < 100 && screen != g_typing) return false;
    char line[32];
    if (screen >= 100) snprintf(line, sizeof line, "k%d:%u%s ", screen, code, pressed ? "+" : "-");
    else snprintf(line, sizeof line, "k%u%s ", code, pressed ? "+" : "-");
    strcat(g_log, line);
    return true;
}

static struct ft_peer g_peer;
static const struct ft_peer_desk g_desk = {NULL, size, seat, pointer, key};

// The reply to "peer <words>" at `now`.
static const char *at(const char *words, uint32_t now) { return ft_peer_command(&g_peer, &g_desk, words, now); }
static const char *say(const char *words) { return at(words, 1000); }
// What the desk was asked to do since the last look.
static bool did(const char *what) {
    const bool same = !strcmp(g_log, what);
    if (!same) fprintf(stderr, "  the desk was asked \"%s\", not \"%s\"\n", g_log, what);
    g_log[0] = 0;
    return same;
}
static bool ok(const char *reply) { return !strcmp(reply, "ok"); }
static bool error(const char *reply) { return !strncmp(reply, "error ", 6); }

int main(void) {
    ft_peer_init(&g_peer);

    // Their pointer: 0 to 65535 across and down the screen it is on, in that screen's pixels.
    assert(ok(say("1 move 0 0")) && did("m0:0,0 "));
    assert(ok(say("1 move 65535 65535")) && did("m0:1920,1080 "));
    assert(ok(say("2 move 32768 16384")) && did("m1:1720,360 "));
    // A button goes down and up where they last moved to, whatever the seat's pointer did since.
    assert(ok(say("2 button 1 1")) && did("b1:1720,360:272+ "));
    assert(ok(say("2 move 0 65535")) && did("m1:0,1440 "));
    assert(ok(say("2 button 1 0")) && did("b1:0,1440:272- "));
    assert(ok(say("2 button 2 1")) && ok(say("2 button 3 1")) && did("b1:0,1440:273+ b1:0,1440:274+ "));
    // Held already, or not held: nothing goes to the seat, so wlroots' count of presses stays theirs.
    assert(!strcmp(say("2 button 2 1"), "ok held already") && !strcmp(say("2 button 1 0"), "ok not held") && did(""));
    assert(ok(say("2 button 2 0")) && ok(say("2 button 3 0")) && did("b1:0,1440:273- b1:0,1440:274- "));
    // Not on the screen their pointer is on: they move there first.
    assert(error(say("1 button 1 1")) && error(say("1 scroll 0 120")) && did(""));
    // A scroll, 120 a notch, goes where they point: a motion back there first.
    assert(ok(say("2 scroll 0 -120")) && did("m1:0,1440 s1:0.00,-1.00 "));
    assert(ok(say("2 scroll 240 60")) && did("m1:0,1440 s1:2.00,0.50 "));

    // Keys go only where typing is that screen's, and come up only if they went down here.
    assert(!strncmp(say("1 key 30 1"), "ok not taken", 12) && did(""));
    assert(!strcmp(say("1 key 30 0"), "ok not held") && did(""));
    assert(ok(say("2 key 42 1")) && ok(say("2 key 30 1")) && did("k42+ k30+ "));
    assert(!strcmp(say("2 key 30 1"), "ok held already") && did(""));
    g_typing = -1;   // typing went to Steam: a held key still comes up, a new one doesn't go down
    assert(!strncmp(say("2 key 31 1"), "ok not taken", 12) && ok(say("2 key 30 0")) && did("k30- "));
    assert(!strcmp(say("2 key 30 0"), "ok not held") && did(""));
    g_typing = 1;

    // Let go: every key, the last pressed first, then every button, where their pointer is.
    assert(ok(say("2 key 29 1")) && ok(say("2 key 56 1")) && ok(say("2 button 1 1")) && ok(say("2 button 3 1")));
    assert(did("k29+ k56+ b1:0,1440:272+ b1:0,1440:274+ "));
    assert(ok(say("letgo")) && did("k56- k29- k42- b1:0,1440:272- b1:0,1440:274- "));
    assert(ok(say("letgo")) && did("") && g_peer.n_keys == 0 && !g_peer.buttons);
    // ...and what they then release was let go already.
    assert(!strcmp(say("2 key 42 0"), "ok not held") && !strcmp(say("2 button 1 0"), "ok not held") && did(""));

    // Eight keys at once, as ft-together's desk allows, and no ninth.
    for (int i = 0; i < FT_PEER_KEYS; ++i) {
        char words[32];
        snprintf(words, sizeof words, "2 key %d 1", 16 + i);
        assert(ok(say(words)));
    }
    assert(error(say("2 key 50 1")) && ok(say("letgo")));
    g_log[0] = 0;

    // Only the screens the desk has for them: not a spare, a room's panel, or a number that is none.
    assert(error(say("3 move 1 1")) && error(say("0 move 1 1")) && error(say("-1 move 1 1")) && error(say("103 key 30 1")) && error(say("116 move 1 1")) && did(""));
    // What isn't one of these is refused, and nothing is done.
    const char *const bad[] = {"", "1", "1 move", "1 move 1", "1 move 1 1 1", "1 move -1 0", "1 move 0 65536", "1 fly 1 1",
                               "1 button 0 1", "1 button 4 1", "1 button 1 2", "2 scroll 0 0", "2 scroll 2401 0", "2 scroll 0 -2401",
                               "2 key 0 1", "2 key 256 1", "2 key 30 2", "2 key 30 -1", "x move 1 1", "1 move 1 x", "on now", "letgo 1",
                               "1 move 99999999999 1"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) assert(error(say(bad[i])) && did(""));

    // A button held from one screen to the next comes up on the next, as the wearer's drag does.
    assert(ok(say("2 move 100 100")) && ok(say("2 button 1 1")) && ok(say("1 move 65535 0")) && ok(say("1 button 1 0")));
    assert(did("m1:5,2 b1:5,2:272+ m0:1920,0 b0:1920,0:272- "));

    // A window of the wearer's PC that stands here as a panel: its input goes through its own
    // stream, a seat apart from the desktop's and from every other window's.
    assert(ok(say("101 move 32768 32768")) && did("m100:640,360 "));
    assert(ok(say("101 button 1 1")) && ok(say("101 move 65535 0")) && ok(say("101 button 1 0")) && did("b100:640,360:272+ m100:1280,0 b100:1280,0:272- "));
    assert(ok(say("101 scroll 0 120")) && did("m100:1280,0 s100:0.00,1.00 "));
    // A key goes down only at the seat their pointer is at...
    assert(ok(say("101 key 30 1")) && did("k100:30+ "));
    assert(!strcmp(say("2 key 31 1"), "ok not taken: their pointer is at another machine") &&
           !strcmp(say("102 key 31 1"), "ok not taken: their pointer is at another machine") && did("") && g_peer.n_keys == 1);
    // ...and comes up there, whichever screen its release names.
    assert(ok(say("2 key 30 0")) && did("k100:30- "));
    // Before their pointer goes to another seat, what they hold comes up where it went down: on the way to the desktop,
    assert(ok(say("101 key 29 1")) && ok(say("101 button 1 1")) && did("k100:29+ b100:1280,0:272+ "));
    assert(ok(say("1 move 0 0")) && did("k100:29- b100:1280,0:272- m0:0,0 ") && !g_peer.buttons && !g_peer.n_keys);
    assert(!strcmp(say("1 button 1 0"), "ok not held") && !strcmp(say("101 key 29 0"), "ok not held") && did(""));
    // on the way from it,
    assert(ok(say("2 move 0 0")) && ok(say("2 key 30 1")) && ok(say("2 button 1 1")) && did("m1:0,0 k30+ b1:0,0:272+ "));
    assert(ok(say("101 move 0 0")) && did("k30- b1:0,0:272- m100:0,0 "));
    // and from one window of the PC's to the next.
    assert(ok(say("101 button 3 1")) && ok(say("102 move 0 65535")) && did("b100:0,0:274+ b100:0,0:274- m101:0,1080 "));
    // The wearer's own hand lets go of what they hold there, as on the desktop.
    assert(ok(say("102 key 42 1")) && ok(say("102 button 2 1")) && did("k101:42+ b101:0,1080:273+ "));
    assert(!ft_peer_own(&g_peer, &g_desk, 1000) && did("k101:42- b101:0,1080:273- "));
    // A key that went down on the desktop before they ever pointed anywhere is the desktop's
    // seat's: it comes up when they go to the PC's window, where no key goes down before they do.
    struct ft_peer first;
    ft_peer_init(&first);
    assert(!strcmp(ft_peer_command(&first, &g_desk, "101 key 30 1", 1000), "ok not taken: their pointer is at another machine") && did(""));
    assert(ok(ft_peer_command(&first, &g_desk, "2 key 30 1", 1000)) && ok(ft_peer_command(&first, &g_desk, "101 move 0 0", 1000)) && did("k30+ k30- m100:0,0 "));

    // A screen that goes while they hold a button there: letting go forgets it and asks nothing of the seat.
    assert(ok(say("2 move 100 100")) && ok(say("2 button 1 1")) && ok(say("2 key 30 1")));
    g_log[0] = 0;
    g_peer.screen = 5;
    assert(ok(say("letgo")) && did("k30- ") && !g_peer.buttons);
    // ...and the next thing they say finds it gone: what they hold comes up, and they point nowhere.
    g_peer.screen = 1;
    assert(ok(say("2 button 3 1")) && ok(say("2 key 30 1")) && did("b1:5,2:274+ k30+ "));
    g_peer.screen = 5;
    assert(error(say("2 button 3 0")) && did("k30- ") && g_peer.screen == -1 && !g_peer.buttons && !g_peer.n_keys);
    // What the seat let go of itself isn't theirs to let go again.
    assert(ok(say("2 move 0 0")) && ok(say("2 button 1 1")) && ok(say("2 key 30 1")) && did("m1:0,0 b1:0,0:272+ k30+ "));
    ft_peer_buttons_gone(&g_peer), ft_peer_keys_gone(&g_peer);
    assert(ok(say("letgo")) && did(""));

    // Whose turn the pointer is. The wearer's hand: a press, a scroll, a spin to the front, and
    // anything with a button of theirs down. Not a pointer that rests or passes, nor a release
    // of nothing, nor anything while the button that is down is the other's.
    const struct ft_event move = {.type = FT_MOTION}, press = {.type = FT_BUTTON, .pressed = true}, release = {.type = FT_BUTTON},
                          scroll = {.type = FT_SCROLL}, front = {.type = FT_FRONT}, leave = {.type = FT_LEAVE};
    assert(ft_peer_hand(&g_peer, &press, false) && ft_peer_hand(&g_peer, &scroll, false) && ft_peer_hand(&g_peer, &front, false));
    assert(!ft_peer_hand(&g_peer, &move, false) && !ft_peer_hand(&g_peer, &release, false) && !ft_peer_hand(&g_peer, &leave, false));
    assert(ft_peer_hand(&g_peer, &move, true) && ft_peer_hand(&g_peer, &release, true) && ft_peer_hand(&g_peer, &leave, true));
    assert(ok(at("2 button 1 1", 20000)) && did("b1:0,0:272+ "));
    assert(!ft_peer_hand(&g_peer, &move, true) && !ft_peer_hand(&g_peer, &release, true) && !ft_peer_hand(&g_peer, &leave, true));
    assert(ft_peer_hand(&g_peer, &press, true));
    // The pointer is the other's while they hold a button, however long, and for a moment after
    // they moved, pressed or scrolled: not after a release alone, a key or a refusal.
    assert(ft_peer_holds(&g_peer, 20000) && ft_peer_holds(&g_peer, 80000));
    assert(ok(at("2 button 1 0", 30000)) && did("b1:0,0:272- ") && !ft_peer_holds(&g_peer, 30000));
    assert(ok(at("2 move 0 0", 40000)) && ft_peer_holds(&g_peer, 40000) && ft_peer_holds(&g_peer, 41499) && !ft_peer_holds(&g_peer, 41500));
    assert(ok(at("2 scroll 0 120", 50000)) && ft_peer_holds(&g_peer, 51000) && !ft_peer_holds(&g_peer, 51500));
    assert(ok(at("2 key 30 1", 60000)) && ok(at("2 key 30 0", 60001)) && error(at("9 move 0 0", 60002)) && !ft_peer_holds(&g_peer, 60003));
    g_log[0] = 0;
    // The wearer's hand takes it: what the other holds comes up at once, and the pointer is the wearer's.
    assert(ok(at("2 move 65535 65535", 70000)) && ok(at("2 key 42 1", 70001)) && ok(at("2 button 2 1", 70002)) && ft_peer_holds(&g_peer, 70003));
    g_log[0] = 0;
    assert(!ft_peer_own(&g_peer, &g_desk, 70004) && did("k42- b1:3440,1440:273- ") && !ft_peer_holds(&g_peer, 70004));   // (nobody said "peer on")
    // ...until the other is at it again, which ft-together passes on only once the wearer's hand is off (room/desk.h).
    assert(!strcmp(at("2 button 2 0", 70005), "ok not held") && !ft_peer_holds(&g_peer, 70005));
    assert(ok(at("2 move 0 0", 72000)) && ft_peer_holds(&g_peer, 72001));
    g_log[0] = 0;

    // The wearer's own hand: told only while someone may be given a screen, and not too often.
    struct ft_peer own;
    ft_peer_init(&own);
    assert(!ft_peer_own(&own, &g_desk, 1000));                                       // nobody said "peer on"
    assert(ok(ft_peer_command(&own, &g_desk, "on", 2000)));
    assert(ft_peer_own(&own, &g_desk, 2100) && !ft_peer_own(&own, &g_desk, 2200) && !ft_peer_own(&own, &g_desk, 2599));   // once, then a moment's quiet
    assert(ft_peer_own(&own, &g_desk, 2600) && !ft_peer_own(&own, &g_desk, 2700));
    assert(ft_peer_own(&own, &g_desk, 6999) && !ft_peer_own(&own, &g_desk, 7000) && !ft_peer_own(&own, &g_desk, 9000));   // "peer on" stands five seconds
    assert(ok(ft_peer_command(&own, &g_desk, "on", 9000)) && ft_peer_own(&own, &g_desk, 9001));
    // ...across the clock going round
    assert(ok(ft_peer_command(&own, &g_desk, "on", 0xfffffff0u)) && ft_peer_own(&own, &g_desk, 0xfffffffeu) && !ft_peer_own(&own, &g_desk, 10) &&
           ft_peer_own(&own, &g_desk, 600) && !ft_peer_own(&own, &g_desk, 6000));
    assert(did(""));
    puts("ok");
    return 0;
}
