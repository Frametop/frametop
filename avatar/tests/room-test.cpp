// The other people in a shared room, the sums (avatar/room): your floor laid in the room
// (floor.h), a wearer as the room's pose and back (wire.h), the sheet someone is drawn on
// (billboard.h), who is there to draw (people.h), and one of them as a body on their sheet
// (figure.h). No GPU, OpenVR, sockets or headset.
#include "../abstract/abstract.h"
#include "../room/billboard.h"
#include "../room/figure.h"
#include "../room/floor.h"
#include "../room/people.h"
#include "../room/wire.h"
#include "../view/demo.h"

#include <cstdio>
#include <cstring>
#include <string>

using avatar::Vec;
using bodypose::Turn;
using roomfloor::Floor;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static bool near(const Vec &a, const Vec &b, double within) { return (a - b).norm() < within; }
static Vec ahead(double yaw) { return {-std::sin(yaw), 0, -std::cos(yaw)}; }

static void floors() {
    check("a head that looks along -z faces 0, one that looks along -x a quarter turn to the left", std::fabs(roomfloor::Facing({0, 0, -1})) < 1e-9 &&
                                                                                                         std::fabs(roomfloor::Facing({-1, -0.3, 0}) - bodypose::kPi / 2) < 1e-9);
    check("...and one that looks straight down faces 0", roomfloor::Facing({0, -1, 0}) == 0);
    const Floor f{1.5, -2, 0.7};
    const Vec p{0.3, 1.2, -0.8};
    check("a place goes into the room and back", near(roomfloor::FromRoom(f, roomfloor::ToRoom(f, p)), p, 1e-9) && std::fabs(roomfloor::ToRoom(f, p).y - 1.2) < 1e-12);
    check("with no turn, the floor is only moved", near(roomfloor::ToRoom({2, 3, 0}, p), {2.3, 1.2, 2.2}, 1e-9));

    // someone who stood at (0.4, -1.1) of their floor facing 2.0 comes in, and is given the spot (5, 6) facing -1.2
    const Floor laid = roomfloor::Lay(5, 6, -1.2, 0.4, -1.1, 2.0);
    check("a floor is laid so that where you stood is your spot", near(roomfloor::ToRoom(laid, {0.4, 1.6, -1.1}), {5, 1.6, 6}, 1e-9));
    check("...and the way you faced is the way the spot faces", std::fabs(bodypose::Wrap(roomfloor::Facing(roomfloor::Turned(laid) * ahead(2.0)) - -1.2)) < 1e-9);
    check("a step to your right is a step to the spot's right", near(roomfloor::ToRoom(laid, Vec{0.4, 0, -1.1} + Turn::About({0, 1, 0}, 2.0) * Vec{1, 0, 0}),
                                                                      Vec{5, 0, 6} + Turn::About({0, 1, 0}, -1.2) * Vec{1, 0, 0}, 1e-9));
    const Floor walked{-0.9, -1.5, 2.2}, both = roomfloor::Then(laid, walked);
    check("one floor laid on another is both, one after the other", near(roomfloor::ToRoom(both, p), roomfloor::ToRoom(laid, roomfloor::ToRoom(walked, p)), 1e-9) &&
                                                                        near(roomfloor::Turned(both) * p, roomfloor::Turned(laid) * (roomfloor::Turned(walked) * p), 1e-9));
    const Floor same = roomfloor::Lay(1, 2, 0.5, 1, 2, 0.5);
    check("someone who stands on their spot already has the room's own floor", std::fabs(same.x) < 1e-9 && std::fabs(same.z) < 1e-9 && std::fabs(same.yaw) < 1e-9);
}

static void poses() {
    const bodyfile::Body body = abstract::Body(0);
    const handfit::Fit fit(body);
    wearer::Seen seen;
    demo::Wearer("wave", 0.7, 1.52, -0.08, seen);
    const Floor mine{3, -1, 0.9};
    const std::vector<uint8_t> bytes = proto::Pack(roomwire::ToRoom(seen, mine));
    proto::Pose read;
    check("a waving wearer is a pose the room takes, under 215 bytes", proto::Read(bytes.data(), bytes.size(), read) && bytes.size() <= 215 && read.hand[1].tracked &&
                                                                         read.hand[1].fingers && !read.hand[0].tracked);
    check("...told in the room's directions", near({read.eyes[0], read.eyes[1], read.eyes[2]}, roomfloor::ToRoom(mine, seen.eyes), 1e-5));

    const wearer::Seen back = roomwire::FromRoom(read, mine);
    bool fingers = true;
    for (int k = 0; k < bodypose::kFingerBones; ++k) fingers = fingers && back.hand[1].bone[k].AngleTo(seen.hand[1].bone[k]) < 0.03;
    check("back on the same floor they are who they were", near(back.eyes, seen.eyes, 1e-5) && back.head.AngleTo(seen.head) < 1e-3 && std::fabs(back.eyeHeight - 1.52) < 1e-3 &&
                                                              back.has[1] && !back.has[0] && back.hand[1].right && near(back.hand[1].wrist, seen.hand[1].wrist, 1e-5) &&
                                                              back.hand[1].palm.AngleTo(seen.hand[1].palm) < 1e-3);
    check("...their fingers within two degrees", fingers);
    const bodypose::Input was = wearer::Pose(seen, fit, 0), is = wearer::Pose(back, fit, 0);
    check("a body is posed from them as from the wearer", is.hand[1].tracked && is.hand[1].fingers && is.hand[1].turn.AngleTo(was.hand[1].turn) < 1e-3 &&
                                                            is.hand[1].finger[4].AngleTo(was.hand[1].finger[4]) < 0.03 && !is.hand[0].tracked);

    // the room's owner stands at its middle facing -z; a guest is on the spot 0.9 m to their right, and
    // stood at (2, 2) of their own floor facing a quarter turn left when they came in
    const Floor owner = roomfloor::Lay(0, 0, 0, 0, 0, 0), guest = roomfloor::Lay(0.9, 0, 0, 2, 2, bodypose::kPi / 2);
    demo::Wearer("stand", 0, 1.6, 0, seen);
    const wearer::Seen there = roomwire::FromRoom(roomwire::ToRoom(seen, owner), guest);
    const Vec left = Turn::About({0, 1, 0}, bodypose::kPi / 2) * Vec{-1, 0, 0};   // the guest's own left, on their floor
    check("a guest sees the owner beside them, to their left, at their own eye height", near(there.eyes, Vec{2, 1.6, 2} + left * 0.9, 1e-5));
    check("...facing the way the guest faced when they came in", std::fabs(bodypose::Wrap(roomfloor::Facing(there.head * Vec{0, 0, -1}) - bodypose::kPi / 2)) < 1e-4);
    seen = wearer::Seen();
    demo::Wearer("reach", 1, 1.6, 0, seen);
    seen.hand[0].fingers = seen.hand[0].hasElbow = false, seen.hand[1].hasElbow = true, seen.hand[1].elbow = {0.3, 1.1, -0.1};
    const wearer::Seen far = roomwire::FromRoom(roomwire::ToRoom(seen, owner), guest);
    check("a hand with no fingers known stays one, and an elbow goes with its hand", far.has[0] && !far.hand[0].fingers && !far.hand[0].hasElbow && far.hand[1].hasElbow &&
                                                                                       near(far.hand[1].elbow, roomfloor::FromRoom(guest, Vec{0.3, 1.1, -0.1}), 1e-5));
}

static Vec moved(const float m[12], const Vec &p) {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7], m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

static void sheets() {
    using namespace bodyfile;
    const Body body = abstract::Body(1);
    const std::vector<billboard::Ball> rest = billboard::Balls(body);
    bool holds = rest.size() == 33;   // the head, two hands, thirty finger bones
    for (int v = 0; v < body.verts(); ++v) {
        const Vec p = Vec{double(body.pos[3 * v]), double(body.pos[3 * v + 1]), double(body.pos[3 * v + 2])} * double(body.unit);
        bool in = false;
        for (const billboard::Ball &b : rest) in = in || (b.role == body.bones[4 * v] && (p - b.centre).norm() <= b.radius + 1e-9);
        holds = holds && in;
    }
    check("a body's balls hold its mesh, one for each bone something hangs from", holds && rest[0].role == kHead && rest[0].radius < 0.13);

    const handfit::Fit fit(body);
    bodypose::Solver solver(body);
    const double aspect = 1;
    bool all = true, faces = true, sized = true;
    double standWide = 0, reachWide = 0;
    for (const char *name : {"stand", "wave", "reach", "lean", "sit"}) {
        bodypose::Input in;
        bodypose::Pose pose;
        float bones[kRoles][12];
        solver.Reset();
        demo::Pose(name, 1, body, fit, in), solver.Solve(in, pose), solver.Matrices(pose, bones);
        const std::vector<billboard::Ball> balls = billboard::Posed(rest, bones);
        // from in front, the side, behind, above them, below them, and close by
        const Vec from[6] = {{0, 1.6, -2.5}, {3, 1.5, 0.2}, {-0.4, 1.7, 4}, {0.5, 2.4, -1.2}, {-1, 0.6, -1.5}, {0.1, 1.55, -0.75}};
        for (const Vec &at : from) {
            if (&at == &from[5] && (!std::strcmp(name, "reach") || !std::strcmp(name, "lean"))) continue;   // their hand is in your face: below
            const Vec across = Vec{0, 1, 0}.cross(Vec{-at.x, 0, -at.z}.unit());
            const Vec eyes[2] = {at - across * 0.032, at + across * 0.032};
            billboard::Sheet sheet;
            if (!billboard::Around(balls, eyes, aspect, sheet)) {
                all = false;
                continue;
            }
            const mirror::Glass &g = sheet.glass;
            faces = faces && g.out.dot(at - g.centre) > 0 && std::fabs(g.out.y) < 1e-9 && near(g.up, {0, 1, 0}, 1e-9) && near(g.right.cross(g.up), g.out, 1e-9);
            const double steps = g.width / billboard::kStep;
            sized = sized && g.width >= billboard::kSmallest - 1e-9 && std::fabs(steps - std::round(steps)) < 1e-6 && std::fabs(g.height - g.width / aspect) < 1e-9;
            if (&at == &from[0] && !std::strcmp(name, "stand")) standWide = g.width;
            if (&at == &from[0] && !std::strcmp(name, "reach")) reachWide = g.width;
            for (int e = 0; e < 2; ++e) {
                viewmath::Mat4 clip;
                if (!billboard::EyeView(sheet, eyes[e], clip)) all = false;
                for (int v = 0; v < body.verts(); ++v) {
                    const Vec p = Vec{double(body.pos[3 * v]), double(body.pos[3 * v + 1]), double(body.pos[3 * v + 2])} * double(body.unit);
                    const Vec on = viewmath::Project(clip, moved(bones[body.bones[4 * v]], p));
                    all = all && std::fabs(on.x) <= 1 && std::fabs(on.y) <= 1 && std::fabs(on.z) <= 1;
                }
            }
        }
    }
    check("from any side, in any pose, all of them is on their sheet for both eyes", all);
    check("the sheet stands upright and faces whoever looks", faces);
    check("it keeps its shape, is never tiny, and grows a step at a time", sized);
    check("it is no bigger than they need: someone standing fits a sheet under 1.3 m, with nothing wasted on a body that isn't drawn", standWide > 0.5 && standWide < 1.3);
    check("...and hands held out toward you need no more", reachWide > 0.4 && reachWide < 1.3);

    bodypose::Input in;
    bodypose::Pose pose;
    float bones[kRoles][12];
    solver.Reset();
    demo::Pose("stand", 1, body, fit, in), solver.Solve(in, pose), solver.Matrices(pose, bones);
    const std::vector<billboard::Ball> balls = billboard::Posed(rest, bones);
    billboard::Sheet sheet;
    const Vec inside[2] = {{-0.03, 1.6, -0.05}, {0.03, 1.6, -0.05}}, touching[2] = {{-0.03, 1.6, -0.2}, {0.03, 1.6, -0.2}};
    const std::vector<billboard::Ball> headOnly = {balls[0]};
    check("eyes inside their head see no sheet, and eyes right up against a head none of it", !billboard::Around(balls, inside, 1, sheet) && !billboard::Around(headOnly, inside, 1, sheet) &&
                                                                                                !billboard::Around(headOnly, touching, 1, sheet));
    // they hold both hands out, and you stand where the hands are
    demo::Pose("reach", 1, body, fit, in), solver.Reset(), solver.Solve(in, pose), solver.Matrices(pose, bones);
    const std::vector<billboard::Ball> reaching = billboard::Posed(rest, bones);
    const Vec close[2] = {in.hand[1].wrist + Vec{-0.03, 0.05, -0.1}, in.hand[1].wrist + Vec{0.03, 0.05, -0.1}};
    bool head = billboard::Around(reaching, close, 1, sheet);
    for (int e = 0; head && e < 2; ++e) {
        viewmath::Mat4 clip;
        head = billboard::EyeView(sheet, close[e], clip);
        const Vec on = viewmath::Project(clip, reaching[0].centre);
        head = head && std::fabs(on.x) < 0.9 && std::fabs(on.y) < 0.9;
    }
    check("a hand held right up to your eyes doesn't take the rest of them away", head && sheet.glass.width < 2.5);
    const Vec ok[2] = {{-0.03, 1.6, -2}, {0.03, 1.6, -2}};
    check("nobody to draw, or a sheet of no shape, is no sheet", !billboard::Around({}, ok, 1, sheet) && !billboard::Around(balls, ok, 0, sheet) && billboard::Around(balls, ok, 0.75, sheet) &&
                                                                    std::fabs(sheet.glass.width / sheet.glass.height - 0.75) < 1e-9);

    // a small wearer's body is scaled down, and so are its balls
    in.eyeHeight = 1.2, in.eyes.y = 1.2;
    solver.Reset(), solver.Solve(in, pose), solver.Matrices(pose, bones);
    const std::vector<billboard::Ball> small = billboard::Posed(rest, bones);
    check("a smaller wearer's body has smaller balls", std::fabs(small[0].radius / rest[0].radius - pose.scale) < 1e-6 && pose.scale < 0.8);
}

static std::string state(const char *mode, int me, const char *people) {
    return std::string("{\"mode\":\"") + mode + "\",\"note\":\"\",\"me\":" + std::to_string(me) + ",\"owner\":0,\"people\":[" + people + "],\"asks\":[],\"codes\":0}";
}

static std::vector<uint8_t> watched(int person, float x) {
    proto::Pose p;
    p.eyes[0] = x, p.eyes[1] = 1.6f;
    std::vector<uint8_t> out = {0x81, uint8_t(person)};
    const std::vector<uint8_t> pose = proto::Pack(p);
    out.insert(out.end(), pose.begin(), pose.end());
    return out;
}

static void people() {
    const char *const kAda = "{\"id\":0,\"name\":\"ada\",\"login\":\"ada@x\",\"away\":0,\"linked\":true,\"spot\":[1,0,2,0.5]}";
    const char *const kBen = "{\"id\":1,\"name\":\"ben\",\"login\":\"ben@x\",\"away\":1,\"linked\":true,\"spot\":[1.9,0,2,0.5]}";
    const char *const kCy = "{\"id\":1,\"name\":\"cy\",\"login\":\"cy@x\",\"away\":0,\"linked\":false,\"spot\":[1.9,0,2,0.5]}";
    roompeople::People p;
    check("before any word of a room, nobody is in one", !p.inRoom() && p.me() == -1 && !p.Drawn(0, 0));
    check("ft-together's state is read: who is here, who you are, where they stand", p.Room(state("joined", 1, (std::string(kAda) + "," + kBen).c_str())) && p.inRoom() && p.me() == 1 &&
                                                                                       p.person(0).here && p.person(0).name == "ada" && p.person(0).spot[0] == 1 &&
                                                                                       p.person(0).spot[1] == 2 && p.person(0).spot[2] == 0.5 && p.person(1).away == 1 &&
                                                                                       !p.person(2).here);
    check("nobody is drawn before a pose of theirs comes", !p.Drawn(0, 10));
    std::vector<uint8_t> d = watched(0, 0.25f);
    check("a watched pose is theirs from then on, and they are drawn", p.Pose(d.data(), d.size(), 10) && p.Drawn(0, 10.5) && p.person(0).pose.eyes[0] == 0.25f);
    check("...for a second: then they aren't, until the next", p.Drawn(0, 11) && !p.Drawn(0, 11.01) && p.Pose(d.data(), d.size(), 12) && p.Drawn(0, 12.2));
    std::vector<uint8_t> own = watched(1, 0), nobody = watched(2, 0), outside = watched(7, 0), cut = d, other = d;
    cut.pop_back(), other[0] = 0x80;
    check("a pose of your own, of a place nobody has, or of no place isn't taken", !p.Pose(own.data(), own.size(), 12) && !p.Pose(nobody.data(), nobody.size(), 12) &&
                                                                                     !p.Pose(outside.data(), outside.size(), 12) && !p.Drawn(1, 12) && !p.Drawn(2, 12) &&
                                                                                     !p.Drawn(7, 12) && !p.Drawn(-1, 12));
    check("...nor what isn't a pose", !p.Pose(cut.data(), cut.size(), 12) && !p.Pose(other.data(), other.size(), 12) && !p.Pose(d.data(), 2, 12) && !p.Pose(nullptr, 0, 12));

    const unsigned since = p.person(0).since;
    check("the same people again changes nothing", p.Room(state("joined", 1, (std::string(kAda) + "," + kBen).c_str())) && p.person(0).since == since && p.Drawn(0, 12.2));
    // you are in place 0 of another room now, and someone else has place 1
    const unsigned benSince = p.person(1).since;
    check("someone new in a place isn't the one before", p.Room(state("hosting", 0, (std::string(kAda) + "," + kCy).c_str())) && p.me() == 0 && p.person(1).name == "cy" &&
                                                           p.person(1).since == benSince + 1 && !p.person(1).posed && !p.Drawn(0, 12.2));
    d = watched(1, 0.5f);
    check("...and is drawn from their own first pose", p.Pose(d.data(), d.size(), 13) && p.Drawn(1, 13));
    check("someone who left isn't there", p.Room(state("hosting", 0, kAda)) && !p.person(1).here && !p.Drawn(1, 13) && !p.Pose(d.data(), d.size(), 13));

    check("outside a room there is nobody, and that is still an answer", p.Room(state("idle", -1, "")) && !p.inRoom() && p.me() == -1 && !p.person(0).here);
    p.Room(state("joined", 1, (std::string(kAda) + "," + kBen).c_str()));
    check("what isn't ft-together's state is no room", !p.Room("{\"mode\":\"joined\"") && !p.inRoom() && !p.Room("[]") && !p.Room(state("dancing", 0, kAda)) &&
                                                         !p.Room(state("joined", 4, kAda)) && !p.Room(state("joined", 1, kAda)) && !p.inRoom());
    const std::string twice = std::string(kAda) + "," + kAda, far = "{\"id\":1,\"name\":\"x\",\"login\":\"\",\"away\":0,\"linked\":true,\"spot\":[1e9,0,2,0.5]}";
    const std::string short3 = "{\"id\":1,\"name\":\"x\",\"login\":\"\",\"away\":0,\"linked\":true,\"spot\":[1,0,2]}", text = "{\"id\":1,\"name\":\"x\",\"login\":\"\",\"away\":0,\"spot\":[1,0,\"2\",0]}";
    check("...nor is a room with two in one place, or someone standing nowhere", !p.Room(state("joined", 0, twice.c_str())) && !p.Room(state("joined", 0, (std::string(kAda) + "," + far).c_str())) &&
                                                                                   !p.Room(state("joined", 0, (std::string(kAda) + "," + short3).c_str())) &&
                                                                                   !p.Room(state("joined", 0, (std::string(kAda) + "," + text).c_str())) && !p.inRoom());
    p.Room(state("joined", 1, (std::string(kAda) + "," + kBen).c_str()));
    d = watched(0, 0);
    p.Pose(d.data(), d.size(), 20);
    p.Gone();
    check("when ft-together goes quiet, so does the room", !p.inRoom() && !p.Drawn(0, 20) && !p.person(0).here);
}

static void figures() {
    using namespace bodyfile;
    const Body body = abstract::Body(2);
    figure::Figure fig(body);
    const Floor theirs{0.6, -0.4, 0.5}, mine{-1.0, 2.0, -0.8};
    const Vec anywhere[2] = {{0.17, 1.6, -1.8}, {0.23, 1.6, -1.8}};
    figure::View view;
    check("nobody is seen before their pose comes", !fig.posed() && !fig.Seen(anywhere, 1, view));

    // someone waves on their own floor; the room is told; whoever draws them has another floor
    wearer::Seen seen;
    proto::Pose told;
    bool ok = true;
    for (int i = 0; i <= 30; ++i) {
        demo::Wearer("wave", i / 30.0, 1.6, -0.08, seen);
        const std::vector<uint8_t> bytes = proto::Pack(roomwire::ToRoom(seen, theirs));
        ok = ok && proto::Read(bytes.data(), bytes.size(), told) && fig.Pose(told, mine, 1 / 30.0);
    }
    const auto posed = [&](int role) {
        const float *m = fig.bones()[role], *at = body.at[role];
        return Vec{m[0] * at[0] + m[1] * at[1] + m[2] * at[2] + m[3], m[4] * at[0] + m[5] * at[1] + m[6] * at[2] + m[7], m[8] * at[0] + m[9] * at[1] + m[10] * at[2] + m[11]};
    };
    const Vec head = roomfloor::FromRoom(mine, roomfloor::ToRoom(theirs, seen.eyes)), wrist = roomfloor::FromRoom(mine, roomfloor::ToRoom(theirs, seen.hand[1].wrist));
    check("a pose the room told is solved", ok && fig.posed());
    check("...and their head and hand are where they are in the room, on the drawer's floor", (posed(kHead) - head).norm() < 0.2 && (posed(kRightHand) - wrist).norm() < 0.03 &&
                                                                                                  (head - seen.eyes).norm() > 1);

    // looked at from in front of them, both eyes
    const Vec faces = roomfloor::Turned(mine).inverse() * (roomfloor::Turned(theirs) * (seen.head * Vec{0, 0, -1}));
    const Vec mid = Vec{head.x, 1.6, head.z} + Vec{faces.x, 0, faces.z}.unit() * 1.5, side = (Vec{faces.x, 0, faces.z}.unit() * -1).cross(Vec{0, 1, 0}) * 0.032;
    const Vec eyes[2] = {mid - side, mid + side};
    check("they are seen, by both eyes", fig.Seen(eyes, 1, view) && view.sees[0] && view.sees[1]);
    bool within = true;
    for (int e = 0; e < 2; ++e)
        for (int role : {kHead, kRightHand, kLeftHand}) {
            const Vec p = viewmath::Project(view.clip[e], posed(role));
            within = within && std::fabs(p.x) < 1 && std::fabs(p.y) < 1 && std::fabs(p.z) < 1;
        }
    check("...with their head and both hands within each eye's picture, and its depth", within);
    const Vec l = viewmath::Project(view.clip[0], posed(kHead)), r = viewmath::Project(view.clip[1], posed(kHead));
    const double depth = (posed(kHead) - view.sheet.glass.centre).dot(view.sheet.glass.out);
    check("what is in front of the sheet is further left for the right eye, and what is behind it further right", (depth > 0) == (r.x < l.x) && std::fabs(depth) > 1e-3);
    check("their head is over the hand that hangs, and first in the picture: the top goes first",
          posed(kHead).y > posed(kLeftHand).y + 0.3 && l.y < viewmath::Project(view.clip[0], posed(kLeftHand)).y);

    const Vec behind[2] = {view.sheet.glass.centre - view.sheet.glass.out * 0.5, view.sheet.glass.centre - view.sheet.glass.out * 0.5 + side};
    figure::View other;
    check("from the other side of them the sheet turns round to face you", fig.Seen(behind, 1, other) && other.sheet.glass.out.dot(view.sheet.glass.out) < -0.9);
    proto::Pose bad = told;
    bad.eyes[0] = std::nanf("");
    const Vec before = posed(kHead);
    check("a pose that can't be solved leaves them as they were", !fig.Pose(bad, mine, 0.03) && fig.posed() && (posed(kHead) - before).norm() == 0);
    fig.Reset();
    check("forgotten, they aren't seen until the next pose", !fig.posed() && !fig.Seen(eyes, 1, view) && fig.Pose(told, mine, 0) && fig.Seen(eyes, 1, view));
}

int main() {
    floors();
    poses();
    sheets();
    people();
    figures();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
