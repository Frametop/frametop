#pragma once
#include "placement.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>

namespace framekeyboard {
struct SavedPlacement {
    Transform transform;
    double width;
    std::uint64_t universe;
    std::optional<Transform> dashboard{};
    std::optional<Transform> dashboard_bar{};
    bool dashboard_full_rotation{};
};
std::optional<SavedPlacement> load_placement(const std::filesystem::path& path);
void save_placement(const std::filesystem::path& path, const SavedPlacement& placement);
} // namespace framekeyboard
