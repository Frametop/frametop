#include "input_state.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <locale>
#include <set>
#include <vector>
using namespace vr;
namespace frame {
namespace {
void number(std::ostream &o, double v) { if (std::isfinite(v)) o << v; else o << "null"; }
void transform_json(std::ostream &o, const double *v, const HmdQuaternion_t &q) {
    o << "{\"position\":[";
    for (int i = 0; i < 3; ++i) { if (i) o << ','; number(o, v[i]); }
    o << "],\"quaternion\":[";
    number(o, q.w); o << ','; number(o, q.x); o << ','; number(o, q.y); o << ','; number(o, q.z);
    o << "]}";
}
void pose_json(std::ostream &o, const std::optional<DriverPose_t> &p) {
    if (!p) { o << "null"; return; }
    o << "{\"position\":[";
    for (int i = 0; i < 3; ++i) { if (i) o << ','; number(o, p->vecPosition[i]); }
    o << "],\"quaternion\":[";
    number(o, p->qRotation.w); o << ','; number(o, p->qRotation.x); o << ',';
    number(o, p->qRotation.y); o << ','; number(o, p->qRotation.z);
    o << "],\"valid\":" << p->poseIsValid << ",\"connected\":" << p->deviceIsConnected;
    o << ",\"world_from_driver\":"; transform_json(o, p->vecWorldFromDriverTranslation, p->qWorldFromDriverRotation);
    o << ",\"driver_from_head\":"; transform_json(o, p->vecDriverFromHeadTranslation, p->qDriverFromHeadRotation);
    o << '}';
}
}
void InputState::reset(IVRServerDriverHost *h, IVRDriverInput *i) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    host_ = h; input_ = i; index_ = k_unTrackedDeviceIndexInvalid; container_ = 0;
    physical_.reset(); requested_.reset(); worn_.reset(); proximity_.clear(); sequence_ = 0;
    controllers_ = {};
}
void InputState::activate(uint32_t i, PropertyContainerHandle_t c) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    index_ = i; container_ = c; physical_.reset(); proximity_.clear(); ++sequence_;
}
void InputState::deactivate(uint32_t i) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (index_ != i) return;
    index_ = k_unTrackedDeviceIndexInvalid; container_ = 0; proximity_.clear(); physical_.reset(); ++sequence_;
}
void InputState::controller_activate(unsigned hand, uint32_t index, ControllerInputs inputs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto &c = controllers_.at(hand);
    c = {};
    c.index = index;
    c.inputs = std::move(inputs);
    c.inputs_ready = input_ && !c.inputs.empty();
    for (auto &entry : c.inputs) {
        auto &component = entry.second;
        auto error = component.creation_error;
        if (error != VRInputError_None) {
            c.inputs_ready = false;
            c.error_path = entry.first; c.input_error = error;
        } else if (input_ && !controller_update_locked(c, entry.first, component, 0)) {
            c.inputs_ready = false;
        }
    }
    ++sequence_;
    if (host_) {
        const auto pose = controller_pose(hand);
        host_->TrackedDevicePoseUpdated(index, pose, sizeof(pose));
    }
}
void InputState::controller_deactivate(unsigned hand) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    controller_release_locked(hand, true);
    auto &c = controllers_.at(hand);
    // The SDK has no unregister method. Deactivate invalidates this generation;
    // retain any forced-release error for status, but never reuse its handles.
    auto error_path = c.error_path;
    const auto error = c.input_error;
    c = {};
    c.error_path = std::move(error_path); c.input_error = error;
    ++sequence_;
}
DriverPose_t InputState::controller_pose(unsigned hand) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto &c = controllers_.at(hand);
    if (c.index != k_unTrackedDeviceIndexInvalid && c.requested) return *c.requested;
    DriverPose_t p{};
    p.qRotation.w = p.qWorldFromDriverRotation.w = p.qDriverFromHeadRotation.w = 1;
    p.result = TrackingResult_Uninitialized;
    return p;
}
void InputState::component(PropertyContainerHandle_t c, const char *n, VRInputComponentHandle_t h) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (container_ && c == container_ && n && !strcmp(n, "/proximity")) {
        proximity_.emplace(h, Proximity{}); ++sequence_;
    }
}
void InputState::physical_pose(uint32_t i, const DriverPose_t &p, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (i == index_) {
        if (size == sizeof(p)) physical_ = p;
        if (requested_) return;
    }
    host_->TrackedDevicePoseUpdated(i, p, size);
}
EVRInputError InputState::physical_boolean(VRInputComponentHandle_t h, bool value, double offset) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = proximity_.find(h);
    if (it != proximity_.end()) {
        it->second.physical = value; it->second.offset = offset;
        if (worn_) return VRInputError_None;
    }
    return input_->UpdateBooleanComponent(h, value, offset);
}
bool InputState::controller_update_locked(Controller &c, const std::string &path, ControllerInput &component, float value) {
    const auto error = component.boolean ? input_->UpdateBooleanComponent(component.handle, value != 0, 0) :
        input_->UpdateScalarComponent(component.handle, value, 0);
    if (error != VRInputError_None) {
        c.error_path = path; c.input_error = error;
        return false;
    }
    component.value = value; component.published = true;
    return true;
}
bool InputState::controller_input_release_locked(unsigned hand) {
    auto &c = controllers_.at(hand);
    if (!input_ || c.index == k_unTrackedDeviceIndexInvalid) return true;
    if (c.inputs_ready) { c.input_error = VRInputError_None; c.error_path.clear(); }
    bool ok = true;
    // Attempt every registered component even after an error, to release as much
    // as possible. Failed writes keep their last confirmed value in status.
    for (auto &entry : c.inputs) if (entry.second.handle != k_ulInvalidInputComponentHandle)
        if (!controller_update_locked(c, entry.first, entry.second, 0)) ok = false;
    return ok;
}
bool InputState::controller_release_locked(unsigned hand, bool force) {
    auto &c = controllers_.at(hand);
    const bool ok = controller_input_release_locked(hand);
    // A socket caller can retry a failed neutralization while still connected.
    // Runtime Deactivate cannot be refused: log failure before dropping handles.
    if (!ok && !force) return false;
    if (!ok) std::fprintf(stderr, "frame-input: forced controller release failed for %s: %s code %d\n",
                          hand == 0 ? "left" : "right", c.error_path.c_str(), int(c.input_error));
    const bool was_connected = bool(c.requested);
    c.requested.reset();
    if (was_connected && host_ && c.index != k_unTrackedDeviceIndexInvalid) {
        const auto pose = controller_pose(hand);
        host_->TrackedDevicePoseUpdated(c.index, pose, sizeof(pose));
    }
    return ok;
}
void InputState::publish_locked() {
    for (const auto &c : controllers_) if (host_ && c.index != k_unTrackedDeviceIndexInvalid && c.requested) {
        const auto pose = *c.requested;
        host_->TrackedDevicePoseUpdated(c.index, pose, sizeof(pose));
    }
    if (index_ == k_unTrackedDeviceIndexInvalid) return;
    if (requested_ && host_) host_->TrackedDevicePoseUpdated(index_, *requested_, sizeof(DriverPose_t));
    if (worn_ && input_) for (const auto &entry : proximity_) input_->UpdateBooleanComponent(entry.first, *worn_, 0);
}
void InputState::tick() { std::lock_guard<std::recursive_mutex> lock(mutex_); publish_locked(); }
std::string InputState::status_locked(bool ok, const char *error) {
    std::ostringstream o; o.imbue(std::locale::classic()); o << std::setprecision(17) << std::boolalpha;
    o << "{\"ok\":" << ok << ",\"sequence\":" << sequence_ << ",\"hmd_index\":";
    if (index_ == k_unTrackedDeviceIndexInvalid) o << "null"; else o << index_;
    o << ",\"hmd_container\":" << container_ << ",\"proximity_ready\":" << !proximity_.empty()
      << ",\"proximity_handles\":[";
    bool first = true;
    for (const auto &entry : proximity_) { if (!first) o << ','; o << entry.first; first = false; }
    o << "],\"pose_override\":" << bool(requested_) << ",\"worn_override\":";
    if (worn_) o << *worn_; else o << "null";
    o << ",\"pose\":"; pose_json(o, requested_ ? requested_ : physical_);
    o << ",\"physical_pose\":"; pose_json(o, physical_);
    o << ",\"physical_worn\":";
    if (!proximity_.empty() && proximity_.begin()->second.physical) o << *proximity_.begin()->second.physical;
    else o << "null";
    o << ",\"requested_pose\":";
    if (!requested_) o << "null";
    else {
        const auto &p = *requested_;
        o << '[' << p.vecPosition[0] << ',' << p.vecPosition[1] << ',' << p.vecPosition[2] << ','
          << p.qRotation.w << ',' << p.qRotation.x << ',' << p.qRotation.y << ',' << p.qRotation.z << ']';
    }
    o << ",\"controllers\":{";
    for (unsigned hand = 0; hand < controllers_.size(); ++hand) {
        const auto &c = controllers_[hand];
        if (hand) o << ',';
        o << '\"' << (hand == 0 ? "left" : "right") << "\":{\"device_index\":";
        if (c.index == k_unTrackedDeviceIndexInvalid) o << "null"; else o << c.index;
        o << ",\"pose_override\":" << bool(c.requested) << ",\"pose\":";
        pose_json(o, c.requested); o << ",\"synthetic\":true,\"inputs_ready\":" << c.inputs_ready << ",\"inputs\":{";
        bool first_input = true;
        for (const auto &entry : c.inputs) if (entry.second.published) {
            if (!first_input) o << ',';
            first_input = false;
            o << '\"' << entry.first << "\":";
            if (entry.second.boolean) o << bool(entry.second.value); else o << entry.second.value;
        }
        o << "},\"input_error\":";
        if (c.input_error == VRInputError_None) o << "null";
        else o << "{\"path\":\"" << c.error_path << "\",\"code\":" << int(c.input_error) << '}';
        o << '}';
    }
    o << '}';
    if (error) o << ",\"error\":\"" << error << '\"'; // Internal fixed strings, never unescaped user data.
    o << "}\n";
    return o.str();
}
std::string InputState::command(const std::string &line) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::istringstream in(line); in.imbue(std::locale::classic());
    std::string operation, extra;
    bool publication_ok = true;
    in >> operation;
    if (operation == "status") {
        if (in >> extra) return status_locked(false, "status takes no arguments");
        return status_locked(true, nullptr);
    }
    if (operation == "controller-inputs") {
        std::string side, path, token;
        if (!(in >> side) || (side != "left" && side != "right"))
            return status_locked(false, "controller-inputs needs left or right");
        auto &c = controllers_[side == "left" ? 0 : 1];
        if (!input_ || !c.inputs_ready) return status_locked(false, "controller inputs are not ready");
        if (!c.requested) return status_locked(false, "controller pose is not connected");
        std::set<std::string> seen;
        std::vector<std::pair<ControllerInputs::iterator, float>> updates;
        while (in >> path) {
            if (!(in >> token)) return status_locked(false, "controller-inputs needs path/value pairs");
            auto component = c.inputs.find(path);
            if (component == c.inputs.end()) return status_locked(false, "unsupported controller input path for this side");
            if (!seen.insert(path).second) return status_locked(false, "duplicate controller input path");
            double value;
            if (component->second.boolean) {
                if (token != "0" && token != "1") return status_locked(false, "boolean input needs 0 or 1");
                value = token == "1" ? 1 : 0;
            } else {
                std::istringstream scalar(token); scalar.imbue(std::locale::classic());
                if (!(scalar >> value) || !scalar.eof() || !std::isfinite(value) || value > 1 ||
                    value < (component->second.two_sided ? -1 : 0))
                    return status_locked(false, "scalar input must be finite and within its normalized range");
            }
            updates.emplace_back(component, static_cast<float>(value));
        }
        if (updates.empty()) return status_locked(false, "controller-inputs needs path/value pairs");
        // Validation is atomic; OpenVR component publication is not transactional.
        // Preserve caller order and record only successful writes. Never claim rollback.
        ++sequence_;
        c.input_error = VRInputError_None; c.error_path.clear();
        for (const auto &update : updates) if (!controller_update_locked(c, update.first->first, update.first->second, update.second))
            return status_locked(false, "controller input publication failed; earlier writes may have applied");
        return status_locked(true, nullptr);
    }
    if (operation == "pose" || operation == "controller-pose") {
        int hand = -1;
        if (operation == "controller-pose") {
            std::string side;
            if (!(in >> side) || (side != "left" && side != "right"))
                return status_locked(false, "controller-pose needs left or right");
            hand = side == "left" ? 0 : 1;
        }
        double values[7];
        for (auto &v : values) if (!(in >> v) || !std::isfinite(v)) return status_locked(false, "pose needs seven finite numbers");
        if (in >> extra) return status_locked(false, "pose needs seven finite numbers");
        double scale = 0;
        for (int i = 3; i < 7; ++i) scale = std::fmax(scale, std::fabs(values[i]));
        if (scale == 0) return status_locked(false, "quaternion must be nonzero");
        double norm = 0;
        for (int i = 3; i < 7; ++i) { values[i] /= scale; norm += values[i] * values[i]; }
        norm = std::sqrt(norm);
        if (hand >= 0) {
            if (!host_ || controllers_[hand].index == k_unTrackedDeviceIndexInvalid)
                return status_locked(false, "controller is not active");
        } else if (!host_ || index_ == k_unTrackedDeviceIndexInvalid) return status_locked(false, "HMD is not active");
        DriverPose_t p{};
        for (int i = 0; i < 3; ++i) p.vecPosition[i] = values[i];
        p.qRotation = {values[3] / norm, values[4] / norm, values[5] / norm, values[6] / norm};
        p.qWorldFromDriverRotation.w = p.qDriverFromHeadRotation.w = 1;
        p.poseIsValid = p.deviceIsConnected = true;
        p.result = TrackingResult_Running_OK;
        if (hand >= 0) controllers_[hand].requested = p;
        else requested_ = p;
    } else if (operation == "controller-release" || operation == "controller-input-release") {
        std::string side;
        if (!(in >> side) || (side != "left" && side != "right" && side != "all") || (in >> extra))
            return status_locked(false, "controller release needs left, right or all");
        for (unsigned hand = 0; hand < controllers_.size(); ++hand) {
            if ((side == "left" && hand == 1) || (side == "right" && hand == 0)) continue;
            const bool ok = operation == "controller-input-release" ? controller_input_release_locked(hand) : controller_release_locked(hand);
            publication_ok = ok && publication_ok;
        }
    } else if (operation == "worn") {
        std::string value;
        if (!(in >> value) || (value != "0" && value != "1") || (in >> extra))
            return status_locked(false, "worn needs 0 or 1");
        if (!input_ || proximity_.empty()) return status_locked(false, "HMD proximity is not ready");
        worn_ = value == "1";
    } else if (operation == "release" || operation == "pose-release" || operation == "worn-release") {
        if (in >> extra) return status_locked(false, "release takes no arguments");
        if (operation == "release") for (unsigned hand = 0; hand < controllers_.size(); ++hand)
            publication_ok = controller_release_locked(hand) && publication_ok;
        if (operation != "worn-release") {
            if (requested_ && physical_ && host_ && index_ != k_unTrackedDeviceIndexInvalid)
                host_->TrackedDevicePoseUpdated(index_, *physical_, sizeof(DriverPose_t));
            requested_.reset();
        }
        if (operation != "pose-release") {
            if (worn_ && input_) for (const auto &entry : proximity_) if (entry.second.physical)
                input_->UpdateBooleanComponent(entry.first, *entry.second.physical, entry.second.offset);
            worn_.reset();
        }
    } else return status_locked(false, "invalid command");
    ++sequence_;
    publish_locked();
    return status_locked(publication_ok, publication_ok ? nullptr : "controller input release failed; retry to neutralize remaining inputs");
}
} // namespace frame
