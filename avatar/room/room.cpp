// The other people in a shared room, live (room.h).
#include "room.h"

#include "../abstract/abstract.h"
#include "../view/sheet.h"
#include "../view/stage.h"
#include "../view/wearer.h"
#include "../view/windowview.h"
#include "client.h"
#include "figure.h"
#include "handcut.h"

#include <signal.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace room {

using avatar::Vec;

namespace {

constexpr int64_t kDrawNs = 30'000'000;   // someone is drawn as often as their pose comes, and no more: the headset has games to run
constexpr int64_t kSendNs = 30'000'000;   // your own pose goes out this often
constexpr double kInSight = 80 * bodypose::kPi / 180;   // ...and they are drawn only while within this of where the head looks
constexpr int kOrder = 3;   // among the overlays: over the mirror, under the panels (5-21)

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

int64_t MonoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

double CpuSeconds() {
    rusage r;
    getrusage(RUSAGE_SELF, &r);
    return double(r.ru_utime.tv_sec + r.ru_stime.tv_sec) + double(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}

// Whoever has one place in the room, once someone there was first drawn: their body on the GPU,
// what poses it, and the sheet they are shown on.
struct Drawn {
    bool made = false, failed = false;
    unsigned since = 0;   // which newcomer to the place this is (roompeople::Person::since)
    std::unique_ptr<figure::Figure> figure;
    bodygpu::Avatar avatar;
    sheet::Sheet sheet;
    int64_t posedAt = 0, drawnAt = 0;

    bool Make(const stage::Stage &stage, int place, int side) {
        const bodyfile::Body body = abstract::Body(place);
        const std::string key = "frametop.avatars.person." + std::to_string(place), name = "Person " + std::to_string(place + 1);
        if (!sheet.Make(stage.maker, key.c_str(), name.c_str(), kOrder, side, side)) return failed = true, false;
        avatar.Upload(stage.painter, body);
        figure = std::make_unique<figure::Figure>(body);
        return made = true;
    }
    // Out of sight, and what was known of how they moved forgotten.
    void Rest() {
        if (!made) return;
        sheet.Hide();
        figure->Reset();
    }
    void Free(const stage::Stage &stage) {
        sheet.Free(stage.maker);
        avatar.Free();
        *this = Drawn();
    }
};

}  // namespace

int Run(const Ask &ask) {
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    roomclient::Client client;
    std::string err;
    if (!client.Open(ask.own, ask.together, true, err)) return std::fprintf(stderr, "avatars: %s\n", err.c_str()), 1;
    stage::Stage stage;
    int status = stage.Open("avatars") ? 0 : 1;
    const int side = std::min(std::max(ask.side, 256), 1280);
    if (status == 0) std::printf("ft-avatars: the room's people, %dx%d an eye each\n", side, side);

    Drawn drawn[proto::kPeople];
    wearer::Watch watch;
    handcut::Hands hands;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    const int64_t start = MonoNs();
    int64_t lastSent = 0, lastStatus = start;
    int draws = 0, sent = 0;
    unsigned posesIn = 0;
    double cpu0 = CpuSeconds();
    std::string statusLine = "starting";

    while (status == 0 && !g_stop && (ask.seconds <= 0 || double(MonoNs() - start) / 1e9 < ask.seconds)) {
        const auto tick = std::chrono::steady_clock::now();
        const int64_t now = MonoNs();
        const double nowS = double(now) / 1e9;
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
            if (ev.eventType == vr::VREvent_Quit) vr::VRSystem()->AcknowledgeQuit_Exiting(), g_stop = 1;

        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
        const vr::TrackedDevicePose_t &hp = poses[vr::k_unTrackedDeviceIndex_Hmd];
        const handcut::Mat &head = hp.mDeviceToAbsoluteTracking;
        double eyes[2][3] = {};
        if (hp.bPoseIsValid) handcut::EyePositions(head, eyes);
        const Vec eye[2] = {{eyes[0][0], eyes[0][1], eyes[0][2]}, {eyes[1][0], eyes[1][1], eyes[1][2]}}, between = (eye[0] + eye[1]) * 0.5;
        const Vec ahead = hp.bPoseIsValid ? wearer::Watch::Ahead(head) : Vec{0, 0, -1};
        const double stands[3] = {between.x, between.z, roomfloor::Facing(ahead)};
        client.Tick(nowS, hp.bPoseIsValid ? stands : nullptr);

        roomclient::Question q;
        while (client.Next(q)) {
            std::string reply = "error state|quit";
            if (q.text == "state") reply = "ok " + statusLine;
            else if (q.text == "quit") reply = "ok", g_stop = 1;
            client.Reply(q, reply);
        }

        const roompeople::People &people = client.people();
        const bool there = hp.bPoseIsValid && people.inRoom() && client.laid();   // you are somewhere in the room
        if (there && now - lastSent >= kSendNs) {
            static const std::vector<handcut::Hand> kNone;
            const std::vector<handcut::Hand> &tracked = hands.Update(head, now) ? hands.shown() : kNone;
            const wearer::Seen seen = watch.See(head, eyes, tracked, lastSent ? std::min(1.0, double(now - lastSent) / 1e9) : 0);
            sent += client.Send(roomwire::ToRoom(seen, client.floor()));
            lastSent = now;
        }

        for (int place = 0; place < proto::kPeople; ++place) {
            Drawn &d = drawn[place];
            if (d.made) d.sheet.Show(stage.maker);
            if (!there || !people.Drawn(place, nowS) || d.failed) {
                d.Rest();
                continue;
            }
            if (!d.made && !d.Make(stage, place, side)) continue;
            const roompeople::Person &person = people.person(place);
            if (person.since != d.since) d.figure->Reset(), d.since = person.since;
            if (d.sheet.drawing() || now - d.drawnAt < kDrawNs) continue;
            if (!d.figure->Pose(person.pose, client.floor(), std::min(1.0, double(now - d.posedAt) / 1e9))) continue;
            d.posedAt = now;
            figure::View view;
            if (!d.figure->Seen(eye, 1, view)) {
                d.sheet.Hide();
                continue;
            }
            if (d.sheet.visible() && !mirror::InSight(view.sheet.glass, between, ahead, kInSight)) continue;   // behind you: as they were
            vroutput::Buffer &into = d.sheet.Next(&view.sheet.glass);
            windowview::Draw(stage.painter, d.avatar, d.figure->bones(), view.clip, view.sees, billboard::Light(), side, side);
            vroutput::Fence(stage.maker, into);
            d.drawnAt = now, ++draws;
        }

        if (now - lastStatus >= int64_t((ask.status > 0 ? ask.status : 2) * 1e9)) {
            const double dt = double(now - lastStatus) / 1e9, cpu1 = CpuSeconds();
            int here = 0, shown = 0;
            for (int place = 0; place < proto::kPeople; ++place) here += people.person(place).here && people.inRoom(), shown += drawn[place].made && drawn[place].sheet.visible();
            char line[320];
            if (!people.inRoom()) std::snprintf(line, sizeof line, "not in a room; CPU %.1f%%", 100 * (cpu1 - cpu0) / dt);
            else
                std::snprintf(line, sizeof line, "in a room of %d as %s, floor %s; %d shown, %.1f draws/s; poses %.1f/s in, %.1f/s out; CPU %.1f%%", here,
                              people.person(people.me()).name.c_str(), client.laid() ? "laid" : "not laid yet", shown, draws / dt, (client.posesIn() - posesIn) / dt, sent / dt,
                              100 * (cpu1 - cpu0) / dt);
            statusLine = line;
            if (ask.status > 0) std::printf("%s\n", line);
            draws = sent = 0, posesIn = client.posesIn(), lastStatus = now, cpu0 = cpu1;
        }
        // outside a room there is nothing to keep up with
        std::this_thread::sleep_until(tick + std::chrono::microseconds(people.inRoom() ? 11111 : 100000));
    }

    for (Drawn &d : drawn) d.Free(stage);
    stage.Close();
    client.Close();
    return status;
}

}  // namespace room
