#include "framekeyboard/updates.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <json-c/json.h>
#include <memory>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace framekeyboard {
namespace {
int startup_fd = -1;
double startup_started = -1, startup_last = -1;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
std::string field(json_object* object, const char* key) {
    json_object* value{};
    return json_object_object_get_ex(object, key, &value) &&
                   json_object_get_type(value) == json_type_string
               ? json_object_get_string(value)
               : "";
}
std::string failure_message(const std::string& code) {
    static const std::pair<const char*, const char*> reasons[] = {
        {"network-unavailable", "Network unavailable. Check your connection."},
        {"server-unavailable", "GitHub could not serve the update. Try again later."},
        {"rate-limit", "GitHub request limit reached. Try again later."},
        {"download-timeout", "The update download timed out."},
        {"download-failed", "The update download is incomplete or invalid."},
        {"checksum-failed", "Update checksum verification failed."},
        {"install-busy", "Another installation is running."},
        {"not-installed-release", "Install a release package before updating."},
        {"installation-changed", "The installation changed. Relaunch Full Keyboard."},
        {"wrong-platform", "Updates require a Steam Frame ARM64 installation."},
        {"version-mismatch", "The package version does not match the release."},
        {"startup-failed", "The updated keyboard could not finish startup."},
        {"service-handoff-failed", "The keyboard service could not take over the update."},
        {"validation-failed", "The update package could not run its validation."},
        {"invalid-release", "The release package or metadata is invalid."},
        {"disk-full", "Not enough disk space to install the update."},
        {"permission-denied", "The installation is not writable."},
        {"files-unavailable", "Update files could not be read or written."},
        {"helper-unavailable", "The update helper could not start."},
        {"connection-lost", "The update helper connection was lost."}};
    for (const auto& [reason, message] : reasons) {
        if (code == reason) {
            return message;
        }
    }
    return "The update could not be completed. Try again.";
}
} // namespace
std::vector<std::string> update_restart_arguments(const std::vector<std::string>& arguments) {
    std::vector<std::string> result;
    bool value = false;
    for (const auto& argument : arguments) {
        if (value) {
            result.push_back(argument);
            value = false;
        } else if (argument != "--autostart") {
            result.push_back(argument);
            value = argument == "--config-dir" || argument == "--data-dir" ||
                    argument == "--ei-socket" || argument == "--text-socket" || argument == "--input" ||
                    argument == "--target-language" || argument == "--duration" ||
                    argument == "--render" || argument == "--render-settings";
        }
    }
    return result;
}
Updates::Updates(std::string version, std::vector<std::string> arguments, std::filesystem::path helper)
    : version_(std::move(version)), arguments_(update_restart_arguments(arguments)),
      helper_(std::move(helper)) {
}
Updates::~Updates() {
    if (handed_off_) {
        if (fd_ >= 0) {
            close(fd_);
        }
    } else {
        stop();
    }
}
void Updates::stop() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (child_ > 0) {
        if (waitpid(child_, nullptr, WNOHANG) == 0) {
            kill(child_, SIGTERM);
            for (int i = 0; i < 100 && waitpid(child_, nullptr, WNOHANG) == 0; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (waitpid(child_, nullptr, WNOHANG) == 0) {
                kill(child_, SIGKILL);
                waitpid(child_, nullptr, 0);
            }
        }
        child_ = -1;
    }
}
bool Updates::start() {
    if (fd_ >= 0) {
        return true;
    }
    if (child_ > 0) {
        return false;
    }
    const auto executable = std::filesystem::canonical("/proc/self/exe");
    auto helper = helper_;
    if (helper.empty()) {
        helper = executable.parent_path().parent_path() / "share/framekeyboard/update/update_helper.py";
        if (!std::filesystem::is_regular_file(helper)) {
            helper = executable.parent_path() / "update/update_helper.py";
        }
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets) != 0) {
        return false;
    }
    std::vector<std::string> args{
        "python3",           "-I",        helper.string(), "--fd", "3", "--executable",
        executable.string(), "--version", version_,        "--"};
    args.insert(args.end(), arguments_.begin(), arguments_.end());
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    // Reserve a descriptor above 3 so dup2 never aliases another action.
    const int source = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (source >= 0) {
        posix_spawn_file_actions_adddup2(&actions, source, 3);
        posix_spawn_file_actions_addclose(&actions, source);
    }
    posix_spawn_file_actions_addclosefrom_np(&actions, 4);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    // Keep the launcher's normal log destinations across helper/app replacement.
    pid_t child{};
    const int result =
        source < 0 ? errno : posix_spawnp(&child, "python3", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (source >= 0) {
        close(source);
    }
    close(sockets[1]);
    if (result != 0) {
        close(sockets[0]);
        return false;
    }
    fd_ = sockets[0];
    child_ = child;
    return true;
}
bool Updates::send(const std::string& operation) {
    Json packet(json_object_new_object(), json_object_put);
    json_object_object_add(packet.get(), "op", json_object_new_string(operation.c_str()));
    json_object_object_add(packet.get(), "version", json_object_new_string(offer_.c_str()));
    const std::string data = json_object_to_json_string_ext(packet.get(), JSON_C_TO_STRING_PLAIN);
    return fd_ >= 0 && ::send(fd_, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL) ==
                           static_cast<ssize_t>(data.size());
}
bool Updates::busy() const {
    return state_ == State::Checking || state_ == State::Installing || state_ == State::Prepared;
}
std::string Updates::label() const {
    switch (state_) {
    case State::Checking:
        return "Checking for updates...";
    case State::Latest:
        return "You are on latest version";
    case State::Available:
        return "Download and install update";
    case State::Installing:
        return progress_.empty() ? "Downloading update..." : progress_;
    case State::Error:
        return "Update failed. Try again";
    case State::Prepared:
        return "Restarting keyboard...";
    case State::Idle:
        return "Check for updates";
    }
    return {};
}
std::string Updates::failure_reason() const {
    return state_ == State::Error ? failure_ : "";
}
void Updates::fail(double now, const std::string& reason) {
    state_ = State::Error;
    failure_ = failure_message(reason);
    std::cerr << failure_ << '\n';
    reset_at_ = now + 5;
}
void Updates::click(double now) {
    if (busy() || state_ == State::Latest || state_ == State::Error) {
        return;
    }
    if (!start() || !send(offer_.empty() ? "check" : "install")) {
        fail(now, "helper-unavailable");
        return;
    }
    state_ = offer_.empty() ? State::Checking : State::Installing;
    progress_.clear();
}
bool Updates::tick(double now) {
    const auto before = label();
    const auto before_reason = failure_reason();
    if ((state_ == State::Latest || state_ == State::Error) && now >= reset_at_) {
        state_ = offer_.empty() ? State::Idle : State::Available;
    }
    for (int i = 0; fd_ >= 0 && i < 8; ++i) {
        std::array<char, 2048> data{};
        const auto count = recv(fd_, data.data(), data.size(), MSG_DONTWAIT | MSG_TRUNC);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;
        }
        if (count <= 0 || count >= static_cast<ssize_t>(data.size())) {
            close(fd_);
            fd_ = -1;
            if (!prepared()) {
                fail(now, "connection-lost");
            }
            break;
        }
        Json packet(json_tokener_parse(data.data()), json_object_put);
        const auto state = packet ? field(packet.get(), "state") : "";
        if (state == "latest" && state_ == State::Checking) {
            state_ = State::Latest;
            reset_at_ = now + 5;
        } else if (state == "available" && state_ == State::Checking) {
            offer_ = field(packet.get(), "version");
            state_ = offer_.empty() ? State::Error : State::Available;
            reset_at_ = now + 5;
        } else if (state == "progress" && state_ == State::Installing) {
            progress_ =
                field(packet.get(), "phase") == "verify" ? "Verifying update..." : "Preparing update...";
        } else if (state == "prepared" && state_ == State::Installing) {
            state_ = State::Prepared;
        } else if (state == "error") {
            fail(now, field(packet.get(), "reason"));
        }
    }
    if (child_ > 0 && !prepared() && waitpid(child_, nullptr, WNOHANG) == child_) {
        child_ = -1;
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (busy()) {
            fail(now, "connection-lost");
        }
    }
    return before != label() || before_reason != failure_reason();
}
bool Updates::handoff() {
    const bool supervised = std::getenv("FRAMEKEYBOARD_SERVICE") && std::getenv("NOTIFY_SOCKET");
    handed_off_ = prepared() && send(supervised ? "activate-supervised" : "activate");
    if (!handed_off_ || !supervised) {
        return handed_off_;
    }
    // VR, input and the instance lock are already gone. Keep the registered
    // MainPID alive until the helper verifies systemd has adopted its successor.
    // Allow both 45-second startup windows plus termination and manager queries.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(150);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{fd_, POLLIN, 0};
        if (poll(&descriptor, 1, 250) < 0) {
            return false;
        }
        if (!descriptor.revents) {
            continue;
        }
        std::array<char, 2048> packet{};
        const auto count = recv(fd_, packet.data(), packet.size(), MSG_DONTWAIT | MSG_TRUNC);
        if (count <= 0 || count > static_cast<ssize_t>(packet.size())) {
            return false;
        }
        const std::string text(packet.data(), static_cast<std::size_t>(count));
        Json result(json_tokener_parse(text.c_str()), json_object_put);
        return result && field(result.get(), "state") == "activated";
    }
    return false;
}
UpdateStartup::UpdateStartup() {
    if (const char* value = std::getenv("FRAMEKEYBOARD_UPDATE_READY_FD")) {
        const std::string text(value);
        unsetenv("FRAMEKEYBOARD_UPDATE_READY_FD");
        int fd{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), fd);
        struct stat info{};
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && fd >= 3 &&
            fstat(fd, &info) == 0 && S_ISSOCK(info.st_mode)) {
            startup_fd = fd;
            startup_started = startup_last = -1;
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            const auto service = std::getenv("FRAMEKEYBOARD_SERVICE");
            background_ = service && *service;
        }
    }
}
UpdateStartup::~UpdateStartup() {
    if (startup_fd >= 0) {
        close(startup_fd);
        startup_fd = -1;
    }
}
void update_startup_ready(double now) {
    if (startup_fd >= 0) {
        // Hidden residents complete a loop every 250 ms. A long stall restarts
        // probation; elapsed time alone must not acknowledge a blocked main loop.
        if (!std::isfinite(now)) {
            startup_started = startup_last = -1;
            return;
        }
        if (startup_last < 0 || now < startup_last || now - startup_last > 1) {
            startup_started = now;
        }
        startup_last = now;
        if (now - startup_started < 3) {
            return;
        }
        const auto ignored = ::send(startup_fd, "1", 1, MSG_NOSIGNAL);
        (void)ignored;
        close(startup_fd);
        startup_fd = -1;
    }
}
} // namespace framekeyboard
