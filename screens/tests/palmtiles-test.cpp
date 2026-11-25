// The palm menu's tiles (palmtiles.h): the file's lines, what's left out, what each page shows, the default
// card and QUICK_SETTINGS's pick of it, what works while paused, what a tile says and whether it's lit, where
// the palm's pinch starts. And a label in its tile (labelfit.h): its lines, its size, what's cut.
#include "../labelfit.h"
#include "../palmtiles.h"

#include <cstdio>
#include <string>

int main() {
    int failed = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        failed += !ok;
    };
    using namespace palmtiles;
    auto names = [](const std::vector<Entry> &es) {
        std::string s;
        for (const Entry &e : es) s += (s.empty() ? "" : " ") + e.action;
        return s;
    };

    const Menu d = Defaults(nullptr);
    check("the default first page", names(d.first) == "steam_menu screens_toggle layout_reset spin_prev spin_next dock_all" &&
                                        d.first[2].label == "Reset Layout…" && d.first[0].label == "Steam Menu");
    check("the default Quick Settings", names(d.quick) == "screens_toggle keyboard_toggle desktopless_toggle hands_toggle pause_toggle dictate");
    check("the default eye row", d.eyes.action == "gaze_quickcal" && d.eyesShown);
    check("QUICK_SETTINGS picks its toggles, in the card's order", names(Defaults("dictate,bogus, screens_toggle ,screens").quick) == "screens_toggle dictate");
    check("...empty: none, and the first page is whole", Defaults("").quick.empty() && Defaults("").first.size() == 6);

    {
        const Menu m = Parse("# a comment\n\n"
                             "menu\t1\trun:gonk\tGonk it\n"
                             "menu\t0\tsteam_menu\tSteam Menu\n"
                             "menu\t1\tprofile:My Desk\t Desk \r\n"
                             "menu\t2\tdock_all\tBad flag\n"
                             "menu\t1\tdock_all\n"
                             "menu\t1\t\tNo action\n"
                             "menu\t1\tdock_all\t\n"
                             "menu\t1\tdock_all\tTwo\tlabels\n"
                             "menu 1 dock_all Spaces\n"
                             "other\t1\tdock_all\tNo such page\n"
                             "eyes\t0\trun:eye\tMine\n"
                             "eyes\t1\tgaze_quickcal\tSecond\n"
                             "quick\t1\tdictate\tTalk");
        check("good lines, trimmed; hidden and bad ones left out", names(m.first) == "run:gonk profile:My Desk" && m.first[1].label == "Desk");
        check("the last line needs no newline", names(m.quick) == "dictate" && m.quick[0].label == "Talk");
        check("the first eye row, hidden as it says", m.eyes.action == "run:eye" && !m.eyesShown);
        const Menu none = Parse("");
        check("an empty file: no tiles, the default eye row", none.first.empty() && none.quick.empty() && none.eyes.action == "gaze_quickcal" && none.eyesShown);
    }
    {
        std::string text;
        for (int i = 0; i < 9; ++i) text += "menu\t" + std::string(i == 1 ? "0" : "1") + "\trun:c" + std::to_string(i) + "\tC\n";
        check("a page shows its first six that are on", names(Parse(text).first) == "run:c0 run:c2 run:c3 run:c4 run:c5 run:c6");
    }

    check("paused: Steam Menu, Game Pause, dictation, release keys and commands",
          WorksPaused("steam_menu") && WorksPaused("pause_toggle") && WorksPaused("dictate") && WorksPaused("release_keys") && WorksPaused("run:gonk"));
    check("...nothing else", !WorksPaused("screens_toggle") && !WorksPaused("layout_reset") && !WorksPaused("profile:run:x") &&
                                 !WorksPaused("gaze_quickcal") && !WorksPaused("dictat"));

    const Entry screens{"screens_toggle", "Hide Screens"}, mine{"screens_toggle", "Desk"}, plain{"dock_all", "Hide Screens"};
    check("the screens tile by its own name says which it would do", Words(screens, true) == "Hide Screens" && Words(screens, false) == "Show Screens" &&
                                                                          Words({"screens_toggle", "Show Screens"}, true) == "Hide Screens");
    check("...by yours, yours; another action's label is only a label", Words(mine, true) == "Desk" && Words(mine, false) == "Desk" && Words(plain, false) == "Hide Screens");
    check("lit: a toggle that's on, unless it says so", Lit(mine, true, false) && !Lit(mine, false, false) && !Lit(screens, true, false) &&
                                                            Lit({"pause_toggle", "Pause"}, false, true) && !Lit({"pause_toggle", "Pause"}, true, false) &&
                                                            !Lit(plain, true, true));

    check("the pinch starts on Steam Menu, wherever it is", PinchSlot(d) == 0 && PinchSlot(Parse("menu\t1\tdock_all\tD\nmenu\t1\tsteam_menu\tS\n")) == 1);
    check("...hidden: on the first tile", PinchSlot(Parse("menu\t0\tsteam_menu\tS\nmenu\t1\tdock_all\tD\n")) == 0);
    check("...no tiles: on the header's button", PinchSlot(Parse("")) == kMax);

    // Labels: 10 px a byte at size 20, so 13 letters a line of 130 px.
    auto width = [](const std::string &t, int size) { return int(t.size()) * size / 2; };
    auto joined = [](const labelfit::Fit &f) {
        std::string s;
        for (const std::string &l : f.lines) s += (s.empty() ? "" : "/") + l;
        return std::to_string(f.size) + ":" + s;
    };
    auto fit = [&](const char *t) { return joined(labelfit::Make(t, {28, 24, 20}, 130, 3, width)); };
    check("a short label is one line, the biggest size", fit("Gonk") == "28:Gonk");
    check("two words break between them", fit("Spin Around") == "28:Spin/Around");
    check("as many words to a line as fit", fit("Do a b now") == "28:Do a b/now");
    check("a long word takes a smaller size", fit("Desktopless") == "20:Desktopless");
    check("more lines than fit: smaller", fit("aaaaa bbbbb cccc dddd eeee") == "24:aaaaa/bbbbb cccc/dddd eeee");
    check("a word no size fits is cut", fit("Antidisestablish") == "20:Antidisest…");
    check("too many words: the last line is cut", fit("aaaaaaaaaaaa bbbbbbbbbbbb cccccccccccc dddd") == "20:aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccc…");
    check("cut between UTF-8 characters", labelfit::Shorter("a…") == "a" && labelfit::Shorter("") == "");
    check("spaces alone: no lines", labelfit::Make("  ", {28}, 130, 3, width).lines.empty());

    std::printf("%d failed\n", failed);
    return failed != 0;
}
