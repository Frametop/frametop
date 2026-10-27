#pragma once

#include "../vendor/openvr.h"
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace frame_observe {

// Public OpenVR interfaces only. Never link to vrclient's private ABI.
class Sdk {
    void *library_ = nullptr;
    bool initialized_ = false;
    using Init = uint32_t (*)(vr::EVRInitError *, vr::EVRApplicationType, const char *);
    using Shutdown = void (*)();
    using Valid = bool (*)(const char *);
    using Generic = void *(*)(const char *, vr::EVRInitError *);
    Shutdown shutdown_ = nullptr;
    Valid valid_ = nullptr;
    Generic generic_ = nullptr;

    template<class T> T symbol(const char *name) {
        void *address = dlsym(library_, name);
        if (!address) throw std::runtime_error(std::string("OpenVR symbol unavailable: ") + name);
        return reinterpret_cast<T>(address);
    }
    void close() noexcept {
        if (initialized_) shutdown_();
        initialized_ = false;
        if (library_) dlclose(library_);
        library_ = nullptr;
    }

public:
    Sdk() {
        try {
            std::vector<std::string> paths;
            if (const char *override_path = std::getenv("OPENVR_API_LIBRARY")) {
                // An explicit override must never silently fall back to live VR.
                paths.emplace_back(override_path);
            } else {
#if defined(__aarch64__)
                const std::string platform = "linuxarm64";
#else
                const std::string platform = "linux64";
#endif
                paths.push_back("/opt/steamvr/bin/" + platform + "/libopenvr_api.so");
                if (const char *home = std::getenv("HOME")) {
                    for (const char *steam : {"/.steam/steam", "/.local/share/Steam"})
                        paths.push_back(std::string(home) + steam + "/steamapps/common/SteamVR/bin/" + platform + "/libopenvr_api.so");
                }
                paths.emplace_back("libopenvr_api.so");
            }
            std::string reason;
            for (const auto &path : paths) {
                if (path.empty()) { reason = "empty library path"; continue; }
                library_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (library_) break;
                if (const char *error = dlerror()) reason = error;
            }
            if (!library_) throw std::runtime_error("cannot load OpenVR API library: " + reason);
            auto init = symbol<Init>("VR_InitInternal2");
            shutdown_ = symbol<Shutdown>("VR_ShutdownInternal");
            valid_ = symbol<Valid>("VR_IsInterfaceVersionValid");
            generic_ = symbol<Generic>("VR_GetGenericInterface");
            vr::EVRInitError error = vr::VRInitError_None;
            init(&error, vr::VRApplication_Background, nullptr);
            if (error != vr::VRInitError_None)
                throw std::runtime_error("OpenVR Background initialization failed: " + std::to_string(error));
            initialized_ = true;
            // Match the public header's VR_Init version guard.
            if (!valid_(vr::IVRSystem_Version))
                throw std::runtime_error(std::string("OpenVR header/runtime version mismatch: ") + vr::IVRSystem_Version);
        } catch (...) { close(); throw; }
    }
    ~Sdk() { close(); }
    Sdk(const Sdk &) = delete;
    Sdk &operator=(const Sdk &) = delete;

    template<class T> T *get(const char *version) {
        if (!valid_(version))
            throw std::runtime_error(std::string("OpenVR interface version unavailable: ") + version);
        vr::EVRInitError error = vr::VRInitError_None;
        auto *result = static_cast<T *>(generic_(version, &error));
        if (!result || error != vr::VRInitError_None)
            throw std::runtime_error(std::string("OpenVR interface unavailable: ") + version + " error " + std::to_string(error));
        return result;
    }
};

} // namespace frame_observe
