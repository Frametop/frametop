#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace framekeyboard {
// Boot opt-in is checked once. User-requested update restarts are normal launches.
std::vector<std::string> update_restart_arguments(const std::vector<std::string>& arguments);
// The UI only polls private packets. Network and installation work runs in a child.
class Updates {
  public:
    Updates(std::string version, std::vector<std::string> arguments, std::filesystem::path helper = {});
    ~Updates();
    Updates(const Updates&) = delete;
    Updates& operator=(const Updates&) = delete;
    std::string label() const;
    std::string failure_reason() const;
    bool busy() const;
    void click(double now);
    bool tick(double now);
    bool prepared() const { return state_ == State::Prepared; }
    // Call after App, input, VR, replacement and the instance lock are gone.
    bool handoff();

  private:
    enum class State { Idle, Checking, Latest, Available, Installing, Error, Prepared };
    bool start();
    bool send(const std::string& operation);
    void stop();
    void fail(double now, const std::string& reason = {});
    std::string version_, offer_, progress_, failure_;
    std::vector<std::string> arguments_;
    std::filesystem::path helper_;
    State state_{State::Idle};
    double reset_at_{};
    int fd_{-1}, child_{-1};
    bool handed_off_{};
};

// A private socket acknowledges three seconds of completed-loop progress after an update.
class UpdateStartup {
  public:
    UpdateStartup();
    ~UpdateStartup();
    // Update/rollback is explicit; hidden presentation does not recheck boot opt-in.
    bool background_start() const { return background_; }

  private:
    bool background_{};
};
void update_startup_ready(double now);
} // namespace framekeyboard
