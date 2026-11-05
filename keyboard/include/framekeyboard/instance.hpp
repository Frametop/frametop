#pragma once

#include <filesystem>

namespace framekeyboard {

// One VR owner per login session. A second launch sends a recenter request and
// exits before initializing OpenVR or creating another virtual input device.
class VrInstance {
  public:
    explicit VrInstance(const std::filesystem::path& directory = runtime_directory(),
                        bool notify_existing = true);
    ~VrInstance();
    VrInstance(const VrInstance&) = delete;
    VrInstance& operator=(const VrInstance&) = delete;
    bool is_owner() const { return owner_; }
    bool poll_recenter();
    static std::filesystem::path runtime_directory();

  private:
    void start_listener();
    bool notify_owner();
    std::filesystem::path socket_path_;
    int lock_{-1};
    int listener_{-1};
    bool owner_{};
};
} // namespace framekeyboard
