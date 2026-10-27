#pragma once
#include "openvr_driver.h"
#include "controller_inputs.hpp"
#include <atomic>
#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace frame {
// Runtime calls and snapshot/override changes share one lock. In particular a
// publication copied before release can never land after release's restoration.
// Recursive for synchronous runtime callbacks such as GetPose during publication.
// Registration and property writes must happen outside this lock.
class InputState {
    std::recursive_mutex mutex_;
    vr::IVRServerDriverHost *host_ = nullptr;
    vr::IVRDriverInput *input_ = nullptr;
    vr::TrackedDeviceIndex_t index_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t container_ = 0;
    std::optional<vr::DriverPose_t> physical_, requested_;
    struct Controller {
        uint32_t index = vr::k_unTrackedDeviceIndexInvalid;
        std::optional<vr::DriverPose_t> requested;
        ControllerInputs inputs;
        bool inputs_ready = false;
        std::string error_path;
        vr::EVRInputError input_error = vr::VRInputError_None;
    };
    std::array<Controller, 2> controllers_{};
    struct Proximity { std::optional<bool> physical; double offset = 0; };
    std::map<vr::VRInputComponentHandle_t, Proximity> proximity_;
    std::optional<bool> worn_;
    uint64_t sequence_ = 0;
    bool controller_update_locked(Controller &, const std::string &, ControllerInput &, float);
    void publish_locked();
    bool controller_input_release_locked(unsigned hand);
    bool controller_release_locked(unsigned hand, bool force = false);
    std::string status_locked(bool ok, const char *error);
public:
    void reset(vr::IVRServerDriverHost *, vr::IVRDriverInput *);
    void activate(uint32_t, vr::PropertyContainerHandle_t);
    void deactivate(uint32_t);
    void controller_activate(unsigned hand, uint32_t index, ControllerInputs inputs);
    void controller_deactivate(unsigned hand);
    vr::DriverPose_t controller_pose(unsigned hand);
    void component(vr::PropertyContainerHandle_t, const char *, vr::VRInputComponentHandle_t);
    void physical_pose(uint32_t, const vr::DriverPose_t &, uint32_t);
    vr::EVRInputError physical_boolean(vr::VRInputComponentHandle_t, bool, double);
    void tick();
    std::string command(const std::string &);
};

class Control {
    InputState &state_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    int listener_ = -1;
    std::string path_;
    unsigned long device_ = 0, inode_ = 0;
    void run();
public:
    explicit Control(InputState &state) : state_(state) {}
    ~Control();
    bool start();
    void stop();
};
} // namespace frame
