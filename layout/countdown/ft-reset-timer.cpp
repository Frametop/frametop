// Local Frametop reset countdown. CPU drawing; three uploads in a short-lived process.
// --render-dir DIR is entirely offline and never initializes OpenVR.
#include <openvr.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
constexpr int kWidth = 640, kHeight = 360, kDurationMs = 3000;
using Clock = std::chrono::steady_clock;
using Pixels = std::vector<unsigned char>;
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }

bool Expired(double deadline) {
    if (!deadline) return false;
    // Python's time.monotonic() on the Linux host and this Distrobox use the
    // same CLOCK_MONOTONIC. Wall-clock adjustments do not extend the request.
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        throw std::runtime_error("cannot read monotonic clock");
    return double(now.tv_sec) + double(now.tv_nsec) / 1e9 >= deadline;
}

int NumberAt(int elapsedMs) {
    return elapsedMs >= kDurationMs ? 0 : 3 - std::max(0, elapsedMs) / 1000;
}

void Blend(Pixels &p, int x, int y, const std::array<int, 3> &rgb, float alpha) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight || alpha <= 0) return;
    auto *q = &p[(y * kWidth + x) * 4];
    const float old = q[3] / 255.f, a = std::min(alpha, 1.f), out = a + old * (1 - a);
    for (int c = 0; c < 3; ++c) q[c] = static_cast<unsigned char>(std::lround((rgb[c] * a + q[c] * old * (1 - a)) / out));
    q[3] = static_cast<unsigned char>(std::lround(out * 255));
}

class Renderer {
    std::vector<unsigned char> bytes_;
    stbtt_fontinfo font_{};

    void Text(Pixels &p, const char *s, int height, int centerY, std::array<int, 3> rgb) {
        const float scale = stbtt_ScaleForPixelHeight(&font_, float(height));
        int width = 0, ascent, descent, gap;
        stbtt_GetFontVMetrics(&font_, &ascent, &descent, &gap);
        for (const char *c = s; *c; ++c) {
            int advance, bearing;
            stbtt_GetCodepointHMetrics(&font_, *c, &advance, &bearing);
            width += int(std::lround(advance * scale));
        }
        int x = (kWidth - width) / 2;
        const int baseline = centerY + int(std::lround((ascent + descent) * scale / 2));
        for (const char *c = s; *c; ++c) {
            int w, h, xoff, yoff, advance, bearing;
            unsigned char *bitmap = stbtt_GetCodepointBitmap(&font_, 0, scale, *c, &w, &h, &xoff, &yoff);
            for (int yy = 0; yy < h; ++yy)
                for (int xx = 0; xx < w; ++xx)
                    Blend(p, x + xoff + xx, baseline + yoff + yy, rgb, bitmap[yy * w + xx] / 255.f);
            stbtt_FreeBitmap(bitmap, nullptr);
            stbtt_GetCodepointHMetrics(&font_, *c, &advance, &bearing);
            x += int(std::lround(advance * scale));
        }
    }

public:
    explicit Renderer(const std::string &path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f || f.tellg() <= 0 || f.tellg() > 20 * 1024 * 1024) throw std::runtime_error("cannot read font: " + path);
        bytes_.resize(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        if (!f.read(reinterpret_cast<char *>(bytes_.data()), std::streamsize(bytes_.size())) ||
            !stbtt_InitFont(&font_, bytes_.data(), stbtt_GetFontOffsetForIndex(bytes_.data(), 0)))
            throw std::runtime_error("cannot initialize font: " + path);
    }

    Pixels Draw(int number) {
        Pixels p(kWidth * kHeight * 4, 0);
        // Rounded dark panel with a quiet rim and transparent outside corners.
        for (int y = 12; y < kHeight - 12; ++y) {
            for (int x = 12; x < kWidth - 12; ++x) {
                const float qx = std::abs(x + .5f - kWidth / 2.f) - (kWidth / 2.f - 40);
                const float qy = std::abs(y + .5f - kHeight / 2.f) - (kHeight / 2.f - 40);
                const float d = std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - 28;
                const float cover = std::clamp(.5f - d, 0.f, 1.f);
                Blend(p, x, y, {20, 25, 35}, .95f * cover);
                if (d > -2) Blend(p, x, y, {112, 133, 158}, .4f * cover);
            }
        }
        Text(p, "Recentering", 30, 54, {180, 195, 213});
        const char digit[] = {char('0' + number), 0};
        Text(p, digit, 166, 157, {246, 249, 253});
        Text(p, "Look straight ahead", 34, 270, {231, 238, 246});
        for (int dot = 0; dot < 3; ++dot) {
            const int cx = kWidth / 2 + (dot - 1) * 24;
            for (int y = 308; y < 323; ++y)
                for (int x = cx - 7; x <= cx + 7; ++x) {
                    const float cover = std::clamp(6.5f - std::hypot(float(x - cx), float(y - 315)), 0.f, 1.f);
                    Blend(p, x, y, dot < number ? std::array<int, 3>{111, 189, 244} : std::array<int, 3>{60, 73, 92}, cover);
                }
        }
        return p;
    }
};

void Check(vr::EVROverlayError error, const char *operation) {
    if (error != vr::VROverlayError_None)
        throw std::runtime_error(std::string(operation) + ": " + vr::VROverlay()->GetOverlayErrorNameFromEnum(error));
}

class Overlay {
    bool initialized_ = false;
    vr::VROverlayHandle_t handle_ = vr::k_ulOverlayHandleInvalid;
public:
    ~Overlay() {
        if (handle_ != vr::k_ulOverlayHandleInvalid && vr::VROverlay()) {
            vr::VROverlay()->HideOverlay(handle_);
            vr::VROverlay()->DestroyOverlay(handle_);
        }
        if (initialized_) vr::VR_Shutdown();
    }

    bool Open(double deadline) {
        if (Expired(deadline)) return false;
        vr::EVRInitError error = vr::VRInitError_None;
        vr::VR_Init(&error, vr::VRApplication_Background);
        if (error != vr::VRInitError_None) throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        initialized_ = true;
        if (Expired(deadline)) return false;
        if (!vr::VROverlay()) throw std::runtime_error("OpenVR overlay interface unavailable");
        // Existing Frametop pointer code excludes frametop.guide* from collision rays.
        const std::string key = "frametop.guide.reset-countdown." + std::to_string(getpid());
        Check(vr::VROverlay()->CreateOverlay(key.c_str(), "Frametop reset countdown", &handle_), "create");
        if (Expired(deadline)) return false;
        Check(vr::VROverlay()->SetOverlayInputMethod(handle_, vr::VROverlayInputMethod_None), "input disabled");
        Check(vr::VROverlay()->SetOverlayFlag(handle_, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false), "interaction disabled");
        Check(vr::VROverlay()->SetOverlayWidthInMeters(handle_, .40f), "width");
        Check(vr::VROverlay()->SetOverlaySortOrder(handle_, 100), "sort order");
        vr::HmdMatrix34_t pose{};
        pose.m[0][0] = pose.m[1][1] = pose.m[2][2] = 1;
        pose.m[1][3] = .06f;
        pose.m[2][3] = -1.f;
        Check(vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(handle_, vr::k_unTrackedDeviceIndex_Hmd, &pose), "head-relative position");
        return !Expired(deadline);
    }

    void Picture(Pixels &pixels) {
        // Only three uploads, each in this short-lived process; no persistent Raw upload loop.
        Check(vr::VROverlay()->SetOverlayRaw(handle_, pixels.data(), kWidth, kHeight, 4), "image upload");
    }
    void Show() { Check(vr::VROverlay()->ShowOverlay(handle_), "show"); }
    bool QuitRequested() {
        vr::VREvent_t event{};
        while (vr::VROverlay()->PollNextOverlayEvent(handle_, &event, sizeof event)) {
            if (event.eventType == vr::VREvent_ImageFailed) throw std::runtime_error("OpenVR failed to load countdown image");
            if (event.eventType == vr::VREvent_Quit) return true;
        }
        return false;
    }
};

void CheckOffline(const std::array<Pixels, 3> &frames) {
    for (int elapsed : {0, 999, 1000, 1999, 2000, 2999, 3000, 9999}) {
        const int wanted = elapsed >= 3000 ? 0 : elapsed < 1000 ? 3 : elapsed < 2000 ? 2 : 1;
        if (NumberAt(elapsed) != wanted) throw std::runtime_error("countdown timing self-check failed");
    }
    if (frames[0] == frames[1] || frames[1] == frames[2]) throw std::runtime_error("rendered numbers are not distinct");
    for (const auto &frame : frames) {
        if (frame.size() != kWidth * kHeight * 4 || frame[3] != 0)
            throw std::runtime_error("frame dimensions or transparency self-check failed");
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        std::string renderDir, font;
        bool checkOnly = false;
        double deadline = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if ((arg == "--render-dir" || arg == "--font") && i + 1 < argc) {
                (arg == "--render-dir" ? renderDir : font) = argv[++i];
            } else if (arg == "--deadline" && i + 1 < argc) {
                const char *value = argv[++i];
                char *end = nullptr;
                deadline = std::strtod(value, &end);
                if (end == value || *end || !std::isfinite(deadline) || deadline <= 0)
                    throw std::runtime_error("--deadline needs positive finite CLOCK_MONOTONIC seconds");
            } else if (arg == "--check") {
                checkOnly = true;
            } else if (arg == "--help") {
                std::puts("ft-reset-timer [--font FILE] [--deadline SECONDS] [--render-dir DIR | --check]\nShows a non-interactive, head-following 3-2-1 for three seconds.\n--deadline is an absolute CLOCK_MONOTONIC expiry, used by ft-layout.\n--render-dir and --check are offline; they never initialize OpenVR.");
                return 0;
            } else {
                throw std::runtime_error("unknown or incomplete option: " + arg);
            }
        }
        if (Expired(deadline)) return 3;
        if (font.empty()) {
            for (const char *path : {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                     "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf"}) {
                if (std::filesystem::exists(path)) { font = path; break; }
            }
            if (font.empty()) throw std::runtime_error("Noto Sans font unavailable; pass --font FILE");
        }
        Renderer renderer(font);
        std::array<Pixels, 3> frames{renderer.Draw(3), renderer.Draw(2), renderer.Draw(1)};
        CheckOffline(frames);
        if (!renderDir.empty()) {
            std::filesystem::create_directories(renderDir);
            for (int i = 0; i < 3; ++i) {
                const auto path = std::filesystem::path(renderDir) / ("countdown-" + std::to_string(3 - i) + ".png");
                if (!stbi_write_png(path.c_str(), kWidth, kHeight, 4, frames[i].data(), kWidth * 4))
                    throw std::runtime_error("cannot write " + path.string());
            }
            return 0;
        }
        if (checkOnly) { std::puts("offline timing/render checks passed"); return 0; }
        std::signal(SIGINT, Stop);
        std::signal(SIGTERM, Stop);
        std::signal(SIGHUP, Stop);
        Overlay overlay;
        if (!overlay.Open(deadline)) return 3;
        overlay.Picture(frames[0]);
        if (Expired(deadline)) return 3;
        overlay.Show();
        const auto start = Clock::now();
        int displayed = 3;
        while (!stopped) {
            if (Expired(deadline)) return 3;
            const int elapsed = int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
            const int number = NumberAt(elapsed);
            if (number == 0) break;
            if (number != displayed) {
                overlay.Picture(frames[3 - number]);
                displayed = number;
            }
            if (overlay.QuitRequested()) return 3;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return stopped ? 3 : 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "ft-reset-timer: %s\n", error.what());
        return 1;
    }
}
