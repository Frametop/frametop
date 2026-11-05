#pragma once
#include <filesystem>

namespace framekeyboard {
bool steamvr_running(const std::filesystem::path& proc = "/proc");
void require_steamvr();

// Tracking recovery must remain observable after a replacement lease hides the panel.
class TrackingReadiness {
  public:
    bool should_sample(bool dashboard_visible, bool panel_requested, bool panel_visible,
                       double now) const;
    void sampled(bool valid, double now);
    void request_sample() { next_check_ = 0; }
    bool waiting() const { return waiting_; }

  private:
    bool waiting_{};
    double next_check_{};
};
} // namespace framekeyboard
