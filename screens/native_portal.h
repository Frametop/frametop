#pragma once
#include <chrono>
#include <string>
namespace native_portal {
// Intent belongs to ft-screens and is accessed only on its VR thread.
class Owner {
    bool wanted_ = false;
    std::string reason_ = "publisher-unavailable";
    std::chrono::steady_clock::time_point publisher_{};
public:
    bool command(const char*, char*, int);
    bool wanted() const { return wanted_; }
    bool connected() const {
        return publisher_ != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - publisher_ < std::chrono::milliseconds(500);
    }
    void shutdown() { wanted_ = false; publisher_ = {}; }
};
}
