// Steam's own windows as panels of ours (screens/steamwin.h): what each one is, when it's taken,
// given back, taken again after Steam tags it itself, and given up on.
#include "../steamwin.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

using steamwin::Action;
using gsforward::Event;

static Event Ev(Event::Type t, uint32_t id, const char *title = "", uint64_t target = 0) {
    Event e;
    e.type = t, e.win.id = id, e.win.title = title, e.win.target = target;
    e.win.kind = gsforward::Kind::Main, e.win.w = 1920, e.win.h = 1080;
    return e;
}
static bool Has(const std::vector<Action> &a, Action::Type t, uint64_t overlay = ~0ull) {
    for (const Action &x : a)
        if (x.type == t && (overlay == ~0ull || x.overlay == overlay)) return true;
    return false;
}

int main() {
    const uint64_t steams = 0x380000027ull, ours = 0x99ull;
    {   // off: nothing taken; a tag of ours left behind goes back to Steam
        steamwin::Tracker t;
        t.SetSteams(steams);
        auto a = t.On(Ev(Event::Mapped, 5, "SteamVR", steams), 0);
        check("off: Steam's window left alone", a.empty());
        steamwin::Tracker u;
        u.SetSteams(steams);
        a = u.On(Ev(Event::Mapped, 5, "SteamVR", 0x1234), 0);
        check("off: a stale tag of ours back to Steam's overlay", a.size() == 1 && Has(a, Action::Tag, steams));
        steamwin::Tracker v;
        v.SetSteams(steams);
        a = v.On(Ev(Event::Mapped, 6, "SteamVR", 0), 0);
        check("off: an untagged one isn't tagged (gamescope's own panel is Steam's business)", a.empty());
    }
    {   // on: made, tagged, shown; other windows ignored
        steamwin::Tracker t;
        t.SetSteams(steams);
        check("on before it maps: nothing yet", t.Switch(true).empty());
        auto a = t.On(Ev(Event::Mapped, 5, "SteamVR", steams), 0);
        check("mapped: a panel asked for", a.size() == 1 && a[0].type == Action::Make && a[0].win.id == 5);
        a = t.Made(ours);
        check("made: tagged ours, shown", a.size() == 2 && Has(a, Action::Tag, ours) && Has(a, Action::Show));
        Event m = Ev(Event::Moved, 5);
        m.win.w = 1280, m.win.h = 720;
        a = t.On(m, 0);
        check("its size changes: the panel follows", a.size() == 1 && a[0].type == Action::Move && a[0].win.w == 1280);
        check("our own tag coming back isn't Steam's", t.On(Ev(Event::Retarget, 5, "", ours), 0).empty());
        a = t.On(Ev(Event::Retarget, 5, "", steams), 100);
        check("Steam tags it back: ours again", a.size() == 1 && Has(a, Action::Tag, ours) && t.Retaken() == 1);
        a = t.On(Ev(Event::Unmapped, 5), 200);
        check("unmapped: hidden", a.size() == 1 && a[0].type == Action::Hide);
        a = t.On(Ev(Event::Mapped, 5, "SteamVR", ours), 300);
        check("mapped again: shown, no new panel", a.size() == 1 && a[0].type == Action::Show);
        a = t.Switch(false);
        check("off: Steam's again, the panel gone (which one in the drop)", Has(a, Action::Tag, steams) && Has(a, Action::Drop, ours) && t.Panel() == 0);
        a = t.Switch(true);
        check("on again while it's mapped: a panel asked for", a.size() == 1 && a[0].type == Action::Make);
    }
    {   // Steam keeps taking it back: given up on, given back
        steamwin::Tracker t;
        t.SetSteams(steams);
        t.Switch(true);
        t.On(Ev(Event::Mapped, 5, "SteamVR", steams), 0);
        t.Made(ours);
        std::vector<Action> a;
        for (int i = 0; i < steamwin::kMaxRetakes; ++i) a = t.On(Ev(Event::Retarget, 5, "", steams), i * 100);
        check("retaken kMaxRetakes times: given back, said", Has(a, Action::Tag, steams) && Has(a, Action::GaveUp) && t.Panel() == 0);
        check("then left alone", t.On(Ev(Event::Mapped, 5, "SteamVR", steams), 1000).empty());
        steamwin::Tracker u;
        u.SetSteams(steams);
        u.Switch(true);
        u.On(Ev(Event::Mapped, 5, "SteamVR", steams), 0);
        u.Made(ours);
        for (int i = 0; i < steamwin::kMaxRetakes; ++i) a = u.On(Ev(Event::Retarget, 5, "", steams), i * steamwin::kRetakeWindowMs);
        check("now and then (each one more than the window apart): taken each time", Has(a, Action::Tag, ours) && u.Panel() == ours);
    }
    {   // never taken without Steam's overlay to give it back to
        steamwin::Tracker t;
        t.Switch(true);
        check("no Steam overlay known: not taken", t.On(Ev(Event::Mapped, 5, "SteamVR", 0x27), 0).empty());
        auto a = t.SetSteams(steams);
        check("found later: taken then", a.size() == 1 && a[0].type == Action::Make);
    }
    {   // it goes, or the display does
        steamwin::Tracker t;
        t.SetSteams(steams);
        t.Switch(true);
        t.On(Ev(Event::Mapped, 5, "SteamVR", steams), 0);
        t.Made(ours);
        auto a = t.On(Ev(Event::Gone, 5), 0);
        check("its window gone: the panel goes", a.size() == 1 && Has(a, Action::Drop, ours) && t.Panel() == 0);
        steamwin::Tracker u;
        u.SetSteams(steams);
        u.Switch(true);
        u.On(Ev(Event::Mapped, 8, "SteamVR", steams), 0);
        u.Made(ours);
        a = u.Closed();
        check("the display gone: the panel goes, nothing tagged", a.size() == 1 && a[0].type == Action::Drop);
        steamwin::Tracker v;
        v.SetSteams(steams);
        v.Switch(true);
        v.On(Ev(Event::Mapped, 9, "SteamVR", steams), 0);
        v.Made(ours);
        a = v.Shutdown();
        check("ft-screens going: back to Steam", a.size() == 1 && Has(a, Action::Tag, steams));
        steamwin::Tracker w;
        w.SetSteams(steams);
        w.Switch(true);
        w.On(Ev(Event::Mapped, 9, "SteamVR", steams), 0);
        w.Made(ours);
        a = w.Lost();
        check("its panel closed from Frametop: back to Steam", a.size() == 1 && Has(a, Action::Tag, steams) && !w.Panel());
        check("and not taken straight back", w.On(Ev(Event::Mapped, 9, "SteamVR", steams), 10).empty());
    }
    {   // what each window is
        using steamwin::Spec;
        auto is = [](const char *title, Spec::Role role, int slot = -1) {
            const Spec s = steamwin::Classify(title);
            return s.role == role && s.slot == slot;
        };
        check("the dashboard window: tile 0", is("SteamVR", Spec::Tile, 0));
        check("Steam's menu: tile 1", is("SteamVR - valve.steam.gamepadui.frame.menu.67340005 (5)", Spec::Tile, 1));
        check("the bar: Steam's", is("SteamVR - valve.steam.gamepadui.bar (1)", Spec::Steams));
        check("the launcher: Steam's", is("SteamVR - valve.steam.gamepadui.barpopup.67340001 (2)", Spec::Steams));
        check("the footer: a follower", is("SteamVR - valve.steam.gamepadui.floatingfooter (4)", Spec::Follow));
        check("the keyboard: a follower", is("SteamVR - Keyboard", Spec::Follow));
        check("a helper page: Steam's", is("data:text/html,<body></body>", Spec::Steams));
        check("someone else's: Steam's", is("SteamVR - com.example.thing (3)", Spec::Steams));
        check("keys: the menu's", steamwin::OverlayKey("SteamVR - valve.steam.gamepadui.frame.menu.67340005 (5)") ==
                                      "valve.steam.gamepadui.frame.menu.67340005");
        check("keys: the notifications'", steamwin::OverlayKey("SteamVR - Notifications") == "valve.steam.gamepadui.notifications");
        check("keys: the main one", steamwin::OverlayKey("SteamVR") == "valve.steam.gamepadui.main");
    }
    {   // the fleet: one tracker a window, Steam's own left out, one window a tile slot
        steamwin::Fleet f;
        f.Switch(true);
        check("the bar: not kept", f.On(Ev(Event::Mapped, 7, "SteamVR - valve.steam.gamepadui.bar (1)", steams), 0).empty() &&
                                       !f.Find(7));
        f.On(Ev(Event::Mapped, 5, "SteamVR", steams), 0);
        f.On(Ev(Event::Mapped, 6, "SteamVR - valve.steam.gamepadui.floatingfooter (4)", 0x31), 0);
        check("kept, waiting for Steam's overlays", f.Find(5) && f.Find(6) && f.Windows().size() == 2);
        auto a = f.SetSteams(5, steams);
        check("its overlay found: a tile asked for", a.size() == 1 && a[0].type == Action::Make && a[0].spec.slot == 0);
        a = f.SetSteams(6, 0x31);
        check("the footer's: a follower asked for", a.size() == 1 && a[0].spec.role == steamwin::Spec::Follow);
        a = f.Made(5, ours);
        check("made: tagged ours", Has(a, Action::Tag, ours) && f.TileIn(0) && f.TileIn(0)->Panel() == ours);
        f.On(Ev(Event::Mapped, 8, "SteamVR", steams), 0);
        check("a second dashboard window: the first one dropped", !f.Find(5) && f.Find(8) && f.TileIn(0)->Window() == 8);
        a = f.On(Ev(Event::Gone, 6), 0);
        check("gone: forgotten", !f.Find(6));
        f.SetSteams(8, steams);
        f.Made(8, ours);
        a = f.Switch(false);
        check("off: all given back", Has(a, Action::Tag, steams) && Has(a, Action::Drop));
        a = f.Closed();
        check("the display gone: nothing kept", f.Windows().empty());
    }
    return failed ? 1 : 0;
}
