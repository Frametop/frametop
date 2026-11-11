// A room's own rules (together/room): its invites, where its people stand, the panels they
// share, and how often someone may say a thing.
#include "../room/allowance.h"
#include "../room/invites.h"
#include "../room/panels.h"
#include "../room/seats.h"

#include <cctype>
#include <cstdio>
#include <set>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static uint64_t g_seed = 7;
static uint64_t roll() { return g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull, g_seed >> 11; }

using Answer = room::Invites::Answer;

static void invites() {
    room::Invites in;
    const std::string kAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    std::set<std::string> seen;
    bool ok = true;
    for (int i = 0; i < 2000; ++i) {
        const std::string code = in.Make(0, roll);
        ok = ok && code.size() == 8 && code.find_first_not_of(kAlphabet) == std::string::npos && seen.insert(code).second;
    }
    check("codes: eight characters that read aloud, and no two alike in two thousand", ok && in.waiting() == room::Invites::kMostCodes);
    std::set<char> used;
    for (const std::string &code : seen) used.insert(code.begin(), code.end());
    check("...using every letter of the alphabet", used.size() == kAlphabet.size());

    in.Clear();
    const std::string code = in.Make(100, roll);
    std::string typed = code.substr(0, 4) + " - " + code.substr(4);
    for (char &c : typed) c = char(std::tolower(c));
    ok = in.Use(code.substr(0, 7), "a", 100) == Answer::kWrong && in.Use(code + "A", "a", 100) == Answer::kWrong && in.Use("", "a", 100) == Answer::kWrong && in.waiting() == 1;
    check("a code cut short, one with more after it and none at all are wrong", ok);
    ok = in.Use(typed, "a", 100) == Answer::kGood && in.waiting() == 0 && in.Use(code, "b", 100) == Answer::kWrong;
    check("a good one is good typed small with a dash, also after wrong ones, and only once", ok);

    const std::string late = in.Make(200, roll);
    ok = in.waiting() == 1 && in.Use("nope", "c", 200 + room::Invites::kGoodForS) == Answer::kWrong && in.waiting() == 0 && in.Use(late, "c", 800) == Answer::kWrong;
    const std::string inTime = in.Make(200, roll);
    check("a code is good for ten minutes and not a second more", ok && in.Use(inTime, "d", 200 + room::Invites::kGoodForS - 1) == Answer::kGood);

    in.Clear();
    std::vector<std::string> codes;
    for (int i = 0; i < 9; ++i) codes.push_back(in.Make(1000 + i, roll));
    ok = in.waiting() == 8 && in.Use(codes[0], "e", 1010) == Answer::kWrong;
    for (int i = 1; i < 9; ++i) ok = ok && in.Use(codes[size_t(i)], "f", 1010) == Answer::kGood;
    check("eight wait at most: a ninth takes the oldest one's place", ok && in.waiting() == 0);

    in.Clear();
    const std::string prize = in.Make(2000, roll);
    ok = true;
    for (int i = 0; i < room::Invites::kMostWrong; ++i) ok = ok && in.Use("guess", "g", 2000 + i) == Answer::kWrong;
    ok = ok && in.Use(prize, "g", 2010) == Answer::kRefused && in.waiting() == 1 && in.Use("guess", "h", 2010) == Answer::kWrong;
    check("five wrong from one address, and the right one from there isn't looked at", ok);
    ok = in.Use(prize, "g", 2000 + room::Invites::kWrongForS - 1) == Answer::kRefused;
    const std::string next = in.Make(2000 + room::Invites::kWrongForS, roll);
    check("...until ten minutes after its first wrong one", ok && in.Use(next, "g", 2000 + room::Invites::kWrongForS) == Answer::kGood);

    // many addresses, each wrong: nothing breaks, and a good code is still good
    in.Clear();
    const std::string kept = in.Make(3000, roll);
    for (int i = 0; i < 2000; ++i) in.Use("guess", "100.64." + std::to_string(i / 250) + "." + std::to_string(i % 250), 3000);
    check("wrong guesses from two thousand addresses, and the code still works for its bearer", in.Use(kept, "z", 3000) == Answer::kGood);
}

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void seats() {
    proto::Spot owner;
    owner.at[0] = 2, owner.at[1] = 0, owner.at[2] = -1, owner.yaw = 0;
    const proto::Spot first = room::SpotBeside(owner, 1), second = room::SpotBeside(owner, 2), third = room::SpotBeside(owner, 3);
    bool ok = near(first.at[0], 2.9f) && near(second.at[0], 1.1f) && near(third.at[0], 3.8f);
    for (const proto::Spot &s : {first, second, third}) ok = ok && near(s.at[2], -1) && s.at[1] == 0 && s.yaw == 0;
    check("guests stand to the owner's right, left and further right, 0.9 m apart", ok);
    owner.yaw = float(M_PI / 2);   // a quarter turn to the left: the owner faces -x, their right is -z
    const proto::Spot turned = room::SpotBeside(owner, 1);
    check("...along the owner's own right, whichever way they face", near(turned.at[0], 2) && near(turned.at[2], -1.9f) && turned.yaw == owner.yaw);
    const proto::Spot same = room::SpotBeside(owner, 0), bad = room::SpotBeside(owner, 9), worse = room::SpotBeside(owner, -3);
    check("a place there isn't is the owner's own spot", same.at[0] == 2 && bad.at[0] == 2 && worse.at[0] == 2 && bad.at[2] == -1);

    proto::Room r;
    ok = room::FreeId(r) == 0;
    for (int id : {0, 1, 3}) {
        proto::Person p;
        p.id = uint8_t(id);
        r.people.push_back(p);
    }
    ok = ok && room::FreeId(r) == 2;
    proto::Person p;
    p.id = 2;
    r.people.push_back(p);
    check("the lowest free place is the next one's, and a full room has none", ok && room::FreeId(r) == -1);
}

static proto::Panel panel(int owner, int number, float x = 0) {
    proto::Panel p;
    p.id = proto::PanelId(owner, number), p.width = 1920, p.height = 1080, p.at[0] = x;
    return p;
}

static void panels() {
    using room::Shared;
    proto::Room r;
    for (int id : {0, 2}) {
        proto::Person p;
        p.id = uint8_t(id);
        r.people.push_back(p);
    }
    bool ok = room::SetPanel(r, 2, panel(2, 1)) == Shared::kChanged && room::SetPanel(r, 0, panel(0, 2)) == Shared::kChanged && room::SetPanel(r, 0, panel(0, 0)) == Shared::kChanged;
    ok = ok && r.panels.size() == 3 && r.panels[0].id == 0 && r.panels[1].id == 2 && r.panels[2].id == 17;
    check("people share their panels, and the room keeps them in the order of their ids", ok);
    ok = room::SetPanel(r, 0, panel(0, 2)) == Shared::kSame && room::SetPanel(r, 0, panel(0, 2, 0.5f)) == Shared::kChanged && r.panels.size() == 3 && r.panel(2)->at[0] == 0.5f;
    check("a panel said again is the same one, moved if it says so", ok);
    ok = room::SetPanel(r, 0, panel(2, 0)) == Shared::kNo && room::SetPanel(r, 2, panel(0, 0, 9)) == Shared::kNo && r.panel(0)->at[0] == 0;
    check("nobody shares a panel as someone else's, or moves one that is", ok && r.panels.size() == 3);
    check("someone who isn't in the room shares nothing", room::SetPanel(r, 1, panel(1, 0)) == Shared::kNo && room::SetPanel(r, 7, panel(7, 0)) == Shared::kNo);
    check("a headset shares three panels, and no fourth", room::SetPanel(r, 2, panel(2, room::kFramePanels)) == Shared::kNo && room::SetPanel(r, 2, panel(2, 7)) == Shared::kNo);
    ok = !room::DropPanel(r, 0, 17) && !room::DropPanel(r, 2, 0) && !room::DropPanel(r, 2, 18) && r.panels.size() == 3;
    check("only its owner takes a panel back", ok && room::DropPanel(r, 2, 17) && r.panels.size() == 2 && !r.panel(17));
    room::SetPanel(r, 2, panel(2, 0));
    ok = room::DropPanelsOf(r, 0) && r.panels.size() == 1 && r.panels[0].id == 16 && !room::DropPanelsOf(r, 0);
    check("someone's panels go with them, and nobody else's", ok);
}

static void allowance() {
    room::Allowance a(10, 5);
    int spent = 0;
    for (int i = 0; i < 100; ++i) spent += a.Spend(0);
    check("so many at once, and no more", spent == 5 && !a.Spend(0.05));
    spent = 0;
    for (int i = 1; i <= 1000; ++i) spent += a.Spend(0.001 * i);
    check("so many a second, however fast they come", spent >= 9 && spent <= 11);
    spent = 0;
    for (int i = 0; i < 100; ++i) spent += a.Spend(60);
    check("a quiet minute saves up only what may come at once", spent == 5);
    check("a clock that steps back gives nothing away", !a.Spend(-1000) && !a.Spend(-999.95));
}

int main() {
    invites();
    seats();
    panels();
    allowance();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
