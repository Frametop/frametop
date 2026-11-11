// A picture of someone in a shared room (roompicture.h).
#include "roompicture.h"

#include "../abstract/abstract.h"
#include "../view/canvas.h"
#include "../view/demo.h"
#include "../view/windowview.h"
#include "figure.h"
#include "sentbody.h"

#include <algorithm>
#include <cstdio>

namespace roompicture {

using avatar::Vec;
using roomfloor::Floor;

namespace {

constexpr double kEyesApart = 0.064, kEyesUp = 1.6;   // the eyes that look, metres
constexpr double kStep = 1.0 / 30;                    // between the poses told
// Where the two floors lie in the room: anywhere, so long as neither is the room's own.
constexpr Floor kTheirs{0.6, -0.4, 0.5}, kMine{-1.0, 2.0, -0.8};

}  // namespace

int Person(const Ask &ask) {
    bodyfile::Body body;
    std::string why;
    if (ask.model.empty()) body = abstract::Body(ask.place);
    else if (!sentbody::Read(ask.model, body, why)) return std::fprintf(stderr, "avatars: %s isn't drawn: %s\n", ask.model.c_str(), why.c_str()), 1;
    figure::Figure figure(body);
    const double until = std::min(std::max(ask.at, 0.0), 30.0);
    wearer::Seen seen;
    for (double t = 0;; t += kStep) {
        if (!demo::Wearer(ask.pose.c_str(), std::min(t, until), 1.6, -0.08, seen)) return std::fprintf(stderr, "avatars: no pose called %s (%s)\n", ask.pose.c_str(), demo::kNames), 1;
        const std::vector<uint8_t> bytes = proto::Pack(roomwire::ToRoom(seen, kTheirs));
        proto::Pose told;
        if (!proto::Read(bytes.data(), bytes.size(), told) || !figure.Pose(told, kMine, kStep)) return std::fprintf(stderr, "avatars: the pose can't be told\n"), 1;
        if (t >= until) break;
    }
    // the eyes: in front of where they face, on the floor of the one who looks
    const bodypose::Turn onMine = roomfloor::Turned(kMine).inverse() * roomfloor::Turned(kTheirs);
    const Vec them = roomfloor::FromRoom(kMine, roomfloor::ToRoom(kTheirs, seen.eyes));
    Vec faces = onMine * (seen.head * Vec{0, 0, -1});
    faces = Vec{faces.x, 0, faces.z}.norm() > 1e-3 ? Vec{faces.x, 0, faces.z}.unit() : Vec{0, 0, -1};
    const Vec from = bodypose::Turn::About({0, 1, 0}, ask.round * bodypose::kPi / 180) * faces;
    const Vec mid = Vec{them.x, kEyesUp, them.z} + from * std::min(std::max(ask.away, 0.3), 20.0);
    const Vec right = (from * -1).cross(Vec{0, 1, 0}).unit() * (kEyesApart / 2);
    const Vec eyes[2] = {mid - right, mid + right};
    figure::View view;
    if (!figure.Seen(eyes, 1, view)) return std::fprintf(stderr, "avatars: nothing of them is seen from there\n"), 1;

    const int side = std::min(std::max(ask.size, 64), 1024);
    canvas::Canvas canvas;
    bodygpu::Painter painter;
    bodygpu::Avatar avatar;
    int status = 1;
    if (canvas.Open(2 * side * canvas::kFine, side * canvas::kFine) && painter.Init("avatars")) {
        avatar.Upload(painter, body);
        windowview::Draw(painter, avatar, figure.bones(), view.clip, view.sees, billboard::Light(), side * canvas::kFine, side * canvas::kFine);
        if (canvas::Save(ask.out, canvas.Read(true), 2 * side, side)) {
            status = 0;
            std::printf("wrote %s: place %d, %s, in %s, on a sheet %.1f m a side from %.1f m, %dx%d an eye\n", ask.out.c_str(), ask.place, ask.pose.c_str(),
                        ask.model.empty() ? "the abstract body" : "the avatar they sent", view.sheet.glass.width, (mid - view.sheet.glass.centre).norm(), side, side);
        } else {
            std::fprintf(stderr, "avatars: can't write %s\n", ask.out.c_str());
        }
    }
    avatar.Free(), painter.Done(), canvas.Close();
    return status;
}

}  // namespace roompicture
