#pragma once
#include "config.hpp"
#include <functional>
#include <stdexcept>

namespace framekeyboard {
// Boot launches from older units lack FRAMEKEYBOARD_SERVICE. Both those and
// marked update successors need the service's no-restart status, unlike manual VR.
int deliberate_exit_status(bool autostart);
// Raised only after unrelated preferences were saved successfully. The two
// startup-related flags retain their prior saved values when unit setup fails.
class StartupPreferencesWarning : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
void save_preferences_with_startup(const Settings& settings, const fs::path& executable,
                                   const fs::path& config_dir, fs::path unit_directory = {},
                                   std::function<void()> reload = {});
// Runs on the preference writer, never the VR loop. Tests supply a private unit
// directory and reload callback; previews never configure session startup.
// Persist runs after configuration succeeds; its failure restores the old unit.
void configure_startup(const Settings& settings, const fs::path& executable, const fs::path& config_dir,
                       fs::path unit_directory = {}, std::function<void()> reload = {},
                       std::function<void()> persist = {});
} // namespace framekeyboard
