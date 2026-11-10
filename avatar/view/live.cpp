// The mirror, live (live.h).
#include "live.h"

#include "../../conf/ftsock.h"
#include "../bodyfile.h"
#include "../face/ownface.h"
#include "../pose/bodypose.h"
#include "below.h"
#include "belowview.h"
#include "bodygpu.h"
#include "gpuctx.h"
#include "handcut.h"
#include "mirror.h"
#include "mirrorview.h"
#include "rendernode.h"
#include "sheet.h"
#include "vroutput.h"
#include "wearer.h"

#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace live {

using avatar::Vec;

namespace {

constexpr int64_t kDrawNs = 22'000'000;   // the avatar is drawn at half the displays' rate: the headset has games to run
constexpr double kInSight = 80 * bodypose::kPi / 180;   // ...and only while the glass is within this of where the head looks

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

// A model file's body and its name. False with `err` for a file that can't be read or has no body.
bool Read(const std::string &path, bodyfile::Body &body, std::string &name, std::string &err) {
    modelfile::Model model;
    bool has = false;
    if (!bodyfile::ReadFile(path, model, body, has, err)) return false;
    if (!has) return err = path + " has hands and no body", false;
    name = model.info.name;
    return true;
}

// The body worn: its mesh on the GPU, and what poses it from its wearer.
struct Worn {
    std::string name;
    float rest[bodyfile::kRoles][3];   // its bones' rest places
    bodygpu::Avatar avatar;
    std::unique_ptr<bodypose::Solver> solver;
    std::unique_ptr<wearer::Tracked> tracked;

    // In place of the one before, if any.
    void Wear(const bodyfile::Body &body, const std::string &called, const bodygpu::Painter &painter) {
        avatar.Free();
        avatar.Upload(painter, body);
        solver = std::make_unique<bodypose::Solver>(body);
        tracked = std::make_unique<wearer::Tracked>(body);
        std::memcpy(rest, body.at, sizeof rest);
        name = called;
    }
    void Free() { avatar.Free(); }
};

}  // namespace

int Mirror(const Ask &ask) {
    // the file first: one that can't be worn ends here, with nothing started
    bodyfile::Body body;
    std::string name, err;
    if (!Read(ask.model, body, name, err)) return std::fprintf(stderr, "avatars: %s\n", err.c_str()), 1;
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    {
        sockaddr_un addr;
        if (bind(sock, reinterpret_cast<sockaddr *>(&addr), ft_sock_addr(FT_SOCK_AVATARS, &addr)) != 0)
            return std::perror("avatars: bind @" FT_SOCK_AVATARS " (already running?)"), 1;
    }
    vr::EVRInitError vrErr = vr::VRInitError_None;
    vr::VR_Init(&vrErr, vr::VRApplication_Overlay);
    if (vrErr != vr::VRInitError_None) return std::fprintf(stderr, "avatars: openvr: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(vrErr)), 1;
    if (!vr::VRIPCResourceManager()) return std::fprintf(stderr, "avatars: openvr: no IVRIPCResourceManagerClient\n"), 1;

    const int h = std::min(std::max(ask.height, 240), 1600), w = mirror::EyeWidth(mirror::Glass{}, h);
    const int side = std::min(std::max(ask.belowSide, 256), 1280);
    vroutput::Maker maker;
    maker.who = "avatars", maker.egl = gpuctx::Load(), maker.modifiers = vroutput::Modifiers();
    maker.gbm = rendernode::Acquire("avatars");
    EGLContext ctx = EGL_NO_CONTEXT;
    bodygpu::Painter painter;
    Worn worn;
    sheet::Sheet screen, under;   // the mirror; your own body below you
    int status = 1;
    if (maker.gbm && gpuctx::Open("avatars", maker.gbm, maker.egl, gpuctx::kDestroyImage, &maker.dpy, &ctx) && painter.Init("avatars") &&
        screen.Make(maker, "frametop.avatars.mirror", "Mirror", 2, w, h)) {   // under the panels (5-21): a panel in front of the glass covers it
        worn.Wear(body, name, painter);
        status = 0;
        std::printf("ft-avatars: the mirror, %s, %dx%d an eye\n", worn.name.c_str(), w, h);
    }
    // The body below is made when it is first wanted: its buffers are as big as the mirror's twice over.
    bool belowMade = false, belowOn = false;
    const auto wantBelow = [&](bool on) {
        if (on && !belowMade && !under.Make(maker, "frametop.avatars.below", "Your body", 1, side, side)) return under.Free(maker), false;
        belowMade = belowMade || on;
        if (!on && belowMade) under.Hide();
        return belowOn = on, true;
    };
    if (status == 0 && ask.below) wantBelow(true);

    static_assert(face::kWeights == bodymesh::kShapes, "a face's weights go by Shape::Id");
    ownface::Face face;
    float shapes[face::kWeights] = {};
    if (status == 0 && ask.voice) face.Listen("Avatar mouth");
    handcut::Hands hands;
    mirror::Glass glass;
    bool placed = false;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    float bones[bodyfile::kRoles][12], folded[bodyfile::kRoles][12];
    const int64_t start = MonoNs();
    int64_t lastDraw = 0, lastStatus = start, drawnAt = 0;
    int draws = 0, shows = 0, belowDraws = 0;
    double drawMs = 0, upMs = 0, cpu0 = CpuSeconds();
    std::string statusLine = "starting";

    while (status == 0 && !g_stop && (ask.seconds <= 0 || double(MonoNs() - start) / 1e9 < ask.seconds)) {
        const auto tick = std::chrono::steady_clock::now();
        const int64_t now = MonoNs();
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
            if (ev.eventType == vr::VREvent_Quit) vr::VRSystem()->AcknowledgeQuit_Exiting(), g_stop = 1;

        char buf[1100];
        sockaddr_un from{};
        socklen_t fromLen = sizeof from;
        ssize_t n;
        while ((n = recvfrom(sock, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen)) > 0) {
            buf[n] = 0;
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
            std::string reply = "error state|model PATH|place|below on|below off|quit";
            if (!std::strcmp(buf, "state")) {
                reply = "ok " + statusLine;
            } else if (!std::strcmp(buf, "quit")) {
                reply = "ok", g_stop = 1;
            } else if (!std::strcmp(buf, "place")) {
                reply = "ok", placed = false;
            } else if (!std::strcmp(buf, "below on") || !std::strcmp(buf, "below off")) {
                reply = wantBelow(buf[7] == 'n') ? "ok" : "error the body below can't be drawn";
            } else if (!std::strncmp(buf, "model ", 6)) {
                if (Read(buf + 6, body, name, err)) worn.Wear(body, name, painter), reply = "ok";
                else reply = "error " + err;
            }
            if (fromLen > offsetof(sockaddr_un, sun_path)) sendto(sock, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&from), fromLen);
            fromLen = sizeof from;
        }

        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
        const vr::TrackedDevicePose_t &hp = poses[vr::k_unTrackedDeviceIndex_Hmd];
        const handcut::Mat &head = hp.mDeviceToAbsoluteTracking;
        if (screen.Show(maker)) ++shows, upMs += double(MonoNs() - drawnAt) / 1e6;
        if (belowMade) under.Show(maker);
        if (hp.bPoseIsValid && now - lastDraw >= kDrawNs) {
            double eyes[2][3];
            handcut::EyePositions(head, eyes);
            const Vec eye[2] = {{eyes[0][0], eyes[0][1], eyes[0][2]}, {eyes[1][0], eyes[1][1], eyes[1][2]}}, ahead = wearer::Tracked::Ahead(head);
            const Vec between = (eye[0] + eye[1]) * 0.5;
            if (!placed) glass = mirror::Stand(between, ahead, 0), screen.Stand(glass), placed = true;
            // the wearer is followed whether or not they look: what trails their head, trails it
            static const std::vector<handcut::Hand> kNone;
            const std::vector<handcut::Hand> &tracked = hands.Update(head, now) ? hands.shown() : kNone;
            const double since = lastDraw ? double(now - lastDraw) / 1e9 : 0;
            const bodypose::Input in = worn.tracked->See(head, eyes, tracked, since);
            face.After(since, worn.avatar.has(bodyfile::Shape::kBlink), shapes);
            bodypose::Pose pose;
            if (worn.solver->Solve(in, pose)) {
                worn.solver->Matrices(pose, bones);
                worn.avatar.Shape(shapes);
                if (!screen.drawing() && (mirror::InSight(glass, between, ahead, kInSight) || !screen.visible())) {
                    const auto t0 = std::chrono::steady_clock::now();
                    vroutput::Buffer &into = screen.Next();
                    mirrorview::Draw(painter, &worn.avatar, bones, glass, eye, w, h);
                    vroutput::Fence(maker, into);
                    drawnAt = MonoNs();
                    ++draws, drawMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                }
                if (belowOn && !below::LooksDown(ahead, ask.belowFrom)) {
                    under.Hide();
                } else if (belowOn && !under.drawing()) {
                    const mirror::Glass at = below::Under(between);
                    const bool handDrawn[2] = {in.hand[0].tracked, in.hand[1].tracked};
                    std::memcpy(folded, bones, sizeof folded);
                    below::Fold(worn.rest, handDrawn, folded);
                    vroutput::Buffer &into = under.Next(&at);
                    belowview::Draw(painter, worn.avatar, folded, at, eye, side);
                    vroutput::Fence(maker, into);
                    ++belowDraws;
                }
            }
            lastDraw = now;
        }

        if (now - lastStatus >= int64_t((ask.status > 0 ? ask.status : 2) * 1e9)) {
            const double dt = double(now - lastStatus) / 1e9, cpu1 = CpuSeconds();
            char line[320];
            std::snprintf(line, sizeof line, "mirror %s, %s; %.1f draws/s, CPU %.2f ms a draw, up after %.1f ms; CPU %.1f%%; microphone %s; body below %s, %.1f draws/s",
                          screen.visible() ? "shown" : "not shown yet", worn.name.c_str(), draws / dt, draws ? drawMs / draws : 0.0, shows ? upMs / shows : 0.0,
                          100 * (cpu1 - cpu0) / dt, !ask.voice ? "not asked for" : face.hearing() ? "heard" : "not heard",
                          !belowOn ? "off" : under.visible() ? "shown" : "not in sight", belowDraws / dt);
            statusLine = line;
            if (ask.status > 0) std::printf("%s\n", line);
            draws = shows = belowDraws = 0, drawMs = upMs = 0, lastStatus = now, cpu0 = cpu1;
        }
        std::this_thread::sleep_until(tick + std::chrono::microseconds(11111));
    }

    if (belowMade) under.Free(maker);
    screen.Free(maker);
    worn.Free(), painter.Done();
    gpuctx::Close(maker.dpy, ctx);
    if (maker.gbm) rendernode::Release();
    vr::VR_Shutdown();
    close(sock);
    return status;
}

}  // namespace live
