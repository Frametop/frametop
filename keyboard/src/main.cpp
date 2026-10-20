#include "framekeyboard/app.hpp"
#include "framekeyboard/ei_input.hpp"
#include "framekeyboard/native_replacement.hpp"
#include "framekeyboard/startup.hpp"
#include "framekeyboard/updates.hpp"
#include "framekeyboard/vr_runtime.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace fk = framekeyboard;
namespace {
void help() {
    std::cout << "Full Keyboard for Steam Frame " << FRAMEKEYBOARD_VERSION << "\n\n"
              << "  --preview                 Native desktop preview, no system input\n"
              << "  --vr                      Manually launched native VR panel\n"
              << "  --autostart               Start VR replacement hidden, if enabled in Settings\n"
              << "  --render FILE.png         Render the keyboard without a window\n"
              << "  --render-settings FILE    Render the profile selector\n"
              << "  --check                   Validate and list available profiles\n"
              << "  --describe-layout         Describe the active key geometry\n"
              << "  --input ei|uinput         Use compositor input or a virtual kernel device\n"
              << "  --ei-socket PATH          Override the Gamescope input socket\n"
              << "  --text-socket PATH        Override its Japanese Wayland text socket\n"
              << "  --start-enabled           Enable typing at startup with an explicit backend\n"
              << "  --target-language ID      Physical uinput/external-JIS target keymap\n"
              << "  --config-dir PATH         User profiles/settings directory\n"
              << "  --data-dir PATH           Extra bundled profile directory\n"
              << "  --duration SECONDS        Exit a preview/VR smoke test after this time\n\n"
              << "Typing stays disabled unless --start-enabled is supplied.\n"
              << "Native replacement and SteamOS startup are optional in Settings.\n";
}
void signal_handler(int) {
    fk::interrupted = 1;
}
} // namespace
int main(int argc, char** argv) {
    try {
        fk::UpdateStartup startup;
        fk::Updates updates(FRAMEKEYBOARD_VERSION, std::vector<std::string>(argv + 1, argv + argc));
        fk::Options options;
        options.config_dir = fk::default_config_dir();
        bool mode_set = false;
        std::optional<fk::InputBackend> input = fk::InputBackend::None;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc) {
                    throw std::runtime_error("missing value for " + argument);
                }
                return argv[i];
            };
            auto mode = [&](fk::Mode name) {
                if (mode_set) {
                    throw std::runtime_error("choose one operation");
                }
                options.mode = name;
                mode_set = true;
            };
            if (argument == "--help") {
                help();
                return 0;
            } else if (argument == "--version") {
                std::cout << "framekeyboard " << FRAMEKEYBOARD_VERSION << '\n';
                return 0;
            } else if (argument == "--preview") {
                mode(fk::Mode::Preview);
            } else if (argument == "--vr") {
                mode(fk::Mode::Vr);
            } else if (argument == "--render" || argument == "--render-settings") {
                mode(argument == "--render" ? fk::Mode::Render : fk::Mode::RenderSettings);
                options.output = value();
            } else if (argument == "--check") {
                mode(fk::Mode::Check);
            } else if (argument == "--describe-layout") {
                mode(fk::Mode::Describe);
            } else if (argument == "--config-dir") {
                options.config_dir = value();
            } else if (argument == "--data-dir") {
                options.data_dir = value();
            } else if (argument == "--ei-socket") {
                options.ei_socket = value();
            } else if (argument == "--text-socket") {
                options.text_socket = value();
                if (options.text_socket.empty()) {
                    throw std::runtime_error("--text-socket requires a nonempty path");
                }
            } else if (argument == "--input") {
                const auto name = value();
                if (name == "none") {
                    input = fk::InputBackend::None;
                } else if (name == "ei") {
                    input = fk::InputBackend::Ei;
                } else if (name == "uinput") {
                    input = fk::InputBackend::Uinput;
                } else {
                    input.reset();
                }
            } else if (argument == "--start-enabled") {
                options.start_enabled = true;
            } else if (argument == "--autostart") {
                options.autostart = true;
            } else if (argument == "--target-language") {
                options.target_language = value();
            } else if (argument == "--duration") {
                const auto seconds = value();
                std::size_t end = 0;
                options.duration = std::stod(seconds, &end);
                if (end != seconds.size() || !std::isfinite(options.duration) || options.duration <= 0) {
                    throw std::runtime_error("duration must be a positive finite number");
                }
            } else {
                throw std::runtime_error("unknown argument: " + argument);
            }
        }
        if (options.mode == fk::Mode::Help) {
            help();
            return 0;
        }
        if (!input) {
            throw std::runtime_error("input must be none, ei or uinput");
        }
        options.input = *input;
        if (options.input != fk::InputBackend::None &&
            (options.mode != fk::Mode::Vr ||
             (options.input == fk::InputBackend::Uinput && options.target_language.empty()))) {
            throw std::runtime_error("typing requires --vr; uinput also requires --target-language; "
                                     "desktop preview never injects input");
        }
        if (!options.text_socket.empty() && options.input != fk::InputBackend::Ei) {
            throw std::runtime_error("--text-socket requires --input ei");
        }
        if (options.start_enabled && options.input == fk::InputBackend::None) {
            throw std::runtime_error("--start-enabled requires an input backend");
        }
        options.background_update = options.mode == fk::Mode::Vr && startup.background_start();
        const bool start_hidden = options.autostart || options.background_update;
        const int deliberate_exit = fk::deliberate_exit_status(options.autostart);
        if (options.autostart) {
            if (options.mode != fk::Mode::Vr) {
                throw std::runtime_error("--autostart requires --vr");
            }
            std::vector<std::string> errors;
            const auto settings = fk::load_settings(options.config_dir, errors);
            // Check both persisted preferences before opening input, VR or helpers.
            // A stale unit cannot start replacement after the user disables it.
            if (!errors.empty() || !settings.replace_native_keyboard || !settings.start_with_steamos) {
                std::cout << "SteamOS keyboard startup is disabled.\n";
                return deliberate_exit;
            }
        }
        int result{};
        {
            std::unique_ptr<fk::VrInstance> instance;
            if (options.mode == fk::Mode::Vr) {
                instance =
                    std::make_unique<fk::VrInstance>(fk::VrInstance::runtime_directory(), !start_hidden);
                if (!instance->is_owner()) {
                    std::cout << (start_hidden ? "Full Keyboard is already running.\n"
                                               : "Recenter requested from the running keyboard.\n");
                    return deliberate_exit;
                }
                fk::require_steamvr();
            }
            if (options.mode == fk::Mode::Check || options.mode == fk::Mode::Describe) {
                auto profiles = fk::load_profiles(options.data_dir, options.config_dir);
                const auto settings = fk::load_settings(options.config_dir, profiles.errors);
                fk::validate_selection(profiles, settings.active);
                if (options.mode == fk::Mode::Describe) {
                    const auto& layout = profiles.layouts.at(settings.active.layout);
                    std::cout << layout.name << '\n'
                              << layout.keys.size() << " keys; " << layout.width << " x "
                              << layout.height << " design area\n";
                } else {
                    for (const auto& [id, p] : profiles.layouts) {
                        std::cout << "layout " << id << ": " << p.name << '\n';
                    }
                    for (const auto& [id, p] : profiles.languages) {
                        std::cout << "language " << id << ": " << p.name << '\n';
                    }
                    for (const auto& [id, p] : profiles.themes) {
                        std::cout << "theme " << id << ": " << p.name << '\n';
                    }
                }
                for (const auto& error : profiles.errors) {
                    std::cerr << error << '\n';
                }
                return profiles.errors.empty() ? 0 : 1;
            }
            std::unique_ptr<fk::KeySink> sink;
            if (options.input != fk::InputBackend::None && !options.target_language.empty()) {
                const auto profiles = fk::load_profiles(options.data_dir, options.config_dir);
                if (!profiles.languages.contains(options.target_language)) {
                    throw std::runtime_error("unknown target-language profile");
                }
            }
            if (options.input == fk::InputBackend::Ei) {
                if (options.ei_socket.empty()) {
                    options.ei_socket =
                        std::string("/run/user/") + std::to_string(getuid()) + "/gamescope-0-ei";
                }
                sink = std::make_unique<fk::EiSink>(options.ei_socket, options.text_socket);
            } else if (options.input == fk::InputBackend::Uinput) {
                sink = std::make_unique<fk::UInputSink>();
            } else {
                sink = std::make_unique<fk::NullSink>();
            }
            fk::NativeReplacement replacement(*sink);
            fk::SettingsWriter::Write preferences;
            fk::SettingsWriter::Write reconcile_preferences;
            if (options.mode == fk::Mode::Vr) {
                preferences = [config = options.config_dir,
                               executable =
                                   fk::fs::canonical("/proc/self/exe")](const fk::Settings& settings) {
                    fk::save_preferences_with_startup(settings, executable, config);
                };
                reconcile_preferences =
                    [config = options.config_dir,
                     executable = fk::fs::canonical("/proc/self/exe")](const fk::Settings& settings) {
                        fk::configure_startup(settings, executable, config);
                    };
            }
            fk::App app(options, replacement, std::move(preferences),
                        options.mode == fk::Mode::Vr ? &updates : nullptr,
                        std::move(reconcile_preferences));
            if (options.mode == fk::Mode::Render || options.mode == fk::Mode::RenderSettings) {
                if (options.mode == fk::Mode::RenderSettings) {
                    app.show_settings();
                }
                app.paint(fk::monotonic_seconds());
                app.renderer.write_png(options.output);
                return 0;
            }
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
            result = options.mode == fk::Mode::Vr
                         ? fk::run_vr(app, *instance, replacement, options.duration)
                         : fk::run_preview(app, options.duration);
            // A watchdog's SIGTERM must still permit service recovery. systemctl
            // stop already suppresses restart independently of our exit status.
            if (options.mode == fk::Mode::Vr && (app.quitting() || options.duration > 0)) {
                result = deliberate_exit;
            }
            app.flush_settings();
        } // Destroy VR, replacement helpers, input and the instance lock before handoff.
        if (updates.prepared() && !fk::interrupted) {
            if (updates.handoff()) {
                return deliberate_exit;
            }
            if (fk::interrupted) {
                return deliberate_exit;
            }
            // If the prepared helper disappeared, restart the still-current build.
            auto arguments =
                fk::update_restart_arguments(std::vector<std::string>(argv + 1, argv + argc));
            arguments.insert(arguments.begin(), argv[0]);
            std::vector<char*> restart;
            for (auto& argument : arguments) {
                restart.push_back(argument.data());
            }
            restart.push_back(nullptr);
            execv("/proc/self/exe", restart.data());
            throw std::runtime_error("could not restart keyboard after update failure");
        }
        return result;
    } catch (const std::exception& error) {
        std::cerr << "Full Keyboard: " << error.what() << '\n';
        return 1;
    }
}
