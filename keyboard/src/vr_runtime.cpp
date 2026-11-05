#include "framekeyboard/vr_runtime.hpp"
#include <fstream>
#include <stdexcept>

namespace framekeyboard {
bool steamvr_running(const std::filesystem::path& proc) {
    std::error_code error;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator entry(proc, error); !error && entry != end;
         entry.increment(error)) {
        const auto id = entry->path().filename().string();
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        std::ifstream name(entry->path() / "comm");
        std::string process_name;
        if (std::getline(name, process_name) && process_name == "vrserver") {
            return true;
        }
    }
    return false;
}
void require_steamvr() {
    if (!steamvr_running()) {
        throw std::runtime_error("SteamVR must already be running");
    }
}
bool TrackingReadiness::should_sample(bool dashboard_visible, bool panel_requested, bool panel_visible,
                                      double now) const {
    return dashboard_visible && (panel_requested || waiting_) && (panel_visible || now >= next_check_);
}
void TrackingReadiness::sampled(bool valid, double now) {
    waiting_ = !valid;
    next_check_ = now + .25;
}
} // namespace framekeyboard
