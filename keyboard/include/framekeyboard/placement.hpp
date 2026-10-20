#pragma once
#include <array>
#include <optional>

namespace framekeyboard {
// Row-major rigid transform. Its columns are right, up, toward viewer, position.
using Transform = std::array<std::array<double, 4>, 3>;
// Normalize Steam's scaled overlay pose into a rigid anchor, retaining its full rotation.
std::optional<Transform> dashboard_anchor(const Transform& scaled_bottom);
Transform move_with_dashboard(const Transform& panel, const Transform& previous,
                              const Transform& current);
// Steam's main tab freezes when an app tab is selected. Carry its last live
// bottom-center anchor with the persistent dashboard bar during those tabs.
class DashboardAnchor {
  public:
    void restore(std::optional<Transform> anchor, std::optional<Transform> bar,
                 bool full_rotation = true);
    std::optional<Transform> update(std::optional<Transform> main, std::optional<Transform> bar);
    const std::optional<Transform>& bar() const { return bar_; }

  private:
    std::optional<Transform> anchor_, bar_;
    bool full_rotation_{true};
};
// Roll-only horizon assistance. Feed the uncorrected pose each frame so the
// animation never feeds its own correction back into the controller grab.
class HorizonAlignment {
  public:
    Transform update(const Transform& raw, double now, bool grabbed = false, bool enabled = true);
    void reset() { *this = {}; }
    bool animating() const { return animating_; }

  private:
    double started_{}, from_correction_{}, correction_{};
    bool within_threshold_{}, animating_{};
};
enum class PlacementAction { Smaller, Larger };
enum class MoveSource { Laser, Grip };
// Capture belongs to the initiating head/controller and cursor. Another pointer
// must not steal or release movement, even when it reuses the same cursor index.
class MoveCapture {
  public:
    bool begin(MoveSource source, unsigned device, unsigned pointer = 0) {
        if (active()) {
            return false;
        }
        source_ = source;
        device_ = device;
        pointer_ = pointer;
        return true;
    }
    bool active() const { return source_.has_value(); }
    std::optional<MoveSource> source() const { return source_; }
    unsigned device() const { return device_; }
    bool owns_pointer(unsigned device, unsigned pointer) const {
        return source_ == MoveSource::Laser && device_ == device && pointer_ == pointer;
    }
    void reset() { source_.reset(); }

  private:
    std::optional<MoveSource> source_;
    unsigned device_{}, pointer_{};
};
class PanelPlacement {
  public:
    void recenter(const Transform& head, const Transform* dashboard_bottom = nullptr,
                  double height_over_width = 600.0 / 1600.0);
    void adjust(PlacementAction action);
    Transform transform() const { return world_; }
    void set_transform(const Transform& world);
    void set_width(double width);
    void restore(const Transform& world, double width);
    double width() const { return width_; }
    bool ready() const { return ready_; }

  private:
    Transform world_{};
    double width_{.95};
    bool ready_{};
};
// Store the initial controller-to-panel transform, so grabbing never snaps the
// panel center to the ray. Later updates preserve the exact grabbed point.
class PanelDrag {
  public:
    void begin(const Transform& controller, const Transform& panel,
               std::array<double, 3> ray_direction = {0, 0, -1});
    void move_depth(double stick_y, double elapsed_seconds);
    Transform update(const Transform& controller) const;

  private:
    Transform relative_{};
    std::array<double, 3> ray_direction_{};
    double depth_{}, minimum_depth_{}, maximum_depth_{};
};
} // namespace framekeyboard
