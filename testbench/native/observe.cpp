#include "sdk.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using frame_observe::Sdk;
namespace {
int output_fd = STDOUT_FILENO;

void timeout_handler(int) {
    constexpr char message[] = "{\"ok\":false,\"error\":\"operation timed out; capture files may be incomplete\"}\n";
    // Only async-signal-safe operations. Do not block on a hung SDK destructor.
    const auto ignored = write(output_fd, message, sizeof(message) - 1);
    (void)ignored;
    _exit(124);
}

std::string quote(const std::string &s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"';
    return out.str();
}
const char *boolean(bool v) { return v ? "true" : "false"; }
void number(std::ostream &out, double v) {
    if (std::isfinite(v)) out << std::setprecision(10) << v;
    else out << "null";
}
std::string matrix(const vr::HmdMatrix34_t &m) {
    std::ostringstream out;
    out << '[';
    for (int r = 0; r < 3; ++r) {
        if (r) out << ',';
        out << '[';
        for (int c = 0; c < 4; ++c) { if (c) out << ','; number(out, m.m[r][c]); }
        out << ']';
    }
    out << ']';
    return out.str();
}
std::string vector(const vr::HmdVector3_t &v) {
    std::ostringstream out;
    out << '[';
    for (int i = 0; i < 3; ++i) { if (i) out << ','; number(out, v.v[i]); }
    out << ']';
    return out.str();
}
std::string pose(const vr::TrackedDevicePose_t &p) {
    std::ostringstream out;
    out << "{\"valid\":" << boolean(p.bPoseIsValid)
        << ",\"connected\":" << boolean(p.bDeviceIsConnected)
        << ",\"tracking_result\":" << p.eTrackingResult
        << ",\"matrix\":" << matrix(p.mDeviceToAbsoluteTracking)
        << ",\"velocity\":" << vector(p.vVelocity)
        << ",\"angular_velocity\":" << vector(p.vAngularVelocity) << '}';
    return out.str();
}

bool pause_setting(vr::IVRSettings *settings) {
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    bool value = settings->GetBool(vr::k_pch_Power_Section,
        vr::k_pch_Power_PauseCompositorOnStandby_Bool, &error);
    if (error != vr::VRSettingsError_None)
        throw std::runtime_error("reading power.pauseCompositorOnStandby failed: " + std::to_string(error));
    return value;
}

using PoseSnapshot = std::array<std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount>, 3>;
const vr::ETrackingUniverseOrigin origins[] = {vr::TrackingUniverseStanding,
    vr::TrackingUniverseRawAndUncalibrated, vr::TrackingUniverseSeated};
const char *origin_names[] = {"standing", "raw", "seated"};

std::string device_status(vr::IVRSystem *system, vr::TrackedDeviceIndex_t index,
                          const PoseSnapshot &poses, bool controller) {
    struct Property { const char *name; vr::ETrackedDeviceProperty key; };
    const Property properties[] = {{"driver", vr::Prop_TrackingSystemName_String},
        {"model", vr::Prop_ModelNumber_String}, {"serial", vr::Prop_SerialNumber_String}};
    std::ostringstream out, errors;
    std::string serial;
    out << "{\"device_index\":" << index;
    errors << '{';
    for (size_t i = 0; i < 3; ++i) {
        const auto &property = properties[i];
        std::array<char, vr::k_unMaxPropertyStringSize> value{};
        vr::ETrackedPropertyError error = vr::TrackedProp_Success;
        const auto count = system->GetStringTrackedDeviceProperty(index,
            property.key, value.data(), value.size(), &error);
        if (error == vr::TrackedProp_Success && (count == 0 || count > value.size() || value[count - 1] != '\0'))
            error = vr::TrackedProp_BufferTooSmall;
        out << ',' << quote(property.name) << ':';
        if (error == vr::TrackedProp_Success) {
            const std::string text(value.data(), count - 1);
            out << quote(text);
            if (property.key == vr::Prop_SerialNumber_String) serial = text;
        } else out << "null";
        if (i) errors << ',';
        errors << quote(property.name) << ':' << error;
    }
    errors << '}';
    out << ",\"property_errors\":" << errors.str();
    if (controller) {
        // Roles, model names and drivers can also belong to physical devices.
        // Only these exact serials identify our synthetic pose-only controllers.
        out << ",\"role\":" << system->GetControllerRoleForTrackedDeviceIndex(index)
            << ",\"synthetic\":" << boolean(serial == "frame_testbench_left" || serial == "frame_testbench_right");
    } else {
        out << ",\"activity_level\":" << system->GetTrackedDeviceActivityLevel(index);
    }
    out << ",\"poses\":{";
    for (size_t i = 0; i < poses.size(); ++i) {
        if (i) out << ',';
        out << quote(origin_names[i]) << ':' << pose(poses[i][index]);
    }
    out << "}}";
    return out.str();
}

std::string status(Sdk &sdk) {
    auto *system = sdk.get<vr::IVRSystem>(vr::IVRSystem_Version);
    auto *compositor = sdk.get<vr::IVRCompositor>(vr::IVRCompositor_Version);
    auto *settings = sdk.get<vr::IVRSettings>(vr::IVRSettings_Version);
    // OpenVR fills arrays indexed by the actual device index, not controller
    // role or position in an enumeration. Share one read per origin across all devices.
    PoseSnapshot poses{};
    for (size_t i = 0; i < poses.size(); ++i)
        system->GetDeviceToAbsoluteTrackingPose(origins[i], 0, poses[i].data(), poses[i].size());
    std::ostringstream out;
    out << "{\"ok\":true,\"command\":\"status\",\"hmd\":"
        << device_status(system, vr::k_unTrackedDeviceIndex_Hmd, poses, false)
        << ",\"controllers\":[";
    bool first = true;
    for (vr::TrackedDeviceIndex_t index = 0; index < vr::k_unMaxTrackedDeviceCount; ++index) {
        // Keep class-known disconnected devices visible for release readback.
        if (system->GetTrackedDeviceClass(index) != vr::TrackedDeviceClass_Controller) continue;
        if (!first) out << ',';
        first = false;
        out << device_status(system, index, poses, true);
    }
    out << "],\"transforms\":{\"raw_to_standing\":"
        << matrix(system->GetRawZeroPoseToStandingAbsoluteTrackingPose())
        << ",\"seated_to_standing\":" << matrix(system->GetSeatedZeroPoseToStandingAbsoluteTrackingPose()) << '}';
    vr::Compositor_FrameTiming current{}, previous{};
    current.m_nSize = sizeof(current);
    previous.m_nSize = sizeof(previous);
    const bool current_ok = compositor->GetFrameTiming(&current, 0);
    const bool previous_ok = compositor->GetFrameTiming(&previous, 1);
    float vsync_seconds = 0;
    uint64_t vsync_counter = 0;
    const bool vsync_ok = system->GetTimeSinceLastVsync(&vsync_seconds, &vsync_counter);
    out << ",\"compositor\":{\"timing_available\":" << boolean(current_ok)
        << ",\"frame_index\":" << (current_ok ? std::to_string(current.m_nFrameIndex) : "null")
        << ",\"previous_frame_index\":" << (previous_ok ? std::to_string(previous.m_nFrameIndex) : "null")
        << ",\"tracking_space\":" << compositor->GetTrackingSpace()
        << ",\"vsync_frame_counter\":" << (vsync_ok ? std::to_string(vsync_counter) : "null")
        << ",\"seconds_since_vsync\":";
    if (vsync_ok) number(out, vsync_seconds); else out << "null";
    out << ",\"system_time_seconds\":";
    if (current_ok) number(out, current.m_flSystemTimeInSeconds); else out << "null";
    out << ",\"num_frame_presents\":" << (current_ok ? std::to_string(current.m_nNumFramePresents) : "null")
        << ",\"num_dropped_frames\":" << (current_ok ? std::to_string(current.m_nNumDroppedFrames) : "null")
        << ",\"render_pose\":" << (current_ok ? pose(current.m_HmdPose) : "null")
        << "},\"settings\":{\"pauseCompositorOnStandby\":" << boolean(pause_setting(settings)) << "}}";
    return out.str();
}

void input_check(vr::EVRInputError error, const char *operation) {
    if (error != vr::VRInputError_None)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(error));
}

std::string input_origin(vr::IVRInput *input, vr::IVRSystem *system, vr::VRInputValueHandle_t handle) {
    vr::InputOriginInfo_t info{};
    info.trackedDeviceIndex = vr::k_unTrackedDeviceIndexInvalid;
    const auto error = input->GetOriginTrackedDeviceInfo(handle, &info, sizeof(info));
    const bool valid = error == vr::VRInputError_None && info.trackedDeviceIndex < vr::k_unMaxTrackedDeviceCount;
    vr::ETrackedPropertyError serial_error = vr::TrackedProp_InvalidDevice;
    std::string serial;
    if (valid) {
        std::array<char, vr::k_unMaxPropertyStringSize> buffer{};
        const auto count = system->GetStringTrackedDeviceProperty(info.trackedDeviceIndex,
            vr::Prop_SerialNumber_String, buffer.data(), buffer.size(), &serial_error);
        if (serial_error == vr::TrackedProp_Success) {
            if (!count || count > buffer.size() || buffer[count - 1] != '\0') serial_error = vr::TrackedProp_BufferTooSmall;
            else serial.assign(buffer.data(), count - 1);
        }
    }
    std::ostringstream out;
    out << "{\"handle\":" << quote(std::to_string(handle)) << ",\"error\":" << error
        << ",\"device_path_handle\":" << (valid ? quote(std::to_string(info.devicePath)) : "null")
        << ",\"device_index\":" << (valid ? std::to_string(info.trackedDeviceIndex) : "null")
        << ",\"serial\":" << (serial_error == vr::TrackedProp_Success ? quote(serial) : "null")
        << ",\"serial_error\":" << (valid ? std::to_string(serial_error) : "null")
        << ",\"synthetic\":" << boolean(serial == "frame_testbench_left" || serial == "frame_testbench_right") << '}';
    return out.str();
}

struct ObservedAction {
    std::string side, component, action, type;
    int axis = 0;
    bool reserved = false;
    vr::VRActionHandle_t handle = vr::k_ulInvalidActionHandle;
    vr::VRInputValueHandle_t device = vr::k_ulInvalidInputValueHandle;
    vr::EVRInputError handle_error = vr::VRInputError_None;
};

std::string inputs(Sdk &sdk, const fs::path &manifest, unsigned wait_ms) {
    auto *input = sdk.get<vr::IVRInput>(vr::IVRInput_Version);
    auto *system = sdk.get<vr::IVRSystem>(vr::IVRSystem_Version);
    input_check(input->SetActionManifestPath(manifest.c_str()), "SetActionManifestPath");
    vr::VRActiveActionSet_t set{};
    input_check(input->GetActionSetHandle("/actions/observe", &set.ulActionSet), "GetActionSetHandle");
    if (set.ulActionSet == vr::k_ulInvalidActionSetHandle)
        throw std::runtime_error("GetActionSetHandle returned invalid handle");
    std::vector<ObservedAction> actions;
    for (const std::string side : {"left", "right"}) {
        vr::VRInputValueHandle_t device = vr::k_ulInvalidInputValueHandle;
        auto source_error = input->GetInputSourceHandle(("/user/hand/" + side).c_str(), &device);
        if (source_error == vr::VRInputError_None && device == vr::k_ulInvalidInputValueHandle)
            source_error = vr::VRInputError_InvalidHandle;
        auto add = [&](const std::string &part, const std::string &component, const std::string &type, int axis = 0) {
            ObservedAction a;
            a.side = side;
            a.component = "/input/" + part + '/' + component;
            a.action = "/actions/observe/in/" + side + '_' + part + '_' + (type == "vector2" ? "position" : component);
            a.type = type;
            a.axis = axis;
            a.reserved = part == "system" || part == "thumbrest";
            a.device = device;
            a.handle_error = source_error;
            if (a.handle_error == vr::VRInputError_None) {
                a.handle_error = input->GetActionHandle(a.action.c_str(), &a.handle);
                if (a.handle_error == vr::VRInputError_None && a.handle == vr::k_ulInvalidActionHandle)
                    a.handle_error = vr::VRInputError_InvalidHandle;
            }
            actions.push_back(a);
        };
        std::vector<std::string> buttons = {"system", "bumper", "trigger", "grip", "thumbstick"};
        const std::vector<std::string> extra = side == "left"
            ? std::vector<std::string>{"view", "dpad_up", "dpad_right", "dpad_down", "dpad_left"}
            : std::vector<std::string>{"menu", "a", "b", "x", "y"};
        buttons.insert(buttons.end(), extra.begin(), extra.end());
        for (const auto &part : buttons) {
            add(part, "click", "boolean");
            add(part, "touch", "boolean");
        }
        add("thumbrest", "touch", "boolean");
        for (const auto &part : {"trigger", "grip"}) add(part, "value", "vector1");
        add("thumbstick", "x", "vector2");
        add("thumbstick", "y", "vector2", 1);
    }
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(wait_ms);
    unsigned updates = 0;
    for (;;) {
        const auto update_error = input->UpdateActionState(&set, sizeof(set), 1);
        ++updates;
        if (update_error != vr::VRInputError_None && update_error != vr::VRInputError_NoData)
            input_check(update_error, "UpdateActionState");
        bool ready = update_error == vr::VRInputError_None;
        std::ostringstream snapshot;
        snapshot << '{';
        if (update_error == vr::VRInputError_None) {
            for (const std::string side : {"left", "right"}) {
                if (side == "right") snapshot << ',';
                snapshot << quote(side) << ":{";
                bool first = true;
                for (const auto &a : actions) {
                    if (a.side != side) continue;
                    if (!first) snapshot << ',';
                    first = false;
                    auto error = a.handle_error;
                    vr::InputDigitalActionData_t digital{};
                    vr::InputAnalogActionData_t analog{};
                    const bool is_digital = a.type == "boolean";
                    if (error == vr::VRInputError_None) {
                        error = is_digital ? input->GetDigitalActionData(a.handle, &digital, sizeof(digital), a.device)
                            : input->GetAnalogActionData(a.handle, &analog, sizeof(analog), a.device);
                    }
                    const bool success = error == vr::VRInputError_None;
                    const bool active = success && (is_digital ? digital.bActive : analog.bActive);
                    if (!a.reserved && !active) ready = false;
                    snapshot << quote(a.component) << ":{\"action\":" << quote(a.action)
                        << ",\"type\":" << quote(a.type) << ",\"error\":" << error
                        << ",\"active\":" << (success ? boolean(active) : "null") << ",\"value\":";
                    if (!active) snapshot << "null";
                    else if (is_digital) snapshot << boolean(digital.bState);
                    else number(snapshot, a.axis == 1 ? analog.y : analog.x);
                    snapshot << ",\"origin\":";
                    if (active) snapshot << input_origin(input, system, is_digital ? digital.activeOrigin : analog.activeOrigin);
                    else snapshot << "null";
                    snapshot << '}';
                }
                snapshot << '}';
            }
        }
        snapshot << '}';
        const auto now = std::chrono::steady_clock::now();
        if (ready || now >= deadline) {
            input_check(update_error, "UpdateActionState");
            std::ostringstream out;
            out << "{\"ok\":true,\"command\":\"inputs\",\"manifest\":" << quote(manifest.string())
                << ",\"input_available\":" << boolean(system->IsInputAvailable())
                << ",\"action_set\":\"/actions/observe\",\"binding_ready\":" << boolean(ready)
                << ",\"wait_expired\":" << boolean(!ready) << ",\"updates\":" << updates
                << ",\"wait_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count()
                << ",\"inputs\":" << snapshot.str() << '}';
            return out.str();
        }
        // A fresh process may see inactive bindings while SteamVR loads the manifest.
        // Never wait forever for reserved system or SteamVR-internal thumbrest inputs.
        std::this_thread::sleep_for(std::min(std::chrono::milliseconds(50),
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    }
}

std::string set_pause(Sdk &sdk, bool requested) {
    auto *settings = sdk.get<vr::IVRSettings>(vr::IVRSettings_Version);
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    settings->SetBool(vr::k_pch_Power_Section, vr::k_pch_Power_PauseCompositorOnStandby_Bool, requested, &error);
    if (error != vr::VRSettingsError_None)
        throw std::runtime_error("writing power.pauseCompositorOnStandby failed: " + std::to_string(error));
    bool actual = pause_setting(settings);
    if (actual != requested) throw std::runtime_error("power.pauseCompositorOnStandby readback mismatch");
    return std::string("{\"ok\":true,\"command\":\"setting\",\"pauseCompositorOnStandby\":") + boolean(actual) + '}';
}

uint32_t big_endian(const unsigned char *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc;
}
// Require signature, IHDR dimensions, IDAT, every chunk CRC, and terminal IEND.
// A path, a stable file size, or an early PNG signature alone is not completion.
// This checks container completeness, not the decoded pixels or headset optics.
bool complete_png(const fs::path &path) {
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(path, error))) return false;
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 8> header{};
    const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!file.read(reinterpret_cast<char *>(header.data()), 8) || std::memcmp(header.data(), signature, 8)) return false;
    bool ihdr = false, idat = false;
    uint64_t total = 8;
    std::array<unsigned char, 65536> buffer{};
    for (;;) {
        if (!file.read(reinterpret_cast<char *>(header.data()), 8)) return false;
        uint32_t size = big_endian(header.data());
        total += uint64_t(size) + 12;
        if (total > 256U * 1024U * 1024U) return false;
        const std::string type(reinterpret_cast<char *>(header.data() + 4), 4);
        if (!ihdr && (type != "IHDR" || size != 13)) return false;
        if (ihdr && type == "IHDR") return false;
        uint32_t crc = crc_update(0xffffffffU, header.data() + 4, 4);
        uint32_t left = size;
        while (left) {
            size_t count = std::min<size_t>(left, buffer.size());
            if (!file.read(reinterpret_cast<char *>(buffer.data()), count)) return false;
            if (type == "IHDR" && (!big_endian(buffer.data()) || !big_endian(buffer.data() + 4))) return false;
            crc = crc_update(crc, buffer.data(), count);
            left -= uint32_t(count);
        }
        std::array<unsigned char, 4> expected{};
        if (!file.read(reinterpret_cast<char *>(expected.data()), 4) || big_endian(expected.data()) != (crc ^ 0xffffffffU)) return false;
        if (type == "IHDR") ihdr = true;
        if (type == "IDAT" && size) idat = true;
        if (type == "IEND") return size == 0 && ihdr && idat && file.peek() == std::char_traits<char>::eof();
    }
}

std::string capture(Sdk &sdk, const fs::path &directory) {
    auto *screenshots = sdk.get<vr::IVRScreenshots>(vr::IVRScreenshots_Version);
    // Atomic mkdir is the final no-overwrite check, including races since CLI validation.
    if (mkdir(directory.c_str(), 0700) != 0)
        throw std::runtime_error("cannot create fresh capture directory: " + std::string(std::strerror(errno)));
    const std::string preview = (directory / "preview.png").string();
    const std::string stereo = (directory / "stereo.png").string();
    vr::ScreenshotHandle_t handle = vr::k_unScreenshotHandleInvalid;
    const auto error = screenshots->RequestScreenshot(&handle, vr::VRScreenshotType_Stereo,
        (directory / "preview").c_str(), (directory / "stereo").c_str());
    if (error != vr::VRScreenshotError_None || handle == vr::k_unScreenshotHandleInvalid)
        throw std::runtime_error("RequestScreenshot failed: " + std::to_string(error));
    // The process-wide alarm also bounds hung SDK calls and shutdown. Failed or
    // timed-out directories are retained for inspection, never reused or deleted.
    while (!complete_png(preview) || !complete_png(stereo))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return "{\"ok\":true,\"command\":\"capture\",\"screenshot_handle\":" + std::to_string(handle)
        + ",\"preview\":" + quote(preview) + ",\"stereo\":" + quote(stereo)
        + ",\"capture_scope\":\"OpenVR stereo screenshot; may include overlays, cropped FOV; not full lens output or camera proof\"}";
}

std::string debug(Sdk &sdk, uint32_t device, const std::string &request) {
    auto *api = sdk.get<vr::IVRDebug>(vr::IVRDebug_Version);
    // One invocation only: a debug request can have side effects. Never repeat
    // it merely to size the response buffer.
    std::array<char, 65536> response{};
    const auto length = api->DriverDebugRequest(device, request.c_str(), response.data(), response.size());
    if (length > response.size() || !std::memchr(response.data(), '\0', response.size()))
        throw std::runtime_error("DriverDebugRequest response exceeds buffer");
    return "{\"ok\":true,\"command\":\"debug\",\"device_index\":" + std::to_string(device)
        + ",\"response\":" + quote(response.data()) + '}';
}

unsigned integer(const std::string &s, unsigned minimum, unsigned maximum) {
    if (s.empty()) throw std::runtime_error("expected integer");
    unsigned value = 0;
    for (unsigned char c : s) {
        if (c < '0' || c > '9') throw std::runtime_error("expected decimal integer");
        const unsigned digit = c - '0';
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10))
            throw std::runtime_error("integer outside allowed range");
        value = value * 10 + digit;
    }
    if (value < minimum) throw std::runtime_error("integer outside allowed range");
    return value;
}

void emit(const std::string &value) {
    const std::string line = value + '\n';
    size_t offset = 0;
    while (offset < line.size()) {
        const auto n = write(output_fd, line.data() + offset, line.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += size_t(n);
    }
}
} // namespace

int main(int argc, char **argv) {
    // Isolate SDK stdout chatter, including dlopen constructors and shutdown.
    output_fd = dup(STDOUT_FILENO);
    if (output_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return 1;
    try {
        const std::string usage = "usage: frame-observe status | inputs ABS_MANIFEST_PATH | capture ABS_OUTPUT_DIR | setting true|false|1|0 | debug DEVICE_INDEX REQUEST";
        if (argc < 2) throw std::runtime_error(usage);
        const std::string command = argv[1];
        fs::path directory;
        bool requested = false;
        unsigned device = 0;
        unsigned input_wait_ms = 2000;
        if (command == "status" && argc == 2) {}
        else if (command == "inputs" && argc == 3) {
            directory = fs::path(argv[2]);
            if (!directory.is_absolute()) throw std::runtime_error("input manifest must be absolute");
            if (!fs::is_regular_file(directory)) throw std::runtime_error("input manifest must be an existing regular file");
            if (const char *value = std::getenv("FRAME_OBSERVE_INPUT_WAIT_MS"))
                input_wait_ms = integer(value, 0, 10000);
        } else if (command == "capture" && argc == 3) {
            directory = fs::path(argv[2]);
            if (!directory.is_absolute()) throw std::runtime_error("capture output must be absolute");
            std::error_code error;
            auto existing = fs::symlink_status(directory, error);
            if (fs::exists(existing)) throw std::runtime_error("capture output already exists; use a fresh directory");
            if (error && error != std::errc::no_such_file_or_directory)
                throw std::runtime_error("cannot inspect capture output: " + error.message());
        } else if (command == "setting" && argc == 3) {
            const std::string value = argv[2];
            if (value != "true" && value != "false" && value != "1" && value != "0") throw std::runtime_error(usage);
            requested = value == "true" || value == "1";
        } else if (command == "debug" && argc == 4 && argv[3][0]) {
            device = integer(argv[2], 0, vr::k_unMaxTrackedDeviceCount - 1);
        } else throw std::runtime_error(usage);
        unsigned timeout = 20;
        if (const char *value = std::getenv("FRAME_OBSERVE_TIMEOUT_SECONDS")) timeout = integer(value, 1, 120);
        struct sigaction action{};
        action.sa_handler = timeout_handler;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGALRM, &action, nullptr) != 0) throw std::runtime_error("cannot install timeout handler");
        alarm(timeout);
        std::string result;
        {
            Sdk sdk;
            if (command == "status") result = status(sdk);
            else if (command == "inputs") result = inputs(sdk, directory, input_wait_ms);
            else if (command == "setting") result = set_pause(sdk, requested);
            else if (command == "capture") result = capture(sdk, directory);
            else result = debug(sdk, device, argv[3]);
        }
        alarm(0);
        emit(result);
        return 0;
    } catch (const std::exception &error) {
        alarm(0);
        emit("{\"ok\":false,\"error\":" + quote(error.what()) + '}');
        return 1;
    }
}
