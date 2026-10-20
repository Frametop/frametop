#include "framekeyboard/startup.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace framekeyboard {
namespace {
constexpr auto name = "framekeyboard-autostart.service";
constexpr auto marker = "# Managed by Full Keyboard\n";
std::string condition_path(const fs::path& path) {
    std::string result;
    for (const auto c : path.string()) {
        if (static_cast<unsigned char>(c) < 32 || c == 127) {
            throw std::runtime_error("Startup paths cannot contain control characters.");
        }
        result += c;
        if (c == '%') {
            result += '%'; // Literal percent, not a systemd specifier.
        }
    }
    return result;
}
std::string quoted(const fs::path& path) {
    std::string result = "\"";
    for (const auto c : condition_path(path)) {
        if (c == '\\' || c == '"') {
            result += '\\';
        }
        result += c;
    }
    return result + '"';
}
void write_atomic(const fs::path& path, const std::string& text) {
    auto temporary = (path.parent_path() / ".framekeyboard-startup-XXXXXX").string();
    const int fd = mkstemp(temporary.data());
    if (fd < 0) {
        throw std::runtime_error("Could not prepare SteamOS startup settings.");
    }
    try {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto count = write(fd, text.data() + offset, text.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::runtime_error("Could not write SteamOS startup settings.");
            }
            offset += static_cast<std::size_t>(count);
        }
        if (fsync(fd) != 0) {
            throw std::runtime_error("Could not save SteamOS startup settings.");
        }
        fs::rename(temporary, path);
    } catch (...) {
        close(fd);
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
    close(fd);
}
void reload_manager() {
    char command[] = "systemctl", user[] = "--user", reload[] = "daemon-reload";
    char* args[]{command, user, reload, nullptr};
    pid_t pid{};
    if (posix_spawnp(&pid, command, nullptr, nullptr, args, environ) != 0) {
        throw std::runtime_error("SteamOS startup manager is unavailable.");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                return;
            }
            throw std::runtime_error("Could not refresh SteamOS startup settings.");
        }
        if (result < 0 && errno != EINTR) {
            throw std::runtime_error("Could not wait for the SteamOS startup manager.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // This is only our bounded systemctl client, never Steam or a shared service.
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    throw std::runtime_error("Could not refresh SteamOS startup settings.");
}
} // namespace
int deliberate_exit_status(bool autostart) {
    const auto service = std::getenv("FRAMEKEYBOARD_SERVICE");
    return autostart || (service && *service) ? 3 : 0;
}
void save_preferences_with_startup(const Settings& settings, const fs::path& executable,
                                   const fs::path& config_dir, fs::path unit_directory,
                                   std::function<void()> reload) {
    std::vector<std::string> errors;
    const auto previous = load_settings(config_dir, errors);
    if (!errors.empty()) {
        throw std::runtime_error("Could not read saved preferences: " + errors.front());
    }
    bool persist_failed = false;
    try {
        configure_startup(settings, executable, config_dir, std::move(unit_directory), std::move(reload),
                          [&] {
                              try {
                                  save_settings(config_dir, settings);
                              } catch (...) {
                                  persist_failed = true;
                                  throw;
                              }
                          });
    } catch (const std::exception& error) {
        if (persist_failed) {
            throw; // configure_startup already rolled the unit back after a storage error.
        }
        auto saved = settings;
        saved.replace_native_keyboard = previous.replace_native_keyboard;
        saved.start_with_steamos = previous.start_with_steamos;
        save_settings(config_dir, saved); // A failure here is still a real unsaved-preferences error.
        const bool changed = settings.replace_native_keyboard != previous.replace_native_keyboard ||
                             settings.start_with_steamos != previous.start_with_steamos;
        throw StartupPreferencesWarning(
            std::string(changed ? "Other preferences saved. Replacement/startup changes not saved: "
                                : "Preferences saved. SteamOS startup not configured: ") +
            error.what());
    }
}
void configure_startup(const Settings& settings, const fs::path& executable, const fs::path& config_dir,
                       fs::path unit_directory, std::function<void()> reload,
                       std::function<void()> persist) {
    const bool enabled = settings.replace_native_keyboard && settings.start_with_steamos;
    if (unit_directory.empty()) {
        unit_directory = default_config_dir().parent_path() / "systemd/user";
    }
    const auto unit = unit_directory / name;
    const auto link = unit_directory / "steamvr.service.wants" / name;
    const auto legacy_link = unit_directory / "gamescope-session.target.wants" / name;
    const fs::path link_target = fs::path("..") / name;
    std::optional<std::string> previous;
    if (fs::exists(unit) || fs::is_symlink(unit)) {
        if (fs::is_symlink(unit) || !fs::is_regular_file(unit) || fs::file_size(unit) > 65536) {
            throw std::runtime_error("A custom SteamOS startup unit already exists.");
        }
        std::ifstream stream(unit);
        if (!stream) {
            throw std::runtime_error("Could not read SteamOS startup settings.");
        }
        previous = std::string(std::istreambuf_iterator<char>(stream), {});
        if (!previous->starts_with(marker)) {
            throw std::runtime_error("A custom SteamOS startup unit already exists.");
        }
    }
    auto owned_link = [&](const fs::path& path) {
        const bool linked = fs::is_symlink(path);
        if ((linked && fs::read_symlink(path) != link_target) || (!linked && fs::exists(path))) {
            throw std::runtime_error("A custom SteamOS startup link already exists.");
        }
        return linked;
    };
    const bool linked = owned_link(link), legacy_linked = owned_link(legacy_link);
    if (!enabled && !previous && !linked && !legacy_linked) {
        if (persist) {
            persist();
        }
        return;
    }
    std::string contents;
    if (enabled) {
        const auto binary = fs::canonical(executable);
        const auto old = binary.parent_path().parent_path();
        const auto root = old.parent_path().parent_path();
        if (binary.filename() != "framekeyboard" || binary.parent_path().filename() != "bin" ||
            old.parent_path().filename() != "releases" || !fs::is_symlink(root / "current") ||
            fs::canonical(root / "current") != old) {
            throw std::runtime_error("Install a release package before enabling SteamOS startup.");
        }
        const auto current = root / "current";
        contents = std::string(marker) +
                   "[Unit]\nDescription=Full Keyboard background replacement\n"
                   "After=gamescope-session.service steamvr.service\n"
                   "PartOf=graphical-session.target steamvr.service\nStartLimitIntervalSec=0\n"
                   "ConditionPathExists=" +
                   condition_path(current / "bin/framekeyboard") +
                   "\n\n[Service]\nType=exec\nNotifyAccess=all\n"
                   "Environment=FRAMEKEYBOARD_SERVICE=framekeyboard-autostart.service\n"
                   "EnvironmentFile=-/usr/share/deckard/mesavars.sh\nExecStart=" +
                   quoted(current / "bin/framekeyboard") +
                   " --vr --input ei --start-enabled --autostart --data-dir " +
                   quoted(current / "share/framekeyboard") + " --config-dir " +
                   quoted(fs::absolute(config_dir)) +
                   "\nRestart=always\nRestartPreventExitStatus=3\nSuccessExitStatus=3\n"
                   "RestartSec=3\nRestartSteps=5\nRestartMaxDelaySec=60\nTimeoutStopSec=5\n"
                   "# Updater and native-fallback helpers must outlive the parent during handoff.\n"
                   "KillMode=process\n\n[Install]\nWantedBy=steamvr.service\n";
        if (previous == contents && linked && !legacy_linked) {
            if (persist) {
                persist();
            }
            return;
        }
    }
    fs::create_directories(unit_directory);
    try {
        if (enabled) {
            write_atomic(unit, contents);
            fs::create_directories(link.parent_path());
            if (!linked) {
                fs::create_symlink(link_target, link);
            }
        } else {
            fs::remove(link);
            fs::remove(unit);
        }
        fs::remove(legacy_link);
        (reload ? reload : reload_manager)();
        if (persist) {
            persist();
        }
    } catch (...) {
        // Keep a failed toggle from leaving a half-written boot configuration.
        try {
            if (previous) {
                write_atomic(unit, *previous);
            } else {
                fs::remove(unit);
            }
            if (linked && !fs::is_symlink(link)) {
                fs::create_symlink(link_target, link);
            } else if (!linked) {
                fs::remove(link);
            }
            if (legacy_linked && !fs::is_symlink(legacy_link)) {
                fs::create_symlink(link_target, legacy_link);
            } else if (!legacy_linked) {
                fs::remove(legacy_link);
            }
            // A successful provisional reload must not outlive rolled-back files.
            (reload ? reload : reload_manager)();
        } catch (...) {
        }
        throw;
    }
}
} // namespace framekeyboard
