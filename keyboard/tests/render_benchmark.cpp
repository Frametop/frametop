// Opt-in benchmark of the production paint/conversion/upload path. Synthetic
// pointers terminate at NullSink; no connection to a desktop input backend exists.
#include "framekeyboard/app.hpp"
#include "framekeyboard/vr_texture.hpp"
#include "openvr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
using Clock = std::chrono::steady_clock;
namespace {
double cpu_seconds() {
    timespec value{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
        throw std::runtime_error("process CPU clock unavailable");
    }
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) / 1e9;
}
struct Samples {
    std::vector<double> wall;
    double cpu{};
    template <class F> void measure(F&& work) {
        const double start_cpu = cpu_seconds();
        const auto start = Clock::now();
        work();
        wall.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        cpu += cpu_seconds() - start_cpu;
    }
    void print(const char* name) const {
        if (wall.empty()) {
            return;
        }
        auto sorted = wall;
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](double fraction) {
            return sorted[static_cast<std::size_t>(std::ceil(fraction * sorted.size())) - 1];
        };
        std::cout << name << ": n=" << wall.size()
                  << " mean_ms=" << std::accumulate(wall.begin(), wall.end(), 0.0) / wall.size()
                  << " median_ms=" << percentile(.5) << " p95_ms=" << percentile(.95)
                  << " cpu_ms_per_redraw=" << cpu * 1000 / wall.size() << '\n';
    }
};
class Upload {
  public:
    ~Upload() {
        if (connected_) {
            if (handle_) {
                vr::VROverlay()->ClearOverlayTexture(handle_);
                vr::VROverlay()->DestroyOverlay(handle_);
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
            // Match the app's lifetime ordering: the compositor releases images
            // before the Vulkan resources go away.
            vr::VR_Shutdown();
            texture_.destroy();
            context_.destroy();
        }
    }
    void connect(bool prefer_native) {
        bool running = false;
        for (const auto& entry : fs::directory_iterator("/proc")) {
            std::ifstream input(entry.path() / "comm");
            std::string name;
            if (std::getline(input, name) && name == "vrserver") {
                running = true;
                break;
            }
        }
        if (!running) {
            throw std::runtime_error("benchmark will not start SteamVR; open it first");
        }
        vr::EVRInitError error{};
        vr::VR_Init(&error, vr::VRApplication_Overlay);
        if (error != vr::VRInitError_None) {
            throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        }
        connected_ = true;
        if (!vr::VROverlay() || !vr::VRCompositor() || !vr::VRSystem()) {
            throw std::runtime_error("required OpenVR interfaces unavailable");
        }
        const auto key = "org.framekeyboard.benchmark." + std::to_string(getpid());
        if (vr::VROverlay()->CreateOverlay(key.c_str(), "Rendering benchmark", &handle_) !=
            vr::VROverlayError_None) {
            throw std::runtime_error("could not create isolated overlay");
        }
        // Never show the overlay or enable interaction. Upload measurements include
        // the fence wait and API submission, but exclude visible compositor drawing.
        std::string message;
        if (!context_.init(message) || !texture_.create(context_, handle_, message, prefer_native)) {
            throw std::runtime_error(message);
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(context_.physicalDevice(), &properties);
        std::cout << "gpu=" << properties.deviceName
                  << " dashboard_visible=" << vr::VROverlay()->IsDashboardVisible() << '\n';
    }
    bool native_pixels() const { return texture_.native_pixels(); }
    void verify_upload(const PanelRenderer& renderer) {
        if (!connected_) {
            return;
        }
        bool premultiplied = false;
        const auto flag_error =
            vr::VROverlay()->GetOverlayFlag(handle_, vr::VROverlayFlags_IsPremultiplied, &premultiplied);
        if (flag_error != vr::VROverlayError_None || premultiplied != native_pixels()) {
            throw std::runtime_error("overlay alpha flag does not match submitted pixels");
        }
        std::vector<unsigned char> pixels(panel_width * texture_height * 4);
        uint32_t width = 0, height = 0;
        const auto result = vr::VROverlay()->GetOverlayImageData(
            handle_, pixels.data(), static_cast<uint32_t>(pixels.size()), &width, &height);
        if (result != vr::VROverlayError_None) {
            std::cout << "overlay_readback=" << vr::VROverlay()->GetOverlayErrorNameFromEnum(result)
                      << " (visual validation still required)\n";
            return;
        }
        if (width != panel_width || height != texture_height) {
            throw std::runtime_error("overlay readback dimensions differ");
        }
        const auto* native_bytes = cairo_image_surface_get_data(renderer.surface());
        if (native_pixels() && std::equal(pixels.begin(), pixels.end(), native_bytes)) {
            // Frame returns raw BGRA for a BGRA Vulkan image despite OpenVR's
            // documented RGBA readback. This checks transfer bytes, not the final
            // compositor's channel/alpha interpretation on the headset display.
            std::cout << "overlay_readback=raw_BGRA_matches_Cairo; visual_check_required\n";
            return;
        }
        const auto expected = renderer.rgba();
        unsigned checked = 0;
        for (std::size_t at = 0; at < pixels.size(); at += 4) {
            if (expected[at + 3] != 255) {
                continue;
            }
            for (unsigned channel = 0; channel < 4; ++channel) {
                if (pixels[at + channel] != expected[at + channel]) {
                    throw std::runtime_error("overlay readback color mismatch");
                }
            }
            ++checked;
        }
        std::cout << "overlay_readback_opaque_pixels_verified=" << checked << '\n';
    }

    void submit(const PanelRenderer& renderer) {
        if (connected_) {
            std::string error;
            if (!texture_.update(renderer, error)) {
                throw std::runtime_error(error);
            }
        }
    }

  private:
    bool connected_{};
    vr::VROverlayHandle_t handle_{};
    VulkanContext context_;
    PanelTexture texture_;
};
std::pair<double, double> key_position(App& app, const std::string& action) {
    const auto view = app.view();
    for (int y = 96; y < panel_height; y += 4) {
        for (int x = 0; x < panel_width; x += 4) {
            const auto* key = app.renderer.hit_key(view, x, y);
            if (key && key->action == action) {
                return {x, y};
            }
        }
    }
    throw std::runtime_error("benchmark key not found");
}
void workload(App& app, Upload& upload, const char* name, double duration, double keys_per_second,
              bool force_redraw, bool use_vr, bool full_paint) {
    const auto a = key_position(app, "KeyA"), b = key_position(app, "KeyB");
    app.cancel();
    app.paint(monotonic_seconds());
    upload.submit(app.renderer);
    Samples paint, transfer, total;
    unsigned frames = 0, presses = 0, overruns = 0;
    bool held = false;
    std::pair<double, double> pointer = a;
    double next_press = 0, release_at = 0;
    const double start = monotonic_seconds(), start_cpu = cpu_seconds();
    while (monotonic_seconds() - start < duration) {
        const auto frame_start = Clock::now();
        const double now = monotonic_seconds(), elapsed = now - start;
        if (held && elapsed >= release_at) {
            app.up(0, pointer.first, pointer.second);
            held = false;
        }
        if (keys_per_second > 0 && !held && elapsed >= next_press && elapsed < duration - .3) {
            pointer = presses % 2 == 0 ? a : b;
            app.move(0, pointer.first, pointer.second);
            if (!app.down(0, pointer.first, pointer.second, now)) {
                throw std::runtime_error("synthetic key press rejected");
            }
            held = true;
            ++presses;
            release_at = elapsed + .1;
            next_press = elapsed + 1 / keys_per_second;
        }
        const bool dirty = app.tick(now);
        if (force_redraw || dirty) {
            total.measure([&] {
                paint.measure([&] {
                    if (full_paint || force_redraw) {
                        app.renderer.paint(app.view(), now, true);
                        app.dirty = false;
                    } else {
                        app.paint(now);
                    }
                });
                if (use_vr) {
                    transfer.measure([&] { upload.submit(app.renderer); });
                }
            });
        }
        ++frames;
        const auto deadline = frame_start + std::chrono::microseconds(16667);
        overruns += Clock::now() > deadline;
        // Use the production loop's processing-inclusive 60 Hz deadline.
        std::this_thread::sleep_until(deadline);
    }
    const double cpu = cpu_seconds() - start_cpu, wall = monotonic_seconds() - start;
    std::cout << "\nworkload=" << name << " wall_s=" << wall << " cpu_s=" << cpu
              << " cpu_percent_one_core=" << 100 * cpu / wall << " frames=" << frames
              << " redraws=" << total.wall.size() << " presses=" << presses
              << " redraws_per_key=" << (presses ? double(total.wall.size()) / presses : 0)
              << " frames_over_16_667ms=" << overruns << '\n';
    paint.print("paint");

    transfer.print(upload.native_pixels() ? "native_upload" : "rgba_conversion_and_upload");
    total.print("total");
    std::cout << std::flush;
}
} // namespace
int main(int argc, char** argv) {
    fs::path temporary;
    try {
        bool use_vr = false, prefer_native = true, full_paint = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--vr") {
                use_vr = true;
            } else if (arg == "--rgba") {
                prefer_native = false;
            } else if (arg == "--full-paint") {
                full_paint = true;
            } else {
                throw std::runtime_error("usage: render-benchmark [--vr] [--rgba] [--full-paint]");
            }
        }
        char pattern[] = "/tmp/framekeyboard-render-benchmark-XXXXXX";
        const char* directory = mkdtemp(pattern);
        if (!directory) {
            throw std::runtime_error("cannot create isolated config directory");
        }
        temporary = directory;
        Options options;
        options.mode = Mode::Render; // Same panel without the preview-only notice.
        options.config_dir = temporary;
        NullSink sink;
        App app(options, sink);
        Upload upload;
        std::cout << std::fixed << std::setprecision(3)
                  << "1600x600 bundled English/Graphite; NullSink; upload=" << use_vr << '\n';
        if (use_vr) {
            upload.connect(prefer_native);
        }
        for (int i = 0; i < 30; ++i) {
            app.renderer.paint(app.view(), monotonic_seconds(), true);
            app.dirty = false;
            upload.submit(app.renderer);
        }
        upload.verify_upload(app.renderer);
        std::cout << "native_pixels=" << upload.native_pixels() << " full_paint=" << full_paint << '\n';
        workload(app, upload, "idle", 5, 0, false, use_vr, full_paint);
        workload(app, upload, "one_key_per_second", 10, 1, false, use_vr, full_paint);
        workload(app, upload, "four_keys_per_second", 15, 4, false, use_vr, full_paint);
        workload(app, upload, "forced_every_frame", 5, 0, true, use_vr, full_paint);
        fs::remove_all(temporary);
        return 0;
    } catch (const std::exception& error) {
        if (!temporary.empty()) {
            fs::remove_all(temporary);
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
