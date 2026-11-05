#pragma once

#include "config.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace framekeyboard {
// Owns only preference snapshots, never App/input state. One worker serializes
// atomic writes; pending snapshots coalesce so an older write cannot win later.
class SettingsWriter {
  public:
    using Write = std::function<void(const Settings&)>;
    struct Result {
        std::string error;
        std::string startup_warning;
        bool reconcile{};
    };
    explicit SettingsWriter(fs::path directory, Write write = {}, Write reconcile = {});
    ~SettingsWriter();
    SettingsWriter(const SettingsWriter&) = delete;
    SettingsWriter& operator=(const SettingsWriter&) = delete;
    void enqueue(Settings settings);
    // Startup/Reload reconcile saved settings without writing JSON or changing profiles.
    void reconcile(Settings settings);
    std::optional<Result> take_result();
    // Queueing belongs to App's thread. It can defer Reload until all older
    // snapshots finish without waiting for the worker's I/O or startup work.
    bool idle();
    // Explicit durability barrier for shutdown and tests, never a UI action.
    void flush();

  private:
    void run();
    struct Request {
        Settings settings;
        bool reconcile{};
    };
    void queue(Request request);
    Write write_, reconcile_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Request> pending_;
    std::optional<Result> result_;
    bool writing_{}, stopping_{};
    std::thread worker_;
};
} // namespace framekeyboard
